// Atomic ACIR-to-ACSim whole-model lowering (ac-lower-to-acsim).
//
// Converts one frozen, verified ACIR file into one canonical acsim.model in a
// single transaction:
//   ac.module (concrete, () -> ())  -> acsim.module with ownership placements
//   ac.instance / ac.instances      -> acsim.instance (one per named member)
//   ac.array (homogeneous)          -> acsim.array
//   ac.module.extern                -> acsim.binding from the in-memory exact
//                                      binding resolution (no lock round-trip)
//   ac.process (yield-only or i32 queue datapath)
//                                    -> acsim.process enum-PC state machine
//   ac.queue (signless integer fifo, widths 1..64) -> SimQueue + invoke callees
//   watermarks.kind register/regfile (or legacy names pc/busy/rf)
//                                    -> gfsim::Register / RegFile members
//   selected ac.system              -> acsim.model with exact fingerprints,
//                                      canonical construction/destruction
//                                      order, and dispatch rows. Processes
//                                      reschedule through scheduleWork.
//
// Every validation failure is diagnosed with an ACLOWER-* code before any IR
// mutation, so a rejected input never publishes a partial acsim.model.
#include "acir/Conversion/ACIRToACSim/ACIRToACSim.h"

#include "Analysis/ProcessStatePlanInternal.h"
#include "Dialect/ACIR/ProcessLowerability.h"
#include "acir/Analysis/ProcessStatePlan.h"
#include "acir/Bindings/Binding.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACSim/ACSimDialect.h"
#include "acir/Dialect/ACSim/ACSimOps.h"
#include "acir/Dialect/ACSim/ACSimTypes.h"
#include "acir/Transforms/ResolveBindings.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace mlir;

namespace acir {
namespace {

constexpr llvm::StringLiteral kEpoch = "0.5";
constexpr llvm::StringLiteral kResultRoleIdentity = "acsim.result.role";
constexpr uint64_t kMaxExpandedRows = 1U << 20;

InFlightDiagnostic lowerError(Operation *op, llvm::StringRef code,
                              const llvm::Twine &message) {
  return op->emitError() << code << ": " << message;
}

bool isYieldOnlyProcess(ac::ProcessOp process) {
  return process.getCaptures().empty() && process.getBody().hasOneBlock() &&
         llvm::hasSingleElement(process.getBody().front()) &&
         isa<ac::YieldSimOp>(process.getBody().front().front());
}

bool isDatapathArith(Operation *op) {
  return isa<arith::ConstantOp, arith::AddIOp, arith::SubIOp, arith::MulIOp,
             arith::DivUIOp, arith::AndIOp, arith::OrIOp, arith::XOrIOp,
             arith::ShLIOp, arith::ShRUIOp, arith::ShRSIOp, arith::CmpIOp,
             arith::SelectOp, arith::IndexCastOp>(op);
}

enum class DeviceKind { None, Register, RegFile };

DeviceKind deviceKindForQueue(ac::QueueOp queue) {
  if (DictionaryAttr watermarks = queue.getWatermarksAttr()) {
    if (auto kind = watermarks.getAs<StringAttr>("kind")) {
      if (kind.getValue() == "register")
        return DeviceKind::Register;
      if (kind.getValue() == "regfile")
        return DeviceKind::RegFile;
    }
  }
  llvm::StringRef name = queue.getSymName();
  if (name == "rf")
    return DeviceKind::RegFile;
  if (name == "pc" || name == "busy")
    return DeviceKind::Register;
  return DeviceKind::None;
}

struct ResolvedQueue {
  ac::QueueOp op;
  std::string moduleName;
  std::string queueName;
  DeviceKind device = DeviceKind::None;
};

std::optional<ResolvedQueue> resolveQueueRef(Operation *from,
                                             mlir::SymbolRefAttr ref) {
  Operation *target = ac::lookupRuntimeSymbol(from, ref);
  auto queue = dyn_cast_or_null<ac::QueueOp>(target);
  if (!queue)
    return std::nullopt;
  auto ownerMod = queue->getParentOfType<ac::ModuleOp>();
  ResolvedQueue resolved;
  resolved.op = queue;
  resolved.moduleName = ownerMod ? ownerMod.getSymName().str() : std::string();
  resolved.queueName = queue.getSymName().str();
  resolved.device = deviceKindForQueue(queue);
  return resolved;
}

bool integerPayloadCpp(Type type, unsigned &width, std::string &cpp) {
  auto integer = dyn_cast<IntegerType>(type);
  if (!integer || !integer.isSignless())
    return false;
  width = integer.getWidth();
  if (width == 0 || width > 64)
    return false;
  cpp = "gfsim::UInt<" + std::to_string(width) + ">";
  return true;
}

bool isDatapathProcess(ac::ProcessOp process) {
  if (!process.getBody().hasOneBlock())
    return false;
  unsigned yields = 0;
  bool hasBody = false;
  WalkResult walk = process.walk([&](Operation *op) {
    if (op == process.getOperation())
      return WalkResult::advance();
    if (isa<ac::YieldSimOp>(op)) {
      ++yields;
      return WalkResult::advance();
    }
    hasBody = true;
    if (isa<ac::TrySendOp, ac::TryRecvOp, ac::ScheduleOp, ac::WaitUntilOp,
            ac::WaitForOp, ac::AwaitEventOp, ac::StatAddOp, ac::StatOp,
            ac::ProbeOp, ac::RequireOp, ac::EnsureOp, ac::TraceOpenOp,
            ac::TraceNextOp, ac::TraceDecodeOp, ac::TraceEofOp,
            ac::TracePositionOp>(op)) {
      if (isa<ac::WaitUntilOp, ac::WaitForOp, ac::AwaitEventOp>(op) &&
          op->getParentOp() != process.getOperation()) {
        auto ifOp = dyn_cast<scf::IfOp>(op->getParentOp());
        if (!ifOp || ifOp->getParentOp() != process.getOperation() ||
            ifOp.getNumResults() != 0 || &op->getBlock()->back() == op ||
            !isa<scf::YieldOp>(*std::next(Block::iterator(op))) ||
            std::next(Block::iterator(op), 2) != op->getBlock()->end() ||
            &ifOp->getBlock()->back() == ifOp ||
            !isa<ac::YieldSimOp>(*std::next(Block::iterator(ifOp))) ||
            std::next(Block::iterator(ifOp), 2) != ifOp->getBlock()->end())
          return WalkResult::interrupt();
      }
      return WalkResult::advance();
    }
    if (isDatapathArith(op) ||
        isa<scf::IfOp, scf::ForOp, scf::YieldOp, ac::AssertOp>(op))
      return WalkResult::advance();
    return WalkResult::interrupt();
  });
  return !walk.wasInterrupted() && yields == 1 && hasBody;
}

bool isCompletePrefix(llvm::StringRef message) {
  if (message.empty())
    return false;
  unsigned char first = static_cast<unsigned char>(message.front());
  if (!llvm::isAlpha(first) && first != '_')
    return false;
  return llvm::all_of(message, [](char character) {
    unsigned char value = static_cast<unsigned char>(character);
    return llvm::isAlnum(value) || character == '_';
  });
}

std::string completeIdentity(llvm::StringRef message) {
  if (isCompletePrefix(message))
    return ("acir.complete." + message).str();
  return "acir.complete";
}

Value findCompleteReportValue(Value condition) {
  llvm::SmallVector<Value, 8> work = {condition};
  llvm::DenseSet<Value> seen;
  while (!work.empty()) {
    Value current = work.pop_back_val();
    if (!current || !seen.insert(current).second)
      continue;
    if (auto recv = current.getDefiningOp<ac::TryRecvOp>()) {
      if (auto integer = dyn_cast<IntegerType>(recv.getValue().getType());
          integer && integer.isSignless() && integer.getWidth() >= 1)
        return recv.getValue();
    }
    if (Operation *def = current.getDefiningOp())
      for (Value operand : def->getOperands())
        work.push_back(operand);
  }
  return Value();
}

// ---------------------------------------------------------------------------
// Canonical static values: MLIR attributes <-> RFC 8785 JSON
// ---------------------------------------------------------------------------

/// Convert a frozen ACIR static attribute to its canonical JSON value. The
/// accepted domain mirrors the ac-resolve-gfsim-bindings normalizer.
llvm::Expected<llvm::json::Value> staticValueToJson(Attribute attribute) {
  auto unsupported = [&]() {
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "ACLOWER-PARAM-PHASE: unsupported static attribute kind");
  };
  if (auto boolean = dyn_cast<BoolAttr>(attribute))
    return llvm::json::Value(boolean.getValue());
  if (auto integer = dyn_cast<IntegerAttr>(attribute)) {
    const llvm::APInt &value = integer.getValue();
    if (!value.isSignedIntN(64))
      return unsupported();
    return llvm::json::Value(value.getSExtValue());
  }
  if (auto floating = dyn_cast<FloatAttr>(attribute)) {
    double value = floating.getValueAsDouble();
    if (!std::isfinite(value) || (std::signbit(value) && value == 0.0))
      return unsupported();
    return llvm::json::Value(value);
  }
  if (auto string = dyn_cast<StringAttr>(attribute))
    return llvm::json::Value(string.getValue());
  if (auto array = dyn_cast<ArrayAttr>(attribute)) {
    llvm::json::Array values;
    for (Attribute element : array) {
      auto converted = staticValueToJson(element);
      if (!converted)
        return converted.takeError();
      values.push_back(std::move(*converted));
    }
    return llvm::json::Value(std::move(values));
  }
  if (auto dictionary = dyn_cast<DictionaryAttr>(attribute)) {
    llvm::json::Object values;
    for (NamedAttribute named : dictionary) {
      auto converted = staticValueToJson(named.getValue());
      if (!converted)
        return converted.takeError();
      values[named.getName().getValue()] = std::move(*converted);
    }
    return llvm::json::Value(std::move(values));
  }
  if (isa<TypeAttr, SymbolRefAttr>(attribute)) {
    std::string printed;
    llvm::raw_string_ostream output(printed);
    output << attribute;
    return llvm::json::Value(output.str());
  }
  return unsupported();
}

/// Convert a binding-lock JSON static value back to a canonical MLIR
/// attribute. Returns a null attribute for values outside the closed domain.
Attribute jsonToStaticAttribute(OpBuilder &builder,
                                const llvm::json::Value &value) {
  switch (value.kind()) {
  case llvm::json::Value::Boolean:
    return builder.getBoolAttr(*value.getAsBoolean());
  case llvm::json::Value::Number:
    if (auto integer = value.getAsInteger())
      return builder.getI64IntegerAttr(*integer);
    if (auto number = value.getAsNumber())
      return builder.getF64FloatAttr(*number);
    return Attribute();
  case llvm::json::Value::String:
    return builder.getStringAttr(*value.getAsString());
  case llvm::json::Value::Array: {
    llvm::SmallVector<Attribute> elements;
    for (const llvm::json::Value &element : *value.getAsArray()) {
      Attribute converted = jsonToStaticAttribute(builder, element);
      if (!converted)
        return Attribute();
      elements.push_back(converted);
    }
    return builder.getArrayAttr(elements);
  }
  case llvm::json::Value::Object: {
    llvm::SmallVector<NamedAttribute> members;
    for (const auto &member : *value.getAsObject()) {
      Attribute converted = jsonToStaticAttribute(builder, member.second);
      if (!converted)
        return Attribute();
      members.push_back(builder.getNamedAttr(member.first, converted));
    }
    return builder.getDictionaryAttr(members);
  }
  case llvm::json::Value::Null:
    return Attribute();
  }
  return Attribute();
}

/// Fingerprint a canonical JSON descriptor with the shared RFC 8785 + SHA-256
/// recipe used across the binding infrastructure.
std::string fingerprintJson(const llvm::json::Value &value) {
  auto canonical = bindings::canonicalizeJson(value);
  if (!canonical) {
    llvm::consumeError(canonical.takeError());
    return {};
  }
  return bindings::sha256Fingerprint(*canonical);
}

// ---------------------------------------------------------------------------
// acsim.type symbol table
// ---------------------------------------------------------------------------

struct TypeDeclaration {
  std::string identity;
  std::string symbol;
  std::string cpp;
  std::string kind;
  std::string fingerprint;
  std::optional<uint64_t> period;
  uint64_t phase = 0;
  uint64_t tickScale = 1;
  std::optional<std::string> parent;
  std::optional<std::string> bridgeKind;
  std::optional<std::string> bridgeOwner;
};

/// Assigns deterministic canonical symbols and fingerprints to every C++
/// realization identity referenced by binding records or generated process
/// helpers. Identities are interned in sorted order so symbol assignment is
/// independent of discovery order.
class TypeSymbolTable {
public:
  /// Intern one identity. `fingerprint` may be empty, in which case the
  /// fingerprint is the SHA-256 of the identity itself.
  mlir::LogicalResult intern(Operation *reporter, llvm::StringRef identity,
                             llvm::StringRef kind, llvm::StringRef cpp,
                             llvm::StringRef fingerprint = llvm::StringRef()) {
    auto found = entries.find(identity.str());
    if (found != entries.end()) {
      TypeDeclaration &existing = found->second;
      if (existing.kind != kind || existing.cpp != cpp)
        return lowerError(reporter, "ACLOWER-TYPE-MISMATCH",
                          "realization identity '" + identity +
                              "' is used with conflicting acsim.type "
                              "kind or C++ spelling");
      if (!fingerprint.empty() && existing.fingerprint != fingerprint)
        return lowerError(
            reporter, "ACLOWER-FINGERPRINT",
            "realization identity '" + identity +
                "' carries conflicting fingerprints across binding records");
      return mlir::success();
    }
    TypeDeclaration declaration;
    declaration.identity = identity.str();
    declaration.kind = kind.str();
    declaration.cpp = cpp.str();
    declaration.fingerprint = fingerprint.empty()
                                  ? bindings::sha256Fingerprint(identity)
                                  : fingerprint.str();
    entries.emplace(declaration.identity, std::move(declaration));
    return mlir::success();
  }

  mlir::LogicalResult internTimeDomain(ac::TimeDomainOp domain) {
    llvm::json::Object descriptor{
        {"name", domain.getSymName()},
        {"period", static_cast<uint64_t>(domain.getPeriod())},
        {"phase", static_cast<uint64_t>(domain.getPhase())},
        {"tick_scale", static_cast<uint64_t>(domain.getTickScale())}};
    if (auto parent = domain.getParentAttr())
      descriptor["parent"] = parent.getValue();
    else
      descriptor["parent"] = nullptr;
    if (auto bridge = domain.getBridgeAttr()) {
      descriptor["bridge"] = llvm::json::Object{
          {"kind", bridge.getAs<StringAttr>("kind").getValue()},
          {"owner", bridge.getAs<FlatSymbolRefAttr>("owner").getValue()}};
    } else {
      descriptor["bridge"] = nullptr;
    }
    std::string fingerprint =
        fingerprintJson(llvm::json::Value(std::move(descriptor)));
    if (failed(intern(domain, domain.getSymName(), "time_domain",
                      "gfsim::TimeDomainRuntime", fingerprint)))
      return mlir::failure();
    TypeDeclaration &declaration = entries.at(domain.getSymName().str());
    declaration.period = static_cast<uint64_t>(domain.getPeriod());
    declaration.phase = static_cast<uint64_t>(domain.getPhase());
    declaration.tickScale = static_cast<uint64_t>(domain.getTickScale());
    if (auto parent = domain.getParentAttr())
      declaration.parent = parent.getValue().str();
    if (auto bridge = domain.getBridgeAttr()) {
      declaration.bridgeKind =
          bridge.getAs<StringAttr>("kind").getValue().str();
      declaration.bridgeOwner =
          bridge.getAs<FlatSymbolRefAttr>("owner").getValue().str();
    }
    return mlir::success();
  }

  /// Resolve symbols after all identities are interned.
  mlir::LogicalResult finalize(Operation *reporter) {
    llvm::StringMap<std::string> ownerBySymbol;
    for (auto &[identity, declaration] : entries) {
      std::string base = sanitize(declaration.identity);
      std::string symbol = base;
      for (unsigned suffix = 2; ownerBySymbol.count(symbol); ++suffix)
        symbol = base + "_" + std::to_string(suffix);
      ownerBySymbol.try_emplace(symbol, declaration.identity);
      declaration.symbol = symbol;
    }
    ordered.clear();
    for (auto &[identity, declaration] : entries)
      ordered.push_back(&declaration);
    llvm::sort(ordered,
               [](const TypeDeclaration *left, const TypeDeclaration *right) {
                 return left->symbol < right->symbol;
               });
    for (const TypeDeclaration *declaration : ordered)
      if (declaration->symbol.empty())
        return lowerError(reporter, "ACLOWER-FINGERPRINT",
                          "realization identity '" + declaration->identity +
                              "' has no canonical symbol");
    return mlir::success();
  }

  llvm::StringRef symbolFor(llvm::StringRef identity) const {
    auto found = entries.find(identity.str());
    return found == entries.end() ? llvm::StringRef()
                                  : llvm::StringRef(found->second.symbol);
  }

  llvm::ArrayRef<const TypeDeclaration *> declarations() const {
    return ordered;
  }

private:
  static std::string sanitize(llvm::StringRef identity) {
    std::string symbol;
    symbol.reserve(identity.size());
    for (char character : identity)
      symbol.push_back(std::isalnum(static_cast<unsigned char>(character)) ||
                               character == '_'
                           ? character
                           : '_');
    if (symbol.empty() ||
        std::isdigit(static_cast<unsigned char>(symbol.front())))
      symbol.insert(symbol.begin(), '_');
    return symbol;
  }

  std::map<std::string, TypeDeclaration> entries;
  llvm::SmallVector<const TypeDeclaration *> ordered;
};

// ---------------------------------------------------------------------------
// Binding record conversion
// ---------------------------------------------------------------------------

/// Build the exact 20-field acsim.binding record dictionary from a typed
/// binding-lock record, mapping realization identities to canonical symbols.
mlir::Attribute convertBindingRecord(OpBuilder &builder,
                                     const bindings::BindingRecord &record,
                                     const TypeSymbolTable &types) {
  MLIRContext *context = builder.getContext();
  auto string = [&](llvm::StringRef value) {
    return builder.getStringAttr(value);
  };
  auto reference = [&](llvm::StringRef identity) {
    return FlatSymbolRefAttr::get(context, types.symbolFor(identity));
  };
  auto dictionary =
      [&](llvm::ArrayRef<NamedAttribute> members) -> DictionaryAttr {
    return builder.getDictionaryAttr(members);
  };
  auto named = [&](llvm::StringRef key, Attribute value) {
    return builder.getNamedAttr(key, value);
  };

  llvm::SmallVector<Attribute> activationSources;
  for (const bindings::ActivationSourceBinding &source :
       record.activationSources())
    activationSources.push_back(
        dictionary({named("kind", reference(source.kind)),
                    named("name", string(source.name))}));

  llvm::SmallVector<Attribute> constructionArguments;
  for (const llvm::json::Value &argument : record.construction().arguments)
    constructionArguments.push_back(jsonToStaticAttribute(builder, argument));

  const bindings::CppBinding &cpp = record.cpp();
  DictionaryAttr entryPoints =
      dictionary({named("pure", string(cpp.entryPoints.pure)),
                  named("reset", string(cpp.entryPoints.reset)),
                  named("validate", string(cpp.entryPoints.validate)),
                  named("work", string(cpp.entryPoints.work)),
                  named("xfer", string(cpp.entryPoints.xfer))});
  DictionaryAttr cppRecord = dictionary(
      {named("concept", string(cpp.conceptName)),
       named("entry_points", entryPoints), named("header", string(cpp.header)),
       named("symbol", string(cpp.symbol)),
       named("target", string(cpp.target))});

  DictionaryAttr construction = dictionary(
      {named("arguments", builder.getArrayAttr(constructionArguments)),
       named("kind", string(record.construction().kind))});
  DictionaryAttr ownership =
      dictionary({named("kind", string(record.ownership().kind)),
                  named("placement", string(record.ownership().placement))});

  llvm::SmallVector<Attribute> parameters;
  for (const bindings::ParameterBinding &parameter : record.parameters())
    parameters.push_back(dictionary(
        {named("acir_type", string(parameter.acirType)),
         named("cpp_type", string(parameter.cppType)),
         named("mapping", string(parameter.mapping)),
         named("name", string(parameter.name)),
         named("ordinal", builder.getI64IntegerAttr(parameter.ordinal)),
         named("value", jsonToStaticAttribute(builder, parameter.value))}));

  llvm::SmallVector<Attribute> ports;
  for (const bindings::PortBinding &port : record.ports())
    ports.push_back(
        dictionary({named("accessor", reference(port.accessor)),
                    named("cardinality", string(port.cardinality)),
                    named("delegation", string(port.delegation)),
                    named("direction", string(port.direction)),
                    named("interface", reference(port.interface)),
                    named("ownership", string(port.ownership)),
                    named("payload", reference(port.payload)),
                    named("protocol", reference(port.protocol)),
                    named("role", reference(port.role)),
                    named("time_domain", reference(port.timeDomain))}));

  llvm::SmallVector<Attribute> resources;
  for (const bindings::ResourceBinding &resource : record.resources())
    resources.push_back(
        dictionary({named("accessor", reference(resource.accessor)),
                    named("delegation", string(resource.delegation)),
                    named("mode", string(resource.mode)),
                    named("ownership", string(resource.ownership)),
                    named("resource", reference(resource.resource)),
                    named("role", reference(resource.role)),
                    named("time_domain", reference(resource.timeDomain))}));

  llvm::SmallVector<Attribute> results;
  for (const bindings::ResultBinding &result : record.results())
    results.push_back(dictionary({named("cpp_type", reference(result.cppType)),
                                  named("name", string(result.name))}));

  return dictionary(
      {named("activation_sources", builder.getArrayAttr(activationSources)),
       named("availability", string(record.availability())),
       named("binding", string(record.binding())),
       named("binding_schema", string(record.bindingSchema())),
       named("component_schema", reference(record.componentSchema())),
       named("component_schema_fingerprint",
             string(record.componentSchemaFingerprint())),
       named("construction", construction),
       named("contract_epoch", string(record.contractEpoch())),
       named("cpp", cppRecord),
       named("cpp_type", reference(record.cppType())),
       named("effect", string(record.effect())),
       named("fingerprint", string(record.fingerprint())),
       named("implementation", reference(record.implementation())),
       named("ownership", ownership),
       named("parameters", builder.getArrayAttr(parameters)),
       named("ports", builder.getArrayAttr(ports)),
       named("provider", reference(record.provider())),
       named("provider_implementation_fingerprint",
             string(record.providerImplementationFingerprint())),
       named("resources", builder.getArrayAttr(resources)),
       named("results", builder.getArrayAttr(results))});
}

// ---------------------------------------------------------------------------
// Module and placement plans
// ---------------------------------------------------------------------------

struct PortEndpointPlan {
  Value value;
  bindings::PortBinding metadata;
};

struct PlacementPlan {
  enum class Kind { Instance, Array, Process };
  Kind kind = Kind::Instance;
  std::string name;
  // Instance/array realization.
  std::string targetSymbol;
  bool targetIsBinding = false;
  bool targetIsPure = false;
  std::string resultCppType;
  ArrayAttr staticArgs;
  std::string specialization;
  llvm::SmallVector<int64_t, 2> shape;
  // Binding-target dispatch thunks.
  std::string work;
  std::string xfer;
  std::string reset;
  std::string validate;
  llvm::SmallVector<PortEndpointPlan, 2> inputPorts;
  llvm::SmallVector<PortEndpointPlan, 2> outputPorts;
  // Process realization.
  ac::ProcessOp process;
  std::string processDefinitionKey;
  uint64_t fairnessCap = 1;
};

struct BindingEdgePlan {
  unsigned sourcePlacement = 0;
  unsigned targetPlacement = 0;
};

struct PureCallPlan {
  ac::InstanceOp source;
  Value result;
  std::string name;
  std::string binding;
  std::string cppType;
};

struct ModuleResultPlan {
  Value source;
  std::string name;
  std::string cppType;
};

struct ModulePortPlan {
  Value source;
  std::string name;
  bindings::PortBinding metadata;
};

struct ModulePlan {
  ac::ModuleOp source;
  std::string name;
  ArrayAttr staticParams;
  std::string specialization;
  llvm::SmallVector<PlacementPlan, 0> placements;
  llvm::SmallVector<PureCallPlan, 0> pureCalls;
  llvm::SmallVector<ModulePortPlan, 0> ports;
  llvm::SmallVector<ModuleResultPlan, 0> results;
  llvm::SmallVector<BindingEdgePlan, 0> bindingEdges;
};

// ---------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------

class ACIRToACSimPass final
    : public PassWrapper<ACIRToACSimPass, OperationPass<mlir::ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ACIRToACSimPass)

  explicit ACIRToACSimPass(ACIRToACSimPassOptions options)
      : options(std::move(options)) {}

  llvm::StringRef getArgument() const override { return "ac-lower-to-acsim"; }
  llvm::StringRef getDescription() const override {
    return "Atomically lower one frozen ACIR model to canonical ACSim";
  }

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<acsim::ACSimDialect, arith::ArithDialect,
                    cf::ControlFlowDialect, scf::SCFDialect>();
  }

  void runOnOperation() override {
    if (failed(lower(getOperation())))
      signalPassFailure();
  }

private:
  mlir::LogicalResult lower(mlir::ModuleOp input);

  /// Validation and planning. No IR mutation happens in this phase.
  mlir::LogicalResult plan(mlir::ModuleOp input);

  mlir::LogicalResult planModule(ac::ModuleOp module, ModulePlan &planned);
  mlir::LogicalResult planInstanceTarget(Operation *placement,
                                         llvm::StringRef definition,
                                         DictionaryAttr staticArgs,
                                         PlacementPlan &planned);
  mlir::LogicalResult planInstancePorts(ac::InstanceOp instance,
                                        PlacementPlan &planned);
  mlir::LogicalResult planProcesses(mlir::ModuleOp input);
  mlir::LogicalResult expand(mlir::ModuleOp input);

  void expandModule(unsigned moduleIndex, std::string pathPrefix,
                    llvm::SmallSet<unsigned, 8> &active);

  /// Emission. Runs only after every check succeeded.
  mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>> emit(mlir::ModuleOp input);
  void publish(mlir::ModuleOp input, mlir::ModuleOp staged);
  mlir::LogicalResult emitModuleBody(OpBuilder &builder,
                                     const ModulePlan &planned);
  mlir::LogicalResult
  emitProcessBody(OpBuilder &builder, const PlacementPlan &placement,
                  const llvm::DenseMap<Value, Value> &moduleValues);

  std::string moduleFingerprint(ac::ModuleOp module);
  std::string processFingerprint(const ModulePlan &module,
                                 const PlacementPlan &process);
  std::string bindingInstanceFingerprint(const bindings::BindingRecord &record,
                                         ArrayAttr values);

  ACIRToACSimPassOptions options;

  // Planning state.
  ac::SystemOp selectedSystem;
  llvm::StringMap<unsigned> moduleIndexByName; // concrete modules
  llvm::StringMap<ac::ModuleExternOp> externByName;
  llvm::SmallVector<ModulePlan, 0> modules; // sorted by name
  llvm::SmallVector<ac::TimeDomainOp, 0> timeDomains;
  std::optional<bindings::BindingResolutionResult> resolution;
  std::optional<ProcessStatePlanSet> processPlans;
  std::string processPlanBytes;
  TypeSymbolTable typeSymbols;
  std::string wakeTypeSymbol;
  std::string wakeImplSymbol;
  std::string wakeNextTickImplSymbol;
  std::vector<std::string> generatedCalleeIdentities;
  std::vector<std::string> valueTypeIdentities;
  llvm::StringMap<std::string> wakeTypeIdentities;

  struct RuntimeRow {
    unsigned moduleIndex;
    unsigned placementIndex;
    std::string contextPath;
    std::string path;
    llvm::SmallVector<int64_t, 2> indices;
  };
  llvm::SmallVector<std::string> constructionOrder;
  llvm::SmallVector<RuntimeRow> runtimeRows;
  llvm::StringSet<> frozenOwnerPaths;

  // Fingerprints.
  std::string frozenAcirFingerprint;
  std::string bindingLockFingerprint;
  std::string providerFingerprint;
  std::string schemaSetFingerprint;
  std::string profileFingerprint;
  std::string toolchainFingerprint;

  // Set when owner expansion detects an instantiation cycle.
  bool expansionCycle = false;
};

std::string ACIRToACSimPass::moduleFingerprint(ac::ModuleOp module) {
  llvm::json::Object descriptor;
  descriptor["module"] = module.getSymName();
  auto typeSpelling = [](Type type) {
    std::string storage;
    llvm::raw_string_ostream stream(storage);
    stream << type;
    return storage;
  };
  auto staticDictionary = [&](DictionaryAttr dictionary) {
    llvm::json::Object values;
    if (!dictionary)
      return values;
    for (NamedAttribute named : dictionary) {
      auto value = staticValueToJson(named.getValue());
      if (!value) {
        llvm::consumeError(value.takeError());
        continue;
      }
      values[named.getName().getValue()] = std::move(*value);
    }
    return values;
  };
  auto targetFingerprint = [&](llvm::StringRef target) {
    if (auto concrete = moduleIndexByName.find(target);
        concrete != moduleIndexByName.end())
      return modules[concrete->second].specialization;
    if (resolution) {
      std::string key = ("@" + target).str();
      if (const bindings::ResolvedBinding *selection =
              resolution->selectionForResolutionKey(key))
        return selection->record().fingerprint().str();
    }
    return std::string();
  };

  llvm::json::Object interface;
  llvm::json::Array inputs;
  llvm::json::Array results;
  for (Type type : module.getFunctionType().getInputs())
    inputs.push_back(typeSpelling(type));
  for (Type type : module.getFunctionType().getResults())
    results.push_back(typeSpelling(type));
  interface["inputs"] = std::move(inputs);
  interface["results"] = std::move(results);
  descriptor["interface"] = std::move(interface);

  llvm::json::Object parameters;
  for (NamedAttribute named : module.getStaticParams()) {
    auto value = staticValueToJson(named.getValue());
    if (!value) {
      llvm::consumeError(value.takeError());
      continue;
    }
    parameters[named.getName().getValue()] = std::move(*value);
  }
  descriptor["static"] = std::move(parameters);

  std::vector<std::pair<std::string, llvm::json::Object>> definitions;
  auto appendPlacement = [&](llvm::StringRef key, llvm::StringRef kind,
                             llvm::StringRef name, llvm::StringRef target,
                             DictionaryAttr staticArgs) {
    llvm::json::Object entry;
    entry["kind"] = kind;
    entry["name"] = name;
    entry["static"] = staticDictionary(staticArgs);
    entry["target"] = target;
    entry["target_specialization"] = targetFingerprint(target);
    definitions.emplace_back(key.str(), std::move(entry));
  };
  for (Operation &operation : module.getBody().front()) {
    if (auto instance = dyn_cast<ac::InstanceOp>(operation)) {
      appendPlacement(("instance:" + instance.getSymName()).str(), "instance",
                      instance.getSymName(), instance.getDefinition(),
                      instance.getStaticArgs());
      continue;
    }
    if (auto array = dyn_cast<ac::ArrayOp>(operation)) {
      llvm::json::Object entry;
      entry["kind"] = "array";
      entry["name"] = array.getSymName();
      entry["target"] = array.getDefinition();
      entry["target_specialization"] = targetFingerprint(array.getDefinition());
      llvm::json::Array shape;
      for (int64_t extent : array.getShape())
        shape.push_back(extent);
      entry["shape"] = std::move(shape);
      llvm::json::Array staticElements;
      for (Attribute arguments : array.getStaticArgs())
        staticElements.push_back(
            staticDictionary(cast<DictionaryAttr>(arguments)));
      entry["static"] = std::move(staticElements);
      definitions.emplace_back(("array:" + array.getSymName()).str(),
                               std::move(entry));
      continue;
    }
    if (auto collection = dyn_cast<ac::InstancesOp>(operation)) {
      for (auto [index, nameAttribute] :
           llvm::enumerate(collection.getNames())) {
        llvm::StringRef name = cast<StringAttr>(nameAttribute).getValue();
        llvm::StringRef target =
            cast<FlatSymbolRefAttr>(collection.getDefinitions()[index])
                .getValue();
        appendPlacement(
            ("instance:" + name).str(), "instance", name, target,
            cast<DictionaryAttr>(collection.getStaticArgs()[index]));
      }
      continue;
    }
    if (auto process = dyn_cast<ac::ProcessOp>(operation)) {
      llvm::json::Object entry;
      entry["kind"] = "process";
      entry["name"] = process.getSymName();
      entry["process_kind"] = process.getKind();
      llvm::json::Array captureTypes;
      for (Value capture : process.getCaptures())
        captureTypes.push_back(typeSpelling(capture.getType()));
      entry["captures"] = std::move(captureTypes);
      llvm::json::Array skeleton;
      if (auto frozenSkeleton =
              process->getAttrOfType<ArrayAttr>("ac.frozen_process_skeleton"))
        for (Attribute line : frozenSkeleton)
          skeleton.push_back(cast<StringAttr>(line).getValue());
      entry["skeleton"] = std::move(skeleton);
      definitions.emplace_back(("process:" + process.getSymName()).str(),
                               std::move(entry));
      continue;
    }
    if (auto domain = dyn_cast<ac::TimeDomainOp>(operation)) {
      llvm::json::Object entry{
          {"kind", "time_domain"},
          {"name", domain.getSymName()},
          {"period", static_cast<uint64_t>(domain.getPeriod())},
          {"phase", static_cast<uint64_t>(domain.getPhase())},
          {"tick_scale", static_cast<uint64_t>(domain.getTickScale())}};
      if (auto parent = domain.getParentAttr())
        entry["parent"] = parent.getValue();
      if (auto bridge = domain.getBridgeAttr())
        entry["bridge"] = llvm::json::Object{
            {"kind", bridge.getAs<StringAttr>("kind").getValue()},
            {"owner", bridge.getAs<FlatSymbolRefAttr>("owner").getValue()}};
      definitions.emplace_back(("time_domain:" + domain.getSymName()).str(),
                               std::move(entry));
      continue;
    }
    if (auto returnOp = dyn_cast<ac::ReturnOp>(operation)) {
      llvm::json::Object entry;
      entry["kind"] = "return";
      llvm::json::Array operandTypes;
      for (Value operand : returnOp.getOperands())
        operandTypes.push_back(typeSpelling(operand.getType()));
      entry["operands"] = std::move(operandTypes);
      definitions.emplace_back("~return", std::move(entry));
    }
  }
  llvm::sort(definitions, [](const auto &left, const auto &right) {
    return left.first < right.first;
  });
  llvm::json::Array body;
  for (auto &definition : definitions)
    body.push_back(std::move(definition.second));
  descriptor["body"] = std::move(body);
  return fingerprintJson(llvm::json::Value(std::move(descriptor)));
}

std::string ACIRToACSimPass::processFingerprint(const ModulePlan &module,
                                                const PlacementPlan &process) {
  llvm::json::Object descriptor;
  descriptor["module"] = module.name;
  descriptor["module_specialization"] = module.specialization;
  descriptor["process"] = process.name;
  descriptor["process_plan"] = processPlanBytes;
  return fingerprintJson(llvm::json::Value(std::move(descriptor)));
}

std::string ACIRToACSimPass::bindingInstanceFingerprint(
    const bindings::BindingRecord &record, ArrayAttr values) {
  llvm::json::Object descriptor;
  descriptor["binding"] = record.binding();
  descriptor["binding_fingerprint"] = record.fingerprint();
  descriptor["component_schema_fingerprint"] =
      record.componentSchemaFingerprint();
  descriptor["profile"] = options.profile;
  descriptor["provider_implementation_fingerprint"] =
      record.providerImplementationFingerprint();
  llvm::json::Array staticValues;
  for (Attribute value : values) {
    auto converted = staticValueToJson(value);
    if (!converted) {
      llvm::consumeError(converted.takeError());
      continue;
    }
    staticValues.push_back(std::move(*converted));
  }
  descriptor["static"] = std::move(staticValues);
  descriptor["target"] = options.target;
  return fingerprintJson(llvm::json::Value(std::move(descriptor)));
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

mlir::LogicalResult ACIRToACSimPass::planInstanceTarget(
    Operation *placement, llvm::StringRef definition, DictionaryAttr staticArgs,
    PlacementPlan &planned) {
  auto externIt = externByName.find(definition);
  auto moduleIt = moduleIndexByName.find(definition);
  if (externIt == externByName.end() && moduleIt == moduleIndexByName.end())
    return lowerError(placement, "ACLOWER-BINDING-MISSING",
                      "placement definition '@" + definition +
                          "' is not a module or external declaration");

  DictionaryAttr declaredParams;
  if (externIt != externByName.end())
    declaredParams = externIt->second.getStaticParams();
  else
    declaredParams = modules[moduleIt->second].source.getStaticParams();
  // Zero-volume arrays carry no per-element dictionaries; the declared
  // parameters are the single specialization.
  if (staticArgs && staticArgs != declaredParams)
    return lowerError(placement, "ACLOWER-PARAM-PHASE",
                      "placement static arguments must exactly equal the "
                      "frozen static parameters of '@" +
                          definition +
                          "' (per-instance specialization is outside the v0.1 "
                          "lowering stage)");

  if (externIt != externByName.end()) {
    // External declaration: realization comes from the exact binding lock.
    std::string key = ("@" + definition).str();
    const bindings::ResolvedBinding *selection =
        resolution->selectionForResolutionKey(key);
    if (!selection)
      return lowerError(placement, "ACLOWER-BINDING-MISSING",
                        "no exact binding selection exists for external "
                        "declaration '@" +
                            definition + "'");
    const bindings::BindingRecord &record = selection->record();
    if (record.effect() != "stateful" && record.effect() != "pure")
      return lowerError(placement, "ACLOWER-TYPE-MISMATCH",
                        "external declaration '@" + definition +
                            "' resolved to binding '" + record.binding() +
                            "' with unknown effect '" + record.effect() + "'");
    // Registry validation has already proven the effect-specific entry-point
    // set and exact result metadata.
    const bindings::CppEntryPoints &entryPoints = record.cpp().entryPoints;
    planned.targetSymbol = record.binding().str();
    planned.targetIsBinding = true;
    planned.targetIsPure = record.effect() == "pure";
    if (planned.targetIsPure) {
      if (record.results().size() != 1)
        return lowerError(
            placement, "ACLOWER-TYPE-MISMATCH",
            "pure binding '" + record.binding() +
                "' must have exactly one result for acsim.inline");
      planned.resultCppType = record.results().front().cppType;
    }
    OpBuilder builder(placement->getContext());
    llvm::SmallVector<Attribute> values;
    for (const bindings::ParameterBinding &parameter : record.parameters()) {
      Attribute value = jsonToStaticAttribute(builder, parameter.value);
      if (!value)
        return lowerError(placement, "ACLOWER-PARAM-PHASE",
                          "binding '" + record.binding() + "' parameter '" +
                              parameter.name +
                              "' has a value outside the canonical static "
                              "domain");
      values.push_back(value);
    }
    planned.staticArgs = builder.getArrayAttr(values);
    planned.specialization =
        bindingInstanceFingerprint(record, planned.staticArgs);
    if (!planned.targetIsPure) {
      planned.work = entryPoints.work;
      planned.xfer = entryPoints.xfer;
      planned.reset = entryPoints.reset;
      planned.validate = entryPoints.validate;
    }
    return mlir::success();
  }

  // Concrete generated module target.
  ModulePlan &target = modules[moduleIt->second];
  planned.targetSymbol = target.name;
  planned.targetIsBinding = false;
  planned.staticArgs = target.staticParams;
  planned.specialization = target.specialization;
  return mlir::success();
}

mlir::LogicalResult ACIRToACSimPass::planInstancePorts(ac::InstanceOp instance,
                                                       PlacementPlan &planned) {
  if (!planned.targetIsBinding || planned.targetIsPure)
    return mlir::success();
  std::string key = ("@" + instance.getDefinition()).str();
  const bindings::ResolvedBinding *selection =
      resolution->selectionForResolutionKey(key);
  assert(selection && "validated external target must have a binding");
  const bindings::BindingRecord &record = selection->record();
  llvm::SmallVector<bool> used(record.ports().size(), false);

  auto planEndpoint = [&](Value value, llvm::StringRef direction,
                          llvm::SmallVectorImpl<PortEndpointPlan> &endpoints)
      -> mlir::LogicalResult {
    auto endpoint = dyn_cast<ac::EndpointType>(value.getType());
    if (!endpoint) {
      if (direction == "input" || !value.use_empty())
        return lowerError(instance, "ACLOWER-TYPE-MISMATCH",
                          "stateful binding scalar values cannot cross the "
                          "construction graph; use a typed endpoint/resource "
                          "or a pure binding result");
      return mlir::success();
    }
    llvm::StringRef expectedRole = endpoint.getRole().getValue();
    if (direction == "input") {
      ac::InterfaceOp interface;
      for (ac::InterfaceOp candidate :
           instance->getParentOfType<mlir::ModuleOp>()
               .getOps<ac::InterfaceOp>())
        if (candidate.getSymName() == endpoint.getInterface().getValue()) {
          interface = candidate;
          break;
        }
      ac::RoleOp role;
      if (interface)
        for (ac::RoleOp candidate : interface.getOps<ac::RoleOp>())
          if (candidate.getSymName() == endpoint.getRole().getValue()) {
            role = candidate;
            break;
          }
      if (!role)
        return lowerError(instance, "ACLOWER-TYPE-MISMATCH",
                          "endpoint role cannot be resolved for input port "
                          "lowering");
      expectedRole = role.getDual();
    }
    std::optional<unsigned> match;
    for (auto [index, port] : llvm::enumerate(record.ports())) {
      if (!used[index] && port.direction == direction &&
          port.interface == endpoint.getInterface().getValue() &&
          port.role == expectedRole) {
        if (match)
          return lowerError(instance, "ACLOWER-BINDING-AMBIGUOUS",
                            "binding '" + record.binding() +
                                "' has multiple ports matching endpoint " +
                                endpoint.getInterface().getValue() +
                                "::" + expectedRole);
        match = index;
      }
    }
    if (!match)
      return lowerError(instance, "ACLOWER-TYPE-MISMATCH",
                        "binding '" + record.binding() + "' has no exact " +
                            direction + " port for " +
                            endpoint.getInterface().getValue() +
                            "::" + expectedRole);
    used[*match] = true;
    endpoints.push_back({value, record.ports()[*match]});
    return mlir::success();
  };

  for (Value input : instance.getInputs())
    if (failed(planEndpoint(input, "input", planned.inputPorts)))
      return mlir::failure();
  for (Value output : instance.getOutputs())
    if (failed(planEndpoint(output, "output", planned.outputPorts)))
      return mlir::failure();
  if (llvm::any_of(used, [](bool value) { return !value; }))
    return lowerError(instance, "ACLOWER-TYPE-MISMATCH",
                      "binding '" + record.binding() +
                          "' exposes a port that is absent from the external "
                          "module signature");
  return mlir::success();
}

mlir::LogicalResult ACIRToACSimPass::planModule(ac::ModuleOp module,
                                                ModulePlan &planned) {
  FunctionType signature = module.getFunctionType();
  if (signature.getNumInputs() != 0) {
    std::string printed;
    llvm::raw_string_ostream stream(printed);
    stream << signature;
    return lowerError(module, "ACLOWER-TYPE-MISMATCH",
                      "generated ACSim modules do not carry dynamic block "
                      "arguments; module '@" +
                          module.getSymName() + "' has '" + stream.str() + "'");
  }

  OpBuilder builder(module->getContext());
  llvm::SmallVector<Attribute> staticValues;
  for (NamedAttribute named : module.getStaticParams())
    staticValues.push_back(named.getValue());
  planned.staticParams = builder.getArrayAttr(staticValues);
  planned.specialization = moduleFingerprint(module);

  llvm::SmallVector<PlacementPlan, 0> processes;
  ac::ReturnOp moduleReturn;
  for (Operation &operation : module.getBody().front()) {
    if (auto instance = dyn_cast<ac::InstanceOp>(operation)) {
      PlacementPlan placement;
      placement.kind = PlacementPlan::Kind::Instance;
      placement.name = instance.getSymName().str();
      if (failed(planInstanceTarget(instance, instance.getDefinition(),
                                    instance.getStaticArgs(), placement)))
        return mlir::failure();
      if (placement.targetIsPure) {
        if (instance.getNumResults() != 1)
          return lowerError(instance, "ACLOWER-TYPE-MISMATCH",
                            "pure external placement must produce exactly one "
                            "SSA result");
        if (!instance.getInputs().empty())
          return lowerError(instance, "ACLOWER-UNSUPPORTED-CONSTRUCT",
                            "pure external SSA operands require typed graph "
                            "lowering");
        planned.pureCalls.push_back(
            {instance, instance.getResult(0), instance.getSymName().str(),
             placement.targetSymbol, placement.resultCppType});
        continue;
      }
      if (failed(planInstancePorts(instance, placement)))
        return mlir::failure();
      planned.placements.push_back(std::move(placement));
      continue;
    }
    if (auto array = dyn_cast<ac::ArrayOp>(operation)) {
      PlacementPlan placement;
      placement.kind = PlacementPlan::Kind::Array;
      placement.name = array.getSymName().str();
      placement.shape.assign(array.getShape().begin(), array.getShape().end());
      // Homogeneous arrays require one exact specialization per element.
      DictionaryAttr first;
      for (Attribute element : array.getStaticArgs()) {
        auto arguments = dyn_cast<DictionaryAttr>(element);
        if (!arguments)
          return lowerError(array, "ACLOWER-ARRAY",
                            "array static arguments must be concrete "
                            "dictionaries");
        if (!first)
          first = arguments;
        else if (arguments != first)
          return lowerError(array, "ACLOWER-ARRAY",
                            "differently specialized array elements are "
                            "outside the lowering stage; lower them as "
                            "ordered named members instead");
      }
      if (failed(planInstanceTarget(array, array.getDefinition(), first,
                                    placement)))
        return mlir::failure();
      if (placement.targetIsPure)
        return lowerError(array, "ACLOWER-OWNERSHIP",
                          "pure bindings lower to acsim.inline and cannot own "
                          "an ac.array placement");
      planned.placements.push_back(std::move(placement));
      continue;
    }
    if (auto collection = dyn_cast<ac::InstancesOp>(operation)) {
      for (auto [index, definitionAttribute] :
           llvm::enumerate(collection.getDefinitions())) {
        auto definition = cast<FlatSymbolRefAttr>(definitionAttribute);
        auto arguments =
            cast<DictionaryAttr>(collection.getStaticArgs()[index]);
        PlacementPlan placement;
        placement.kind = PlacementPlan::Kind::Instance;
        placement.name =
            cast<StringAttr>(collection.getNames()[index]).getValue().str();
        if (failed(planInstanceTarget(collection, definition.getValue(),
                                      arguments, placement)))
          return mlir::failure();
        if (placement.targetIsPure)
          return lowerError(collection, "ACLOWER-OWNERSHIP",
                            "pure bindings lower to acsim.inline and cannot "
                            "own an ac.instances placement");
        planned.placements.push_back(std::move(placement));
      }
      continue;
    }
    if (auto queue = dyn_cast<ac::QueueOp>(operation)) {
      unsigned width = 0;
      std::string cpp;
      if (!integerPayloadCpp(queue.getPayload(), width, cpp))
        return lowerError(queue, "ACLOWER-UNSUPPORTED-CONSTRUCT",
                          "ac-lower-to-acsim queue datapath requires "
                          "signless integer payloads with widths in [1, 64]");
      if (queue.getOrdering() != "fifo")
        return lowerError(queue, "ACLOWER-UNSUPPORTED-CONSTRUCT",
                          "ac-lower-to-acsim v0.1 queue datapath requires fifo "
                          "ordering");
      if (queue.getDelayTicks() != 1)
        return lowerError(queue, "ACLOWER-UNSUPPORTED-CONSTRUCT",
                          "ac-lower-to-acsim v0.1 only supports delay_ticks=1");
      std::string moduleName = planned.name;
      std::string queueName = queue.getSymName().str();
      DeviceKind device = deviceKindForQueue(queue);
      if (device == DeviceKind::RegFile) {
        if (failed(typeSymbols.intern(queue,
                                      "acir.regfile.read." + moduleName + "." +
                                          queueName,
                                      "implementation", "acir.regfile.read")) ||
            failed(typeSymbols.intern(queue,
                                      "acir.regfile.write." + moduleName + "." +
                                          queueName,
                                      "implementation",
                                      "acir.regfile.write")))
          return mlir::failure();
        continue;
      }
      if (device == DeviceKind::Register) {
        if (failed(typeSymbols.intern(queue,
                                      "acir.register.load." + moduleName + "." +
                                          queueName,
                                      "implementation",
                                      "acir.register.load")) ||
            failed(typeSymbols.intern(queue,
                                      "acir.register.store." + moduleName +
                                          "." + queueName,
                                      "implementation",
                                      "acir.register.store")))
          return mlir::failure();
        continue;
      }
      std::string widthTag = "i" + std::to_string(width);
      std::string identity = "acir.queue." + moduleName + "." + queueName +
                             "." + widthTag;
      if (auto bytes = queue.getByteCapacityAttr())
        identity +=
            ".bytes" + std::to_string(static_cast<int64_t>(bytes.getInt()));
      identity += ".cap" + std::to_string(queue.getEntryCapacity());
      if (failed(typeSymbols.intern(queue, identity, "implementation",
                                    "gfsim::SimQueue")) ||
          failed(typeSymbols.intern(queue,
                                    "acir.queue.push." + moduleName + "." +
                                        queueName,
                                    "implementation", "acir.queue.push")) ||
          failed(typeSymbols.intern(queue,
                                    "acir.queue.pop." + moduleName + "." +
                                        queueName,
                                    "implementation", "acir.queue.pop")))
        return mlir::failure();
      continue;
    }
    if (auto resource = dyn_cast<ac::ResourceOp>(operation)) {
      std::string identity =
          "acir.resource." + planned.name + "." + resource.getSymName().str() +
          ".cap" + std::to_string(resource.getCapacity());
      if (failed(typeSymbols.intern(resource, identity, "implementation",
                                    "gfsim::Resource")) ||
          failed(typeSymbols.intern(resource,
                                    "acir.resource.acquire." + planned.name +
                                        "." + resource.getSymName().str(),
                                    "implementation",
                                    "acir.resource.acquire")) ||
          failed(typeSymbols.intern(resource,
                                    "acir.resource.release." + planned.name +
                                        "." + resource.getSymName().str(),
                                    "implementation",
                                    "acir.resource.release")))
        return mlir::failure();
      continue;
    }
    if (isa<ac::EventQueueOp, ac::InstrumentationOp, ac::StatOp>(operation))
      continue;
    if (auto process = dyn_cast<ac::ProcessOp>(operation)) {
      if (!isYieldOnlyProcess(process) && !isDatapathProcess(process))
        return lowerError(process, "ACLOWER-PROCESS-STATE",
                          "ac-lower-to-acsim v0.1 lowers exactly the "
                          "yield-only process form planned by "
                          "ProcessStatePlan; process '@" +
                              process.getSymName() +
                              "' has an unsupported body");
      PlacementPlan placement;
      placement.kind = PlacementPlan::Kind::Process;
      placement.name = process.getSymName().str();
      placement.process = process;
      processes.push_back(std::move(placement));
      continue;
    }
    if (auto domain = dyn_cast<ac::TimeDomainOp>(operation)) {
      timeDomains.push_back(domain);
      continue;
    }
    if (auto returnOp = dyn_cast<ac::ReturnOp>(operation)) {
      moduleReturn = returnOp;
      continue;
    }
    return lowerError(&operation, "ACLOWER-UNSUPPORTED-CONSTRUCT",
                      "operation '" + operation.getName().getStringRef() +
                          "' has no ACSim realization in the lowering "
                          "stage (queues, resources, address maps, views, "
                          "and instrumentation are rejected, "
                          "never silently dropped)");
  }

  llvm::sort(planned.placements,
             [](const PlacementPlan &left, const PlacementPlan &right) {
               return left.name < right.name;
             });
  for (auto [targetIndex, target] : llvm::enumerate(planned.placements)) {
    for (const PortEndpointPlan &input : target.inputPorts) {
      std::optional<unsigned> sourceIndex;
      for (auto [candidateIndex, candidate] :
           llvm::enumerate(planned.placements))
        if (llvm::any_of(candidate.outputPorts,
                         [&](const PortEndpointPlan &output) {
                           return output.value == input.value;
                         })) {
          sourceIndex = candidateIndex;
          break;
        }
      if (!sourceIndex)
        return lowerError(target.inputPorts.front().value.getDefiningOp(),
                          "ACLOWER-TYPE-MISMATCH",
                          "typed endpoint input has no lowered producer");
      planned.bindingEdges.push_back(
          {*sourceIndex, static_cast<unsigned>(targetIndex)});
    }
  }
  llvm::sort(processes,
             [](const PlacementPlan &left, const PlacementPlan &right) {
               return left.name < right.name;
             });
  llvm::sort(planned.pureCalls,
             [](const PureCallPlan &left, const PureCallPlan &right) {
               return left.name < right.name;
             });
  for (auto &process : processes)
    planned.placements.push_back(std::move(process));
  for (auto [targetIndex, target] : llvm::enumerate(planned.placements)) {
    if (target.kind != PlacementPlan::Kind::Process)
      continue;
    for (Value capture : target.process.getCaptures()) {
      std::optional<unsigned> sourceIndex;
      for (auto [candidateIndex, candidate] :
           llvm::enumerate(planned.placements))
        if (llvm::any_of(candidate.outputPorts,
                         [&](const PortEndpointPlan &output) {
                           return output.value == capture;
                         })) {
          sourceIndex = candidateIndex;
          break;
        }
      if (!sourceIndex)
        return lowerError(target.process, "ACLOWER-TYPE-MISMATCH",
                          "process capture has no lowered typed producer");
      planned.bindingEdges.push_back(
          {*sourceIndex, static_cast<unsigned>(targetIndex)});
    }
  }

  if (!moduleReturn ||
      moduleReturn.getNumOperands() != signature.getNumResults())
    return lowerError(module, "ACLOWER-TYPE-MISMATCH",
                      "module return arity must match the declared result "
                      "interface");
  for (auto [index, operand] : llvm::enumerate(moduleReturn.getOperands())) {
    const PortEndpointPlan *exportedPort = nullptr;
    for (const PlacementPlan &placement : planned.placements)
      for (const PortEndpointPlan &port : placement.outputPorts)
        if (port.value == operand) {
          exportedPort = &port;
          break;
        }
    if (exportedPort) {
      planned.ports.push_back({operand,
                               llvm::formatv("port_{0:08}", index).str(),
                               exportedPort->metadata});
      continue;
    }
    const PureCallPlan *producer = nullptr;
    for (const PureCallPlan &call : planned.pureCalls)
      if (call.result == operand) {
        producer = &call;
        break;
      }
    if (!producer)
      return lowerError(
          moduleReturn, "ACLOWER-UNSUPPORTED-CONSTRUCT",
          "module result " + llvm::Twine(index) +
              " must be a typed endpoint export or be produced by an exact "
              "pure external binding");
    ModuleResultPlan result;
    result.source = operand;
    result.name = llvm::formatv("result_{0:08}", index).str();
    result.cppType = producer->cppType;
    planned.results.push_back(std::move(result));
  }
  return mlir::success();
}

mlir::LogicalResult ACIRToACSimPass::planProcesses(mlir::ModuleOp input) {
  bool hasProcess = false;
  input.walk([&](ac::ProcessOp) { hasProcess = true; });
  if (!hasProcess)
    return mlir::success();

  bool hasDatapath = false;
  input.walk([&](ac::ProcessOp process) {
    if (isDatapathProcess(process))
      hasDatapath = true;
  });

  auto plans =
      hasDatapath
          ? planProcessState(input)
          : detail::PlanSetBuilder::buildYieldOnly(input);
  if (failed(plans))
    return mlir::failure();
  if (failed(verifyProcessStatePlan(*plans)))
    return mlir::failure();
  processPlans = std::move(*plans);
  auto serializedPlans = serializeProcessStatePlan(*processPlans);
  if (!serializedPlans) {
    llvm::consumeError(serializedPlans.takeError());
    return lowerError(input, "ACLOWER-FINGERPRINT",
                      "failed to serialize the canonical process-state plan");
  }
  processPlanBytes = std::move(*serializedPlans);

  // ProcessStatePlan owns the canonical storage and helper identities used by
  // live values. Publish every referenced declaration before the type table is
  // finalized instead of inventing backend-local spellings.
  for (const ProcessValueTypePlan &type : processPlans->valueTypes()) {
    llvm::StringRef identity = type.symbol();
    identity.consume_front("@");
    llvm::StringRef kind = type.kind() == ProcessValueTypeKind::Value
                               ? llvm::StringRef("value")
                               : llvm::StringRef("packet");
    if (failed(typeSymbols.intern(input, identity, kind, type.cpp(),
                                  type.fingerprint())))
      return mlir::failure();
  }
  for (const ProcessGeneratedCalleePlan &callee : processPlans->callees()) {
    llvm::StringRef identity = callee.symbol();
    identity.consume_front("@");
    if (failed(typeSymbols.intern(input, identity, "implementation",
                                  callee.cpp(), callee.fingerprint())))
      return mlir::failure();
  }

  if (hasDatapath) {
    if (failed(typeSymbols.intern(
            input, "acir.impl.wake.next_tick", "implementation",
            "acir::generated::impl_wake_next_tick")) ||
        failed(typeSymbols.intern(input, "acir.complete", "implementation",
                                  "acir.complete")) ||
        failed(typeSymbols.intern(input, "acir.fail", "implementation",
                                  "acir.fail")))
      return mlir::failure();
    LogicalResult completeWalk = mlir::success();
    input.walk([&](ac::AssertOp assertOp) {
      std::string identity = completeIdentity(assertOp.getMessage());
      if (failed(typeSymbols.intern(input, identity, "implementation",
                                    "acir.complete"))) {
        completeWalk = mlir::failure();
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    if (failed(completeWalk))
      return mlir::failure();
    LogicalResult extraWalk = mlir::success();
    input.walk([&](Operation *op) {
      auto module = op->getParentOfType<ac::ModuleOp>();
      std::string moduleName = module ? module.getSymName().str() : std::string();
      if (auto add = dyn_cast<ac::StatAddOp>(op)) {
        std::string identity =
            "acir.stat.add." + moduleName + "." + add.getStat().str();
        if (failed(typeSymbols.intern(op, identity, "implementation",
                                      "acir.stat.add"))) {
          extraWalk = mlir::failure();
          return WalkResult::interrupt();
        }
      }
      if (auto sched = dyn_cast<ac::ScheduleOp>(op)) {
        std::string identity =
            "acir.schedule." + moduleName + "." + sched.getTarget().str();
        if (failed(typeSymbols.intern(op, identity, "implementation",
                                      "acir.schedule"))) {
          extraWalk = mlir::failure();
          return WalkResult::interrupt();
        }
      }
      if (auto probe = dyn_cast<ac::ProbeOp>(op)) {
        std::string identity =
            "acir.probe." + probe.getKind().str() + "." + moduleName + "." +
            probe.getTarget().str();
        if (failed(typeSymbols.intern(op, identity, "implementation",
                                      "acir.probe"))) {
          extraWalk = mlir::failure();
          return WalkResult::interrupt();
        }
      }
      auto internTrace = [&](llvm::StringRef kind, llvm::StringRef source,
                             llvm::StringRef cpp) {
        std::string identity =
            (llvm::Twine("acir.trace.") + kind + "." + source).str();
        return typeSymbols.intern(op, identity, "implementation", cpp);
      };
      LogicalResult traceResult = success();
      if (auto open = dyn_cast<ac::TraceOpenOp>(op))
        traceResult = internTrace("open", open.getSource(), "acir.trace.open");
      else if (auto next = dyn_cast<ac::TraceNextOp>(op))
        traceResult = internTrace("next", next.getSource(), "acir.trace.next");
      else if (auto eof = dyn_cast<ac::TraceEofOp>(op))
        traceResult = internTrace("eof", eof.getSource(), "acir.trace.eof");
      else if (auto position = dyn_cast<ac::TracePositionOp>(op))
        traceResult = internTrace("position", position.getSource(),
                                  "acir.trace.position");
      else if (isa<ac::TraceDecodeOp>(op))
        traceResult =
            typeSymbols.intern(op, "acir.trace.decode", "implementation",
                               "acir.trace.decode");
      if (failed(traceResult)) {
        extraWalk = failure();
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    if (failed(extraWalk))
      return mlir::failure();
  }

  // Adopt the generated next-delta wake helper used by the legacy emitter.
  for (const ProcessGeneratedCalleePlan &callee : processPlans->callees()) {
    if (callee.role() != ProcessHelperRole::WakeNextDelta)
      continue;
    llvm::StringRef symbol = callee.symbol();
    symbol.consume_front("@");
    wakeImplSymbol = symbol.str();
    if (failed(typeSymbols.intern(input, symbol, "implementation", callee.cpp(),
                                  callee.fingerprint())))
      return mlir::failure();
  }
  if (wakeImplSymbol.empty())
    return lowerError(input, "ACLOWER-PROCESS-STATE",
                      "process-state plan has no next-delta wake realization");
  for (const ProcessStatePlan &process : processPlans->processes()) {
    if (process.wakes().empty())
      return lowerError(process.process(), "ACLOWER-PROCESS-STATE",
                        "process-state plan requires a wake realization");
    llvm::StringRef typeKey = process.wakes().front().typeKey();
    typeKey.consume_front("@");
    wakeTypeSymbol = typeKey.str();
  }
  if (failed(typeSymbols.intern(input, wakeTypeSymbol, "wake",
                                "acir::generated::wake_next_delta")))
    return mlir::failure();

  // Attach plan-derived fairness caps to the module placements.
  for (ModulePlan &module : modules)
    for (PlacementPlan &placement : module.placements) {
      if (placement.kind != PlacementPlan::Kind::Process)
        continue;
      std::string key = "@" + module.name + "::@" + placement.name;
      const ProcessStatePlan *plan = processPlans->lookupByDefinitionKey(key);
      if (!plan)
        return lowerError(placement.process, "ACLOWER-PROCESS-STATE",
                          "process-state plan is missing process '@" +
                              placement.name + "'");
      uint64_t fairness = std::max<uint64_t>(plan->fairnessWork(), 2);
      if (isDatapathProcess(placement.process)) {
        uint64_t sourceOps = 0;
        ac::ProcessOp sourceProcess = placement.process;
        sourceProcess.walk([&](Operation *op) {
          if (op != sourceProcess.getOperation())
            ++sourceOps;
        });
        fairness = std::max(fairness, sourceOps * 3 + 16);
      }
      placement.fairnessCap = fairness;
      placement.specialization = processFingerprint(module, placement);
    }
  return mlir::success();
}

mlir::LogicalResult ACIRToACSimPass::plan(mlir::ModuleOp input) {
  auto epoch = input->getAttrOfType<StringAttr>("ac.contract_epoch");
  if (!epoch || epoch.getValue() != kEpoch)
    return lowerError(input, "ACLOWER-EPOCH-MISMATCH",
                      "ac-lower-to-acsim requires ac.contract_epoch exactly "
                      "\"0.5\"");
  auto frozen = input->getAttrOfType<BoolAttr>("ac.topology_frozen");
  auto freezeEpoch = input->getAttrOfType<StringAttr>("ac.freeze_epoch");
  if (!frozen || !frozen.getValue() || !freezeEpoch ||
      freezeEpoch.getValue() != kEpoch)
    return lowerError(input, "ACLOWER-EPOCH-MISMATCH",
                      "ac-lower-to-acsim requires a topology-frozen "
                      "model; run ac-freeze-topology first");
  auto frozenOwners = input->getAttrOfType<ArrayAttr>("ac.frozen_owners");
  if (!frozenOwners)
    return lowerError(input, "ACLOWER-OWNERSHIP",
                      "frozen model is missing its canonical owner manifest");
  for (Attribute owner : frozenOwners) {
    auto record = dyn_cast<DictionaryAttr>(owner);
    auto path = record ? record.getAs<StringAttr>("path") : StringAttr();
    if (!path || !frozenOwnerPaths.insert(path.getValue()).second)
      return lowerError(input, "ACLOWER-OWNERSHIP",
                        "frozen owner manifest has a missing or duplicate "
                        "canonical path");
  }
  if (options.profile.empty() || options.target.empty())
    return lowerError(input, "ACLOWER-PROFILE",
                      "ac-lower-to-acsim requires an exact static build "
                      "profile and toolchain target");

  unsigned selectedCount = 0;
  for (auto system : input.getOps<ac::SystemOp>()) {
    if (!system.getSelected())
      continue;
    ++selectedCount;
    selectedSystem = system;
  }
  if (selectedCount != 1)
    return lowerError(input, "ACLOWER-OWNERSHIP",
                      "ac-lower-to-acsim requires exactly one selected "
                      "ac.system");

  // Inventory concrete modules, externals, and top-level declarations.
  for (Operation &operation : *input.getBody()) {
    if (auto module = dyn_cast<ac::ModuleOp>(operation)) {
      moduleIndexByName[module.getSymName()] = modules.size();
      ModulePlan planned;
      planned.source = module;
      planned.name = module.getSymName().str();
      modules.push_back(std::move(planned));
      continue;
    }
    if (auto external = dyn_cast<ac::ModuleExternOp>(operation)) {
      externByName[external.getSymName()] = external;
      continue;
    }
    if (isa<ac::SystemOp, ac::TypeScopeOp, ac::TypeAliasOp, ac::StructOp,
            ac::EnumOp, ac::PacketOp, ac::TransactionOp,
            ac::InterfaceOp, ac::ProtocolOp>(operation))
      continue; // Pure declarations are fully resolved before lowering.
    return lowerError(&operation, "ACLOWER-UNSUPPORTED-CONSTRUCT",
                      "top-level operation '" +
                          operation.getName().getStringRef() +
                          "' has no ACSim realization in the lowering "
                          "stage");
  }

  llvm::sort(modules, [](const ModulePlan &left, const ModulePlan &right) {
    return left.name < right.name;
  });
  moduleIndexByName.clear();
  for (auto [index, module] : llvm::enumerate(modules))
    moduleIndexByName[module.name] = index;

  llvm::SmallVector<uint32_t> dependencyCount(modules.size());
  llvm::SmallVector<llvm::SmallVector<unsigned, 2>> parentsByChild(
      modules.size());
  for (auto [ownerIndex, module] : llvm::enumerate(modules)) {
    llvm::SmallSet<unsigned, 8> dependencies;
    auto addDependency = [&](llvm::StringRef definition) {
      auto target = moduleIndexByName.find(definition);
      if (target != moduleIndexByName.end())
        dependencies.insert(target->second);
    };
    for (Operation &operation : module.source.getBody().front()) {
      if (auto instance = dyn_cast<ac::InstanceOp>(operation))
        addDependency(instance.getDefinition());
      else if (auto array = dyn_cast<ac::ArrayOp>(operation))
        addDependency(array.getDefinition());
      else if (auto collection = dyn_cast<ac::InstancesOp>(operation))
        for (Attribute definition : collection.getDefinitions())
          addDependency(cast<FlatSymbolRefAttr>(definition).getValue());
    }
    dependencyCount[ownerIndex] = dependencies.size();
    for (unsigned childIndex : dependencies)
      parentsByChild[childIndex].push_back(ownerIndex);
  }
  std::set<std::pair<std::string, unsigned>> readyModules;
  for (auto [index, module] : llvm::enumerate(modules))
    if (dependencyCount[index] == 0)
      readyModules.emplace(module.name, index);
  llvm::SmallVector<ModulePlan, 0> orderedModules;
  orderedModules.reserve(modules.size());
  while (!readyModules.empty()) {
    unsigned childIndex = readyModules.begin()->second;
    readyModules.erase(readyModules.begin());
    orderedModules.push_back(std::move(modules[childIndex]));
    for (unsigned parentIndex : parentsByChild[childIndex])
      if (--dependencyCount[parentIndex] == 0)
        readyModules.emplace(modules[parentIndex].name, parentIndex);
  }
  if (orderedModules.size() != modules.size())
    return lowerError(input, "ACLOWER-OWNERSHIP",
                      "module instantiation cycle cannot produce canonical "
                      "ACSim module order");
  modules = std::move(orderedModules);
  moduleIndexByName.clear();
  for (auto [index, module] : llvm::enumerate(modules))
    moduleIndexByName[module.name] = index;

  // The selected root must be a concrete generated module.
  llvm::StringRef rootName = selectedSystem.getRoot();
  if (!moduleIndexByName.count(rootName))
    return lowerError(selectedSystem, "ACLOWER-OWNERSHIP",
                      "selected system root '@" + rootName +
                          "' must be a concrete ac.module");

  // Resolve exact bindings in memory (shared contract with
  // ac-resolve-gfsim-bindings; no lock file round-trip).
  ResolveBindingsPassOptions resolveOptions;
  resolveOptions.candidates = options.candidates;
  resolveOptions.requests = options.requests;
  resolveOptions.profile = options.profile;
  resolveOptions.target = options.target;
  auto resolved = resolveModuleBindings(input, resolveOptions);
  if (!resolved) {
    input.emitError() << llvm::toString(resolved.takeError());
    return mlir::failure();
  }
  resolution = std::move(*resolved);

  // Seed realization metadata for all modules before planning any owner body,
  // so a symbol-sorted owner may reference a lexically later child module.
  OpBuilder metadataBuilder(input.getContext());
  for (ModulePlan &planned : modules) {
    llvm::SmallVector<Attribute> staticValues;
    for (NamedAttribute named : planned.source.getStaticParams())
      staticValues.push_back(named.getValue());
    planned.staticParams = metadataBuilder.getArrayAttr(staticValues);
    planned.specialization = moduleFingerprint(planned.source);
  }

  // Plan every concrete module body.
  for (auto [index, module] : llvm::enumerate(modules))
    if (failed(planModule(module.source, modules[index])))
      return mlir::failure();

  if (failed(planProcesses(input)))
    return mlir::failure();

  // Intern every binding-record realization identity.
  for (const bindings::ResolvedBinding &selection : resolution->selections()) {
    const bindings::BindingRecord &record = selection.record();
    if (failed(typeSymbols.intern(input, record.componentSchema(), "schema",
                                  record.componentSchema(),
                                  record.componentSchemaFingerprint())) ||
        failed(
            typeSymbols.intern(input, record.implementation(), "implementation",
                               record.implementation(),
                               record.providerImplementationFingerprint())) ||
        failed(typeSymbols.intern(input, record.provider(), "provider",
                                  record.provider())) ||
        failed(typeSymbols.intern(input, record.cppType(), "value",
                                  record.cppType())))
      return mlir::failure();
    for (const bindings::PortBinding &port : record.ports())
      if (failed(typeSymbols.intern(input, port.accessor, "accessor",
                                    port.accessor)) ||
          failed(typeSymbols.intern(input, port.interface, "interface",
                                    port.interface)) ||
          failed(typeSymbols.intern(input, port.payload, "packet",
                                    port.payload)) ||
          failed(typeSymbols.intern(input, port.protocol, "protocol",
                                    port.protocol)) ||
          failed(typeSymbols.intern(input, port.role, "role", port.role)) ||
          failed(typeSymbols.intern(input, port.timeDomain, "time_domain",
                                    port.timeDomain)))
        return mlir::failure();
    for (const bindings::ResourceBinding &resource : record.resources())
      if (failed(typeSymbols.intern(input, resource.accessor, "accessor",
                                    resource.accessor)) ||
          failed(typeSymbols.intern(input, resource.resource, "resource",
                                    resource.resource)) ||
          failed(typeSymbols.intern(input, resource.role, "role",
                                    resource.role)) ||
          failed(typeSymbols.intern(input, resource.timeDomain, "time_domain",
                                    resource.timeDomain)))
        return mlir::failure();
    for (const bindings::ResultBinding &result : record.results())
      if (failed(typeSymbols.intern(input, result.cppType, "value",
                                    result.cppType)))
        return mlir::failure();
    for (const bindings::ActivationSourceBinding &source :
         record.activationSources())
      if (failed(typeSymbols.intern(input, source.kind, "wake", source.kind)))
        return mlir::failure();
  }
  llvm::sort(timeDomains, [](ac::TimeDomainOp left, ac::TimeDomainOp right) {
    return left.getSymName() < right.getSymName();
  });
  for (ac::TimeDomainOp domain : timeDomains)
    if (failed(typeSymbols.internTimeDomain(domain)))
      return mlir::failure();
  if (llvm::any_of(
          modules,
          [](const ModulePlan &module) { return !module.results.empty(); }) &&
      failed(typeSymbols.intern(input, kResultRoleIdentity, "role",
                                "acsim::ResultRole")))
    return mlir::failure();
  if (failed(typeSymbols.finalize(input)))
    return mlir::failure();
  if (llvm::StringRef tick =
          typeSymbols.symbolFor("acir.impl.wake.next_tick");
      !tick.empty())
    wakeNextTickImplSymbol = tick.str();

  // Binding symbols must not collide with type or module symbols.
  for (const bindings::ResolvedBinding &selection : resolution->selections()) {
    llvm::StringRef binding = selection.record().binding();
    if (typeSymbols.symbolFor(binding).data() != nullptr ||
        moduleIndexByName.count(binding))
      return lowerError(input, "ACLOWER-BINDING-AMBIGUOUS",
                        "binding identity '" + binding +
                            "' collides with a type or module symbol");
  }

  // Fingerprints over exact inputs, computed before any mutation.
  std::string frozenText;
  {
    llvm::raw_string_ostream output(frozenText);
    input.print(output);
  }
  frozenAcirFingerprint = bindings::sha256Fingerprint(frozenText);
  bindingLockFingerprint = resolution->lockFingerprint().str();

  llvm::json::Array providers;
  llvm::json::Array schemas;
  {
    std::map<std::string, bool> uniqueProviders;
    std::map<std::string, bool> uniqueSchemas;
    for (const bindings::ResolvedBinding &selection :
         resolution->selections()) {
      uniqueProviders[selection.record().provider().str()] = true;
      uniqueSchemas[selection.record().componentSchema().str()] = true;
    }
    for (auto &[identity, unused] : uniqueProviders)
      providers.push_back(identity);
    for (auto &[identity, unused] : uniqueSchemas)
      schemas.push_back(identity);
  }
  providerFingerprint =
      fingerprintJson(llvm::json::Value(std::move(providers)));
  schemaSetFingerprint = fingerprintJson(llvm::json::Value(std::move(schemas)));
  profileFingerprint = fingerprintJson(llvm::json::Value(options.profile));
  toolchainFingerprint = fingerprintJson(llvm::json::Value(options.target));
  if (providerFingerprint.empty() || schemaSetFingerprint.empty() ||
      profileFingerprint.empty() || toolchainFingerprint.empty())
    return lowerError(input, "ACLOWER-FINGERPRINT",
                      "failed to derive canonical model fingerprints");

  // Deterministic owner/runtime expansion over the planned structure.
  llvm::SmallSet<unsigned, 8> active;
  expandModule(moduleIndexByName.lookup(rootName),
               selectedSystem.getRootName().str(), active);
  if (expansionCycle)
    return lowerError(input, "ACLOWER-OWNERSHIP",
                      "module instantiation cycle cannot produce canonical "
                      "ACSim ownership order");
  const uint64_t maxExpandedRows =
      options.maxExpandedRows != 0 ? options.maxExpandedRows : kMaxExpandedRows;
  if (constructionOrder.size() > maxExpandedRows ||
      runtimeRows.size() > maxExpandedRows)
    return lowerError(input, "ACLOWER-DISPATCH",
                      "expanded hierarchy exceeds the capability bound");
  if (llvm::any_of(constructionOrder,
                   [&](const std::string &path) {
                     return !frozenOwnerPaths.contains(path);
                   }) ||
      !frozenOwnerPaths.contains(selectedSystem.getRootName()))
    return lowerError(input, "ACLOWER-OWNERSHIP",
                      "planned ACSim hierarchy paths do not exactly match "
                      "the frozen owner manifest");
  return mlir::success();
}

void ACIRToACSimPass::expandModule(unsigned moduleIndex, std::string pathPrefix,
                                   llvm::SmallSet<unsigned, 8> &active) {
  // Callers alias constructionOrder elements, and nested expansion appends to
  // that vector, so the incoming reference must be copied before recursing.
  const std::string prefix = pathPrefix;
  ModulePlan &module = modules[moduleIndex];
  active.insert(moduleIndex);
  for (size_t placementIndex = 0; placementIndex < module.placements.size();
       ++placementIndex) {
    const PlacementPlan &placement = module.placements[placementIndex];
    auto elementPath = [&](llvm::ArrayRef<int64_t> indices) {
      std::string path = prefix;
      path.push_back('.');
      path.append(placement.name);
      llvm::raw_string_ostream stream(path);
      for (int64_t index : indices)
        stream << '[' << index << ']';
      return path;
    };
    auto expandOne = [&](llvm::ArrayRef<int64_t> indices) {
      std::string path = elementPath(indices);
      constructionOrder.push_back(path);
      if (placement.kind == PlacementPlan::Kind::Process ||
          placement.targetIsBinding) {
        RuntimeRow row;
        row.moduleIndex = moduleIndex;
        row.placementIndex = static_cast<unsigned>(placementIndex);
        row.contextPath = pathPrefix;
        row.path = path;
        row.indices.assign(indices.begin(), indices.end());
        runtimeRows.push_back(std::move(row));
        return;
      }
      unsigned targetIndex = moduleIndexByName.lookup(placement.targetSymbol);
      if (active.contains(targetIndex)) {
        // An instantiation cycle can never produce canonical ACSim.
        expansionCycle = true;
        constructionOrder.pop_back();
        return;
      }
      expandModule(targetIndex, std::move(path), active);
    };

    if (placement.kind == PlacementPlan::Kind::Array) {
      uint64_t volume = 1;
      for (int64_t extent : placement.shape) {
        if (extent == 0) {
          volume = 0;
          break;
        }
        volume *= static_cast<uint64_t>(extent);
      }
      for (uint64_t ordinal = 0; ordinal < volume; ++ordinal) {
        llvm::SmallVector<int64_t, 2> indices(placement.shape.size(), 0);
        uint64_t remainder = ordinal;
        for (size_t dimension = placement.shape.size(); dimension > 0;
             --dimension) {
          uint64_t extent =
              static_cast<uint64_t>(placement.shape[dimension - 1]);
          indices[dimension - 1] = static_cast<int64_t>(remainder % extent);
          remainder /= extent;
        }
        expandOne(indices);
      }
      continue;
    }
    expandOne({});
  }
  active.erase(moduleIndex);
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

void appendOccurrenceKey(llvm::raw_ostream &stream,
                         const ProcessOccurrenceId &occurrence) {
  stream << static_cast<unsigned>(occurrence.kind()) << ':';
  switch (occurrence.kind()) {
  case ProcessOccurrenceKind::Original: {
    const ProcessOriginalOccurrence &original = occurrence.original();
    stream << original.operationPath() << '[';
    for (const ProcessCallSitePlan &callSite : original.callSites()) {
      stream << callSite.operationPath() << '(';
      llvm::interleaveComma(callSite.iterationVector(), stream);
      stream << ")";
    }
    stream << "](";
    llvm::interleaveComma(original.iterationVector(), stream);
    stream << ')';
    break;
  }
  case ProcessOccurrenceKind::SyntheticLoop:
    appendOccurrenceKey(stream, occurrence.syntheticLoop().anchor());
    stream << ":loop:"
           << static_cast<unsigned>(occurrence.syntheticLoop().phase());
    break;
  case ProcessOccurrenceKind::SyntheticWrapper:
    appendOccurrenceKey(stream, occurrence.syntheticWrapper().anchor());
    stream << ":wrapper:" << occurrence.syntheticWrapper().transition().value()
           << ':' << occurrence.syntheticWrapper().slot().value() << ':'
           << static_cast<unsigned>(occurrence.syntheticWrapper().direction());
    break;
  case ProcessOccurrenceKind::SyntheticConstant:
    appendOccurrenceKey(stream, occurrence.syntheticConstant().anchor());
    stream << ":constant:" << occurrence.syntheticConstant().constant();
    break;
  }
}

std::string plannedValueKey(const ProcessPlannedValue &value) {
  std::string key;
  llvm::raw_string_ostream stream(key);
  stream << static_cast<unsigned>(value.kind()) << ':';
  switch (value.kind()) {
  case ProcessPlannedValueKind::Original:
    appendOccurrenceKey(stream, value.original().occurrence());
    stream << ':' << value.original().coordinate().ownerPath() << ':'
           << value.original().coordinate().index();
    break;
  case ProcessPlannedValueKind::Capture:
    stream << value.capture().capture().value();
    break;
  case ProcessPlannedValueKind::LiveSlot:
    stream << value.liveSlot().slot().value();
    break;
  case ProcessPlannedValueKind::Synthetic:
    appendOccurrenceKey(stream, value.synthetic().occurrence());
    stream << ':' << value.synthetic().coordinate().ownerPath() << ':'
           << value.synthetic().coordinate().index();
    break;
  case ProcessPlannedValueKind::Constant:
    stream << value.constant().value();
    break;
  }
  return key;
}

mlir::LogicalResult ACIRToACSimPass::emitProcessBody(
    OpBuilder &builder, const PlacementPlan &placement,
    const llvm::DenseMap<Value, Value> &moduleValues) {
  MLIRContext *context = builder.getContext();
  Location loc = placement.process->getLoc();
  ac::ProcessOp sourceProcess = placement.process;
  auto owner = sourceProcess->getParentOfType<ac::ModuleOp>();
  std::string moduleName = owner ? owner.getSymName().str() : std::string();
  std::string definitionKey = "@" + moduleName + "::@" + placement.name;
  assert(processPlans && "validated process plan must be available");
  const ProcessStatePlan *processPlan =
      processPlans->lookupByDefinitionKey(definitionKey);
  assert(processPlan && "validated process must have a canonical plan");

  llvm::SmallVector<Operation *> waits;
  llvm::SmallVector<Operation *> allWaits;
  for (Operation &op : sourceProcess.getBody().front()) {
    if (isa<ac::WaitUntilOp, ac::WaitForOp, ac::AwaitEventOp>(op) &&
        op.getParentOp() == sourceProcess.getOperation())
      waits.push_back(&op);
  }
  sourceProcess.walk([&](Operation *op) {
    if (isa<ac::WaitUntilOp, ac::WaitForOp, ac::AwaitEventOp>(op))
      allWaits.push_back(op);
  });

  llvm::SmallVector<Attribute> pcAttrs;
  for (const ProcessPcPlan &pc : processPlan->pcs())
    pcAttrs.push_back(FlatSymbolRefAttr::get(context, pc.name()));
  assert(!pcAttrs.empty() && "validated process plan must have an entry PC");
  auto entry = cast<FlatSymbolRefAttr>(pcAttrs.front());

  llvm::DenseMap<Operation *, FlatSymbolRefAttr> resumePcByWait;
  for (auto [index, wait] : llvm::enumerate(allWaits)) {
    assert(index + 1 < pcAttrs.size() &&
           "each planned suspension must have a resume PC");
    resumePcByWait[wait] = cast<FlatSymbolRefAttr>(pcAttrs[index + 1]);
  }

  struct LiveSlotEmission {
    std::string name;
    Type storageType;
    Value source;
    FlatSymbolRefAttr wrap;
    FlatSymbolRefAttr unwrap;
  };
  llvm::SmallVector<LiveSlotEmission> liveSlots;
  llvm::SmallVector<Attribute> liveAttrs;
  for (const ProcessLiveSlotPlan &slot : processPlan->liveSlots()) {
    const ProcessValueTypePlan &storage =
        processPlans->valueTypes()[slot.storageType().value()];
    llvm::StringRef storageIdentity = storage.symbol();
    storageIdentity.consume_front("@");
    auto storageType = acsim::ValueType::get(
        context, FlatSymbolRefAttr::get(
                     context, typeSymbols.symbolFor(storageIdentity)));
    Value source;
    for (const ProcessPlannedValue &member : slot.memberValues())
      if (member.kind() == ProcessPlannedValueKind::Original) {
        source = member.original().value();
        break;
      }
    assert(source && "live slot must retain an original source value");
    assert(slot.wrapCallee() && slot.unwrapCallee() &&
           "scalar live slot must have wrap and unwrap helpers");
    auto helperReference = [&](ProcessCalleeId id) {
      llvm::StringRef identity = processPlans->callees()[id.value()].symbol();
      identity.consume_front("@");
      return FlatSymbolRefAttr::get(context,
                                    typeSymbols.symbolFor(identity));
    };
    liveSlots.push_back({slot.name().str(), storageType, source,
                         helperReference(*slot.wrapCallee()),
                         helperReference(*slot.unwrapCallee())});
    liveAttrs.push_back(builder.getDictionaryAttr(
        {builder.getNamedAttr("name", builder.getStringAttr(slot.name())),
         builder.getNamedAttr("type", TypeAttr::get(storageType))}));
  }

  llvm::SmallVector<Value> captureValues;
  llvm::SmallVector<Attribute> captureNames;
  for (auto [index, capture] :
       llvm::enumerate(sourceProcess.getCaptures())) {
    Value mapped = moduleValues.lookup(capture);
    assert(mapped && "validated process capture must be emitted");
    captureValues.push_back(mapped);
    captureNames.push_back(builder.getStringAttr(
        llvm::formatv("capture{0:08}", index).str()));
  }

  auto process = acsim::ProcessOp::create(
      builder, loc, captureValues, placement.name,
      builder.getArrayAttr(captureNames),
      entry.getValue(), builder.getArrayAttr(pcAttrs),
      builder.getArrayAttr(liveAttrs),
      placement.fairnessCap, placement.specialization,
      /*statesCount=*/static_cast<unsigned>(pcAttrs.size()));

  for (Region &region : process.getStates()) {
    Block *block = new Block();
    region.push_back(block);
    for (Value capture : captureValues)
      block->addArgument(capture.getType(), loc);
  }
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(&process.getStates().front().front());
  auto wakeType = acsim::WakeType::get(
      context, FlatSymbolRefAttr::get(context, wakeTypeSymbol));
  llvm::StringRef nextTickImpl = wakeNextTickImplSymbol.empty()
                                     ? llvm::StringRef(wakeImplSymbol)
                                     : llvm::StringRef(wakeNextTickImplSymbol);
  auto emitSuspend = [&](llvm::StringRef implSymbol,
                         FlatSymbolRefAttr target = {}) {
    if (!target)
      target = entry;
    auto wake = acsim::InvokeOp::create(
        builder, loc, TypeRange{wakeType}, ValueRange{},
        FlatSymbolRefAttr::get(context, implSymbol));
    acsim::SuspendOp::create(builder, loc, wake.getResults().front(), target);
  };

  if (isYieldOnlyProcess(placement.process)) {
    emitSuspend(wakeImplSymbol);
    return mlir::success();
  }

  Region *state = &process.getStates().front();
  llvm::DenseMap<Value, Value> values;
  auto emitLiveLoads = [&]() {
    for (const LiveSlotEmission &slot : liveSlots) {
      auto load = acsim::LiveLoadOp::create(
          builder, loc, slot.storageType, placement.name, slot.name);
      auto unwrap = acsim::InlineOp::create(
          builder, loc, slot.source.getType(), ValueRange{load.getResult()},
          slot.unwrap);
      values[slot.source] = unwrap.getResult();
    }
  };
  auto emitLiveStores = [&]() -> LogicalResult {
    for (const LiveSlotEmission &slot : liveSlots) {
      Value scalar = values.lookup(slot.source);
      if (!scalar || scalar.getParentRegion() != state) {
        Operation *reporter = slot.source.getDefiningOp();
        if (!reporter)
          reporter = sourceProcess;
        return lowerError(
            reporter, "ACLOWER-PROCESS-STATE",
            "planned live slot '" + llvm::Twine(slot.name) +
                "' has no proven value in the current suspension state; "
                "lowering refuses to synthesize or drop process state");
      }
      auto wrap = acsim::InlineOp::create(builder, loc, slot.storageType,
                                          ValueRange{scalar}, slot.wrap);
      acsim::LiveStoreOp::create(
          builder, loc, wrap.getResult(), placement.name, slot.name);
    }
    return mlir::success();
  };
  LogicalResult emissionStatus = success();
  Operation *mappingConsumer = nullptr;
  std::function<Block *(Block *, Block::iterator, Block::iterator)> emitOps;
  emitOps = [&](Block *current, Block::iterator begin, Block::iterator end) {
    builder.setInsertionPointToEnd(current);
    auto mapValue = [&](Value source) -> Value {
      Value mapped = values.lookup(source);
      if (mapped && mapped.getParentRegion() == current->getParent())
        return mapped;
      if (auto constant = source.getDefiningOp<arith::ConstantOp>()) {
        auto copy =
            arith::ConstantOp::create(builder, loc, constant.getValue());
        values[source] = copy.getResult();
        return copy.getResult();
      }
      if (succeeded(emissionStatus)) {
        Operation *reporter = mappingConsumer;
        if (!reporter)
          reporter = source.getDefiningOp();
        if (!reporter)
          reporter = sourceProcess;
        lowerError(reporter, "ACLOWER-PROCESS-STATE",
                   "SSA operand has no proven value in the current process "
                   "state; lowering refuses backend zero substitution");
        emissionStatus = failure();
      }
      return Value();
    };
    for (auto it = begin; it != end; ++it) {
      Operation &op = *it;
      mappingConsumer = &op;
      for (Value operand : op.getOperands())
        if (!mapValue(operand))
          return current;
      auto copyBin = [&](auto bin) {
        auto copy = std::remove_cvref_t<decltype(bin)>::create(
            builder, loc, mapValue(bin.getLhs()), mapValue(bin.getRhs()));
        values[bin.getResult()] = copy.getResult();
      };
      if (auto constant = dyn_cast<arith::ConstantOp>(op)) {
        auto copy = arith::ConstantOp::create(builder, loc, constant.getValue());
        values[constant.getResult()] = copy.getResult();
        continue;
      }
      if (auto mul = dyn_cast<arith::MulIOp>(op)) {
        copyBin(mul);
        continue;
      }
      if (auto add = dyn_cast<arith::AddIOp>(op)) {
        copyBin(add);
        continue;
      }
      if (auto sub = dyn_cast<arith::SubIOp>(op)) {
        copyBin(sub);
        continue;
      }
      if (auto div = dyn_cast<arith::DivUIOp>(op)) {
        copyBin(div);
        continue;
      }
      if (auto band = dyn_cast<arith::AndIOp>(op)) {
        copyBin(band);
        continue;
      }
      if (auto bor = dyn_cast<arith::OrIOp>(op)) {
        copyBin(bor);
        continue;
      }
      if (auto bxor = dyn_cast<arith::XOrIOp>(op)) {
        copyBin(bxor);
        continue;
      }
      if (auto shl = dyn_cast<arith::ShLIOp>(op)) {
        copyBin(shl);
        continue;
      }
      if (auto shr = dyn_cast<arith::ShRUIOp>(op)) {
        copyBin(shr);
        continue;
      }
      if (auto sra = dyn_cast<arith::ShRSIOp>(op)) {
        copyBin(sra);
        continue;
      }
      if (auto cmp = dyn_cast<arith::CmpIOp>(op)) {
        auto copy = arith::CmpIOp::create(
            builder, loc, cmp.getPredicate(), mapValue(cmp.getLhs()),
            mapValue(cmp.getRhs()));
        values[cmp.getResult()] = copy.getResult();
        continue;
      }
      if (auto select = dyn_cast<arith::SelectOp>(op)) {
        auto copy = arith::SelectOp::create(
            builder, loc, mapValue(select.getCondition()),
            mapValue(select.getTrueValue()),
            mapValue(select.getFalseValue()));
        values[select.getResult()] = copy.getResult();
        continue;
      }
      if (auto cast = dyn_cast<arith::IndexCastOp>(op)) {
        auto copy = arith::IndexCastOp::create(
            builder, loc, cast.getType(), mapValue(cast.getIn()));
        values[cast.getResult()] = copy.getResult();
        continue;
      }
      auto traceCallee = [&](llvm::StringRef kind, llvm::StringRef source) {
        std::string identity =
            (llvm::Twine("acir.trace.") + kind + "." + source).str();
        return FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity));
      };
      if (auto open = dyn_cast<ac::TraceOpenOp>(op)) {
        auto invoke = acsim::InvokeOp::create(
            builder, loc, TypeRange{builder.getI64Type()}, ValueRange{},
            traceCallee("open", open.getSource()));
        auto cast = arith::IndexCastOp::create(
            builder, loc, open.getCursor().getType(), invoke.getResult(0));
        values[open.getCursor()] = cast.getResult();
        continue;
      }
      if (auto next = dyn_cast<ac::TraceNextOp>(op)) {
        auto input = arith::IndexCastOp::create(
            builder, loc, builder.getI64Type(),
            mapValue(next.getInputCursor()));
        auto invoke = acsim::InvokeOp::create(
            builder, loc,
            TypeRange{builder.getI64Type(), next.getEntry().getType(),
                      next.getAdvanced().getType()},
            ValueRange{input.getResult()},
            traceCallee("next", next.getSource()));
        auto cursor = arith::IndexCastOp::create(
            builder, loc, next.getCursor().getType(), invoke.getResult(0));
        values[next.getCursor()] = cursor.getResult();
        values[next.getEntry()] = invoke.getResult(1);
        values[next.getAdvanced()] = invoke.getResult(2);
        continue;
      }
      if (auto decode = dyn_cast<ac::TraceDecodeOp>(op)) {
        auto invoke = acsim::InvokeOp::create(
            builder, loc, TypeRange{decode.getResult().getType()},
            ValueRange{mapValue(decode.getEntry())},
            FlatSymbolRefAttr::get(
                context, typeSymbols.symbolFor("acir.trace.decode")));
        values[decode.getResult()] = invoke.getResult(0);
        continue;
      }
      if (auto eof = dyn_cast<ac::TraceEofOp>(op)) {
        auto input = arith::IndexCastOp::create(
            builder, loc, builder.getI64Type(),
            mapValue(eof.getInputCursor()));
        auto invoke = acsim::InvokeOp::create(
            builder, loc, TypeRange{eof.getEof().getType()},
            ValueRange{input.getResult()},
            traceCallee("eof", eof.getSource()));
        values[eof.getEof()] = invoke.getResult(0);
        continue;
      }
      if (auto position = dyn_cast<ac::TracePositionOp>(op)) {
        auto input = arith::IndexCastOp::create(
            builder, loc, builder.getI64Type(),
            mapValue(position.getInputCursor()));
        auto invoke = acsim::InvokeOp::create(
            builder, loc, TypeRange{builder.getI64Type()},
            ValueRange{input.getResult()},
            traceCallee("position", position.getSource()));
        auto cast = arith::IndexCastOp::create(
            builder, loc, position.getPosition().getType(),
            invoke.getResult(0));
        values[position.getPosition()] = cast.getResult();
        continue;
      }
      if (auto send = dyn_cast<ac::TrySendOp>(op)) {
        auto resolved = resolveQueueRef(&op, send.getQueue());
        std::string declaring =
            resolved ? resolved->moduleName : moduleName;
        llvm::StringRef queue =
            resolved ? llvm::StringRef(resolved->queueName)
                     : ac::runtimeSymbolLeaf(send.getQueue());
        DeviceKind device =
            resolved ? resolved->device : DeviceKind::None;
        if (device == DeviceKind::Register) {
          std::string identity =
              (llvm::Twine("acir.register.store.") + declaring + "." + queue)
                  .str();
          auto invoke = acsim::InvokeOp::create(
              builder, loc, TypeRange{builder.getI1Type()},
              ValueRange{mapValue(send.getValue())},
              FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
          values[send.getAccepted()] = invoke.getResult(0);
          continue;
        }
        if (device == DeviceKind::RegFile) {
          auto next = std::next(it);
          if (next != end) {
            if (auto recv = dyn_cast<ac::TryRecvOp>(*next);
                recv && recv.getQueue() == send.getQueue()) {
              std::string identity =
                  (llvm::Twine("acir.regfile.read.") + declaring + "." + queue)
                      .str();
              auto invoke = acsim::InvokeOp::create(
                  builder, loc,
                  TypeRange{recv.getValue().getType(), builder.getI1Type()},
                  ValueRange{mapValue(send.getValue())},
                  FlatSymbolRefAttr::get(context,
                                         typeSymbols.symbolFor(identity)));
              values[send.getAccepted()] = invoke.getResult(1);
              values[recv.getValue()] = invoke.getResult(0);
              values[recv.getReceived()] = invoke.getResult(1);
              ++it;
              continue;
            }
            if (auto send2 = dyn_cast<ac::TrySendOp>(*next);
                send2 && send2.getQueue() == send.getQueue()) {
              std::string identity =
                  (llvm::Twine("acir.regfile.write.") + declaring + "." +
                   queue)
                      .str();
              auto invoke = acsim::InvokeOp::create(
                  builder, loc, TypeRange{builder.getI1Type()},
                  ValueRange{mapValue(send.getValue()),
                             mapValue(send2.getValue())},
                  FlatSymbolRefAttr::get(context,
                                         typeSymbols.symbolFor(identity)));
              values[send.getAccepted()] = invoke.getResult(0);
              values[send2.getAccepted()] = invoke.getResult(0);
              ++it;
              continue;
            }
          }
        }
        std::string identity =
            (llvm::Twine("acir.queue.push.") + declaring + "." + queue)
                .str();
        auto invoke = acsim::InvokeOp::create(
            builder, loc, TypeRange{builder.getI1Type()},
            ValueRange{mapValue(send.getValue())},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
        values[send.getAccepted()] = invoke.getResult(0);
        continue;
      }
      if (auto recv = dyn_cast<ac::TryRecvOp>(op)) {
        auto resolved = resolveQueueRef(&op, recv.getQueue());
        std::string declaring =
            resolved ? resolved->moduleName : moduleName;
        llvm::StringRef queue =
            resolved ? llvm::StringRef(resolved->queueName)
                     : ac::runtimeSymbolLeaf(recv.getQueue());
        DeviceKind device =
            resolved ? resolved->device : DeviceKind::None;
        if (device == DeviceKind::Register) {
          std::string identity =
              (llvm::Twine("acir.register.load.") + declaring + "." + queue)
                  .str();
          auto invoke = acsim::InvokeOp::create(
              builder, loc,
              TypeRange{recv.getValue().getType(), builder.getI1Type()},
              ValueRange{},
              FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
          values[recv.getValue()] = invoke.getResult(0);
          values[recv.getReceived()] = invoke.getResult(1);
          continue;
        }
        std::string identity =
            (llvm::Twine("acir.queue.pop.") + declaring + "." + queue)
                .str();
        auto invoke = acsim::InvokeOp::create(
            builder, loc,
            TypeRange{recv.getValue().getType(), builder.getI1Type()},
            ValueRange{},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
        values[recv.getValue()] = invoke.getResult(0);
        values[recv.getReceived()] = invoke.getResult(1);
        continue;
      }
      if (auto assertOp = dyn_cast<ac::AssertOp>(op)) {
        if (auto ifOp = assertOp->getParentOfType<scf::IfOp>()) {
          if (Value report = findCompleteReportValue(ifOp.getCondition())) {
            std::string identity = completeIdentity(assertOp.getMessage());
            acsim::InvokeOp::create(
                builder, loc, TypeRange{},
                ValueRange{mapValue(report)},
                FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
            continue;
          }
        }
        acsim::InvokeOp::create(
            builder, loc, TypeRange{},
            ValueRange{mapValue(assertOp.getCondition())},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor("acir.fail")));
        continue;
      }
      if (auto require = dyn_cast<ac::RequireOp>(op)) {
        acsim::InvokeOp::create(
            builder, loc, TypeRange{},
            ValueRange{mapValue(require.getCondition())},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor("acir.fail")));
        continue;
      }
      if (auto ensure = dyn_cast<ac::EnsureOp>(op)) {
        acsim::InvokeOp::create(
            builder, loc, TypeRange{},
            ValueRange{mapValue(ensure.getCondition())},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor("acir.fail")));
        continue;
      }
      if (auto add = dyn_cast<ac::StatAddOp>(op)) {
        std::string identity =
            "acir.stat.add." + moduleName + "." + add.getStat().str();
        acsim::InvokeOp::create(
            builder, loc, TypeRange{},
            ValueRange{mapValue(add.getValue())},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
        continue;
      }
      if (auto sched = dyn_cast<ac::ScheduleOp>(op)) {
        std::string identity =
            "acir.schedule." + moduleName + "." + sched.getTarget().str();
        acsim::InvokeOp::create(
            builder, loc, TypeRange{},
            ValueRange{mapValue(sched.getValue()),
                       mapValue(sched.getDelay())},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
        continue;
      }
      if (auto probe = dyn_cast<ac::ProbeOp>(op)) {
        std::string identity =
            "acir.probe." + probe.getKind().str() + "." + moduleName + "." +
            probe.getTarget().str();
        auto invoke = acsim::InvokeOp::create(
            builder, loc, TypeRange{probe.getValue().getType()}, ValueRange{},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
        values[probe.getValue()] = invoke.getResult(0);
        continue;
      }
      if (auto wait = dyn_cast<ac::WaitForOp>(op)) {
        std::string identity =
            "acir.resource.acquire." + moduleName + "." +
            wait.getResource().str();
        auto invoke = acsim::InvokeOp::create(
            builder, loc, TypeRange{builder.getI1Type()}, ValueRange{},
            FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
        (void)invoke;
        continue;
      }
      if (isa<ac::WaitUntilOp, ac::WaitForOp, ac::AwaitEventOp>(op)) {
        // Top-level waits are split by the outer slice loop. A nested wait is
        // a real suspension edge inside its current acyclic control-flow path.
        if (op.getParentOp() != sourceProcess.getOperation()) {
          if (failed(emitLiveStores())) {
            emissionStatus = failure();
            return current;
          }
          emitSuspend(nextTickImpl, resumePcByWait.lookup(&op));
          return current;
        }
        continue;
      }
      if (auto forOp = dyn_cast<scf::ForOp>(op)) {
        auto trip = ac::analyzeStaticFor(forOp);
        if (failed(trip) || !forOp.getInitArgs().empty())
          continue;
        Block &body = *forOp.getBody();
        Type ivType = forOp.getInductionVar().getType();
        for (uint64_t iteration = 0; iteration < trip->tripCount; ++iteration) {
          int64_t iv =
              trip->lowerBound +
              static_cast<int64_t>(iteration) * trip->step;
          auto constant = arith::ConstantOp::create(
              builder, loc, builder.getIntegerAttr(ivType, iv));
          values[forOp.getInductionVar()] = constant.getResult();
          current =
              emitOps(current, body.begin(), std::prev(body.end()));
          if (failed(emissionStatus))
            return current;
          builder.setInsertionPointToEnd(current);
        }
        continue;
      }
      if (auto ifOp = dyn_cast<scf::IfOp>(op)) {
        Block *thenBlock = new Block();
        Block *joinBlock = new Block();
        for (Type type : ifOp.getResultTypes())
          joinBlock->addArgument(type, loc);
        state->push_back(thenBlock);
        state->push_back(joinBlock);
        Block *elseBlock = joinBlock;
        if (!ifOp.getElseRegion().empty()) {
          elseBlock = new Block();
          joinBlock->getParent()->push_back(elseBlock);
          elseBlock->moveBefore(joinBlock);
        }
        cf::CondBranchOp::create(builder, loc, mapValue(ifOp.getCondition()),
                                 thenBlock, ValueRange{}, elseBlock,
                                 ValueRange{});
        Block &thenSrc = ifOp.getThenRegion().front();
        Block *thenEnd = emitOps(thenBlock, thenSrc.begin(), thenSrc.end());
        if (failed(emissionStatus))
          return current;
        if (thenEnd->empty() ||
            !thenEnd->back().hasTrait<OpTrait::IsTerminator>()) {
          builder.setInsertionPointToEnd(thenEnd);
          auto yield = cast<scf::YieldOp>(thenSrc.back());
          llvm::SmallVector<Value> operands;
          for (Value operand : yield.getOperands())
            operands.push_back(mapValue(operand));
          cf::BranchOp::create(builder, loc, joinBlock, operands);
        }
        if (elseBlock != joinBlock) {
          Block &elseSrc = ifOp.getElseRegion().front();
          Block *elseEnd = emitOps(elseBlock, elseSrc.begin(), elseSrc.end());
          if (failed(emissionStatus))
            return current;
          if (elseEnd->empty() ||
              !elseEnd->back().hasTrait<OpTrait::IsTerminator>()) {
            builder.setInsertionPointToEnd(elseEnd);
            auto yield = cast<scf::YieldOp>(elseSrc.back());
            llvm::SmallVector<Value> operands;
            for (Value operand : yield.getOperands())
              operands.push_back(mapValue(operand));
            cf::BranchOp::create(builder, loc, joinBlock, operands);
          }
        }
        if (joinBlock != &state->back()) {
          Region *region = joinBlock->getParent();
          joinBlock->moveBefore(region, region->end());
        }
        current = joinBlock;
        builder.setInsertionPointToEnd(current);
        for (auto [result, argument] :
             llvm::zip_equal(ifOp.getResults(), joinBlock->getArguments()))
          values[result] = argument;
        continue;
      }
      if (isa<scf::YieldOp>(op))
        continue;
      if (isa<ac::YieldSimOp>(op)) {
        llvm::StringRef impl = wakeNextTickImplSymbol.empty()
                                   ? llvm::StringRef(wakeImplSymbol)
                                   : llvm::StringRef(wakeNextTickImplSymbol);
        emitSuspend(impl);
        return current;
      }
    }
    return current;
  };
  Block &sourceBody = sourceProcess.getBody().front();
  auto bindCaptures = [&](Region &region) {
    for (auto [arg, mapped] : llvm::zip_equal(
             sourceBody.getArguments(), region.front().getArguments()))
      values[arg] = mapped;
  };

  auto suspendSlice = [&](Region &region, Value condition,
                          FlatSymbolRefAttr onTrue, FlatSymbolRefAttr onFalse) {
    Block *tail = &region.back();
    if (!tail->empty() && tail->back().hasTrait<OpTrait::IsTerminator>()) {
      tail = new Block();
      region.push_back(tail);
    }
    builder.setInsertionPointToEnd(tail);
    if (condition) {
      Block *advance = new Block();
      Block *retry = new Block();
      region.push_back(advance);
      region.push_back(retry);
      cf::CondBranchOp::create(builder, loc, condition, advance, ValueRange{},
                               retry, ValueRange{});
      builder.setInsertionPointToEnd(advance);
      emitSuspend(nextTickImpl, onTrue);
      builder.setInsertionPointToEnd(retry);
      emitSuspend(nextTickImpl, onFalse);
    } else {
      emitSuspend(nextTickImpl, onTrue);
    }
  };

  auto finishRegion = [&](Region &region) {
    Block *tail = &region.back();
    if (tail->empty() || !tail->back().hasTrait<OpTrait::IsTerminator>()) {
      builder.setInsertionPointToEnd(tail);
      emitSuspend(nextTickImpl, entry);
    }
  };

  Block::iterator cursor = sourceBody.begin();
  for (size_t index = 0; index < waits.size(); ++index) {
    state = &process.getStates()[index];
    bindCaptures(*state);
    Block *start = &state->front();
    builder.setInsertionPointToStart(start);
    if (index != 0)
      emitLiveLoads();
    emitOps(start, cursor, Block::iterator(waits[index]));
    if (failed(emissionStatus))
      return mlir::failure();
    auto currentPc = cast<FlatSymbolRefAttr>(pcAttrs[index]);
    auto nextPc = resumePcByWait.lookup(waits[index]);
    Value condition;
    if (auto waitUntil = dyn_cast<ac::WaitUntilOp>(waits[index]))
      condition = values.lookup(waitUntil.getCondition());
    else if (auto waitFor = dyn_cast<ac::WaitForOp>(waits[index])) {
      std::string identity = "acir.resource.acquire." + moduleName + "." +
                             waitFor.getResource().str();
      Block *tail = &state->back();
      if (tail->empty() || !tail->back().hasTrait<OpTrait::IsTerminator>())
        builder.setInsertionPointToEnd(tail);
      auto invoke = acsim::InvokeOp::create(
          builder, loc, TypeRange{builder.getI1Type()}, ValueRange{},
          FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity)));
      condition = invoke.getResult(0);
    }
    if (failed(emitLiveStores()))
      return mlir::failure();
    suspendSlice(*state, condition, nextPc, currentPc);
    cursor = std::next(Block::iterator(waits[index]));
  }
  state = &process.getStates()[waits.size()];
  bindCaptures(*state);
  builder.setInsertionPointToStart(&state->front());
  if (!waits.empty())
    emitLiveLoads();
  emitOps(&state->front(), cursor, sourceBody.end());
  if (failed(emissionStatus))
    return mlir::failure();
  finishRegion(*state);

  // A nested suspension resumes at the enclosing branch continuation. For the
  // currently lowerable form there are no operations after the nested wait in
  // that branch, so the resume PC rejoins the next-tick loop directly.
  for (size_t index = waits.size() + 1; index < pcAttrs.size(); ++index) {
    state = &process.getStates()[index];
    bindCaptures(*state);
    builder.setInsertionPointToEnd(&state->front());
    emitLiveLoads();
    finishRegion(*state);
  }
  return mlir::success();
}

mlir::LogicalResult
ACIRToACSimPass::emitModuleBody(OpBuilder &builder,
                                const ModulePlan &planned) {
  MLIRContext *context = builder.getContext();
  llvm::SmallVector<Attribute> portRecords;
  llvm::SmallVector<Attribute> resultRecords;
  llvm::SmallVector<Attribute> exports;
  auto reference = [&](llvm::StringRef identity) {
    return FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(identity));
  };
  for (const ModulePortPlan &port : planned.ports) {
    const bindings::PortBinding &metadata = port.metadata;
    portRecords.push_back(builder.getDictionaryAttr(
        {builder.getNamedAttr("accessor", reference(metadata.accessor)),
         builder.getNamedAttr("cardinality",
                              builder.getStringAttr(metadata.cardinality)),
         builder.getNamedAttr("delegation",
                              builder.getStringAttr(metadata.delegation)),
         builder.getNamedAttr("direction",
                              builder.getStringAttr(metadata.direction)),
         builder.getNamedAttr("interface", reference(metadata.interface)),
         builder.getNamedAttr("name", builder.getStringAttr(port.name)),
         builder.getNamedAttr("ownership",
                              builder.getStringAttr(metadata.ownership)),
         builder.getNamedAttr("payload", reference(metadata.payload)),
         builder.getNamedAttr("protocol", reference(metadata.protocol)),
         builder.getNamedAttr("role", reference(metadata.role)),
         builder.getNamedAttr("time_domain", reference(metadata.timeDomain))}));
    exports.push_back(FlatSymbolRefAttr::get(context, port.name));
  }
  for (const ModuleResultPlan &result : planned.results) {
    resultRecords.push_back(builder.getDictionaryAttr(
        {builder.getNamedAttr(
             "cpp_type", FlatSymbolRefAttr::get(
                             context, typeSymbols.symbolFor(result.cppType))),
         builder.getNamedAttr("name", builder.getStringAttr(result.name))}));
    exports.push_back(FlatSymbolRefAttr::get(context, result.name));
  }
  DictionaryAttr interface = builder.getDictionaryAttr(
      {builder.getNamedAttr("ports", builder.getArrayAttr(portRecords)),
       builder.getNamedAttr("resources", builder.getArrayAttr({})),
       builder.getNamedAttr("results", builder.getArrayAttr(resultRecords))});
  auto module = acsim::ModuleOp::create(
      builder, planned.source->getLoc(), builder.getStringAttr(planned.name),
      interface, planned.staticParams,
      builder.getStringAttr(planned.specialization),
      builder.getArrayAttr(exports));
  Block *body = new Block();
  module.getBody().push_back(body);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(body);
  llvm::DenseMap<Value, Value> emittedValues;
  llvm::SmallVector<Value> owners(planned.placements.size());
  llvm::SmallVector<llvm::SmallVector<Value, 2>> inputProjections(
      planned.placements.size());

  // Rank 0: owned placements.
  for (auto [placementIndex, placement] : llvm::enumerate(planned.placements)) {
    switch (placement.kind) {
    case PlacementPlan::Kind::Instance: {
      auto target = SymbolRefAttr::get(context, placement.targetSymbol);
      auto ownerType = acsim::OwnerType::get(context, target);
      owners[placementIndex] =
          acsim::InstanceOp::create(
              builder, planned.source->getLoc(), ownerType,
              builder.getStringAttr(placement.name), target,
              placement.staticArgs,
              builder.getStringAttr(placement.specialization))
              .getResult();
      break;
    }
    case PlacementPlan::Kind::Array: {
      auto target = SymbolRefAttr::get(context, placement.targetSymbol);
      auto ownerType = acsim::OwnerType::get(context, target);
      auto shape = builder.getDenseI64ArrayAttr(placement.shape);
      auto arrayType = acsim::ArrayType::get(context, shape, ownerType);
      owners[placementIndex] =
          acsim::ArrayOp::create(
              builder, planned.source->getLoc(), arrayType,
              builder.getStringAttr(placement.name), target,
              placement.staticArgs,
              builder.getStringAttr(placement.specialization), shape)
              .getResult();
      break;
    }
    case PlacementPlan::Kind::Process:
      break;
    }
  }

  auto emitPort = [&](Value base, const PortEndpointPlan &endpoint) {
    const bindings::PortBinding &port = endpoint.metadata;
    auto type = acsim::PortType::get(
        context,
        FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(port.interface)),
        FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(port.role)),
        FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(port.payload)),
        FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(port.protocol)));
    return acsim::PortOp::create(
               builder, planned.source->getLoc(), type, base,
               FlatSymbolRefAttr::get(context,
                                      typeSymbols.symbolFor(port.accessor)))
        .getResult();
  };

  // Rank 2: exact typed endpoint projections.
  for (auto [placementIndex, placement] : llvm::enumerate(planned.placements)) {
    if (!owners[placementIndex])
      continue;
    for (const PortEndpointPlan &endpoint : placement.outputPorts)
      emittedValues[endpoint.value] =
          emitPort(owners[placementIndex], endpoint);
    for (const PortEndpointPlan &endpoint : placement.inputPorts)
      inputProjections[placementIndex].push_back(
          emitPort(owners[placementIndex], endpoint));
  }

  // Rank 3: ACIR SSA endpoint uses become exact construction-time binds.
  for (auto [placementIndex, placement] : llvm::enumerate(planned.placements))
    for (auto [inputIndex, endpoint] : llvm::enumerate(placement.inputPorts)) {
      Value source = emittedValues.lookup(endpoint.value);
      assert(source && "validated endpoint producer must be projected");
      acsim::BindOp::create(builder, planned.source->getLoc(), source,
                            inputProjections[placementIndex][inputIndex],
                            builder.getStringAttr("port"));
    }

  // Rank 4: pure binding calls. Static constructor arguments specialize the
  // binding and therefore do not become dynamic acsim.inline operands.
  for (const PureCallPlan &call : planned.pureCalls) {
    auto resultType = acsim::ExprType::get(
        context,
        FlatSymbolRefAttr::get(context, typeSymbols.symbolFor(call.cppType)));
    auto inlineOp = acsim::InlineOp::create(
        builder, call.source->getLoc(), resultType, ValueRange{},
        FlatSymbolRefAttr::get(context, call.binding));
    emittedValues[call.result] = inlineOp.getResult();
  }

  // Rank 6: ordered endpoint and scalar exports.
  llvm::SmallVector<Value> returned;
  for (const ModulePortPlan &port : planned.ports) {
    Value value = emittedValues.lookup(port.source);
    assert(value && "validated module port producer must be emitted");
    auto exportOp = acsim::ExportOp::create(
        builder, planned.source->getLoc(), value.getType(), value,
        builder.getStringAttr(port.name), reference(port.metadata.role));
    returned.push_back(exportOp.getResult());
  }
  llvm::StringRef resultRole = typeSymbols.symbolFor(kResultRoleIdentity);
  for (const ModuleResultPlan &result : planned.results) {
    Value value = emittedValues.lookup(result.source);
    assert(value && "validated module result producer must be emitted");
    auto exportOp = acsim::ExportOp::create(
        builder, planned.source->getLoc(), value.getType(), value,
        builder.getStringAttr(result.name),
        FlatSymbolRefAttr::get(context, resultRole));
    returned.push_back(exportOp.getResult());
  }

  // Rank 8: stateful processes.
  for (const PlacementPlan &placement : planned.placements)
    if (placement.kind == PlacementPlan::Kind::Process)
      if (failed(emitProcessBody(builder, placement, emittedValues)))
        return mlir::failure();

  acsim::ReturnOp::create(builder, planned.source->getLoc(), returned);
  return mlir::success();
}

mlir::FailureOr<mlir::OwningOpRef<mlir::ModuleOp>>
ACIRToACSimPass::emit(mlir::ModuleOp input) {
  MLIRContext *context = input.getContext();
  mlir::OwningOpRef<mlir::ModuleOp> staged =
      mlir::ModuleOp::create(input.getLoc());
  (*staged)->setAttr("ac.contract_epoch", input->getAttr("ac.contract_epoch"));
  OpBuilder builder(context);
  builder.setInsertionPointToEnd(staged->getBody());

  llvm::SmallVector<Attribute> construction;
  llvm::SmallVector<Attribute> destructionAttrs;
  for (const std::string &path : constructionOrder)
    construction.push_back(builder.getStringAttr(path));
  for (auto it = constructionOrder.rbegin(); it != constructionOrder.rend();
       ++it)
    destructionAttrs.push_back(builder.getStringAttr(*it));

  DictionaryAttr fingerprints = builder.getDictionaryAttr(
      {builder.getNamedAttr("frozen_acir",
                            builder.getStringAttr(frozenAcirFingerprint)),
       builder.getNamedAttr("binding_lock",
                            builder.getStringAttr(bindingLockFingerprint)),
       builder.getNamedAttr("provider",
                            builder.getStringAttr(providerFingerprint)),
       builder.getNamedAttr("profile",
                            builder.getStringAttr(profileFingerprint)),
       builder.getNamedAttr("toolchain",
                            builder.getStringAttr(toolchainFingerprint)),
       builder.getNamedAttr("schema_set",
                            builder.getStringAttr(schemaSetFingerprint))});

  auto model = acsim::ModelOp::create(
      builder, input.getLoc(),
      builder.getStringAttr(selectedSystem.getSymName()),
      builder.getStringAttr(kEpoch),
      FlatSymbolRefAttr::get(context, selectedSystem.getRoot()),
      builder.getArrayAttr(construction),
      builder.getArrayAttr(destructionAttrs), fingerprints);

  Block *modelBody = new Block();
  model.getBody().push_back(modelBody);
  builder.setInsertionPointToStart(modelBody);

  // Rank 0: acsim.type declarations, strictly symbol-sorted.
  for (const TypeDeclaration *declaration : typeSymbols.declarations()) {
    IntegerAttr period;
    IntegerAttr phase;
    IntegerAttr tickScale;
    FlatSymbolRefAttr parent;
    DictionaryAttr bridge;
    if (declaration->period) {
      period = builder.getI64IntegerAttr(*declaration->period);
      phase = builder.getI64IntegerAttr(declaration->phase);
      tickScale = builder.getI64IntegerAttr(declaration->tickScale);
    }
    if (declaration->parent)
      parent = FlatSymbolRefAttr::get(
          context, typeSymbols.symbolFor(*declaration->parent));
    if (declaration->bridgeKind)
      bridge = builder.getDictionaryAttr(
          {builder.getNamedAttr(
               "kind", builder.getStringAttr(*declaration->bridgeKind)),
           builder.getNamedAttr(
               "owner",
               FlatSymbolRefAttr::get(context, *declaration->bridgeOwner))});
    acsim::TypeOp::create(builder, input.getLoc(),
                          builder.getStringAttr(declaration->symbol),
                          builder.getStringAttr(declaration->cpp),
                          builder.getStringAttr(declaration->kind),
                          builder.getStringAttr(declaration->fingerprint),
                          period, phase, tickScale, parent, bridge);
  }

  // Rank 1: acsim.binding records, strictly symbol-sorted.
  {
    llvm::SmallVector<const bindings::BindingRecord *> records;
    for (const bindings::ResolvedBinding &selection : resolution->selections())
      records.push_back(&selection.record());
    llvm::sort(records, [](const bindings::BindingRecord *left,
                           const bindings::BindingRecord *right) {
      return left->binding() < right->binding();
    });
    for (const bindings::BindingRecord *record : records)
      acsim::BindingOp::create(builder, input.getLoc(),
                               builder.getStringAttr(record->binding()),
                               cast<DictionaryAttr>(convertBindingRecord(
                                   builder, *record, typeSymbols)));
  }

  // Rank 2: acsim.module declarations, child-before-parent with
  // symbol-sorted ties between independent nodes.
  for (const ModulePlan &planned : modules)
    if (failed(emitModuleBody(builder, planned)))
      return mlir::failure();

  // Rank 3: one typed dispatch row per runtime object, dense IDs.
  llvm::SmallVector<acsim::DispatchOp> dispatches;
  for (auto [id, row] : llvm::enumerate(runtimeRows)) {
    const ModulePlan &module = modules[row.moduleIndex];
    const PlacementPlan &placement = module.placements[row.placementIndex];
    auto target =
        SymbolRefAttr::get(context, module.name,
                           {FlatSymbolRefAttr::get(context, placement.name)});
    std::string work = placement.work;
    std::string xfer = placement.xfer;
    std::string reset = placement.reset;
    std::string validate = placement.validate;
    if (placement.kind == PlacementPlan::Kind::Process) {
      std::string base =
          ("acsim_generated::" + module.name + "::s" +
           module.specialization.substr(7) + "::" + placement.name + "::p" +
           placement.specialization.substr(7) + "::");
      work = base + "work";
      xfer = base + "xfer";
      reset = base + "reset";
      validate = base + "validate";
    }
    dispatches.push_back(acsim::DispatchOp::create(
        builder, input.getLoc(), acsim::ObjectIdType::get(context),
        acsim::ActivationIdType::get(context), target,
        builder.getStringAttr(row.path),
        builder.getDenseI64ArrayAttr(row.indices),
        builder.getI64IntegerAttr(static_cast<int64_t>(id)),
        builder.getI64IntegerAttr(static_cast<int64_t>(id)),
        builder.getStringAttr(work), builder.getStringAttr(xfer),
        builder.getStringAttr(reset), builder.getStringAttr(validate)));
  }

  // Rank 4: static activation adjacency. Every runtime object has its self
  // wake, and each typed construction bind adds source-object -> target-object
  // within the same expanded module context.
  std::set<std::pair<unsigned, unsigned>> activationEdges;
  for (unsigned id = 0; id < dispatches.size(); ++id)
    activationEdges.emplace(id, id);
  for (auto [moduleIndex, module] : llvm::enumerate(modules))
    for (const BindingEdgePlan &edge : module.bindingEdges)
      for (auto [sourceId, source] : llvm::enumerate(runtimeRows)) {
        if (source.moduleIndex != moduleIndex ||
            source.placementIndex != edge.sourcePlacement)
          continue;
        for (auto [targetId, target] : llvm::enumerate(runtimeRows))
          if (target.moduleIndex == moduleIndex &&
              target.placementIndex == edge.targetPlacement &&
              target.contextPath == source.contextPath)
            activationEdges.emplace(sourceId, targetId);
      }
  for (auto [source, target] : activationEdges)
    acsim::ActivateOp::create(builder, input.getLoc(),
                              dispatches[source].getActivation(),
                              dispatches[target].getObject());

  if (failed(mlir::verify(*staged)) ||
      failed(acsim::verifyCanonicalACSimFile(*staged)))
    return mlir::failure();
  return staged;
}

void ACIRToACSimPass::publish(mlir::ModuleOp input, mlir::ModuleOp staged) {
  Operation *model = &staged.getBody()->front();
  model->remove();

  llvm::SmallVector<Operation *> obsolete;
  for (Operation &operation : *input.getBody())
    obsolete.push_back(&operation);
  for (Operation *operation : obsolete)
    operation->erase();
  input.getBody()->push_back(model);

  llvm::SmallVector<NamedAttribute> retained;
  for (NamedAttribute attribute : input->getAttrs())
    if (attribute.getName() == "ac.contract_epoch")
      retained.push_back(attribute);
  input->setAttrs(retained);
}

mlir::LogicalResult ACIRToACSimPass::lower(mlir::ModuleOp input) {
  if (failed(plan(input)))
    return mlir::failure();
  auto staged = emit(input);
  if (failed(staged))
    return mlir::failure();
  publish(input, **staged);
  return mlir::success();
}

} // namespace

std::unique_ptr<mlir::Pass>
createACIRToACSimPass(ACIRToACSimPassOptions options) {
  return std::make_unique<ACIRToACSimPass>(std::move(options));
}

} // namespace acir
