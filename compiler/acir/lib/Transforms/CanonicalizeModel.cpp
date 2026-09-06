#include "acir/Transforms/Passes.h"

#include "Analysis/ModelAnalysisInternal.h"
#include "acir/Analysis/ModelAnalysis.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACIR/ACIRResources.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"

using namespace mlir;

namespace acir {
namespace {

std::string token(Attribute attribute) {
  std::string storage;
  llvm::raw_string_ostream stream(storage);
  stream << attribute;
  return storage;
}

unsigned topLevelRank(Operation *operation) {
  StringRef name = operation->getName().getStringRef();
  return llvm::StringSwitch<unsigned>(name)
      .Case("ac.system", 0)
      .Cases({"ac.type_scope", "ac.type_alias", "ac.struct", "ac.enum",
              "ac.packet", "ac.transaction"},
             1)
      .Cases({"ac.interface", "ac.protocol"}, 2)
      .Case("func.func", 3)
      .Cases({"ac.module", "ac.module.extern"}, 4)
      .Default(5);
}

unsigned graphRank(Operation *operation) {
  StringRef name = operation->getName().getStringRef();
  if (name == "ac.return")
    return 100;
  return llvm::StringSwitch<unsigned>(name)
      .Cases({"ac.instance", "ac.array", "ac.instances", "ac.view"}, 0)
      .Cases({"ac.queue", "ac.event_queue", "ac.resource", "ac.address_space",
              "ac.address_map", "ac.time_domain"},
             1)
      .Case("ac.process", 2)
      .Case("ac.stat", 3)
      .Cases({"ac.require", "ac.ensure"}, 4)
      .Default(5);
}

unsigned typeScopeRank(Operation *) { return 0; }

unsigned interfaceRank(Operation *operation) {
  return llvm::StringSwitch<unsigned>(operation->getName().getStringRef())
      .Case("ac.role", 0)
      .Case("ac.port", 1)
      .Case("ac.guarantee", 2)
      .Default(3);
}

unsigned protocolRank(Operation *operation) {
  return llvm::StringSwitch<unsigned>(operation->getName().getStringRef())
      .Case("ac.role", 0)
      .Case("ac.state", 1)
      .Case("ac.event", 2)
      // Transition order is semantically ordered and must remain stable.
      .Case("ac.transition", 3)
      .Case("ac.guarantee", 4)
      .Default(5);
}

std::string stableKey(Operation *operation, unsigned rank) {
  std::string rankKey = std::to_string(rank);
  rankKey.insert(rankKey.begin(), 3 - std::min<size_t>(3, rankKey.size()), '0');
  std::string key = rankKey + '|';
  key.append(operation->getName().getStringRef());
  key.push_back('|');
  if (auto name = operation->getAttrOfType<StringAttr>(
          SymbolTable::getSymbolAttrName()))
    key.append(name.getValue());
  key.push_back('|');
  SmallVector<NamedAttribute> attributes(operation->getAttrs().begin(),
                                         operation->getAttrs().end());
  llvm::sort(attributes, [](NamedAttribute left, NamedAttribute right) {
    return left.getName().getValue() < right.getName().getValue();
  });
  for (NamedAttribute attribute : attributes) {
    StringRef name = attribute.getName().getValue();
    if (name.starts_with("ac.frozen_") || name == "ac.freeze_proven")
      continue;
    key.append(name);
    key.push_back('=');
    key.append(token(attribute.getValue()));
    key.push_back(';');
  }
  return key;
}

LogicalResult sortBlock(Block &block, bool topologyFrozen,
                        llvm::function_ref<unsigned(Operation *)> rank) {
  SmallVector<Operation *> current;
  for (Operation &operation : block)
    current.push_back(&operation);
  SmallVector<Operation *> sorted = current;
  llvm::stable_sort(sorted, [&](Operation *left, Operation *right) {
    unsigned leftRank = rank(left);
    unsigned rightRank = rank(right);
    if (leftRank == rightRank && isa<ac::TransitionOp>(left) &&
        isa<ac::TransitionOp>(right))
      return false;
    return stableKey(left, leftRank) < stableKey(right, rightRank);
  });
  if (current == sorted)
    return success();
  if (topologyFrozen)
    return block.getParentOp()->emitError(
        "topology declaration order was mutated after ac-freeze-topology");
  for (Operation *operation : sorted)
    operation->moveBefore(&block, block.end());
  return success();
}

} // namespace

LogicalResult canonicalizeModel(ModuleOp model) {
  if (failed(detail::preflightModelStructure(model)))
    return failure();
  if (detail::hasTopologyFreezeEvidence(model))
    return verifyModel(model);
  bool frozen = false;
  ac::normalizeAddressMaps(model);
  if (failed(sortBlock(*model.getBody(), frozen, topLevelRank)))
    return failure();
  for (ac::ModuleOp module : model.getOps<ac::ModuleOp>())
    if (!module.getBody().empty() &&
        failed(sortBlock(module.getBody().front(), frozen, graphRank)))
      return failure();
  for (ac::TypeScopeOp scope : model.getOps<ac::TypeScopeOp>())
    if (!scope.getBody().empty() &&
        failed(sortBlock(scope.getBody().front(), frozen, typeScopeRank)))
      return failure();
  for (ac::InterfaceOp interface : model.getOps<ac::InterfaceOp>())
    if (!interface.getBody().empty() &&
        failed(sortBlock(interface.getBody().front(), frozen, interfaceRank)))
      return failure();
  for (ac::ProtocolOp protocol : model.getOps<ac::ProtocolOp>())
    if (!protocol.getBody().empty() &&
        failed(sortBlock(protocol.getBody().front(), frozen, protocolRank)))
      return failure();

  UnknownLoc unknown = UnknownLoc::get(model.getContext());
  model.walk([&](Operation *operation) {
    operation->setLoc(unknown);
    for (Region &region : operation->getRegions())
      for (Block &block : region)
        for (BlockArgument argument : block.getArguments())
          argument.setLoc(unknown);
  });
  return success();
}

namespace {
#define GEN_PASS_DEF_CANONICALIZEMODELPASS
#include "acir/Transforms/Passes.h.inc"

struct CanonicalizeModelPass
    : impl::CanonicalizeModelPassBase<CanonicalizeModelPass> {
  void runOnOperation() override {
    if (failed(canonicalizeModel(getOperation())))
      signalPassFailure();
  }
};
} // namespace

std::unique_ptr<Pass> createCanonicalizeModelPass() {
  return std::make_unique<CanonicalizeModelPass>();
}

} // namespace acir
