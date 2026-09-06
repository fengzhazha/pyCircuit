#include "acir/Dialect/ACIR/ACIROps.h"
#include "ACIROpsTestHooks.h"
#include "ProcessLowerability.h"
#include "acir/Dialect/ACIR/ACIRResources.h"
#include "acir/Dialect/ACIR/GraphRegion.h"

#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/FunctionImplementation.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

#include <limits>
#include <optional>

using namespace mlir;

namespace acir::ac {
namespace {

thread_local detail::ProcessLivenessWork *processLivenessWorkCollector =
    nullptr;

} // namespace

static DictionaryAttr activationQueueResource(MLIRContext *context,
                                              ActivationResourceKind kind,
                                              size_t ordinal) {
  Builder builder(context);
  NamedAttrList fields;
  fields.set("kind", ActivationResourceKindAttr::get(context, kind));
  fields.set("ordinal", builder.getI64IntegerAttr(ordinal));
  return builder.getDictionaryAttr(fields);
}

static DictionaryAttr activationStateResource(MLIRContext *context,
                                              StringRef resource) {
  Builder builder(context);
  NamedAttrList fields;
  fields.set("kind", ActivationResourceKindAttr::get(
                         context, ActivationResourceKind::State));
  fields.set("resource", FlatSymbolRefAttr::get(context, resource));
  return builder.getDictionaryAttr(fields);
}

static std::optional<bool> constantVarBool(Value value) {
  auto constant = value.getDefiningOp<VarConstantOp>();
  auto integer =
      constant ? dyn_cast<IntegerAttr>(constant.getValue()) : IntegerAttr();
  if (!integer)
    return std::nullopt;
  return !integer.getValue().isZero();
}

static RuleGuardKind guardKindFor(Value value) {
  return constantVarBool(value) == true ? RuleGuardKind::Always
                                        : RuleGuardKind::Predicate;
}

static bool presenceImpliesCandidate(Value present, Value candidate) {
  return present == candidate || constantVarBool(candidate) == true;
}

static bool isBooleanComplement(Value candidate, Value base) {
  auto compare = candidate.getDefiningOp<VarCmpOp>();
  if (!compare || compare.getPredicate() != "eq")
    return false;
  return (compare.getLhs() == base &&
          constantVarBool(compare.getRhs()) == false) ||
         (compare.getRhs() == base &&
          constantVarBool(compare.getLhs()) == false);
}

static bool areBooleanComplements(Value left, Value right) {
  return isBooleanComplement(left, right) || isBooleanComplement(right, left);
}

static LogicalResult verifyActivationEvidence(Operation *operation,
                                              ValueRange inputs,
                                              ValueRange outputs, Region &body,
                                              bool required) {
  auto sources = operation->getAttrOfType<ArrayAttr>("ac.activation_sources");
  auto transaction =
      operation->getAttrOfType<ArrayAttr>("ac.transaction_resources");
  auto initiallyActive =
      operation->getAttrOfType<BoolAttr>("ac.initially_active");
  if (!sources && !transaction && !initiallyActive)
    return required ? operation->emitOpError(
                          "requires typed activation/transaction evidence")
                    : success();
  if (!sources || !transaction || !initiallyActive)
    return operation->emitOpError(
        "activation/transaction evidence must be complete");

  Builder builder(operation->getContext());
  SmallVector<Attribute> expectedSources;
  SmallVector<Attribute> expectedTransaction;
  for (size_t index = 0; index < inputs.size(); ++index) {
    DictionaryAttr resource = activationQueueResource(
        operation->getContext(), ActivationResourceKind::InputQueue, index);
    expectedSources.push_back(resource);
    expectedTransaction.push_back(resource);
  }
  for (size_t index = 0; index < outputs.size(); ++index) {
    DictionaryAttr resource = activationQueueResource(
        operation->getContext(), ActivationResourceKind::OutputQueue, index);
    expectedSources.push_back(resource);
    expectedTransaction.push_back(resource);
  }
  llvm::StringSet<> sourceState;
  llvm::StringSet<> transactionState;
  body.walk([&](Operation *nested) {
    FlatSymbolRefAttr resource;
    if (auto read = dyn_cast<TableGetOp>(nested))
      resource = read.getTableAttr();
    else if (auto match = dyn_cast<TableMatchOp>(nested))
      resource = match.getTableAttr();
    else if (auto choose = dyn_cast<TableChooseOp>(nested))
      resource = choose.getTableAttr();
    else if (auto proposal = dyn_cast<TableProposeOp>(nested)) {
      resource = proposal.getTableAttr();
      if (transactionState.insert(resource.getValue()).second)
        expectedTransaction.push_back(activationStateResource(
            operation->getContext(), resource.getValue()));
    }
    if (resource && sourceState.insert(resource.getValue()).second)
      expectedSources.push_back(activationStateResource(operation->getContext(),
                                                        resource.getValue()));
  });
  if (sources != builder.getArrayAttr(expectedSources) ||
      transaction != builder.getArrayAttr(expectedTransaction) ||
      initiallyActive.getValue() != inputs.empty())
    return operation->emitOpError(
        "activation/transaction evidence must exactly match typed resources");
  return success();
}

static LogicalResult
verifyTypedRuleSummary(Operation *operation, ValueRange inputs,
                       ValueRange outputs, Region &body, ArrayAttr footprints,
                       IntegerAttr priority, StringRef prefix) {
  auto name = [&](StringRef suffix) { return prefix.str() + suffix.str(); };
  auto guard = operation->getAttrOfType<RuleGuardKindAttr>(name("guard_kind"));
  auto schedule =
      operation->getAttrOfType<RuleScheduleKindAttr>(name("schedule_kind"));
  auto checks = operation->getAttrOfType<ArrayAttr>(name("checks_typed"));
  auto effects = operation->getAttrOfType<ArrayAttr>(name("effects_typed"));
  auto presence = operation->getAttrOfType<ArrayAttr>(name("output_presence"));
  auto stateAccesses =
      operation->getAttrOfType<ArrayAttr>(name("state_accesses"));
  auto arbitration =
      operation->getAttrOfType<ArrayAttr>(name("arbitration_membership"));
  if (!guard || !schedule || !checks || !effects || !presence ||
      !stateAccesses || !arbitration || !footprints || !priority)
    return operation->emitOpError(
        "requires complete typed rule summary evidence");

  bool predicate = false;
  body.walk([&](FiringConditionOp condition) {
    auto constant = condition.getCondition().getDefiningOp<VarConstantOp>();
    auto value =
        constant ? dyn_cast<IntegerAttr>(constant.getValue()) : IntegerAttr();
    predicate |= !value || value.getValue().isZero();
  });
  const RuleGuardKind expectedGuard =
      predicate ? RuleGuardKind::Predicate : RuleGuardKind::Always;
  SmallVector<TableProposeOp> proposals;
  body.walk([&](TableProposeOp proposal) { proposals.push_back(proposal); });
  const RuleScheduleKind expectedSchedule =
      proposals.empty() ? RuleScheduleKind::Independent
                        : RuleScheduleKind::LexicalPriority;
  if (guard.getValue() != expectedGuard ||
      schedule.getValue() != expectedSchedule)
    return operation->emitOpError(
        "typed guard/schedule evidence does not match the body");

  Builder builder(operation->getContext());
  auto queueRecord = [&](StringRef kindName, Attribute kind, size_t ordinal,
                         RuleGuardKind path) {
    NamedAttrList fields;
    fields.set(kindName, kind);
    fields.set("ordinal", builder.getI64IntegerAttr(ordinal));
    fields.set("guard_kind",
               RuleGuardKindAttr::get(operation->getContext(), path));
    return builder.getDictionaryAttr(fields);
  };
  SmallVector<Attribute> expectedChecks;
  SmallVector<Attribute> expectedEffects;
  SmallVector<Attribute> expectedPresence;
  SmallVector<Value> outputPresenceValues(outputs.size());
  body.walk([&](FiringOutputOp output) {
    if (output.getOrdinal() >= 0 &&
        static_cast<size_t>(output.getOrdinal()) < outputPresenceValues.size())
      outputPresenceValues[output.getOrdinal()] = output.getWhen();
  });
  for (size_t index = 0; index < inputs.size(); ++index) {
    expectedChecks.push_back(
        queueRecord("kind",
                    RuleCheckKindAttr::get(operation->getContext(),
                                           RuleCheckKind::InputAvailable),
                    index, RuleGuardKind::Always));
    expectedEffects.push_back(
        queueRecord("kind",
                    RuleEffectKindAttr::get(operation->getContext(),
                                            RuleEffectKind::InputConsume),
                    index, expectedGuard));
  }
  for (size_t index = 0; index < outputs.size(); ++index) {
    const RuleGuardKind outputGuard =
        outputPresenceValues[index] ? guardKindFor(outputPresenceValues[index])
                                    : expectedGuard;
    expectedChecks.push_back(
        queueRecord("kind",
                    RuleCheckKindAttr::get(operation->getContext(),
                                           RuleCheckKind::OutputCapacity),
                    index, outputGuard));
    expectedEffects.push_back(
        queueRecord("kind",
                    RuleEffectKindAttr::get(operation->getContext(),
                                            RuleEffectKind::OutputProduce),
                    index, outputGuard));
    NamedAttrList output;
    output.set("ordinal", builder.getI64IntegerAttr(index));
    output.set("presence_kind", RuleOutputPresenceKindAttr::get(
                                    operation->getContext(),
                                    outputGuard == RuleGuardKind::Always
                                        ? RuleOutputPresenceKind::Always
                                        : RuleOutputPresenceKind::Predicate));
    expectedPresence.push_back(builder.getDictionaryAttr(output));
  }

  SmallVector<Attribute> expectedConflicts;
  for (Attribute attribute : footprints) {
    auto footprint = dyn_cast<DictionaryAttr>(attribute);
    auto access =
        footprint ? footprint.getAs<StringAttr>("access") : StringAttr();
    auto resource = footprint ? footprint.getAs<FlatSymbolRefAttr>("resource")
                              : FlatSymbolRefAttr();
    auto indexKind =
        footprint ? footprint.getAs<StringAttr>("index_kind") : StringAttr();
    auto footprintGuard = footprint
                              ? footprint.getAs<RuleGuardKindAttr>("guard_kind")
                              : RuleGuardKindAttr();
    if (!access || !resource || !indexKind || !footprintGuard)
      return operation->emitOpError(
          "typed rule summary requires valid state footprints");
    const bool read = access.getValue() == "read";
    const RuleGuardKind stateGuard =
        read ? RuleGuardKind::Always : footprintGuard.getValue();
    NamedAttrList effect;
    effect.set("kind",
               RuleEffectKindAttr::get(operation->getContext(),
                                       read ? RuleEffectKind::StateRead
                                            : RuleEffectKind::StateWrite));
    effect.set("resource", resource);
    effect.set("guard_kind",
               RuleGuardKindAttr::get(operation->getContext(), stateGuard));
    expectedEffects.push_back(builder.getDictionaryAttr(effect));

    NamedAttrList conflict;
    conflict.set("kind", RuleStateAccessKindAttr::get(
                             operation->getContext(),
                             read ? RuleStateAccessKind::Read
                                  : (access.getValue() == "replace"
                                         ? RuleStateAccessKind::Replace
                                         : RuleStateAccessKind::FieldWrite)));
    conflict.set("resource", resource);
    RuleIndexKind typedIndex =
        indexKind.getValue() == "static"
            ? RuleIndexKind::Static
            : (indexKind.getValue() == "dynamic" ? RuleIndexKind::Dynamic
                                                 : RuleIndexKind::All);
    conflict.set("index_kind",
                 RuleIndexKindAttr::get(operation->getContext(), typedIndex));
    conflict.set("guard_kind",
                 RuleGuardKindAttr::get(operation->getContext(), stateGuard));
    if (auto fields = footprint.getAs<ArrayAttr>("fields"))
      conflict.set("fields", fields);
    expectedConflicts.push_back(builder.getDictionaryAttr(conflict));
  }

  SmallVector<Attribute> expectedArbitration;
  llvm::StringSet<> seenResources;
  for (TableProposeOp proposal : proposals) {
    if (!seenResources.insert(proposal.getTable()).second)
      continue;
    NamedAttrList record;
    record.set("resource", proposal.getTableAttr());
    record.set("priority", priority);
    expectedArbitration.push_back(builder.getDictionaryAttr(record));
  }
  if (checks != builder.getArrayAttr(expectedChecks) ||
      effects != builder.getArrayAttr(expectedEffects) ||
      presence != builder.getArrayAttr(expectedPresence) ||
      stateAccesses != builder.getArrayAttr(expectedConflicts) ||
      arbitration != builder.getArrayAttr(expectedArbitration))
    return operation->emitOpError(
        "typed checks/effects/presence/state-access/arbitration summary must "
        "exactly match the body");
  return success();
}

LogicalResult verifyLoweredRuleTransformContract(TransformOp transform) {
  constexpr llvm::StringLiteral kPrefix = "ac.rule_";
  llvm::StringSet<> allowed = {
      "ac.rule_definition",
      "ac.rule_stable_id",
      "ac.rule_time_domain",
      "ac.rule_priority",
      "ac.rule_footprints",
      "ac.rule_effects_typed",
      "ac.rule_checks_typed",
      "ac.rule_output_presence",
      "ac.rule_state_accesses",
      "ac.rule_guard_kind",
      "ac.rule_schedule_kind",
      "ac.rule_arbitration_membership",
  };
  bool hasRuleProof = false;
  for (NamedAttribute attribute : transform->getAttrs()) {
    StringRef name = attribute.getName().getValue();
    if (!name.starts_with(kPrefix))
      continue;
    hasRuleProof = true;
    if (!allowed.contains(name))
      return transform.emitOpError()
             << "has unknown lowered-rule proof '" << name << "'";
  }
  if (!hasRuleProof)
    return success();

  auto requireString = [&](StringRef name) -> FailureOr<StringAttr> {
    auto value = transform->getAttrOfType<StringAttr>(name);
    if (!value || value.getValue().empty()) {
      transform.emitOpError()
          << "requires non-empty lowered-rule proof '" << name << "'";
      return failure();
    }
    return value;
  };
  FailureOr<StringAttr> definition = requireString("ac.rule_definition");
  FailureOr<StringAttr> stableId = requireString("ac.rule_stable_id");
  FailureOr<StringAttr> domain = requireString("ac.rule_time_domain");
  auto priority = transform->getAttrOfType<IntegerAttr>("ac.rule_priority");
  auto footprints = transform->getAttrOfType<ArrayAttr>("ac.rule_footprints");
  if (failed(definition) || failed(stableId) || failed(domain) ||
      !priority || priority.getInt() < 0 || !footprints)
    return failure();
  if (transform.getInputs().empty() || transform.getOutputs().size() != 1)
    return transform.emitOpError("lowered rule requires at least one input and "
                                 "exactly one output Queue");
  if ((*domain).getValue() != "cycle" || !footprints.empty())
    return transform.emitOpError(
        "has invalid phase-one lowered-rule domain/footprint proof");
  auto model = transform->getParentOfType<mlir::ModuleOp>();
  auto graphDomain =
      model ? model->getAttrOfType<StringAttr>("ac.queue_graph_domain")
            : StringAttr();
  if (!graphDomain || graphDomain.getValue() != (*domain).getValue())
    return transform.emitOpError(
        "lowered-rule domain must match the exact QueueGraph domain");
  if (failed(verifyTypedRuleSummary(transform.getOperation(),
                                    transform.getInputs(),
                                    transform.getOutputs(), transform.getBody(),
                                    footprints, priority, "ac.rule_")))
    return failure();
  return verifyActivationEvidence(transform.getOperation(),
                                  transform.getInputs(), transform.getOutputs(),
                                  transform.getBody(), true);
}

LogicalResult TransformOp::verify() {
  if (getInputs().empty())
    return emitOpError("requires at least one input queue");
  if (getOutputs().empty())
    return emitOpError("requires at least one output queue");

  ArrayRef<int64_t> depths = getOutputDepthsAttr().asArrayRef();
  ArrayRef<int64_t> latencies = getOutputLatenciesAttr().asArrayRef();
  if (depths.size() != getOutputs().size())
    return emitOpError("output depth count must match result count");
  if (latencies.size() != getOutputs().size())
    return emitOpError("output latency count must match result count");
  if (llvm::any_of(depths, [](int64_t value) { return value <= 0; }))
    return emitOpError("output depths must be positive");
  if (llvm::any_of(latencies, [](int64_t value) { return value <= 0; }))
    return emitOpError("output latencies must be positive");

  Block &block = getBody().front();
  if (block.getNumArguments() != getInputs().size())
    return emitOpError("body argument count must match input queue count");
  for (size_t index = 0; index < getInputs().size(); ++index) {
    Value input = getInputs()[index];
    BlockArgument argument = block.getArgument(index);
    auto queue = cast<QueueType>(input.getType());
    Type expected = VarType::get(getContext(), queue.getElementType());
    if (argument.getType() != expected)
      return emitOpError() << "body argument " << index << " must be "
                           << expected;
  }

  for (Operation &operation : block.without_terminator()) {
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "body operation '" << operation.getName()
                           << "' must be pure";
  }

  auto yield = dyn_cast<TransformYieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("body must terminate with ac.transform.yield");
  if (yield.getValues().size() != getOutputs().size())
    return emitOpError("yielded value count must match output queue count");
  for (size_t index = 0; index < getOutputs().size(); ++index) {
    Value output = getOutputs()[index];
    Value value = yield.getValues()[index];
    auto queue = cast<QueueType>(output.getType());
    Type expected = VarType::get(getContext(), queue.getElementType());
    if (value.getType() != expected)
      return emitOpError() << "yielded value " << index << " must be "
                           << expected;
  }
  return verifyLoweredRuleTransformContract(*this);
}

static TableOp resolveTable(Operation *operation, FlatSymbolRefAttr reference);
static bool tableVisibleFrom(Operation *operation, TableOp table);
static LogicalResult verifyStaticallySafeRuleTableIndex(Operation *operation,
                                                        TableOp table,
                                                        Value index);
static LogicalResult verifyTableFields(Operation *endpoint, TableOp table,
                                       ArrayAttr fields, StringRef kind);
static FailureOr<uint64_t> tableEntryFieldCount(Operation *endpoint,
                                                TableOp table);
static bool tableWriteFieldsAreComplete(Operation *endpoint, TableOp table,
                                        ArrayAttr writeFields);

LogicalResult RuleOp::verify() {
  // Rules retain at most one output until optional-output branches and CFG
  // joins are materialized. Zero-output rules are consume-only state
  // transitions; zero-input rules must still produce or update state.
  if (getOutputs().size() > 1)
    return emitOpError("rule currently supports at most one output");
  if (getName().empty() || getStableId().empty())
    return emitOpError(
        "requires non-empty definition and stable instance names");
  if (getTimeDomain() != "cycle")
    return emitOpError("phase-one rule requires exact time domain 'cycle'");
  auto model = (*this)->getParentOfType<mlir::ModuleOp>();
  auto modelKind =
      model ? model->getAttrOfType<StringAttr>("ac.model_kind") : StringAttr();
  if (modelKind && modelKind.getValue() == "queue_graph") {
    auto graphDomain =
        model->getAttrOfType<StringAttr>("ac.queue_graph_domain");
    if (!graphDomain || graphDomain.getValue() != getTimeDomain())
      return emitOpError("rule domain must match the exact QueueGraph domain");
  }
  if (getTypeState() != TypeConstraintState::Exact)
    return emitOpError("phase-one frontend rule requires an exact Queue type");
  for (StringRef name : {"ac.rule.effects", "ac.rule.checks",
                         "ac.rule.handshake", "ac.rule.guard",
                         "ac.rule.schedule"})
    if ((*this)->hasAttr(name))
      return emitOpError() << "legacy rule summary attribute '" << name
                           << "' is not part of canonical ACIR";
  ArrayRef<int64_t> depths = getOutputDepthsAttr().asArrayRef();
  ArrayRef<int64_t> latencies = getOutputLatenciesAttr().asArrayRef();
  if (depths.size() != getOutputs().size() ||
      llvm::any_of(depths, [](int64_t value) { return value <= 0; }))
    return emitOpError("output depths must match results and be positive");
  if (latencies.size() != getOutputs().size() ||
      llvm::any_of(latencies, [](int64_t value) { return value <= 0; }))
    return emitOpError("output latencies must match results and be positive");

  Block &block = getBody().front();
  if (block.getNumArguments() != getInputs().size())
    return emitOpError("body argument count must match input Queue count");
  for (auto [input, argument] :
       llvm::zip_equal(getInputs(), block.getArguments())) {
    auto queue = cast<QueueType>(input.getType());
    Type expected = VarType::get(getContext(), queue.getElementType());
    if (argument.getType() != expected)
      return emitOpError("body arguments must match input Queue payloads");
  }
  SmallVector<TableProposeOp> proposals;
  SmallVector<TableGetOp> tableReads;
  unsigned conditions = 0;
  Value conditionValue;
  for (Operation &operation : block.without_terminator())
    if (auto proposal = dyn_cast<TableProposeOp>(operation)) {
      proposals.push_back(proposal);
    } else if (auto get = dyn_cast<TableGetOp>(operation)) {
      tableReads.push_back(get);
    } else if (auto condition = dyn_cast<RuleConditionOp>(operation)) {
      ++conditions;
      conditionValue = condition.getCondition();
    } else if (!isMemoryEffectFree(&operation) &&
               !isa<TypeConstraintMarkerOp, ValueFactMarkerOp,
                    PendingObligationMarkerOp, VarAssignOp, VarAssignElementOp>(
                   operation) &&
               !isa<VarMatchOp, VarChooseOp, TableMatchOp, TableChooseOp>(
                   operation) &&
               !isa<RuleOutputOp, StateSnapshotOp, StateSnapshotSetOp>(
                   operation))
      return emitOpError() << "body operation '" << operation.getName()
                           << "' must be pure in the phase-one rule subset";
  if (conditions > 1)
    return emitOpError("permits at most one functional condition");
  SmallVector<RuleOutputOp> outputPaths;
  getBody().walk([&](RuleOutputOp output) { outputPaths.push_back(output); });
  const bool hasPathEvidence =
      !outputPaths.empty() || llvm::any_of(proposals, [](TableProposeOp op) {
        return static_cast<bool>(op.getWhen());
      });
  if (hasPathEvidence) {
    if (conditions != 1)
      return emitOpError("SSA path evidence requires one rule condition");
    if (outputPaths.size() != getOutputs().size())
      return emitOpError("requires one SSA presence record per output");
    llvm::SmallDenseSet<int64_t> ordinals;
    for (RuleOutputOp output : outputPaths) {
      if (!ordinals.insert(output.getOrdinal()).second)
        return output.emitOpError(
            "output presence must uniquely name one rule result");
      if (!presenceImpliesCandidate(output.getWhen(), conditionValue) ||
          (output.getWhen() != conditionValue &&
           (getInputs().size() != 1 ||
            constantVarBool(conditionValue) != true)))
        return output.emitOpError(
            "optional output presence requires one input and a true candidate");
    }
    SmallVector<Value> divergentPresences;
    for (TableProposeOp proposal : proposals) {
      if (!proposal.getWhen() ||
          !presenceImpliesCandidate(proposal.getWhen(), conditionValue))
        return proposal.emitOpError(
            "state proposal presence must imply the rule condition");
      if (proposal.getWhen() != conditionValue) {
        if (getInputs().size() != 1)
          return proposal.emitOpError(
              "conditional-effect presence requires one input");
        if (!llvm::is_contained(divergentPresences, proposal.getWhen())) {
          if (divergentPresences.size() >= 2 ||
              (!divergentPresences.empty() &&
               !areBooleanComplements(divergentPresences.front(),
                                      proposal.getWhen())))
            return proposal.emitOpError(
                "conditional-effect presences must share one predicate or "
                "one complementary pair");
          divergentPresences.push_back(proposal.getWhen());
        }
      }
    }
  }
  llvm::StringSet<> proposalOwners;
  for (TableProposeOp proposal : proposals)
    if (!proposalOwners.insert(proposal.getTable()).second)
      return proposal.emitOpError(
          "first multi-state rule slice permits one proposal per owner");
  if (proposals.empty() && !tableReads.empty())
    return emitOpError("Table observation requires a stateful Table proposal");
  for (TableGetOp read : tableReads)
    if (TableOp table = resolveTable(read, read.getTableAttr());
        !table || failed(verifyStaticallySafeRuleTableIndex(read, table,
                                                            read.getIndex()))) {
      return failure();
    }
  if (getInputs().empty() && getOutputs().empty() && proposals.empty())
    return emitOpError("rule without Queue endpoints must update state");
  auto yield = dyn_cast<RuleReturnOp>(block.getTerminator());
  if (!yield || yield.getValues().size() != getOutputs().size())
    return emitOpError("body return count must match output Queue count");
  for (auto [value, output] :
       llvm::zip_equal(yield.getValues(), getOutputs())) {
    auto outputQueue = cast<QueueType>(output.getType());
    Type expectedOutput =
        VarType::get(getContext(), outputQueue.getElementType());
    if (value.getType() != expectedOutput)
      return emitOpError("returned payload must match output Queue type");
  }
  return success();
}

static LogicalResult verifyI1VarCondition(Operation *operation,
                                          Value condition) {
  auto variable = dyn_cast<VarType>(condition.getType());
  if (!variable || !variable.getElementType().isInteger(1))
    return operation->emitOpError("condition must be !ac.var<i1>");
  return success();
}

LogicalResult RuleConditionOp::verify() {
  return verifyI1VarCondition(*this, getCondition());
}

LogicalResult FiringConditionOp::verify() {
  return verifyI1VarCondition(*this, getCondition());
}

LogicalResult RuleOutputOp::verify() {
  if (failed(verifyI1VarCondition(*this, getWhen())))
    return failure();
  RuleOp rule = (*this)->getParentOfType<RuleOp>();
  if (!rule || getOrdinal() < 0 ||
      static_cast<size_t>(getOrdinal()) >= rule.getOutputs().size())
    return emitOpError("ordinal must name one rule output");
  auto queue = cast<QueueType>(rule.getOutputs()[getOrdinal()].getType());
  if (getValue().getType() !=
      VarType::get(getContext(), queue.getElementType()))
    return emitOpError("value must match the selected rule output payload");
  auto returned =
      dyn_cast<RuleReturnOp>(rule.getBody().front().getTerminator());
  if (!returned)
    return emitOpError("requires ac.rule.return");
  Value returnedValue = returned.getValues()[getOrdinal()];
  if (returnedValue != getValue()) {
    auto obligation = returnedValue.getDefiningOp<PendingObligationMarkerOp>();
    if (!obligation || obligation.getInput() != getValue())
      return emitOpError("value must be the matching ac.rule.return payload");
  }
  return success();
}

LogicalResult FiringOutputOp::verify() {
  if (failed(verifyI1VarCondition(*this, getWhen())))
    return failure();
  FiringOp firing = (*this)->getParentOfType<FiringOp>();
  if (!firing || getOrdinal() < 0 ||
      static_cast<size_t>(getOrdinal()) >= firing.getOutputs().size())
    return emitOpError("ordinal must name one firing output");
  auto queue = cast<QueueType>(firing.getOutputs()[getOrdinal()].getType());
  if (getValue().getType() !=
      VarType::get(getContext(), queue.getElementType()))
    return emitOpError("value must match the selected firing output payload");
  auto yielded =
      dyn_cast<FiringYieldOp>(firing.getBody().front().getTerminator());
  if (!yielded || yielded.getValues()[getOrdinal()] != getValue())
    return emitOpError("value must be the matching ac.firing.yield operand");
  return success();
}

LogicalResult StateSnapshotOp::verify() {
  if (!isa_and_nonnull<RuleOp, FiringOp>((*this)->getParentOp()))
    return emitOpError("must be nested directly in ac.rule or ac.firing");
  if (failed(verifyI1VarCondition(*this, getPredicate())))
    return failure();
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the snapshot scope ancestry");
  if (table.getEntries() > 64)
    return emitOpError("state snapshot supports at most 64 entries");
  if (failed(verifyTableFields(*this, table, getReadFields(), "read")))
    return failure();
  if (!tableWriteFieldsAreComplete(*this, table, getReadFields())) {
    FailureOr<uint64_t> fieldCount = tableEntryFieldCount(*this, table);
    if (failed(fieldCount) ||
        static_cast<uint64_t>(table.getEntries()) * *fieldCount > 64)
      return emitOpError(
          "field-qualified snapshot exceeds the 64-bit entry/field relation");
  }
  if (getIndexKind() == RuleIndexKind::All) {
    if (getIndex())
      return emitOpError("all-entry snapshot must not carry an index");
    return success();
  }
  if (!getIndex())
    return emitOpError("indexed snapshot requires an index");
  if (failed(verifyStaticallySafeRuleTableIndex(*this, table, getIndex())))
    return failure();
  const bool isStatic =
      static_cast<bool>(getIndex().getDefiningOp<VarConstantOp>());
  if (isStatic != (getIndexKind() == RuleIndexKind::Static))
    return emitOpError("index_kind must match the snapshot index definition");
  return success();
}

LogicalResult StateSnapshotSetOp::verify() {
  if (!isa_and_nonnull<RuleOp, FiringOp>((*this)->getParentOp()))
    return emitOpError("must be nested directly in ac.rule or ac.firing");
  if (failed(verifyI1VarCondition(*this, getPredicate())))
    return failure();
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the snapshot-set scope ancestry");
  if (table.getEntries() > 64)
    return emitOpError("snapshot-set target supports at most 64 entries");
  if (failed(verifyTableFields(*this, table, getReadFields(), "read")))
    return failure();
  if (!tableWriteFieldsAreComplete(*this, table, getReadFields())) {
    FailureOr<uint64_t> fieldCount = tableEntryFieldCount(*this, table);
    if (failed(fieldCount) ||
        static_cast<uint64_t>(table.getEntries()) * *fieldCount > 64)
      return emitOpError(
          "field-qualified snapshot-set exceeds the 64-bit entry/field "
          "relation");
  }
  Region *sourceRegion = nullptr;
  if (auto match = getSource().getDefiningOp<TableMatchOp>()) {
    if (match->getParentOp() != (*this)->getParentOp())
      return emitOpError(
          "source table.match must belong to the owning rule/firing");
    sourceRegion = &match.getPredicate();
  } else if (auto choose = getSource().getDefiningOp<TableChooseOp>()) {
    if (getSource() != choose.getIndex() ||
        choose->getParentOp() != (*this)->getParentOp())
      return emitOpError(
          "source table.choose must use the owning rule/firing's index result");
    sourceRegion = &choose.getKey();
  } else {
    return emitOpError(
        "source must be an owning rule/firing table.match mask or "
        "table.choose index");
  }
  bool foundTarget = false;
  sourceRegion->walk([&](TableGetOp read) {
    foundTarget |= resolveTable(read, read.getTableAttr()) == table;
  });
  if (!foundTarget)
    return emitOpError(
        "source evaluation must contain a region-local read of the target "
        "table");
  return success();
}

LogicalResult TypeConstraintMarkerOp::verify() {
  if (getState() == TypeConstraintState::Exact)
    return emitOpError("exact facts must not remain marker-wrapped");
  return success();
}

LogicalResult ValueFactMarkerOp::verify() {
  if (getIdentity().empty() || getPathPredicate().empty())
    return emitOpError("requires non-empty identity and path predicate");
  return success();
}

LogicalResult PendingObligationMarkerOp::verify() {
  if (getOrigin().empty() || getPathPredicate().empty())
    return emitOpError("requires non-empty origin and path predicate");
  return success();
}

LogicalResult SourceOp::verify() {
  if (getDepth() <= 0)
    return emitOpError("depth must be positive");
  if (getLatency() <= 0)
    return emitOpError("latency must be positive");
  return success();
}

LogicalResult ObserveOp::verify() {
  if (getName().empty())
    return emitOpError("name must be non-empty");
  return success();
}

LogicalResult ExpectOp::verify() {
  if (getMessage().empty())
    return emitOpError("message must be non-empty");
  Block &block = getPredicate().front();
  Type payload = cast<QueueType>(getInput().getType()).getElementType();
  Type expected = VarType::get(getContext(), payload);
  if (block.getNumArguments() != 1 ||
      block.getArgument(0).getType() != expected)
    return emitOpError("predicate argument must match queue payload Var");
  for (Operation &operation : block.without_terminator())
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "predicate operation '" << operation.getName()
                           << "' must be pure";
  auto yield = dyn_cast<ExpectYieldOp>(block.getTerminator());
  if (!yield || !cast<VarType>(yield.getCondition().getType())
                     .getElementType()
                     .isInteger(1))
    return emitOpError("predicate must terminate with an i1 ac.expect.yield");
  return success();
}

LogicalResult BroadcastOp::verify() {
  if (getOutputs().size() < 2)
    return emitOpError("requires at least two output queues");
  for (auto [index, output] : llvm::enumerate(getOutputs()))
    if (output.getType() != getInput().getType())
      return emitOpError() << "output queue " << index
                           << " must match input queue type";
  ArrayRef<int64_t> depths = getOutputDepthsAttr().asArrayRef();
  ArrayRef<int64_t> latencies = getOutputLatenciesAttr().asArrayRef();
  if (depths.size() != getOutputs().size())
    return emitOpError("output depth count must match result count");
  if (latencies.size() != getOutputs().size())
    return emitOpError("output latency count must match result count");
  if (llvm::any_of(depths, [](int64_t value) { return value <= 0; }))
    return emitOpError("output depths must be positive");
  if (llvm::any_of(latencies, [](int64_t value) { return value <= 0; }))
    return emitOpError("output latencies must be positive");
  return success();
}

LogicalResult ForkOp::verify() {
  if (getOutputs().size() < 2)
    return emitOpError("requires at least two output queues");
  for (auto [index, output] : llvm::enumerate(getOutputs()))
    if (output.getType() != getInput().getType())
      return emitOpError() << "output queue " << index
                           << " must match input queue type";
  ArrayRef<int64_t> depths = getOutputDepthsAttr().asArrayRef();
  ArrayRef<int64_t> latencies = getOutputLatenciesAttr().asArrayRef();
  if (depths.size() != getOutputs().size() ||
      llvm::any_of(depths, [](int64_t value) { return value <= 0; }))
    return emitOpError("output depths must match results and be positive");
  if (latencies.size() != getOutputs().size() ||
      llvm::any_of(latencies, [](int64_t value) { return value <= 0; }))
    return emitOpError("output latencies must match results and be positive");
  return success();
}

LogicalResult RouteOp::verify() {
  if (getOutputs().size() < 2)
    return emitOpError("requires at least two output queues");
  for (auto [index, output] : llvm::enumerate(getOutputs()))
    if (output.getType() != getInput().getType())
      return emitOpError() << "output queue " << index
                           << " must match input queue type";
  ArrayRef<int64_t> depths = getOutputDepthsAttr().asArrayRef();
  ArrayRef<int64_t> latencies = getOutputLatenciesAttr().asArrayRef();
  if (depths.size() != getOutputs().size() ||
      llvm::any_of(depths, [](int64_t value) { return value <= 0; }))
    return emitOpError("output depths must match results and be positive");
  if (latencies.size() != getOutputs().size() ||
      llvm::any_of(latencies, [](int64_t value) { return value <= 0; }))
    return emitOpError("output latencies must match results and be positive");
  Block &block = getSelector().front();
  if (block.getNumArguments() != 1 ||
      block.getArgument(0).getType() !=
          VarType::get(getContext(),
                       cast<QueueType>(getInput().getType()).getElementType()))
    return emitOpError("selector argument must match input payload Var");
  auto yield = dyn_cast<RouteYieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("selector must terminate with ac.route.yield");
  Type selector = cast<VarType>(yield.getSelector().getType()).getElementType();
  if (!isa<IntegerType, EnumType>(selector))
    return emitOpError("selector must be an integer or enum Var");
  return success();
}

LogicalResult SelectOp::verify() {
  if (getInputs().size() < 3)
    return emitOpError("requires one control and at least two data queues");
  llvm::DenseSet<Value> uniqueInputs;
  for (Value input : getInputs())
    if (!uniqueInputs.insert(input).second)
      return emitOpError("input queue operands must be unique");
  for (auto [index, input] : llvm::enumerate(getInputs().drop_front()))
    if (input.getType() != getOutput().getType())
      return emitOpError() << "data input queue " << index
                           << " must match output queue type";
  if (getDepth() <= 0 || getLatency() <= 0)
    return emitOpError("depth and latency must be positive");

  Block &block = getKey().front();
  Type controlPayload =
      cast<QueueType>(getInputs().front().getType()).getElementType();
  Type expected = VarType::get(getContext(), controlPayload);
  if (block.getNumArguments() != 1 ||
      block.getArgument(0).getType() != expected)
    return emitOpError("key argument must match control queue payload Var");
  for (Operation &operation : block.without_terminator())
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "key operation '" << operation.getName()
                           << "' must be pure";
  auto yield = dyn_cast<SelectYieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("key must terminate with ac.select.yield");
  Type selector = cast<VarType>(yield.getSelector().getType()).getElementType();
  auto integer = dyn_cast<IntegerType>(selector);
  if (!integer || integer.getWidth() > 64)
    return emitOpError("key must yield an integer Var with width at most 64");
  const uint64_t candidates = getInputs().size() - 1;
  if (integer.getWidth() < 64 &&
      candidates > (uint64_t{1} << integer.getWidth()))
    return emitOpError("data input count must fit selector width");
  return success();
}

LogicalResult MergeOp::verify() {
  if (getInputs().size() < 2)
    return emitOpError("requires at least two input queues");
  for (auto [index, input] : llvm::enumerate(getInputs()))
    if (input.getType() != getOutput().getType())
      return emitOpError() << "input queue " << index
                           << " must match output queue type";
  if (getPolicy() != "round_robin" && getPolicy() != "priority")
    return emitOpError("policy must be 'round_robin' or 'priority'");
  if (getDepth() <= 0 || getLatency() <= 0)
    return emitOpError("depth and latency must be positive");
  return success();
}

LogicalResult BarrierOp::verify() {
  if (getInputs().size() < 2)
    return emitOpError("requires at least two input queues");
  if (getOutputs().size() != getInputs().size())
    return emitOpError("output count must match input count");
  llvm::DenseSet<Value> uniqueInputs;
  for (auto [index, input] : llvm::enumerate(getInputs())) {
    if (!uniqueInputs.insert(input).second)
      return emitOpError("input queue operands must be unique");
    if (getOutputs()[index].getType() != input.getType())
      return emitOpError() << "output queue " << index
                           << " must match its input queue type";
  }
  ArrayRef<int64_t> depths = getOutputDepthsAttr().asArrayRef();
  ArrayRef<int64_t> latencies = getOutputLatenciesAttr().asArrayRef();
  if (depths.size() != getOutputs().size() ||
      llvm::any_of(depths, [](int64_t value) { return value <= 0; }))
    return emitOpError("output depths must match results and be positive");
  if (latencies.size() != getOutputs().size() ||
      llvm::any_of(latencies, [](int64_t value) { return value <= 0; }))
    return emitOpError("output latencies must match results and be positive");
  return success();
}

LogicalResult ReorderOp::verify() {
  if (getInput().getType() != getOutput().getType())
    return emitOpError("output queue must match input queue type");
  if (getCapacity() <= 0 || getDepth() <= 0 || getLatency() <= 0)
    return emitOpError("capacity, depth, and latency must be positive");
  if (getStart() < 0)
    return emitOpError("start must be non-negative");

  Block &block = getKey().front();
  Type payload = cast<QueueType>(getInput().getType()).getElementType();
  Type expected = VarType::get(getContext(), payload);
  if (block.getNumArguments() != 1 ||
      block.getArgument(0).getType() != expected)
    return emitOpError("key argument must match queue payload Var");
  for (Operation &operation : block.without_terminator())
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "key operation '" << operation.getName()
                           << "' must be pure";
  auto yield = dyn_cast<ReorderYieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("key must terminate with ac.reorder.yield");
  auto key = cast<VarType>(yield.getKey().getType()).getElementType();
  auto integer = dyn_cast<IntegerType>(key);
  if (!integer || integer.getWidth() > 64)
    return emitOpError("key must be an integer Var with width at most 64");
  if (integer.getWidth() < 64 &&
      static_cast<uint64_t>(getStart()) >= (uint64_t{1} << integer.getWidth()))
    return emitOpError("start must fit key width");
  return success();
}

LogicalResult DependencyOp::verify() {
  if (getInput().getType() != getOutput().getType())
    return emitOpError("output queue must match input queue type");
  if (getCapacity() <= 0 || getResources() <= 0 || getDepth() <= 0 ||
      getLatency() <= 0)
    return emitOpError(
        "capacity, resources, depth, and latency must be positive");
  if (getNoDependency() < 0)
    return emitOpError("no_dependency must be non-negative");

  Type payload = cast<QueueType>(getInput().getType()).getElementType();
  Type argumentType = VarType::get(getContext(), payload);
  auto verifyPolicy = [&](Region &region,
                          StringRef name) -> FailureOr<IntegerType> {
    Block &block = region.front();
    if (block.getNumArguments() != 1 ||
        block.getArgument(0).getType() != argumentType) {
      emitOpError() << name << " argument must match queue payload Var";
      return failure();
    }
    for (Operation &operation : block.without_terminator())
      if (!isMemoryEffectFree(&operation)) {
        emitOpError() << name << " operation '" << operation.getName()
                      << "' must be pure";
        return failure();
      }
    auto yield = dyn_cast<DependencyYieldOp>(block.getTerminator());
    if (!yield) {
      emitOpError() << name << " must terminate with ac.dependency.yield";
      return failure();
    }
    auto value = cast<VarType>(yield.getValue().getType()).getElementType();
    auto integer = dyn_cast<IntegerType>(value);
    if (!integer || integer.getWidth() > 64) {
      emitOpError() << name
                    << " must yield an integer Var with width at most 64";
      return failure();
    }
    return integer;
  };

  FailureOr<IntegerType> key = verifyPolicy(getKey(), "key");
  FailureOr<IntegerType> dependency = verifyPolicy(getWaitsFor(), "waits_for");
  FailureOr<IntegerType> resource = verifyPolicy(getResource(), "resource");
  FailureOr<IntegerType> cost = verifyPolicy(getCost(), "cost");
  if (failed(key) || failed(dependency) || failed(resource) || failed(cost))
    return failure();
  if (*key != *dependency)
    return emitOpError("key and waits_for must use the same integer Var type");
  if (dependency->getWidth() < 64 &&
      static_cast<uint64_t>(getNoDependency()) >=
          (uint64_t{1} << dependency->getWidth()))
    return emitOpError("no_dependency must fit dependency width");
  if (resource->getWidth() < 64 && static_cast<uint64_t>(getResources()) >
                                       (uint64_t{1} << resource->getWidth()))
    return emitOpError("resources must fit resource width");
  return success();
}

LogicalResult CreditOp::verify() {
  if (getInput().getType() != getOutput().getType())
    return emitOpError("output queue must match input queue type");
  if (getCredits() <= 0 || getDepth() <= 0 || getLatency() <= 0)
    return emitOpError("credits, depth, and latency must be positive");

  Block &block = getCost().front();
  Type payload = cast<QueueType>(getInput().getType()).getElementType();
  Type expected = VarType::get(getContext(), payload);
  if (block.getNumArguments() != 1 ||
      block.getArgument(0).getType() != expected)
    return emitOpError("cost argument must match queue payload Var");
  for (Operation &operation : block.without_terminator())
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "cost operation '" << operation.getName()
                           << "' must be pure";
  auto yield = dyn_cast<CreditYieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("cost must terminate with ac.credit.yield");
  Type cost = cast<VarType>(yield.getCost().getType()).getElementType();
  auto integer = dyn_cast<IntegerType>(cost);
  if (!integer || integer.getWidth() > 64)
    return emitOpError("cost must yield an integer Var with width at most 64");
  return success();
}

LogicalResult FeedbackOp::verify() {
  if (getInput().getType() != getOutput().getType())
    return emitOpError("output queue must match input queue type");
  if (getDepth() <= 0 || getLatency() <= 0 || getMaxIterations() <= 0)
    return emitOpError("depth, latency, and max_iterations must be positive");

  Block &block = getBody().front();
  Type payload = cast<QueueType>(getInput().getType()).getElementType();
  Type expectedValue = VarType::get(getContext(), payload);
  if (block.getNumArguments() != 1 ||
      block.getArgument(0).getType() != expectedValue)
    return emitOpError("body argument must match queue payload Var");
  for (Operation &operation : block.without_terminator())
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "body operation '" << operation.getName()
                           << "' must be pure";
  auto yield = dyn_cast<FeedbackYieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("body must terminate with ac.feedback.yield");
  if (yield.getValue().getType() != expectedValue)
    return emitOpError("yielded value must match queue payload Var");
  if (yield.getContinueValue().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), 1)))
    return emitOpError("continue value must be !ac.var<i1>");
  return success();
}

LogicalResult ScopeOp::verify() {
  Block &block = getBody().front();
  if (block.getNumArguments() != getInputs().size())
    return emitOpError("body argument count must match input queue count");
  for (size_t index = 0; index < getInputs().size(); ++index)
    if (block.getArgument(index).getType() != getInputs()[index].getType())
      return emitOpError() << "body argument " << index
                           << " must match input queue type";
  auto yield = dyn_cast<ScopeYieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("body must terminate with ac.scope.yield");
  if (yield.getQueues().size() != getOutputs().size())
    return emitOpError("yielded queue count must match result count");
  for (size_t index = 0; index < getOutputs().size(); ++index)
    if (yield.getQueues()[index].getType() != getOutputs()[index].getType())
      return emitOpError() << "yielded queue " << index
                           << " must match result type";
  return success();
}

LogicalResult FiringOp::verify() {
  if (getOutputs().size() > 1)
    return emitOpError("currently supports at most one output Queue");
  if (getOutputDepthsAttr().size() != getOutputs().size() ||
      getOutputLatenciesAttr().size() != getOutputs().size())
    return emitOpError("output depth/latency counts must match results");
  if (llvm::any_of(getOutputDepthsAttr().asArrayRef(),
                   [](int64_t value) { return value <= 0; }) ||
      llvm::any_of(getOutputLatenciesAttr().asArrayRef(),
                   [](int64_t value) { return value <= 0; }))
    return emitOpError("output depths and latencies must be positive");
  if (getStableId().empty())
    return emitOpError("requires explicit stable identity");
  for (StringRef name : {"functional_guard", "checks", "handshake",
                         "schedule", "effects"})
    if ((*this)->hasAttr(name))
      return emitOpError() << "legacy firing summary attribute '" << name
                           << "' is not part of canonical ACIR";
  if (getTimeDomain() != "cycle")
    return emitOpError("phase-one firing requires exact time domain 'cycle'");
  auto model = (*this)->getParentOfType<mlir::ModuleOp>();
  auto modelKind =
      model ? model->getAttrOfType<StringAttr>("ac.model_kind") : StringAttr();
  if (modelKind && modelKind.getValue() == "queue_graph") {
    auto graphDomain =
        model->getAttrOfType<StringAttr>("ac.queue_graph_domain");
    if (!graphDomain || graphDomain.getValue() != getTimeDomain())
      return emitOpError(
          "firing domain must match the exact QueueGraph domain");
  }
  SmallVector<TableProposeOp> proposals;
  SmallVector<TableGetOp> tableReads;
  SmallVector<FiringConditionOp> conditions;
  getBody().walk(
      [&](TableProposeOp proposal) { proposals.push_back(proposal); });
  getBody().walk([&](TableGetOp read) { tableReads.push_back(read); });
  getBody().walk(
      [&](FiringConditionOp condition) { conditions.push_back(condition); });
  for (TableGetOp read : tableReads)
    if (TableOp table = resolveTable(read, read.getTableAttr());
        !table || failed(verifyStaticallySafeRuleTableIndex(read, table,
                                                            read.getIndex()))) {
      return failure();
    }
  if (getInputs().empty() && getOutputs().empty() && proposals.empty())
    return emitOpError("firing without Queue endpoints must update state");
  if (conditions.size() > 1)
    return emitOpError("permits at most one functional condition");
  SmallVector<FiringOutputOp> outputPaths;
  getBody().walk([&](FiringOutputOp output) { outputPaths.push_back(output); });
  for (FiringOutputOp output : outputPaths)
    if (output.getOrdinal() < 0 ||
        static_cast<size_t>(output.getOrdinal()) >= getOutputs().size())
      return output.emitOpError("ordinal must name one firing output");
  const bool hasPathEvidence =
      !outputPaths.empty() || llvm::any_of(proposals, [](TableProposeOp op) {
        return static_cast<bool>(op.getWhen());
      });
  if (hasPathEvidence) {
    if (conditions.size() != 1)
      return emitOpError("SSA path evidence requires one firing condition");
    if (outputPaths.size() != getOutputs().size())
      return emitOpError("requires one SSA presence record per output");
    Value condition = conditions.front().getCondition();
    llvm::SmallDenseSet<int64_t> ordinals;
    for (FiringOutputOp output : outputPaths) {
      if (!ordinals.insert(output.getOrdinal()).second)
        return output.emitOpError(
            "output presence must uniquely name one firing result");
      if (!presenceImpliesCandidate(output.getWhen(), condition) ||
          (output.getWhen() != condition &&
           (getInputs().size() != 1 || constantVarBool(condition) != true)))
        return output.emitOpError(
            "optional output presence requires one input and a true candidate");
    }
    SmallVector<Value> divergentPresences;
    for (TableProposeOp proposal : proposals) {
      if (!proposal.getWhen() ||
          !presenceImpliesCandidate(proposal.getWhen(), condition))
        return proposal.emitOpError(
            "state proposal presence must imply the firing condition");
      if (proposal.getWhen() != condition) {
        if (getInputs().size() != 1)
          return proposal.emitOpError(
              "conditional-effect presence requires one input");
        if (!llvm::is_contained(divergentPresences, proposal.getWhen())) {
          if (divergentPresences.size() >= 2 ||
              (!divergentPresences.empty() &&
               !areBooleanComplements(divergentPresences.front(),
                                      proposal.getWhen())))
            return proposal.emitOpError(
                "conditional-effect presences must share one predicate or "
                "one complementary pair");
          divergentPresences.push_back(proposal.getWhen());
        }
      }
    }
  }
  llvm::StringSet<> proposalOwners;
  for (TableProposeOp proposal : proposals)
    if (!proposalOwners.insert(proposal.getTable()).second)
      return proposal.emitOpError(
          "first multi-state firing slice permits one proposal per owner");
  auto priority = (*this)->getAttrOfType<IntegerAttr>("ac.rule_priority");
  auto footprints = (*this)->getAttrOfType<ArrayAttr>("ac.rule_footprints");
  const bool requiresInferredSchedule =
      modelKind && modelKind.getValue() == "queue_graph";
  if ((requiresInferredSchedule && (!priority || !footprints)) ||
      static_cast<bool>(priority) != static_cast<bool>(footprints) ||
      (priority && priority.getInt() < 0))
    return emitOpError(
        "requires inferred non-negative priority and typed footprints");
  if (footprints) {
    SmallVector<Operation *> stateOperations;
    getBody().walk([&](Operation *operation) {
      if (isa<TableGetOp, TableMatchOp, TableChooseOp, TableProposeOp>(
              operation))
        stateOperations.push_back(operation);
    });
    if (footprints.size() != stateOperations.size())
      return emitOpError(
          "inferred footprint count must match state operations");
    for (auto [rawFootprint, operation] :
         llvm::zip_equal(footprints, stateOperations)) {
      auto footprint = dyn_cast<DictionaryAttr>(rawFootprint);
      auto access =
          footprint ? footprint.getAs<StringAttr>("access") : StringAttr();
      auto resource = footprint ? footprint.getAs<FlatSymbolRefAttr>("resource")
                                : FlatSymbolRefAttr();
      auto indexKind =
          footprint ? footprint.getAs<StringAttr>("index_kind") : StringAttr();
      auto footprintGuard =
          footprint ? footprint.getAs<RuleGuardKindAttr>("guard_kind")
                    : RuleGuardKindAttr();
      if (!access || !resource || !indexKind || !footprintGuard ||
          (indexKind.getValue() != "static" &&
           indexKind.getValue() != "dynamic" && indexKind.getValue() != "all"))
        return emitOpError("has malformed inferred state footprint");
      Value index;
      StringRef expectedAccess;
      FlatSymbolRefAttr expectedResource;
      ArrayAttr expectedFields;
      RuleGuardKind expectedFootprintGuard = RuleGuardKind::Always;
      if (auto read = dyn_cast<TableGetOp>(operation)) {
        index = read.getIndex();
        expectedAccess = "read";
        expectedResource = read.getTableAttr();
      } else if (auto match = dyn_cast<TableMatchOp>(operation)) {
        expectedAccess = "read";
        expectedResource = match.getTableAttr();
      } else if (auto choose = dyn_cast<TableChooseOp>(operation)) {
        expectedAccess = "read";
        expectedResource = choose.getTableAttr();
      } else {
        auto proposal = cast<TableProposeOp>(operation);
        index = proposal.getIndex();
        expectedAccess = proposal.getMode();
        expectedResource = proposal.getTableAttr();
        expectedFields = proposal.getWriteFieldsAttr();
        expectedFootprintGuard =
            proposal.getWhen()
                ? guardKindFor(proposal.getWhen())
                : (conditions.empty()
                       ? RuleGuardKind::Always
                       : guardKindFor(conditions.front().getCondition()));
      }
      StringRef expectedIndexKind =
          !index
              ? "all"
              : (index.getDefiningOp<VarConstantOp>() ? "static" : "dynamic");
      auto fields = footprint.getAs<ArrayAttr>("fields");
      if (access.getValue() != expectedAccess || resource != expectedResource ||
          indexKind.getValue() != expectedIndexKind ||
          fields != expectedFields ||
          footprintGuard.getValue() != expectedFootprintGuard)
        return emitOpError(
            "inferred footprint must exactly match its state operation");
    }
  }
  const bool validArity =
      getOutputs().size() <= 1 &&
      (!getInputs().empty() || !getOutputs().empty() || !proposals.empty());
  if (conditions.empty() && requiresInferredSchedule) {
    return emitOpError("requires one typed functional condition");
  }
  if (!validArity)
    return emitOpError("has invalid phase-one Queue/state arity");

  Block &block = getBody().front();
  if (block.getNumArguments() != getInputs().size())
    return emitOpError("body argument count must match input Queue count");
  for (auto [input, argument] :
       llvm::zip_equal(getInputs(), block.getArguments())) {
    auto queue = cast<QueueType>(input.getType());
    Type expected = VarType::get(getContext(), queue.getElementType());
    if (argument.getType() != expected)
      return emitOpError("body arguments must match input Queue payloads");
  }
  for (Operation &operation : block.without_terminator())
    if (!isMemoryEffectFree(&operation) &&
        !isa<TableGetOp, TableProposeOp, TableMatchOp, TableChooseOp,
             VarAssignOp, FiringConditionOp, FiringOutputOp, StateSnapshotOp,
             StateSnapshotSetOp>(operation))
      return emitOpError() << "body operation '" << operation.getName()
                           << "' must be pure after marker elimination";
  auto yield = dyn_cast<FiringYieldOp>(block.getTerminator());
  if (!yield || yield.getValues().size() != getOutputs().size())
    return emitOpError("body must yield one payload per output Queue");
  for (auto [output, value] :
       llvm::zip_equal(getOutputs(), yield.getValues())) {
    auto queue = cast<QueueType>(output.getType());
    Type expected = VarType::get(getContext(), queue.getElementType());
    if (value.getType() != expected)
      return emitOpError("yielded values must match output Queue payloads");
  }
  if (failed(verifyTypedRuleSummary(getOperation(), getInputs(), getOutputs(),
                                    getBody(), footprints, priority, "ac.")))
    return failure();
  return verifyActivationEvidence(getOperation(), getInputs(), getOutputs(),
                                  getBody(), true);
}

namespace detail {

ScopedProcessLivenessWorkCollector::ScopedProcessLivenessWorkCollector(
    ProcessLivenessWork &work)
    : previous(processLivenessWorkCollector) {
  work = {};
  processLivenessWorkCollector = &work;
}

ScopedProcessLivenessWorkCollector::~ScopedProcessLivenessWorkCollector() {
  processLivenessWorkCollector = previous;
}

} // namespace detail

namespace {

struct NamedRef {
  SymbolRefAttr name;
  StringRef opName;
};

std::optional<NamedRef> namedRef(Type type) {
  return TypeSwitch<Type, std::optional<NamedRef>>(type)
      .Case<StructType>([](auto type) {
        return NamedRef{type.getName(), StructOp::getOperationName()};
      })
      .Case<PacketType>([](auto type) {
        return NamedRef{type.getName(), PacketOp::getOperationName()};
      })
      .Case<TransactionType>([](auto type) {
        return NamedRef{type.getName(), TransactionOp::getOperationName()};
      })
      .Case<EnumType>([](auto type) {
        return NamedRef{type.getName(), EnumOp::getOperationName()};
      })
      .Default([](Type) { return std::nullopt; });
}

Operation *lookup(Operation *from, SymbolRefAttr name) {
  if (name.getNestedReferences().size() != 1)
    return SymbolTable::lookupNearestSymbolFrom(from, name);
  Operation *scope = nullptr;
  if (auto enclosing = from->getParentOfType<TypeScopeOp>();
      enclosing && enclosing.getSymNameAttr() == name.getRootReference())
    scope = enclosing;
  if (!scope) {
    auto root = FlatSymbolRefAttr::get(name.getRootReference());
    scope = SymbolTable::lookupNearestSymbolFrom(from, root);
    if (!scope)
      if (auto module = from->getParentOfType<mlir::ModuleOp>())
        scope = SymbolTable::lookupSymbolIn(module, root);
  }
  if (!isa_and_nonnull<TypeScopeOp>(scope))
    return nullptr;
  return SymbolTable::lookupSymbolIn(scope, name.getLeafReference());
}

LogicalResult requireQualified(Operation *from, SymbolRefAttr name) {
  if (name.getNestedReferences().size() == 1)
    return success();
  return from->emitOpError(
      "named data references require a qualified symbol such as "
      "'@types::@S'");
}

LogicalResult verifyNamedTypes(Operation *from, Type type) {
  LogicalResult result = success();
  type.walk([&](Type nested) {
    auto ref = namedRef(nested);
    if (!ref)
      return WalkResult::advance();
    if (failed(requireQualified(from, ref->name))) {
      result = failure();
      return WalkResult::interrupt();
    }
    Operation *decl = lookup(from, ref->name);
    if (!decl) {
      from->emitOpError() << "unresolved named data type '" << ref->name << "'";
      result = failure();
      return WalkResult::interrupt();
    }
    if (decl->getName().getStringRef() != ref->opName) {
      from->emitOpError() << "named type '" << ref->name << "' requires "
                          << ref->opName << " but resolves to "
                          << decl->getName();
      result = failure();
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return result;
}

LogicalResult verifyPlacement(Operation *op) {
  if (isa_and_nonnull<TypeScopeOp>(op->getParentOp()))
    return success();
  return op->emitOpError(
      "named data declarations must be direct children of ac.type_scope");
}

FailureOr<DictionaryAttr> fieldDictionary(Operation *op, Attribute field) {
  auto dictionary = dyn_cast<DictionaryAttr>(field);
  if (!dictionary || !dictionary.getAs<StringAttr>("name") ||
      !dictionary.getAs<TypeAttr>("type")) {
    op->emitOpError("field metadata requires string 'name' and type 'type'");
    return failure();
  }
  return dictionary;
}

StringRef fieldName(DictionaryAttr field) {
  return field.getAs<StringAttr>("name").getValue();
}

Type fieldType(DictionaryAttr field) {
  return field.getAs<TypeAttr>("type").getValue();
}

bool isNormativeValueType(Type type) {
  if (isa<IntegerType, FloatType, IndexType, StructType, PacketType,
          TransactionType, EnumType>(type))
    return true;
  if (auto vector = dyn_cast<mlir::VectorType>(type))
    return isNormativeValueType(vector.getElementType());
  if (auto array = dyn_cast<ValueArrayType>(type))
    return isNormativeValueType(array.getElementType());
  if (auto tuple = dyn_cast<mlir::TupleType>(type))
    return llvm::all_of(tuple.getTypes(), isNormativeValueType);
  return false;
}

bool isProtocolPayloadType(Type type) {
  return isNormativeValueType(type) && !containsChannelType(type);
}

bool isTopologyLeaf(Type type) {
  return isa<FlowType, EndpointType, ResourceRefType, ChannelType,
             ResourceTokenType>(type);
}

Type findNestedTopologyLeaf(Type type) {
  if (isTopologyLeaf(type))
    return {};
  Type found;
  type.walk([&](Type nested) {
    if (!isTopologyLeaf(nested))
      return WalkResult::advance();
    found = nested;
    return WalkResult::interrupt();
  });
  return found;
}

template <typename OpTy>
OpTy lookupChild(Operation *container, FlatSymbolRefAttr name) {
  return dyn_cast_or_null<OpTy>(SymbolTable::lookupSymbolIn(container, name));
}

ProtocolOp lookupProtocol(Operation *from, FlatSymbolRefAttr name) {
  auto module = from->getParentOfType<mlir::ModuleOp>();
  return module ? dyn_cast_or_null<ProtocolOp>(
                      SymbolTable::lookupSymbolIn(module, name))
                : ProtocolOp();
}

bool isCarrierAction(StringRef action) {
  return action == "offer" || action == "response" || action == "notify";
}

bool matchesCarrierEvent(ProtocolOp protocol, Type payload,
                         FlatSymbolRefAttr from = {},
                         FlatSymbolRefAttr to = {}) {
  return llvm::any_of(protocol.getBody().getOps<EventOp>(), [&](EventOp event) {
    return isCarrierAction(event.getAction()) &&
           event.getPayload() == payload &&
           (!from || event.getFromAttr() == from) &&
           (!to || event.getToAttr() == to);
  });
}

LogicalResult verifyRoleReference(Operation *from, Operation *container,
                                  FlatSymbolRefAttr name, StringRef subject) {
  if (lookupChild<RoleOp>(container, name))
    return success();
  return from->emitOpError()
         << "unresolved " << subject << " role '@" << name.getValue() << "'";
}

LogicalResult verifyRoleContainer(Operation *container) {
  llvm::SmallDenseSet<StringRef> names;
  for (RoleOp role : container->getRegion(0).getOps<RoleOp>())
    if (!names.insert(role.getSymName()).second)
      return role.emitOpError()
             << "redefinition of symbol named '" << role.getSymName() << "'";
  for (RoleOp role : container->getRegion(0).getOps<RoleOp>()) {
    if (role.getCardinality() != "exclusive" &&
        role.getCardinality() != "shared")
      return role.emitOpError() << "unsupported role cardinality '"
                                << role.getCardinality() << "'";
    RoleOp dual = lookupChild<RoleOp>(container, role.getDualAttr());
    if (!dual)
      return role.emitOpError() << "unresolved dual role '@"
                                << role.getDualAttr().getValue() << "'";
    if (dual == role)
      return role.emitOpError("role cannot be its own dual");
    if (dual.getDualAttr() !=
        FlatSymbolRefAttr::get(role.getContext(), role.getSymName()))
      return role.emitOpError("role duality must be symmetric");
    if (dual.getCardinality() != role.getCardinality())
      return role.emitOpError("dual roles must have matching cardinality");
  }
  return success();
}

bool hasStringValue(StringRef value, ArrayRef<StringRef> accepted) {
  return llvm::is_contained(accepted, value);
}

GuaranteeOp findGuarantee(ProtocolOp protocol, StringRef kind) {
  for (GuaranteeOp guarantee : protocol.getBody().getOps<GuaranteeOp>())
    if (guarantee.getKind() == kind)
      return guarantee;
  return {};
}

LogicalResult verifyStringGuarantee(GuaranteeOp op,
                                    ArrayRef<StringRef> accepted) {
  auto value = dyn_cast<StringAttr>(op.getValue());
  if (!value || !hasStringValue(value.getValue(), accepted))
    return op.emitOpError()
           << "unsupported " << op.getKind() << " value '"
           << (value ? value.getValue() : StringRef("<non-string>")) << "'";
  return success();
}

bool isAllowedGuardExpression(Operation *operation) {
  return llvm::StringSwitch<bool>(operation->getName().getStringRef())
      .Cases({"arith.constant",
              "arith.cmpi",
              "arith.cmpf",
              "arith.addi",
              "arith.subi",
              "arith.muli",
              "arith.divui",
              "arith.divsi",
              "arith.remui",
              "arith.remsi",
              "arith.andi",
              "arith.ori",
              "arith.xori",
              "arith.shli",
              "arith.shrui",
              "arith.shrsi",
              "arith.select",
              "arith.index_cast",
              "arith.extui",
              "arith.extsi",
              "arith.trunci",
              "arith.addf",
              "arith.subf",
              "arith.mulf",
              "arith.divf",
              "arith.negf",
              "index.constant",
              "index.add",
              "index.sub",
              "index.mul",
              "index.divs",
              "index.divu",
              "index.rems",
              "index.remu",
              "index.cmp",
              "index.casts",
              "index.castu"},
             true)
      .Default(false);
}

LogicalResult verifyFields(Operation *op, ArrayAttr fields) {
  llvm::SmallDenseSet<StringRef> seen;
  for (Attribute attribute : fields) {
    FailureOr<DictionaryAttr> field = fieldDictionary(op, attribute);
    if (failed(field))
      return failure();
    StringRef name = fieldName(*field);
    Type type = fieldType(*field);
    if (!seen.insert(name).second)
      return op->emitOpError() << "duplicate field '" << name << "'";
    if (!isNormativeValueType(type))
      return op->emitOpError()
             << "field '" << name << "' has non-value type " << type;
    if (failed(verifyNamedTypes(op, type)))
      return failure();
    if (field->get("max_length")) {
      return op->emitOpError()
             << "field '" << name << "' cannot declare removed max_length";
    }
  }
  return success();
}

ArrayAttr declarationFields(Operation *op) {
  return op->getAttrOfType<ArrayAttr>("fields");
}

Operation *recordDecl(Operation *from, Type type) {
  auto ref = namedRef(type);
  if (!ref || (ref->opName != StructOp::getOperationName() &&
               ref->opName != PacketOp::getOperationName() &&
               ref->opName != TransactionOp::getOperationName()))
    return nullptr;
  if (failed(requireQualified(from, ref->name)))
    return nullptr;
  Operation *decl = lookup(from, ref->name);
  return decl && decl->getName().getStringRef() == ref->opName ? decl : nullptr;
}

std::optional<unsigned> findField(Operation *decl, StringRef name) {
  for (auto [index, attribute] : llvm::enumerate(declarationFields(decl))) {
    auto field = cast<DictionaryAttr>(attribute);
    if (fieldName(field) == name)
      return index;
  }
  return std::nullopt;
}

Type fieldType(Operation *decl, unsigned index) {
  return fieldType(cast<DictionaryAttr>(declarationFields(decl)[index]));
}

SmallVector<NamedRef> directValueReferences(Type type) {
  if (auto ref = namedRef(type))
    return {*ref};
  if (auto vector = dyn_cast<mlir::VectorType>(type))
    return directValueReferences(vector.getElementType());
  if (auto array = dyn_cast<ValueArrayType>(type))
    return directValueReferences(array.getElementType());
  if (auto tuple = dyn_cast<mlir::TupleType>(type)) {
    SmallVector<NamedRef> result;
    for (Type element : tuple.getTypes())
      llvm::append_range(result, directValueReferences(element));
    return result;
  }
  return {};
}

LogicalResult verifyNoRecursion(Operation *root) {
  auto rootName =
      root->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
  llvm::SmallDenseSet<Operation *> active;
  std::function<LogicalResult(Operation *)> visit =
      [&](Operation *current) -> LogicalResult {
    if (!active.insert(current).second) {
      root->emitOpError() << "unbounded value recursion through '@"
                          << rootName.getValue() << "'";
      return failure();
    }
    if (ArrayAttr fields = declarationFields(current)) {
      for (Attribute attribute : fields) {
        Type type = fieldType(cast<DictionaryAttr>(attribute));
        for (NamedRef ref : directValueReferences(type)) {
          Operation *next = lookup(root, ref.name);
          if (next && declarationFields(next) && failed(visit(next)))
            return failure();
        }
      }
    }
    active.erase(current);
    return success();
  };
  return visit(root);
}

LogicalResult verifyRecordDeclaration(Operation *op, ArrayAttr fields) {
  if (failed(verifyPlacement(op)) || failed(verifyFields(op, fields)))
    return failure();
  return verifyNoRecursion(op);
}

SymbolRefAttr declarationReference(Operation *declaration) {
  auto scope = cast<TypeScopeOp>(declaration->getParentOp());
  auto leaf = FlatSymbolRefAttr::get(
      declaration->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()));
  return SymbolRefAttr::get(declaration->getContext(), scope.getSymName(),
                            ArrayRef<FlatSymbolRefAttr>{leaf});
}

Type declarationType(Operation *declaration) {
  SymbolRefAttr reference = declarationReference(declaration);
  return TypeSwitch<Operation *, Type>(declaration)
      .Case<StructOp>([&](auto) {
        return StructType::get(declaration->getContext(), reference);
      })
      .Case<PacketOp>([&](auto) {
        return PacketType::get(declaration->getContext(), reference);
      })
      .Case<EnumOp>([&](auto) {
        return EnumType::get(declaration->getContext(), reference);
      })
      .Default([](Operation *) { return Type(); });
}

FailureOr<DictionaryAttr> queryLayout(TypeScopeOp scope, Type type) {
  DataLayoutSpecInterface spec = scope.getDataLayoutSpec();
  if (!spec)
    return failure();
  FailureOr<Attribute> value = spec.query(DataLayoutEntryKey(type));
  if (failed(value))
    return failure();
  auto dictionary = dyn_cast<DictionaryAttr>(*value);
  if (!dictionary)
    return failure();
  return dictionary;
}

LogicalResult verifyDeclarationLayout(Operation *declaration) {
  Type type = declarationType(declaration);
  auto scope = cast<TypeScopeOp>(declaration->getParentOp());
  if (succeeded(queryLayout(scope, type)))
    return success();
  return declaration->emitOpError() << "missing DLTI layout entry for " << type;
}

LogicalResult verifyUniqueEnumerants(EnumOp op) {
  llvm::SmallDenseSet<StringRef> seen;
  for (Attribute value : op.getEnumerants()) {
    StringRef text = cast<StringAttr>(value).getValue();
    if (!seen.insert(text).second)
      return op.emitOpError() << "duplicate enumerant '" << text << "'";
  }
  return success();
}

std::string bitfieldFingerprint(int64_t width, ArrayAttr fields) {
  std::string preimage;
  llvm::raw_string_ostream stream(preimage);
  stream << R"({"kind":"bitfield","version":1,"width":)" << width
         << R"(,"fields":[)";
  for (auto [index, attribute] : llvm::enumerate(fields)) {
    DictionaryAttr field = cast<DictionaryAttr>(attribute);
    if (index)
      stream << ',';
    stream << '[' << llvm::json::Value(cast<StringAttr>(field.get("name"))
                                           .getValue())
           << ',' << cast<IntegerAttr>(field.get("msb")).getInt() << ','
           << cast<IntegerAttr>(field.get("lsb")).getInt() << ']';
  }
  stream << "]}";
  llvm::SHA256 sha;
  sha.update(stream.str());
  return "sha256:" + llvm::toHex(sha.final(), /*LowerCase=*/true);
}

} // namespace

DataLayoutSpecInterface TypeScopeOp::getDataLayoutSpec() {
  return getOperation()->getAttrOfType<DataLayoutSpecInterface>(
      DLTIDialect::kDataLayoutAttrName);
}

TargetSystemSpecInterface TypeScopeOp::getTargetSystemSpec() {
  return getOperation()->getAttrOfType<TargetSystemSpecInterface>(
      DLTIDialect::kTargetSystemDescAttrName);
}

LogicalResult TypeAliasOp::verify() {
  if (failed(verifyPlacement(*this)))
    return failure();
  return verifyNamedTypes(*this, getTarget());
}

LogicalResult StructOp::verify() {
  if (failed(verifyRecordDeclaration(*this, getFields())))
    return failure();
  return verifyDeclarationLayout(*this);
}

LogicalResult BitfieldOp::verify() {
  if (failed(verifyPlacement(*this)))
    return failure();
  if (getWidth() <= 0 || getWidth() > 64)
    return emitOpError("width must be in [1, 64]");
  if (getFields().empty())
    return emitOpError("requires at least one field");

  llvm::SmallDenseSet<StringRef> names;
  StringRef previous;
  for (Attribute attribute : getFields()) {
    auto field = dyn_cast<DictionaryAttr>(attribute);
    if (!field || field.size() != 3)
      return emitOpError(
          "fields must contain exact {name, msb, lsb} records");
    auto name = field.getAs<StringAttr>("name");
    auto msb = field.getAs<IntegerAttr>("msb");
    auto lsb = field.getAs<IntegerAttr>("lsb");
    if (!name || name.getValue().empty() || !msb || !lsb)
      return emitOpError(
          "fields must contain non-empty name and integer msb/lsb");
    if (!names.insert(name.getValue()).second)
      return emitOpError() << "duplicate field '" << name.getValue() << "'";
    if (!previous.empty() && previous >= name.getValue())
      return emitOpError("fields must be sorted by UTF-8 name");
    previous = name.getValue();
    if (lsb.getInt() < 0 || msb.getInt() < lsb.getInt() ||
        msb.getInt() >= getWidth())
      return emitOpError() << "field '" << name.getValue()
                           << "' range must satisfy 0 <= lsb <= msb < width";
  }
  if (getFingerprint() != bitfieldFingerprint(getWidth(), getFields()))
    return emitOpError("fingerprint does not match canonical schema");
  return success();
}

LogicalResult TransactionOp::verify() {
  return verifyRecordDeclaration(*this, getFields());
}

LogicalResult PacketOp::verify() {
  if (failed(verifyRecordDeclaration(*this, getFields())))
    return failure();
  return verifyDeclarationLayout(*this);
}

LogicalResult EnumOp::verify() {
  if (failed(verifyPlacement(*this)) || failed(verifyUniqueEnumerants(*this)))
    return failure();
  return verifyDeclarationLayout(*this);
}

LogicalResult VarConstantOp::verify() {
  auto result = cast<VarType>(getResult().getType());
  auto value = dyn_cast<TypedAttr>(getValue());
  if (!value || value.getType() != result.getElementType())
    return emitOpError("attribute type must match Var element type");
  return success();
}

LogicalResult VarEnumOp::verify() {
  auto declaration = dyn_cast_or_null<EnumOp>(lookup(*this, getDeclaration()));
  if (!declaration)
    return emitOpError("declaration must resolve to ac.enum");
  auto result = dyn_cast<EnumType>(cast<VarType>(getResult().getType())
                                       .getElementType());
  if (!result || result.getName() != getDeclaration())
    return emitOpError("result must carry the referenced nominal enum type");
  if (!llvm::any_of(declaration.getEnumerants(), [&](Attribute value) {
        return cast<StringAttr>(value).getValue() == getEnumerant();
      }))
    return emitOpError() << "unknown enumerant '" << getEnumerant() << "'";
  return success();
}

LogicalResult VarTupleOp::verify() {
  auto tuple = dyn_cast<mlir::TupleType>(
      cast<VarType>(getResult().getType()).getElementType());
  if (!tuple || tuple.size() == 0 || tuple.size() != getValues().size())
    return emitOpError("result tuple must match the non-empty operand list");
  for (auto [value, type] : llvm::zip_equal(getValues(), tuple.getTypes()))
    if (value.getType() != VarType::get(getContext(), type))
      return emitOpError("tuple operand types must match result elements");
  return success();
}

LogicalResult VarArrayOp::verify() {
  auto array = dyn_cast<ValueArrayType>(
      cast<VarType>(getResult().getType()).getElementType());
  if (!array || array.getLength() <= 0 ||
      static_cast<size_t>(array.getLength()) != getValues().size())
    return emitOpError("result value_array length must match operands");
  Type expected = VarType::get(getContext(), array.getElementType());
  if (llvm::any_of(getValues(),
                   [&](Value value) { return value.getType() != expected; }))
    return emitOpError("value_array operands must match its element type");
  return success();
}

LogicalResult VarElementOp::verify() {
  Type aggregate = cast<VarType>(getAggregate().getType()).getElementType();
  Type expected;
  int64_t length = 0;
  if (auto tuple = dyn_cast<mlir::TupleType>(aggregate)) {
    length = tuple.size();
    if (getIndex() >= 0 && getIndex() < length)
      expected = tuple.getType(getIndex());
  } else if (auto array = dyn_cast<ValueArrayType>(aggregate)) {
    length = array.getLength();
    if (getIndex() >= 0 && getIndex() < length)
      expected = array.getElementType();
  } else {
    return emitOpError("aggregate must be tuple or value_array");
  }
  if (getIndex() < 0 || getIndex() >= length)
    return emitOpError("aggregate index is out of range");
  if (getResult().getType() != VarType::get(getContext(), expected))
    return emitOpError("result must match the selected aggregate element");
  return success();
}

static VarDeclOp resolveVarDecl(Operation *operation,
                                FlatSymbolRefAttr reference) {
  for (Operation *ancestor = operation->getParentOp(); ancestor;
       ancestor = ancestor->getParentOp()) {
    if (ancestor->getNumRegions() != 1 || !ancestor->getRegion(0).hasOneBlock())
      continue;
    for (VarDeclOp variable :
         ancestor->getRegion(0).front().getOps<VarDeclOp>())
      if (variable.getSymName() == reference.getValue())
        return variable;
  }
  return {};
}

LogicalResult VarDeclOp::verify() {
  if (!isImmutablePayloadType(getValueType()))
    return emitOpError("value type must be an immutable ACIR payload type");
  if (auto shape = getShapeAttr()) {
    ArrayRef<int64_t> dimensions = shape.asArrayRef();
    if (dimensions.size() != 1 || dimensions.front() <= 0)
      return emitOpError(
          "persistent ac.var shape must be one positive dimension");
  }
  auto init = dyn_cast<TypedAttr>(getInit());
  const auto zero = dyn_cast<IntegerAttr>(getInit());
  const bool zeroImage = zero && zero.getValue().isZero();
  if ((!init || init.getType() != getValueType()) &&
      !(isa<StructType>(getValueType()) && zeroImage))
    return emitOpError(
        "init must match value type or be the zero image for a struct");
  if (getOwner().empty() || !getOwner().starts_with('/') ||
      (getOwner().size() > 1 && getOwner().ends_with('/')))
    return emitOpError("owner must be a canonical absolute scope path");
  std::string expected = "var/";
  if (getOwner() != "/") {
    expected.append(getOwner().drop_front());
    expected.push_back('/');
  }
  expected.append(getSymName());
  if (getStableId() != expected)
    return emitOpError("stable_id must match canonical owner/symbol identity");
  return success();
}

LogicalResult VarReadOp::verify() {
  VarDeclOp variable = resolveVarDecl(*this, getVariableAttr());
  if (!variable)
    return emitOpError() << "unresolved ac.var " << getVariable();
  if (variable.getShapeAttr())
    return emitOpError("shaped ac.var requires ac.var.read_element");
  Type expected = VarType::get(getContext(), variable.getValueType());
  if (getResult().getType() != expected)
    return emitOpError("result must match declared ac.var value type");
  return success();
}

static LogicalResult verifyVarElementAccess(Operation *operation,
                                            FlatSymbolRefAttr variableRef,
                                            Value index, Type valueType) {
  VarDeclOp variable = resolveVarDecl(operation, variableRef);
  if (!variable)
    return operation->emitOpError()
           << "unresolved ac.var " << variableRef.getValue();
  auto shape = variable.getShapeAttr();
  if (!shape || shape.asArrayRef().size() != 1)
    return operation->emitOpError("requires a one-dimensional shaped ac.var");
  auto indexVar = dyn_cast<VarType>(index.getType());
  auto indexType = indexVar ? dyn_cast<IntegerType>(indexVar.getElementType())
                            : IntegerType();
  if (!indexType)
    return operation->emitOpError("index must be an integer ac.var");
  const int64_t entries = shape.asArrayRef().front();
  if (auto constant = index.getDefiningOp<VarConstantOp>()) {
    auto value = dyn_cast<IntegerAttr>(constant.getValue());
    if (!value || value.getValue().isNegative() ||
        value.getValue().uge(static_cast<uint64_t>(entries)))
      return operation->emitOpError("constant element index is out of range");
    Type expected =
        VarType::get(operation->getContext(), variable.getValueType());
    if (valueType != expected)
      return operation->emitOpError(
          "element value must match declared ac.var element type");
    return success();
  }
  Type expected =
      VarType::get(operation->getContext(), variable.getValueType());
  if (valueType != expected)
    return operation->emitOpError(
        "element value must match declared ac.var element type");
  return success();
}

LogicalResult VarReadElementOp::verify() {
  return verifyVarElementAccess(*this, getVariableAttr(), getIndex(),
                                getResult().getType());
}

LogicalResult VarAssignOp::verify() {
  if (!isa_and_nonnull<RuleOp, FiringOp>((*this)->getParentOp()))
    return emitOpError("must be nested directly in ac.rule or ac.firing");
  VarDeclOp variable = resolveVarDecl(*this, getVariableAttr());
  if (!variable)
    return emitOpError() << "unresolved ac.var " << getVariable();
  if (variable.getShapeAttr())
    return emitOpError("shaped ac.var requires ac.var.assign_element");
  Type expected = VarType::get(getContext(), variable.getValueType());
  if (getValue().getType() != expected)
    return emitOpError("assigned value must match declared ac.var value type");
  if (getWhen() && failed(verifyI1VarCondition(*this, getWhen())))
    return failure();
  return success();
}

LogicalResult VarAssignElementOp::verify() {
  if (!isa_and_nonnull<RuleOp, FiringOp>((*this)->getParentOp()))
    return emitOpError("must be nested directly in ac.rule or ac.firing");
  if (failed(verifyVarElementAccess(*this, getVariableAttr(), getIndex(),
                                    getValue().getType())))
    return failure();
  if (getWhen() && failed(verifyI1VarCondition(*this, getWhen())))
    return failure();
  return success();
}

static FailureOr<std::pair<VarDeclOp, int64_t>>
resolveVarCollection(Operation *operation, FlatSymbolRefAttr variableRef) {
  VarDeclOp variable = resolveVarDecl(operation, variableRef);
  if (!variable) {
    operation->emitOpError() << "unresolved ac.var " << variableRef.getValue();
    return failure();
  }
  auto shape = variable.getShapeAttr();
  if (!shape || shape.asArrayRef().size() != 1) {
    operation->emitOpError("requires a one-dimensional shaped ac.var");
    return failure();
  }
  return std::make_pair(variable, shape.asArrayRef().front());
}

LogicalResult VarMatchOp::verify() {
  auto collection = resolveVarCollection(*this, getVariableAttr());
  if (failed(collection))
    return failure();
  VarDeclOp variable = collection->first;
  const int64_t entries = collection->second;
  if (entries <= 0 || entries > 64)
    return emitOpError("match domain must contain 1..64 elements");
  if (getMask().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), entries)))
    return emitOpError("mask width must equal the ac.var domain");
  if (!getPredicate().hasOneBlock())
    return emitOpError("predicate must contain exactly one block");
  Block &block = getPredicate().front();
  Type element = VarType::get(getContext(), variable.getValueType());
  if (block.getNumArguments() != 1 || block.getArgument(0).getType() != element)
    return emitOpError("predicate argument must match the ac.var element");
  for (Operation &operation : block.without_terminator())
    if (!isa<SlotGetOp, VarReadOp, VarReadElementOp>(operation) &&
        !isMemoryEffectFree(&operation))
      return emitOpError() << "predicate operation '" << operation.getName()
                           << "' is not permitted";
  auto yield = dyn_cast<VarMatchYieldOp>(block.getTerminator());
  if (!yield ||
      !cast<VarType>(yield.getValue().getType()).getElementType().isInteger(1))
    return emitOpError("predicate must yield !ac.var<i1>");
  return success();
}

LogicalResult VarChooseOp::verify() {
  auto collection = resolveVarCollection(*this, getVariableAttr());
  if (failed(collection))
    return failure();
  VarDeclOp variable = collection->first;
  const int64_t entries = collection->second;
  if (entries <= 0 || entries > 64)
    return emitOpError("choose domain must contain 1..64 elements");
  auto mask = dyn_cast<IntegerType>(
      cast<VarType>(getMask().getType()).getElementType());
  if (!mask || mask.getWidth() != static_cast<unsigned>(entries))
    return emitOpError("candidate mask width must equal the ac.var domain");
  auto match = getMask().getDefiningOp<VarMatchOp>();
  if (!match)
    return emitOpError(
        "candidate mask must be produced directly by ac.var.match");
  if (resolveVarDecl(match, match.getVariableAttr()) != variable)
    return emitOpError("candidate mask must come from the same ac.var");
  if (getCount() != 1)
    return emitOpError("choose supports count=1 only");
  if (getPolicy() != "first" && getPolicy() != "min" && getPolicy() != "max")
    return emitOpError("policy must be first, min, or max");
  unsigned indexWidth =
      std::max<unsigned>(1, llvm::Log2_64_Ceil(static_cast<uint64_t>(entries)));
  if (getIndex().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), indexWidth)))
    return emitOpError("index result width must address the ac.var domain");
  if (getValid().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), 1)))
    return emitOpError("valid result must be !ac.var<i1>");
  if (getPolicy() == "first") {
    if (!getKey().empty() &&
        !(getKey().hasOneBlock() && getKey().front().empty()))
      return emitOpError("first policy does not accept a key region");
    return success();
  }
  if (!getKey().hasOneBlock())
    return emitOpError("min/max policy requires one key region");
  Block &block = getKey().front();
  Type element = VarType::get(getContext(), variable.getValueType());
  if (block.getNumArguments() != 1 || block.getArgument(0).getType() != element)
    return emitOpError("key argument must match the ac.var element");
  for (Operation &operation : block.without_terminator()) {
    if (isa<VarReadOp, VarReadElementOp>(operation))
      return emitOpError() << "key operation '" << operation.getName()
                           << "' is not permitted";
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "key operation '" << operation.getName()
                           << "' is not permitted";
  }
  auto yield = dyn_cast<VarChooseYieldOp>(block.getTerminator());
  auto key =
      yield ? dyn_cast<IntegerType>(
                  cast<VarType>(yield.getValue().getType()).getElementType())
            : IntegerType();
  if (!key || key.getWidth() == 0 || key.getWidth() > 64)
    return emitOpError(
        "min/max key must yield an unsigned fixed-width integer");
  return success();
}

static LogicalResult verifyVarBinary(Operation *operation, Value lhs, Value rhs,
                                     Value result) {
  if (lhs.getType() != rhs.getType() || lhs.getType() != result.getType())
    return operation->emitOpError(
        "operands and result must have one identical Var type");
  Type element = cast<VarType>(result.getType()).getElementType();
  if (!isa<IntegerType, FloatType>(element))
    return operation->emitOpError(
        "arithmetic Var element must be an integer or float");
  return success();
}

LogicalResult VarAddOp::verify() {
  return verifyVarBinary(*this, getLhs(), getRhs(), getResult());
}

LogicalResult VarSubOp::verify() {
  return verifyVarBinary(*this, getLhs(), getRhs(), getResult());
}

LogicalResult VarMulOp::verify() {
  return verifyVarBinary(*this, getLhs(), getRhs(), getResult());
}

static LogicalResult verifyVarBitBinary(Operation *operation, Value lhs,
                                        Value rhs, Value result) {
  if (lhs.getType() != rhs.getType() || lhs.getType() != result.getType())
    return operation->emitOpError(
        "operands and result must have one identical Var type");
  auto element =
      dyn_cast<IntegerType>(cast<VarType>(result.getType()).getElementType());
  if (!element)
    return operation->emitOpError(
        "bit operation Var element must be an integer");
  if (!element.isSignless() || element.getWidth() == 0 ||
      element.getWidth() > 64)
    return operation->emitOpError(
        "bit operation Var element must be a signless integer with width in "
        "[1, 64]");
  return success();
}

LogicalResult VarAndOp::verify() {
  return verifyVarBitBinary(*this, getLhs(), getRhs(), getResult());
}

LogicalResult VarOrOp::verify() {
  return verifyVarBitBinary(*this, getLhs(), getRhs(), getResult());
}

LogicalResult VarXorOp::verify() {
  return verifyVarBitBinary(*this, getLhs(), getRhs(), getResult());
}

LogicalResult VarShlOp::verify() {
  return verifyVarBitBinary(*this, getLhs(), getRhs(), getResult());
}

LogicalResult VarShrOp::verify() {
  return verifyVarBitBinary(*this, getLhs(), getRhs(), getResult());
}

LogicalResult VarMatchesOp::verify() {
  auto input = dyn_cast<IntegerType>(
      cast<VarType>(getInput().getType()).getElementType());
  if (!input || !input.isSignless() || input.getWidth() == 0 ||
      input.getWidth() > 64)
    return emitOpError(
        "input must be an ac.var carrying a signless i1..i64 integer");
  if (getResult().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), 1)))
    return emitOpError("result must be !ac.var<i1>");
  const uint64_t widthMask =
      input.getWidth() == 64
          ? std::numeric_limits<uint64_t>::max()
          : (uint64_t{1} << input.getWidth()) - 1;
  if ((getMask() & ~widthMask) != 0 || (getValue() & ~widthMask) != 0)
    return emitOpError("mask and value must fit the input width");
  if ((getValue() & ~getMask()) != 0)
    return emitOpError("value may set only bits selected by mask");
  return success();
}

LogicalResult VarNotOp::verify() {
  if (getIn().getType() != getResult().getType())
    return emitOpError("operand and result must have one identical Var type");
  auto element = dyn_cast<IntegerType>(
      cast<VarType>(getResult().getType()).getElementType());
  if (!element)
    return emitOpError("bit operation Var element must be an integer");
  if (!element.isSignless() || element.getWidth() == 0 ||
      element.getWidth() > 64)
    return emitOpError(
        "bit operation Var element must be a signless integer with width in "
        "[1, 64]");
  return success();
}

LogicalResult VarPopcountOp::verify() {
  auto input = cast<VarType>(getIn().getType());
  auto result = cast<VarType>(getResult().getType());
  auto inputInt = dyn_cast<IntegerType>(input.getElementType());
  auto resultInt = dyn_cast<IntegerType>(result.getElementType());
  if (!inputInt || !resultInt)
    return emitOpError("input and result payloads must be integer types");
  if (inputInt.getWidth() == 0 || inputInt.getWidth() > 64)
    return emitOpError("input width must be in [1, 64]");

  unsigned required = 0;
  for (unsigned width = inputInt.getWidth(); width != 0; width >>= 1)
    ++required;
  if (resultInt.getWidth() != required)
    return emitOpError()
           << "result width must be ceil(log2(input_width + 1)) = " << required;
  return success();
}

LogicalResult VarCountZerosOp::verify() {
  auto input = cast<VarType>(getIn().getType());
  auto result = cast<VarType>(getResult().getType());
  auto inputInt = dyn_cast<IntegerType>(input.getElementType());
  auto resultInt = dyn_cast<IntegerType>(result.getElementType());
  if (!inputInt || !resultInt)
    return emitOpError("input and result payloads must be integer types");
  if (inputInt.getWidth() == 0 || inputInt.getWidth() > 64)
    return emitOpError("input width must be in [1, 64]");

  unsigned required = 0;
  for (unsigned width = inputInt.getWidth(); width != 0; width >>= 1)
    ++required;
  if (resultInt.getWidth() != required)
    return emitOpError()
           << "result width must be ceil(log2(input_width + 1)) = " << required;
  if (getDirection() != "leading" && getDirection() != "trailing")
    return emitOpError("direction must be leading or trailing");
  return success();
}

LogicalResult VarPriorityEncodeOp::verify() {
  auto input = cast<VarType>(getIn().getType());
  auto index = cast<VarType>(getIndex().getType());
  auto valid = cast<VarType>(getValid().getType());
  auto inputInteger = dyn_cast<IntegerType>(input.getElementType());
  auto indexInteger = dyn_cast<IntegerType>(index.getElementType());
  if (!inputInteger || !inputInteger.isSignless() ||
      inputInteger.getWidth() == 0 || inputInteger.getWidth() > 64)
    return emitOpError("input must carry a signless integer width in [1, 64]");
  if (!indexInteger || !indexInteger.isSignless())
    return emitOpError("index must carry a signless integer");
  unsigned required = 1;
  for (unsigned extent = 2; extent < inputInteger.getWidth(); extent <<= 1)
    ++required;
  if (indexInteger.getWidth() != required)
    return emitOpError()
           << "index width must be max(1, ceil(log2(input_width))) = "
           << required;
  if (valid.getElementType() != IntegerType::get(getContext(), 1))
    return emitOpError("valid must be !ac.var<i1>");
  if (getOrder() != "low" && getOrder() != "high")
    return emitOpError("order must be low or high");
  return success();
}

LogicalResult VarCmpOp::verify() {
  if (getLhs().getType() != getRhs().getType())
    return emitOpError("operands must have the same Var type");
  Type payload = cast<VarType>(getLhs().getType()).getElementType();
  if (!isa<IntegerType, EnumType>(payload))
    return emitOpError("operands must carry integer or enum payloads");
  if (isa<EnumType>(payload) && getPredicate() != "eq" &&
      getPredicate() != "ne")
    return emitOpError("enum comparison supports only eq or ne");
  if (getResult().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), 1)))
    return emitOpError("result must be !ac.var<i1>");
  if (!llvm::is_contained(ArrayRef<StringRef>{"eq", "ne", "slt", "sle", "sgt",
                                              "sge", "ult", "ule", "ugt",
                                              "uge"},
                          getPredicate()))
    return emitOpError(
        "predicate must be eq, ne, slt, sle, sgt, sge, ult, ule, ugt, or uge");
  return success();
}

LogicalResult VarSelectOp::verify() {
  if (failed(verifyI1VarCondition(*this, getCondition())))
    return failure();
  if (getTrueValue().getType() != getFalseValue().getType() ||
      getResult().getType() != getTrueValue().getType())
    return emitOpError("selected values and result must have one Var type");
  return success();
}

static FailureOr<std::pair<int64_t, int64_t>>
bitfieldRange(BitfieldOp schema, StringRef name) {
  for (Attribute attribute : schema.getFields()) {
    DictionaryAttr field = cast<DictionaryAttr>(attribute);
    if (cast<StringAttr>(field.get("name")).getValue() == name)
      return std::make_pair(cast<IntegerAttr>(field.get("lsb")).getInt(),
                            cast<IntegerAttr>(field.get("msb")).getInt());
  }
  return failure();
}

static FailureOr<BitfieldOp>
bitfieldProvenance(Operation *operation, StringRef selectionAttribute) {
  Attribute schemaAttribute = operation->getAttr("ac.bitfield_schema");
  Attribute fingerprintAttribute =
      operation->getAttr("ac.bitfield_fingerprint");
  Attribute selection = operation->getAttr(selectionAttribute);
  if (!schemaAttribute && !fingerprintAttribute && !selection)
    return BitfieldOp();
  auto reference = dyn_cast_or_null<SymbolRefAttr>(schemaAttribute);
  auto fingerprint = dyn_cast_or_null<StringAttr>(fingerprintAttribute);
  if (!reference || !fingerprint || !selection) {
    operation->emitOpError(
        "bitfield provenance requires schema, fingerprint, and selection");
    return failure();
  }
  auto schema = dyn_cast_or_null<BitfieldOp>(lookup(operation, reference));
  if (!schema) {
    operation->emitOpError("bitfield schema reference does not resolve");
    return failure();
  }
  if (fingerprint.getValue() != schema.getFingerprint()) {
    operation->emitOpError("bitfield provenance fingerprint is stale");
    return failure();
  }
  return schema;
}

static LogicalResult verifyNamedBitfieldProvenance(Operation *operation,
                                                   unsigned baseWidth,
                                                   int64_t lsb, int64_t width) {
  FailureOr<BitfieldOp> resolved =
      bitfieldProvenance(operation, "ac.bitfield_field");
  if (failed(resolved))
    return failure();
  if (!*resolved)
    return success();
  auto field = operation->getAttrOfType<StringAttr>("ac.bitfield_field");
  if (!field)
    return operation->emitOpError("bitfield field must be a string");
  FailureOr<std::pair<int64_t, int64_t>> range =
      bitfieldRange(*resolved, field.getValue());
  if (failed(range))
    return operation->emitOpError("bitfield field is absent from its schema");
  if ((*resolved).getWidth() != baseWidth || range->first != lsb ||
      range->second - range->first + 1 != width)
    return operation->emitOpError(
        "bitfield field range does not match its schema");
  return success();
}

static LogicalResult verifyConcatBitfieldProvenance(VarConcatOp operation) {
  FailureOr<BitfieldOp> resolved = bitfieldProvenance(
      operation.getOperation(), "ac.bitfield_fields");
  if (failed(resolved))
    return failure();
  if (!*resolved)
    return success();
  auto fields = operation->getAttrOfType<ArrayAttr>("ac.bitfield_fields");
  if (!fields || fields.size() != operation.getInputs().size())
    return operation.emitOpError(
        "bitfield field list must match concat input arity");
  for (auto [attribute, input] :
       llvm::zip_equal(fields, operation.getInputs())) {
    auto field = dyn_cast<StringAttr>(attribute);
    if (!field)
      return operation.emitOpError("bitfield field names must be strings");
    FailureOr<std::pair<int64_t, int64_t>> range =
        bitfieldRange(*resolved, field.getValue());
    if (failed(range))
      return operation.emitOpError(
          "bitfield concat field is absent from its schema");
    unsigned inputWidth =
        cast<IntegerType>(cast<VarType>(input.getType()).getElementType())
            .getWidth();
    if (range->second - range->first + 1 != inputWidth)
      return operation.emitOpError(
          "bitfield concat input width does not match its field");
  }
  return success();
}

LogicalResult VarExtractOp::verify() {
  auto input = dyn_cast<IntegerType>(
      cast<VarType>(getInput().getType()).getElementType());
  auto result = dyn_cast<IntegerType>(
      cast<VarType>(getResult().getType()).getElementType());
  if (!input || !result || input.getWidth() == 0 || input.getWidth() > 64)
    return emitOpError("input and result must be 1..64-bit integer Vars");
  if (getLsb() < 0 || getWidth() <= 0 ||
      static_cast<uint64_t>(getLsb()) + static_cast<uint64_t>(getWidth()) >
          input.getWidth())
    return emitOpError("slice must be non-empty and within the input width");
  if (result.getWidth() != static_cast<unsigned>(getWidth()))
    return emitOpError("result width must equal the extracted width");
  return verifyNamedBitfieldProvenance(getOperation(), input.getWidth(),
                                       getLsb(), getWidth());
}

LogicalResult VarConcatOp::verify() {
  if (getInputs().empty())
    return emitOpError("requires at least one input");
  uint64_t totalWidth = 0;
  for (Value input : getInputs()) {
    auto integer =
        dyn_cast<IntegerType>(cast<VarType>(input.getType()).getElementType());
    if (!integer || integer.getWidth() == 0 || integer.getWidth() > 64)
      return emitOpError("inputs must be 1..64-bit integer Vars");
    totalWidth += integer.getWidth();
  }
  auto result = dyn_cast<IntegerType>(
      cast<VarType>(getResult().getType()).getElementType());
  if (!result || totalWidth == 0 || totalWidth > 64)
    return emitOpError("concatenated width must be in [1, 64]");
  if (result.getWidth() != totalWidth)
    return emitOpError("result width must equal the sum of input widths");
  return verifyConcatBitfieldProvenance(*this);
}

LogicalResult VarInsertOp::verify() {
  auto base = dyn_cast<IntegerType>(
      cast<VarType>(getBase().getType()).getElementType());
  auto value = dyn_cast<IntegerType>(
      cast<VarType>(getValue().getType()).getElementType());
  if (!base || !value || base.getWidth() == 0 || base.getWidth() > 64 ||
      value.getWidth() == 0 || value.getWidth() > 64)
    return emitOpError("base and value must be 1..64-bit integer Vars");
  if (getResult().getType() != getBase().getType())
    return emitOpError("result must preserve the base Var type");
  if (getLsb() < 0 ||
      static_cast<uint64_t>(getLsb()) + value.getWidth() > base.getWidth())
    return emitOpError("inserted range must be within the base width");
  return verifyNamedBitfieldProvenance(getOperation(), base.getWidth(), getLsb(),
                                       value.getWidth());
}

LogicalResult VarGetOp::verify() {
  auto record = cast<VarType>(getRecord().getType());
  Operation *decl = recordDecl(*this, record.getElementType());
  if (!decl)
    return emitOpError("requires a record-like Var operand");
  auto index = findField(decl, getField());
  if (!index)
    return emitOpError() << "unknown field '" << getField() << "'";
  Type expected = VarType::get(getContext(), fieldType(decl, *index));
  if (getResult().getType() != expected)
    return emitOpError() << "field '" << getField() << "' result must be "
                         << expected;
  return success();
}

LogicalResult VarWithOp::verify() {
  if (getRecord().getType() != getResult().getType())
    return emitOpError("must preserve record Var identity");
  auto record = cast<VarType>(getRecord().getType());
  Operation *decl = recordDecl(*this, record.getElementType());
  if (!decl)
    return emitOpError("requires a record-like Var operand");
  auto index = findField(decl, getField());
  if (!index)
    return emitOpError() << "unknown field '" << getField() << "'";
  Type expected = VarType::get(getContext(), fieldType(decl, *index));
  if (getValue().getType() != expected)
    return emitOpError() << "field '" << getField() << "' expects " << expected;
  return success();
}

static std::string queueScopePath(Operation *operation) {
  SmallVector<StringRef> parts;
  for (Operation *parent = operation->getParentOp(); parent;
       parent = parent->getParentOp())
    if (auto scope = dyn_cast<ScopeOp>(parent))
      parts.push_back(scope.getSymName());
  std::string path;
  for (StringRef part : llvm::reverse(parts)) {
    path.push_back('/');
    path.append(part);
  }
  return path.empty() ? "/" : path;
}

LogicalResult MemoryInstanceOp::verify() {
  auto data = dyn_cast<IntegerType>(getDataType());
  if (!data || data.getWidth() == 0 || data.getWidth() > 64)
    return emitOpError("data type must be an integer no wider than 64 bits");
  if (getEntries() <= 0 || getLatency() <= 0)
    return emitOpError("entries and latency must be positive");
  if (getInit() != 0)
    return emitOpError("memory init must be zero");
  if (getOwner().empty() || !getOwner().starts_with('/') ||
      (getOwner().size() > 1 && getOwner().ends_with('/')))
    return emitOpError("owner must be a canonical absolute scope path");
  if (getStableId().empty())
    return emitOpError("stable_id must be non-empty");

  Operation *root = getOperation();
  while (root->getParentOp())
    root = root->getParentOp();
  std::string expectedStableId = "memory/";
  if (getOwner() != "/") {
    expectedStableId.append(getOwner().drop_front());
    expectedStableId.push_back('/');
  }
  expectedStableId.append(getSymName());
  if (getStableId() != expectedStableId)
    return emitOpError("stable_id must match canonical owner/symbol identity");
  bool ownerExists = getOwner() == "/";
  bool duplicateStableId = false;
  root->walk([&](Operation *operation) {
    if (auto scope = dyn_cast<ScopeOp>(operation)) {
      std::string path = queueScopePath(scope);
      if (path != "/")
        path.push_back('/');
      path.append(scope.getSymName());
      ownerExists |= path == getOwner();
    }
    if (auto other = dyn_cast<MemoryInstanceOp>(operation))
      duplicateStableId |=
          other != *this && other.getStableId() == getStableId();
  });
  if (!ownerExists)
    return emitOpError("owner does not name a declared scope path");
  if (duplicateStableId)
    return emitOpError("stable_id must be unique");
  unsigned requests = 0;
  root->walk([&](MemoryRequestOp request) {
    auto resolved =
        dyn_cast_or_null<MemoryInstanceOp>(SymbolTable::lookupNearestSymbolFrom(
            request, request.getInstanceAttr()));
    if (resolved == *this)
      ++requests;
  });
  if (requests == 0)
    return emitOpError("must have at least one memory.request endpoint");
  return success();
}

LogicalResult MemoryRequestOp::verify() {
  if (getInput().getType() != getOutput().getType())
    return emitOpError("output queue must match input queue type");
  if (getOrdinal() < 0 || getDepth() <= 0)
    return emitOpError("ordinal must be non-negative and depth positive");

  auto instance = dyn_cast_or_null<MemoryInstanceOp>(
      SymbolTable::lookupNearestSymbolFrom(*this, getInstanceAttr()));
  if (!instance)
    return emitOpError() << "unresolved memory instance " << getInstance();
  const std::string requestScope = queueScopePath(*this);
  StringRef owner = instance.getOwner();
  StringRef requestPath(requestScope);
  const bool visible =
      owner == "/" || requestPath == owner ||
      (requestPath.size() > owner.size() && requestPath.starts_with(owner) &&
       requestPath[owner.size()] == '/');
  if (!visible)
    return emitOpError("memory instance is outside the request scope ancestry");
  auto endpointPath = (*this)->getAttrOfType<StringAttr>("ac.endpoint_path");
  auto endpointName = (*this)->getAttrOfType<StringAttr>("ac.name");
  if (!endpointPath || endpointPath.getValue().empty() || !endpointName ||
      endpointName.getValue().empty())
    return emitOpError("requires stable ac.endpoint_path and ac.name");
  std::string expectedEndpointPath = requestScope;
  if (expectedEndpointPath != "/")
    expectedEndpointPath.push_back('/');
  expectedEndpointPath.append(endpointName.getValue());
  if (endpointPath.getValue() != expectedEndpointPath)
    return emitOpError("ac.endpoint_path must match canonical scope/name path");

  Type payload = cast<QueueType>(getInput().getType()).getElementType();
  Operation *declaration = recordDecl(*this, payload);
  if (!declaration)
    return emitOpError("requires a record-like Queue payload");
  auto fieldIndex = findField(declaration, getResultField());
  if (!fieldIndex)
    return emitOpError() << "unknown result_field '" << getResultField() << "'";
  Type dataType = fieldType(declaration, *fieldIndex);
  if (dataType != instance.getDataType())
    return emitOpError(
        "result_field type must match memory instance data type");
  auto dataInteger = dyn_cast<IntegerType>(dataType);
  if (!dataInteger || dataInteger.getWidth() > 64)
    return emitOpError(
        "result_field must carry an integer no wider than 64 bits");

  Type argumentType = VarType::get(getContext(), payload);
  auto verifyPolicy = [&](Region &region, StringRef name) -> FailureOr<Type> {
    Block &block = region.front();
    if (block.getNumArguments() != 1 ||
        block.getArgument(0).getType() != argumentType) {
      emitOpError() << name << " argument must match queue payload Var";
      return failure();
    }
    for (Operation &operation : block.without_terminator())
      if (!isMemoryEffectFree(&operation)) {
        emitOpError() << name << " operation '" << operation.getName()
                      << "' must be pure";
        return failure();
      }
    auto yield = dyn_cast<MemoryYieldOp>(block.getTerminator());
    if (!yield) {
      emitOpError() << name << " must terminate with ac.memory.yield";
      return failure();
    }
    return cast<VarType>(yield.getValue().getType()).getElementType();
  };

  FailureOr<Type> address = verifyPolicy(getAddress(), "address");
  FailureOr<Type> write = verifyPolicy(getWrite(), "write");
  FailureOr<Type> data = verifyPolicy(getData(), "data");
  if (failed(address) || failed(write) || failed(data))
    return failure();
  auto addressInteger = dyn_cast<IntegerType>(*address);
  if (!addressInteger || addressInteger.getWidth() > 64)
    return emitOpError(
        "address must yield an integer Var no wider than 64 bits");
  if (addressInteger.getWidth() < 64 &&
      static_cast<uint64_t>(instance.getEntries()) >
          (uint64_t{1} << addressInteger.getWidth()))
    return emitOpError("entries must fit address width");
  if (!write->isInteger(1))
    return emitOpError("write must yield !ac.var<i1>");
  if (*data != dataType)
    return emitOpError("data must match result_field type");

  Operation *root = getOperation();
  while (root->getParentOp())
    root = root->getParentOp();
  DenseSet<int64_t> ordinals;
  StringSet<> endpointPaths;
  Type payloadType;
  uint64_t maximumOrdinal = 0;
  unsigned endpointCount = 0;
  WalkResult endpointResult = root->walk([&](MemoryRequestOp request) {
    auto resolved =
        dyn_cast_or_null<MemoryInstanceOp>(SymbolTable::lookupNearestSymbolFrom(
            request, request.getInstanceAttr()));
    if (resolved != instance)
      return WalkResult::advance();
    ++endpointCount;
    maximumOrdinal = std::max(maximumOrdinal, request.getOrdinal());
    if (!ordinals.insert(request.getOrdinal()).second) {
      request.emitOpError("duplicate endpoint ordinal for memory instance");
      return WalkResult::interrupt();
    }
    auto path = request->getAttrOfType<StringAttr>("ac.endpoint_path");
    if (!path || !endpointPaths.insert(path.getValue()).second) {
      request.emitOpError(
          "duplicate or missing endpoint path for memory instance");
      return WalkResult::interrupt();
    }
    Type candidate =
        cast<QueueType>(request.getInput().getType()).getElementType();
    if (!payloadType)
      payloadType = candidate;
    else if (payloadType != candidate) {
      request.emitOpError(
          "all endpoints of one memory must use one payload type");
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  if (endpointResult.wasInterrupted())
    return failure();
  if (maximumOrdinal + 1 != endpointCount)
    return emitOpError("memory endpoint ordinals must be contiguous from zero");
  return success();
}

static bool isTableEntryType(Operation *anchor, Type type) {
  if (auto integer = dyn_cast<IntegerType>(type))
    return integer.getWidth() > 0 && integer.getWidth() <= 64;
  auto structure = dyn_cast<StructType>(type);
  if (!structure)
    return false;
  Operation *declaration = recordDecl(anchor, structure);
  if (!declaration)
    return false;
  return llvm::all_of(declarationFields(declaration), [](Attribute rawField) {
    auto field = dyn_cast<DictionaryAttr>(rawField);
    auto type = field ? field.getAs<TypeAttr>("type") : TypeAttr();
    auto integer =
        type ? dyn_cast<IntegerType>(type.getValue()) : IntegerType();
    return integer && integer.getWidth() > 0 && integer.getWidth() <= 64;
  });
}

static FailureOr<uint64_t> tableEntryFieldCount(Operation *endpoint,
                                                TableOp table) {
  auto structure = dyn_cast<StructType>(table.getEntryType());
  if (!structure)
    return uint64_t{1};
  Operation *declaration = recordDecl(endpoint, structure);
  if (!declaration)
    return failure();
  ArrayAttr fields = declarationFields(declaration);
  if (!fields || fields.empty())
    return failure();
  return static_cast<uint64_t>(fields.size());
}

static LogicalResult verifyTableFields(Operation *endpoint, TableOp table,
                                       ArrayAttr fields, StringRef kind) {
  const std::string listName = (kind + "_fields").str();
  if (fields.empty())
    return endpoint->emitOpError() << listName << " must be non-empty";
  StringSet<> allowed;
  llvm::StringMap<unsigned> ordinals;
  if (auto structure = dyn_cast<StructType>(table.getEntryType())) {
    Operation *declaration = recordDecl(endpoint, structure);
    if (!declaration)
      return endpoint->emitOpError("table Entry struct declaration is missing");
    for (auto [ordinal, rawField] :
         llvm::enumerate(declarationFields(declaration))) {
      StringRef name = fieldName(cast<DictionaryAttr>(rawField));
      allowed.insert(name);
      ordinals[name] = ordinal;
    }
  } else {
    allowed.insert("$entry");
    ordinals["$entry"] = 0;
  }
  StringSet<> seen;
  std::optional<unsigned> previousOrdinal;
  for (Attribute rawField : fields) {
    auto field = dyn_cast<StringAttr>(rawField);
    if (!field || field.getValue().empty())
      return endpoint->emitOpError()
             << listName << " must contain non-empty field names";
    if (!seen.insert(field.getValue()).second)
      return endpoint->emitOpError()
             << "duplicate " << kind << " field '" << field.getValue() << "'";
    if (!allowed.contains(field.getValue()))
      return endpoint->emitOpError()
             << "unknown " << kind << " field '" << field.getValue() << "'";
    unsigned ordinal = ordinals.lookup(field.getValue());
    if (previousOrdinal && ordinal <= *previousOrdinal)
      return endpoint->emitOpError()
             << listName << " must follow Table Entry declaration order";
    previousOrdinal = ordinal;
  }
  return success();
}

static LogicalResult verifyTableWriteFields(Operation *endpoint, TableOp table,
                                            ArrayAttr writeFields) {
  return verifyTableFields(endpoint, table, writeFields, "write");
}

static bool tableWriteFieldsAreComplete(Operation *endpoint, TableOp table,
                                        ArrayAttr writeFields) {
  if (auto structure = dyn_cast<StructType>(table.getEntryType())) {
    Operation *declaration = recordDecl(endpoint, structure);
    if (!declaration)
      return false;
    ArrayAttr fields = declarationFields(declaration);
    if (writeFields.size() != fields.size())
      return false;
    for (auto [written, declared] : llvm::zip(writeFields, fields))
      if (cast<StringAttr>(written).getValue() !=
          fieldName(cast<DictionaryAttr>(declared)))
        return false;
    return true;
  }
  return writeFields.size() == 1 &&
         cast<StringAttr>(writeFields[0]).getValue() == "$entry";
}

static LogicalResult verifyTableWriteMode(Operation *endpoint, TableOp table,
                                          StringRef mode,
                                          ArrayAttr writeFields) {
  if (mode != "field" && mode != "replace")
    return endpoint->emitOpError("mode must be 'field' or 'replace'");
  if (mode == "replace" &&
      !tableWriteFieldsAreComplete(endpoint, table, writeFields))
    return endpoint->emitOpError(
        "replace mode must declare every Table Entry field");
  return success();
}

static TableOp resolveTable(Operation *operation, FlatSymbolRefAttr reference) {
  for (Operation *ancestor = operation->getParentOp(); ancestor;
       ancestor = ancestor->getParentOp()) {
    if (ancestor->getNumRegions() != 1 || !ancestor->getRegion(0).hasOneBlock())
      continue;
    for (TableOp table : ancestor->getRegion(0).front().getOps<TableOp>())
      if (table.getSymName() == reference.getValue())
        return table;
  }
  return {};
}

static bool tableVisibleFrom(Operation *operation, TableOp table) {
  std::string requestScope = queueScopePath(operation);
  StringRef owner = table.getOwner();
  StringRef requestPath(requestScope);
  return owner == "/" || requestPath == owner ||
         (requestPath.size() > owner.size() && requestPath.starts_with(owner) &&
          requestPath[owner.size()] == '/');
}

static LogicalResult verifyTableIndex(Operation *operation, TableOp table,
                                      Value index) {
  auto indexType = cast<VarType>(index.getType()).getElementType();
  auto integer = dyn_cast<IntegerType>(indexType);
  if (!integer || integer.getWidth() == 0 || integer.getWidth() > 64)
    return operation->emitOpError(
        "table index must be an integer Var no wider than 64 bits");
  auto constant = index.getDefiningOp<VarConstantOp>();
  auto value =
      constant ? dyn_cast<IntegerAttr>(constant.getValueAttr()) : IntegerAttr();
  if (value &&
      (value.getValue().isNegative() ||
       value.getValue().uge(static_cast<uint64_t>(table.getEntries()))))
    return operation->emitOpError("static table index is out of range");
  return success();
}

static LogicalResult verifyStaticallySafeRuleTableIndex(Operation *operation,
                                                        TableOp table,
                                                        Value index) {
  return verifyTableIndex(operation, table, index);
}

LogicalResult TableOp::verify() {
  if (!isTableEntryType(*this, getEntryType()))
    return emitOpError("entry type must be bool, a <=64-bit integer, or a flat "
                       "integer struct");
  if (getEntries() <= 0)
    return emitOpError("entries must be positive");
  if (getInit() != 0)
    return emitOpError("table init must be zero");
  if (getOwner().empty() || !getOwner().starts_with('/') ||
      (getOwner().size() > 1 && getOwner().ends_with('/')))
    return emitOpError("owner must be a canonical absolute scope path");
  std::string expectedStableId = "table/";
  if (getOwner() != "/") {
    expectedStableId.append(getOwner().drop_front());
    expectedStableId.push_back('/');
  }
  expectedStableId.append(getSymName());
  if (getStableId() != expectedStableId)
    return emitOpError("stable_id must match canonical owner/symbol identity");

  Operation *root = getOperation();
  while (root->getParentOp())
    root = root->getParentOp();
  bool ownerExists = getOwner() == "/";
  bool duplicateStableId = false;
  ModuleOp owningModule = (*this)->getParentOfType<ModuleOp>();
  unsigned endpoints = 0;
  llvm::StringMap<Operation *> fieldWriters;
  std::string overlappingField;
  unsigned replaceWriters = 0;
  unsigned proposalReplaceWriters = 0;
  root->walk([&](Operation *operation) {
    if (auto scope = dyn_cast<ScopeOp>(operation)) {
      std::string path = queueScopePath(scope);
      if (path != "/")
        path.push_back('/');
      path.append(scope.getSymName());
      ownerExists |= path == getOwner();
    }
    if (auto other = dyn_cast<TableOp>(operation))
      duplicateStableId |= other != *this &&
                           other->getParentOfType<ModuleOp>() == owningModule &&
                           other.getStableId() == getStableId();
    if (auto read = dyn_cast<TableReadOp>(operation)) {
      if (resolveTable(read, read.getTableAttr()) == *this)
        ++endpoints;
    }
    if (auto read = dyn_cast<TableGetOp>(operation)) {
      if (resolveTable(read, read.getTableAttr()) == *this)
        ++endpoints;
    }
    if (auto match = dyn_cast<TableMatchOp>(operation)) {
      if (resolveTable(match, match.getTableAttr()) == *this)
        ++endpoints;
    }
    if (auto choose = dyn_cast<TableChooseOp>(operation)) {
      if (resolveTable(choose, choose.getTableAttr()) == *this)
        ++endpoints;
    }
    if (auto write = dyn_cast<TableWriteOp>(operation)) {
      if (resolveTable(write, write.getTableAttr()) == *this) {
        ++endpoints;
        if (write.getMode() == "replace") {
          ++replaceWriters;
          return;
        }
        StringSet<> localFields;
        for (Attribute rawField : write.getWriteFields()) {
          auto field = cast<StringAttr>(rawField).getValue();
          if (localFields.insert(field).second &&
              !fieldWriters.try_emplace(field, operation).second)
            overlappingField = field.str();
        }
      }
    }
    if (auto write = dyn_cast<TableMaskedWriteOp>(operation)) {
      if (resolveTable(write, write.getTableAttr()) == *this) {
        ++endpoints;
        StringSet<> localFields;
        for (Attribute rawField : write.getWriteFields()) {
          auto field = cast<StringAttr>(rawField).getValue();
          if (localFields.insert(field).second &&
              !fieldWriters.try_emplace(field, operation).second)
            overlappingField = field.str();
        }
      }
    }
    if (auto proposal = dyn_cast<TableProposeOp>(operation)) {
      if (resolveTable(proposal, proposal.getTableAttr()) == *this) {
        ++endpoints;
        if (proposal.getMode() == "replace") {
          ++proposalReplaceWriters;
          return;
        }
        StringSet<> localFields;
        for (Attribute rawField : proposal.getWriteFields()) {
          auto field = cast<StringAttr>(rawField).getValue();
          if (localFields.insert(field).second &&
              !fieldWriters.try_emplace(field, operation).second)
            overlappingField = field.str();
        }
      }
    }
    if (auto match = dyn_cast<TableMatchOp>(operation))
      if (resolveTable(match, match.getTableAttr()) == *this)
        ++endpoints;
  });
  if (!ownerExists)
    return emitOpError("owner does not name a declared scope path");
  if (duplicateStableId)
    return emitOpError("stable_id must be unique");
  if (endpoints == 0)
    return emitOpError("must have at least one table read/write endpoint");
  if (!overlappingField.empty())
    return emitOpError() << "write field '" << overlappingField
                         << "' has multiple endpoints";
  if (replaceWriters > 1 ||
      (replaceWriters != 0 && proposalReplaceWriters != 0))
    return emitOpError("has multiple replace writer endpoints");
  return success();
}

LogicalResult TableGetOp::verify() {
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the access scope ancestry");
  if (getResult().getType() != VarType::get(getContext(), table.getEntryType()))
    return emitOpError("result must match table entry Var type");
  return verifyTableIndex(*this, table, getIndex());
}

LogicalResult TableProposeOp::verify() {
  Operation *parent = (*this)->getParentOp();
  if (!isa_and_nonnull<RuleOp, FiringOp>(parent))
    return emitOpError("must be nested directly in ac.rule or ac.firing");
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the proposal scope ancestry");
  if (getMode() != "replace")
    return emitOpError("stateful rule phase one supports replace mode only");
  if (failed(verifyTableWriteFields(*this, table, getWriteFields())) ||
      failed(verifyTableWriteMode(*this, table, getMode(), getWriteFields())))
    return failure();
  if (getValue().getType() != VarType::get(getContext(), table.getEntryType()))
    return emitOpError("proposal value must match the Table Entry Var type");
  if (getWhen() && failed(verifyI1VarCondition(*this, getWhen())))
    return failure();
  if (failed(verifyStaticallySafeRuleTableIndex(*this, table, getIndex())))
    return failure();
  return success();
}

static FailureOr<Type> verifyTablePolicy(Operation *endpoint, Region &region,
                                         StringRef name, Type argumentType,
                                         bool allowGet) {
  if (!region.hasOneBlock()) {
    endpoint->emitOpError() << name << " must contain exactly one block";
    return failure();
  }
  Block &block = region.front();
  const unsigned expectedArguments = argumentType ? 1 : 0;
  if (block.getNumArguments() != expectedArguments ||
      (argumentType && block.getArgument(0).getType() != argumentType)) {
    endpoint->emitOpError() << name << " argument must match endpoint input";
    return failure();
  }
  FlatSymbolRefAttr endpointTable;
  if (auto read = dyn_cast<TableReadOp>(endpoint))
    endpointTable = read.getTableAttr();
  else if (auto write = dyn_cast<TableWriteOp>(endpoint))
    endpointTable = write.getTableAttr();
  else if (auto write = dyn_cast<TableMaskedWriteOp>(endpoint))
    endpointTable = write.getTableAttr();
  auto verifyCapture = [&](Value operand) -> LogicalResult {
    if (auto argument = dyn_cast<BlockArgument>(operand)) {
      if (argument.getOwner() == &block)
        return success();
      return endpoint->emitOpError()
             << name << " captures an external block argument";
    }
    Operation *definition = operand.getDefiningOp();
    if (!definition || definition->getBlock() == &block)
      return success();
    if (auto match = dyn_cast<TableMatchOp>(definition)) {
      if (match.getTableAttr() == endpointTable)
        return success();
    } else if (auto choose = dyn_cast<TableChooseOp>(definition)) {
      if (choose.getTableAttr() == endpointTable)
        return success();
    }
    return endpoint->emitOpError()
           << name
           << " may only capture shared match/choose results for its Table";
  };
  for (Operation &operation : block)
    for (Value operand : operation.getOperands())
      if (failed(verifyCapture(operand)))
        return failure();
  for (Operation &operation : block.without_terminator()) {
    if (isa<SlotGetOp>(operation))
      continue;
    if (auto match = dyn_cast<TableMatchOp>(operation)) {
      FlatSymbolRefAttr endpointTable;
      if (auto read = dyn_cast<TableReadOp>(endpoint))
        endpointTable = read.getTableAttr();
      else if (auto write = dyn_cast<TableWriteOp>(endpoint))
        endpointTable = write.getTableAttr();
      else if (auto write = dyn_cast<TableMaskedWriteOp>(endpoint))
        endpointTable = write.getTableAttr();
      if (match.getTableAttr() != endpointTable) {
        endpoint->emitOpError() << name << " match belongs to another Table";
        return failure();
      }
      continue;
    }
    if (auto choose = dyn_cast<TableChooseOp>(operation)) {
      FlatSymbolRefAttr endpointTable;
      if (auto read = dyn_cast<TableReadOp>(endpoint))
        endpointTable = read.getTableAttr();
      else if (auto write = dyn_cast<TableWriteOp>(endpoint))
        endpointTable = write.getTableAttr();
      else if (auto write = dyn_cast<TableMaskedWriteOp>(endpoint))
        endpointTable = write.getTableAttr();
      if (choose.getTableAttr() != endpointTable) {
        endpoint->emitOpError()
            << name << " selection belongs to another Table";
        return failure();
      }
      continue;
    }
    if (allowGet)
      if (auto get = dyn_cast<TableGetOp>(operation)) {
        FlatSymbolRefAttr endpointTable;
        if (auto read = dyn_cast<TableReadOp>(endpoint))
          endpointTable = read.getTableAttr();
        else if (auto write = dyn_cast<TableWriteOp>(endpoint))
          endpointTable = write.getTableAttr();
        else if (auto write = dyn_cast<TableMaskedWriteOp>(endpoint))
          endpointTable = write.getTableAttr();
        if (get.getTableAttr() != endpointTable) {
          endpoint->emitOpError()
              << name << " may only observe its endpoint table";
          return failure();
        }
        continue;
      }
    if (!isMemoryEffectFree(&operation)) {
      endpoint->emitOpError() << name << " operation '" << operation.getName()
                              << "' is not permitted";
      return failure();
    }
  }
  auto yield = dyn_cast<TableYieldOp>(block.getTerminator());
  if (!yield) {
    endpoint->emitOpError() << name << " must terminate with ac.table.yield";
    return failure();
  }
  return cast<VarType>(yield.getValue().getType()).getElementType();
}

LogicalResult TableReadOp::verify() {
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the read scope ancestry");
  if (getDepth() <= 0 || getLatency() <= 0)
    return emitOpError("depth and latency must be positive");
  if (cast<QueueType>(getOutput().getType()).getElementType() !=
      table.getEntryType())
    return emitOpError("output Queue payload must match table entry type");
  Type argumentType;
  if (getInput())
    argumentType = VarType::get(
        getContext(), cast<QueueType>(getInput().getType()).getElementType());
  auto address =
      verifyTablePolicy(*this, getAddress(), "address", argumentType, false);
  auto when = verifyTablePolicy(*this, getWhen(), "when", argumentType, true);
  if (failed(address) || failed(when))
    return failure();
  auto index = dyn_cast<IntegerType>(*address);
  if (!index || index.getWidth() == 0 || index.getWidth() > 64)
    return emitOpError("address must yield an integer Var");
  if (failed(verifyTableIndex(
          *this, table,
          cast<TableYieldOp>(getAddress().front().getTerminator()).getValue())))
    return failure();
  if (!when->isInteger(1))
    return emitOpError("when must yield !ac.var<i1>");
  return success();
}

LogicalResult TableWriteOp::verify() {
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the write scope ancestry");
  if (failed(verifyTableWriteFields(*this, table, getWriteFields())))
    return failure();
  if (failed(verifyTableWriteMode(*this, table, getMode(), getWriteFields())))
    return failure();
  Type argumentType;
  if (getInput())
    argumentType = VarType::get(
        getContext(), cast<QueueType>(getInput().getType()).getElementType());
  auto address =
      verifyTablePolicy(*this, getAddress(), "address", argumentType, false);
  auto enable =
      verifyTablePolicy(*this, getEnable(), "enable", argumentType, false);
  auto value =
      verifyTablePolicy(*this, getValue(), "value", argumentType, true);
  if (failed(address) || failed(enable) || failed(value))
    return failure();
  auto index = dyn_cast<IntegerType>(*address);
  if (!index || index.getWidth() == 0 || index.getWidth() > 64)
    return emitOpError("address must yield an integer Var");
  if (failed(verifyTableIndex(
          *this, table,
          cast<TableYieldOp>(getAddress().front().getTerminator()).getValue())))
    return failure();
  if (!enable->isInteger(1))
    return emitOpError("enable must yield !ac.var<i1>");
  if (*value != table.getEntryType())
    return emitOpError("value must yield the table entry type");
  return success();
}

LogicalResult TableMaskedWriteOp::verify() {
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the write scope ancestry");
  if (failed(verifyTableWriteFields(*this, table, getWriteFields())))
    return failure();
  if (getMode() != "field")
    return emitOpError("masked write mode must be 'field'");
  if (table.getEntries() == 0 || table.getEntries() > 64)
    return emitOpError("masked write domain must contain 1..64 entries");
  auto maskType = dyn_cast<IntegerType>(
      cast<VarType>(getMask().getType()).getElementType());
  if (!maskType || maskType.getWidth() != table.getEntries())
    return emitOpError("mask width must equal the Table entry count");
  auto match = getMask().getDefiningOp<TableMatchOp>();
  if (!match || resolveTable(match, match.getTableAttr()) != table)
    return emitOpError("mask must be produced by match on the same Table");
  auto enable = verifyTablePolicy(*this, getEnable(), "enable", Type(), false);
  Type entryArgument = VarType::get(getContext(), table.getEntryType());
  auto value =
      verifyTablePolicy(*this, getValue(), "value", entryArgument, true);
  if (failed(enable) || failed(value))
    return failure();
  if (!enable->isInteger(1))
    return emitOpError("enable must yield !ac.var<i1>");
  if (*value != table.getEntryType())
    return emitOpError("value must yield the table entry type");
  return success();
}

LogicalResult TableMatchOp::verify() {
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the match scope ancestry");
  if (table.getEntries() <= 0 || table.getEntries() > 64)
    return emitOpError("match domain must contain 1..64 entries");
  if (getMask().getType() !=
      VarType::get(getContext(),
                   IntegerType::get(getContext(), table.getEntries())))
    return emitOpError("mask width must equal the Table domain");
  if (!getPredicate().hasOneBlock())
    return emitOpError("predicate must contain exactly one block");
  Block &block = getPredicate().front();
  Type entry = VarType::get(getContext(), table.getEntryType());
  if (block.getNumArguments() != 1 || block.getArgument(0).getType() != entry)
    return emitOpError("predicate argument must match the Table Entry");
  for (Operation &operation : block.without_terminator())
    if (!isa<SlotGetOp, TableGetOp>(operation) &&
        !isMemoryEffectFree(&operation))
      return emitOpError() << "predicate operation '" << operation.getName()
                           << "' is not permitted";
  auto yield = dyn_cast<TableMatchYieldOp>(block.getTerminator());
  if (!yield ||
      !cast<VarType>(yield.getValue().getType()).getElementType().isInteger(1))
    return emitOpError("predicate must yield !ac.var<i1>");
  return success();
}

LogicalResult TableChooseOp::verify() {
  TableOp table = resolveTable(*this, getTableAttr());
  if (!table)
    return emitOpError() << "unresolved table " << getTable();
  if (!tableVisibleFrom(*this, table))
    return emitOpError("table is outside the choose scope ancestry");
  if (table.getEntries() <= 0 || table.getEntries() > 64)
    return emitOpError("choose domain must contain 1..64 entries");
  auto mask = dyn_cast<IntegerType>(
      cast<VarType>(getMask().getType()).getElementType());
  if (!mask || mask.getWidth() != static_cast<unsigned>(table.getEntries()))
    return emitOpError("candidate mask width must equal the Table domain");
  auto match = getMask().getDefiningOp<TableMatchOp>();
  if (!match)
    return emitOpError("candidate mask must be produced directly by "
                       "ac.table.match");
  if (resolveTable(match, match.getTableAttr()) != table)
    return emitOpError("candidate mask must come from the same Table");
  if (getCount() != 1)
    return emitOpError("choose supports count=1 only");
  if (getPolicy() != "first" && getPolicy() != "min" && getPolicy() != "max")
    return emitOpError("policy must be first, min, or max");
  unsigned indexWidth = std::max<unsigned>(
      1, llvm::Log2_64_Ceil(static_cast<uint64_t>(table.getEntries())));
  if (getIndex().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), indexWidth)))
    return emitOpError("index result width must address the Table domain");
  if (getValid().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), 1)))
    return emitOpError("valid result must be !ac.var<i1>");
  if (getPolicy() == "first") {
    if (!getKey().empty() &&
        !(getKey().hasOneBlock() && getKey().front().empty()))
      return emitOpError("first policy does not accept a key region");
    return success();
  }
  if (!getKey().hasOneBlock())
    return emitOpError("min/max policy requires one key region");
  Block &block = getKey().front();
  Type entry = VarType::get(getContext(), table.getEntryType());
  if (block.getNumArguments() != 1 || block.getArgument(0).getType() != entry)
    return emitOpError("key argument must match the Table Entry");
  for (Operation &operation : block.without_terminator()) {
    if (isa<TableGetOp>(operation)) {
      if (!isa_and_nonnull<RuleOp, FiringOp>((*this)->getParentOp()))
        return emitOpError(
            "key Table reads require transactional rule/firing ownership");
      continue;
    }
    if (!isa<SlotGetOp>(operation) && !isMemoryEffectFree(&operation))
      return emitOpError() << "key operation '" << operation.getName()
                           << "' is not permitted";
  }
  auto yield = dyn_cast<TableChooseYieldOp>(block.getTerminator());
  auto key =
      yield ? dyn_cast<IntegerType>(
                  cast<VarType>(yield.getValue().getType()).getElementType())
            : IntegerType();
  if (!key || key.getWidth() == 0 || key.getWidth() > 64)
    return emitOpError(
        "min/max key must yield an unsigned fixed-width integer");
  return success();
}

static SlotOp resolveSlot(Operation *operation, FlatSymbolRefAttr reference) {
  return dyn_cast_or_null<SlotOp>(
      SymbolTable::lookupNearestSymbolFrom(operation, reference));
}

static bool slotVisibleFrom(Operation *operation, SlotOp slot) {
  std::string requestScope = queueScopePath(operation);
  StringRef owner = slot.getOwner();
  StringRef requestPath(requestScope);
  return owner == "/" || requestPath == owner ||
         (requestPath.size() > owner.size() && requestPath.starts_with(owner) &&
          requestPath[owner.size()] == '/');
}

LogicalResult SlotOp::verify() {
  Type payload = cast<QueueType>(getInput().getType()).getElementType();
  if (!isTableEntryType(*this, payload))
    return emitOpError("input Queue payload must be bool, a <=64-bit integer, "
                       "or a flat integer struct");
  if (getOwner().empty() || !getOwner().starts_with('/') ||
      (getOwner().size() > 1 && getOwner().ends_with('/')))
    return emitOpError("owner must be a canonical absolute scope path");
  std::string expectedStableId = "slot/";
  if (getOwner() != "/") {
    expectedStableId.append(getOwner().drop_front());
    expectedStableId.push_back('/');
  }
  expectedStableId.append(getSymName());
  if (getStableId() != expectedStableId)
    return emitOpError("stable_id must match canonical owner/symbol identity");
  Operation *root = getOperation();
  while (root->getParentOp())
    root = root->getParentOp();
  unsigned releases = 0;
  root->walk([&](SlotReleaseOp release) {
    if (resolveSlot(release, release.getSlotAttr()) == *this)
      ++releases;
  });
  if (releases != 1)
    return emitOpError("slot requires exactly one release endpoint");
  return success();
}

LogicalResult SlotGetOp::verify() {
  SlotOp slot = resolveSlot(*this, getSlotAttr());
  if (!slot)
    return emitOpError() << "unresolved slot " << getSlot();
  if (!slotVisibleFrom(*this, slot))
    return emitOpError("slot is outside the access scope ancestry");
  Type payload = cast<QueueType>(slot.getInput().getType()).getElementType();
  if (getValid().getType() !=
      VarType::get(getContext(), IntegerType::get(getContext(), 1)))
    return emitOpError("valid result must be !ac.var<i1>");
  if (getValue().getType() != VarType::get(getContext(), payload))
    return emitOpError("value result must match slot Queue payload");
  return success();
}

LogicalResult SlotReleaseOp::verify() {
  SlotOp slot = resolveSlot(*this, getSlotAttr());
  if (!slot)
    return emitOpError() << "unresolved slot " << getSlot();
  if (!slotVisibleFrom(*this, slot))
    return emitOpError("slot is outside the release scope ancestry");
  if (!getWhen().hasOneBlock() || getWhen().front().getNumArguments() != 0)
    return emitOpError("when must contain one zero-argument block");
  Block &block = getWhen().front();
  for (Operation &operation : block.without_terminator()) {
    if (auto get = dyn_cast<SlotGetOp>(operation)) {
      if (get.getSlotAttr() != getSlotAttr())
        return emitOpError("when may only observe its endpoint slot");
      continue;
    }
    if (auto get = dyn_cast<TableGetOp>(operation)) {
      (void)get;
      continue;
    }
    if (isa<TableMatchOp, TableChooseOp>(operation))
      continue;
    if (!isMemoryEffectFree(&operation))
      return emitOpError() << "when operation '" << operation.getName()
                           << "' is not permitted";
  }
  auto yield = dyn_cast<SlotYieldOp>(block.getTerminator());
  if (!yield ||
      !cast<VarType>(yield.getValue().getType()).getElementType().isInteger(1))
    return emitOpError("when must terminate with ac.slot.yield !ac.var<i1>");
  return success();
}

LogicalResult InterfaceOp::verify() {
  if (getBody().empty())
    return emitOpError("interface declaration requires a body block");
  for (Operation &child : getBody().front())
    if (!isa<RoleOp, PortOp>(child))
      return emitOpError()
             << "interface body only permits ac.role and ac.port, "
             << "found " << child.getName();
  return verifyRoleContainer(*this);
}

LogicalResult ProtocolOp::verify() {
  if (getBody().empty())
    return emitOpError("protocol declaration requires a body block");
  for (Operation &child : getBody().front())
    if (!isa<RoleOp, StateOp, EventOp, TransitionOp, GuaranteeOp>(child))
      return emitOpError() << "protocol body contains unsupported operation "
                           << child.getName();

  if (failed(verifyRoleContainer(*this)))
    return failure();

  unsigned initialStates = 0;
  for (StateOp state : getBody().getOps<StateOp>())
    initialStates += state.getInitial() ? 1 : 0;
  if (initialStates != 1)
    return emitOpError()
           << "protocol requires exactly one initial state, found "
           << initialStates;

  llvm::SmallDenseSet<StringRef> guaranteeKinds;
  for (GuaranteeOp guarantee : getBody().getOps<GuaranteeOp>())
    if (!guaranteeKinds.insert(guarantee.getKind()).second)
      return guarantee.emitOpError()
             << "duplicate protocol guarantee '" << guarantee.getKind() << "'";

  SmallVector<TransitionOp> transitions;
  for (TransitionOp transition : getBody().getOps<TransitionOp>()) {
    if (!lookupChild<StateOp>(*this, transition.getSourceAttr()))
      return transition.emitOpError()
             << "unresolved transition source state '@"
             << transition.getSourceAttr().getValue() << "'";
    if (!lookupChild<StateOp>(*this, transition.getTargetAttr()))
      return transition.emitOpError()
             << "unresolved transition target state '@"
             << transition.getTargetAttr().getValue() << "'";
    if (!lookupChild<EventOp>(*this, transition.getEventAttr()))
      return transition.emitOpError()
             << "unresolved transition event '@"
             << transition.getEventAttr().getValue() << "'";
    transitions.push_back(transition);
  }

  for (unsigned i = 0; i < transitions.size(); ++i) {
    SmallVector<TransitionOp> overlapping{transitions[i]};
    for (unsigned j = i + 1; j < transitions.size(); ++j)
      if (transitions[i].getSourceAttr() == transitions[j].getSourceAttr() &&
          transitions[i].getEventAttr() == transitions[j].getEventAttr())
        overlapping.push_back(transitions[j]);
    if (overlapping.size() < 2)
      continue;
    llvm::SmallSet<int64_t, 4> priorities;
    for (TransitionOp transition : overlapping) {
      if (!transition.getPriority())
        return transition.emitOpError(
            "overlapping transitions require explicit priority");
      if (!priorities.insert(static_cast<int64_t>(*transition.getPriority()))
               .second)
        return transition.emitOpError(
            "overlapping transitions require unique priority");
    }
  }

  GuaranteeOp stablePending = findGuarantee(*this, "stable_pending");
  bool stable = stablePending && dyn_cast<BoolAttr>(stablePending.getValue()) &&
                cast<BoolAttr>(stablePending.getValue()).getValue();
  for (TransitionOp transition : transitions) {
    EventOp event = lookupChild<EventOp>(*this, transition.getEventAttr());
    if (transition.getTransfer() && transition.getRetain())
      return transition.emitOpError(
          "transition cannot both transfer and retain ownership");
    if (event.getAction() == "offer" && !transition.getTransfer() &&
        !transition.getRetain())
      return transition.emitOpError(
          "offer transition must transfer or retain ownership");
    if (event.getAction() == "offer" && transition.getRetain() && !stable)
      return transition.emitOpError(
          "retained pending offer requires stable_pending = true");
    if (event.getAction() == "retry" && !transition.getRetain())
      return transition.emitOpError(
          "retry transition must retain the pending offer");
    if (event.getAction() == "retry" && transition.getTransfer())
      return transition.emitOpError(
          "retry transition cannot transfer the pending offer");
    if (transition.getRetain() && event.getAction() != "offer" &&
        event.getAction() != "retry")
      return transition.emitOpError(
          "retain is only valid for offer and retry transitions");
  }

  enum class Ownership : uint8_t { NoPending = 1, Pending = 2 };
  SmallVector<StateOp> states(getBody().getOps<StateOp>());
  llvm::StringMap<unsigned> stateIndices;
  for (auto [index, state] : llvm::enumerate(states))
    stateIndices.try_emplace(state.getSymName(), index);
  auto stateIndex = [&](FlatSymbolRefAttr name) -> unsigned {
    auto found = stateIndices.find(name.getValue());
    assert(found != stateIndices.end() &&
           "transition state references were verified");
    return found->second;
  };

  SmallVector<SmallVector<unsigned>> outgoing(states.size());
  SmallVector<unsigned> transitionTargets;
  SmallVector<EventOp> transitionEvents;
  transitionTargets.reserve(transitions.size());
  transitionEvents.reserve(transitions.size());
  for (auto [index, transition] : llvm::enumerate(transitions)) {
    outgoing[stateIndex(transition.getSourceAttr())].push_back(index);
    transitionTargets.push_back(stateIndex(transition.getTargetAttr()));
    transitionEvents.push_back(
        lookupChild<EventOp>(*this, transition.getEventAttr()));
  }

  SmallVector<uint8_t> ownership(states.size(), 0);
  SmallVector<std::pair<unsigned, Ownership>> worklist;
  auto ownershipBit = [](Ownership value) {
    return static_cast<uint8_t>(value);
  };
  auto hasOwnership = [&](unsigned state, Ownership value) {
    return (ownership[state] & ownershipBit(value)) != 0;
  };
  auto addOwnership = [&](unsigned state, Ownership value) {
    uint8_t bit = ownershipBit(value);
    if (ownership[state] & bit)
      return;
    ownership[state] |= bit;
    worklist.emplace_back(state, value);
  };
  for (auto [index, state] : llvm::enumerate(states))
    if (state.getInitial())
      addOwnership(index, Ownership::NoPending);

  auto transferOwnership = [&](unsigned transitionIndex,
                               Ownership input) -> std::optional<Ownership> {
    TransitionOp transition = transitions[transitionIndex];
    StringRef action = transitionEvents[transitionIndex].getAction();
    if (action == "offer") {
      if (input == Ownership::Pending)
        return std::nullopt;
      return transition.getTransfer() ? Ownership::NoPending
                                      : Ownership::Pending;
    }
    if (action == "retry")
      return input == Ownership::Pending
                 ? std::optional<Ownership>(Ownership::Pending)
                 : std::nullopt;
    if (action == "cancel" || action == "reject")
      return input == Ownership::Pending
                 ? std::optional<Ownership>(Ownership::NoPending)
                 : std::nullopt;
    if (transition.getTransfer())
      return input == Ownership::Pending
                 ? std::optional<Ownership>(Ownership::NoPending)
                 : std::nullopt;
    return input;
  };

  for (unsigned cursor = 0; cursor < worklist.size(); ++cursor) {
    auto [source, input] = worklist[cursor];
    for (unsigned transitionIndex : outgoing[source])
      if (std::optional<Ownership> output =
              transferOwnership(transitionIndex, input))
        addOwnership(transitionTargets[transitionIndex], *output);
  }

  uint8_t conflicting =
      ownershipBit(Ownership::NoPending) | ownershipBit(Ownership::Pending);
  for (auto [index, state] : llvm::enumerate(states))
    if (ownership[index] == conflicting)
      return state.emitOpError() << "ownership state conflict at join state '@"
                                 << state.getSymName() << "'";

  auto firstTransition = [&](auto predicate) -> TransitionOp {
    for (auto [index, transition] : llvm::enumerate(transitions))
      if (predicate(index, transition))
        return transition;
    return {};
  };
  if (TransitionOp transition = firstTransition([&](unsigned index, auto op) {
        return transitionEvents[index].getAction() == "offer" &&
               hasOwnership(stateIndex(op.getSourceAttr()), Ownership::Pending);
      }))
    return transition.emitOpError(
        "offer cannot begin while another offer is pending");
  if (TransitionOp transition = firstTransition([&](unsigned index, auto op) {
        return transitionEvents[index].getAction() == "retry" &&
               hasOwnership(stateIndex(op.getSourceAttr()),
                            Ownership::NoPending);
      }))
    return transition.emitOpError("retry requires a pending offer");
  if (TransitionOp transition = firstTransition([&](unsigned index, auto op) {
        StringRef action = transitionEvents[index].getAction();
        return (action == "cancel" || action == "reject") &&
               hasOwnership(stateIndex(op.getSourceAttr()),
                            Ownership::NoPending);
      }))
    return transition.emitOpError(
        "ownership resolution requires a pending offer");
  if (TransitionOp transition = firstTransition([&](unsigned index, auto op) {
        StringRef action = transitionEvents[index].getAction();
        return op.getTransfer() && action != "offer" && action != "retry" &&
               action != "cancel" && action != "reject" &&
               hasOwnership(stateIndex(op.getSourceAttr()),
                            Ownership::NoPending);
      }))
    return transition.emitOpError(
        "ownership transfer requires a pending offer");
  if (TransitionOp transition = firstTransition([&](unsigned index, auto op) {
        if (ownership[stateIndex(op.getSourceAttr())] != 0)
          return false;
        StringRef action = transitionEvents[index].getAction();
        return op.getTransfer() || action == "retry" || action == "cancel" ||
               action == "reject";
      }))
    return transition.emitOpError(
        "ownership resolution is unreachable from the initial state");

  for (auto [index, state] : llvm::enumerate(states)) {
    if (!hasOwnership(index, Ownership::Pending))
      continue;
    if (state.getTerminal())
      return state.emitOpError() << "terminal state '@" << state.getSymName()
                                 << "' is reachable with pending ownership";
    if (outgoing[index].empty())
      return state.emitOpError()
             << "pending ownership reaches state '@" << state.getSymName()
             << "' with no outgoing transition";
  }

  GuaranteeOp maxInflight = findGuarantee(*this, "max_inflight");
  if (maxInflight) {
    auto value = dyn_cast<IntegerAttr>(maxInflight.getValue());
    if (!value || !value.getType().isSignlessInteger(64) || value.getInt() <= 0)
      return maxInflight.emitOpError(
          "max_inflight requires a positive i64 value");
    if (value.getInt() > 1 && !findGuarantee(*this, "correlation"))
      return maxInflight.emitOpError(
          "max_inflight greater than one requires correlation");
  }
  GuaranteeOp backpressure = findGuarantee(*this, "backpressure");
  if (backpressure)
    if (auto value = dyn_cast<StringAttr>(backpressure.getValue());
        value && value.getValue() == "custom" &&
        !findGuarantee(*this, "custom_backpressure"))
      return backpressure.emitOpError(
          "custom backpressure requires a custom_backpressure declaration");
  GuaranteeOp ordering = findGuarantee(*this, "ordering");
  if (ordering)
    if (auto value = dyn_cast<StringAttr>(ordering.getValue());
        value && value.getValue() == "per_key" &&
        !findGuarantee(*this, "correlation"))
      return ordering.emitOpError("per_key ordering requires correlation");
  GuaranteeOp completion = findGuarantee(*this, "completion");
  if (completion) {
    if (auto value = dyn_cast<StringAttr>(completion.getValue())) {
      if (value.getValue() == "on_response" &&
          !findGuarantee(*this, "correlation"))
        return completion.emitOpError(
            "on_response completion requires correlation");
      auto hasReachableAction = [&](StringRef action) {
        return llvm::any_of(transitions, [&](TransitionOp transition) {
          return ownership[stateIndex(transition.getSourceAttr())] &&
                 lookupChild<EventOp>(*this, transition.getEventAttr())
                         .getAction() == action;
        });
      };
      if (value.getValue() == "on_response" && !hasReachableAction("response"))
        return completion.emitOpError(
            "on_response completion requires a reachable response event");
      if (value.getValue() == "on_accept" && !hasReachableAction("accept"))
        return completion.emitOpError(
            "on_accept completion requires a reachable accept event");
      if (value.getValue() == "on_terminal_phase" &&
          llvm::none_of(llvm::enumerate(states), [&](auto indexedState) {
            return indexedState.value().getTerminal() &&
                   ownership[indexedState.index()] != 0;
          }))
        return completion.emitOpError(
            "on_terminal_phase completion requires a reachable terminal state");
    }
  }
  GuaranteeOp correlation = findGuarantee(*this, "correlation");
  if (correlation) {
    auto field = dyn_cast<StringAttr>(correlation.getValue());
    if (field && !field.getValue().empty()) {
      Type correlationType;
      for (TransitionOp transition : transitions) {
        if (!ownership[stateIndex(transition.getSourceAttr())])
          continue;
        EventOp event = lookupChild<EventOp>(*this, transition.getEventAttr());
        if (event.getAction() != "offer")
          continue;
        Operation *declaration = recordDecl(event, event.getPayload());
        std::optional<unsigned> index =
            declaration ? findField(declaration, field.getValue())
                        : std::nullopt;
        if (!index)
          return correlation.emitOpError()
                 << "correlation field '" << field.getValue()
                 << "' is missing from reachable offer/response payload";
        Type type = fieldType(declaration, *index);
        if (!correlationType)
          correlationType = type;
        else if (type != correlationType)
          return correlation.emitOpError()
                 << "correlation field '" << field.getValue() << "' has type "
                 << type << " but expected " << correlationType;
      }
      if (!correlationType)
        return correlation.emitOpError()
               << "correlation field '" << field.getValue()
               << "' requires a reachable offer event";
      for (TransitionOp transition : transitions) {
        if (!ownership[stateIndex(transition.getSourceAttr())])
          continue;
        EventOp event = lookupChild<EventOp>(*this, transition.getEventAttr());
        if (event.getAction() != "offer" && event.getAction() != "response")
          continue;
        Operation *declaration = recordDecl(event, event.getPayload());
        std::optional<unsigned> index =
            declaration ? findField(declaration, field.getValue())
                        : std::nullopt;
        if (!index)
          return correlation.emitOpError()
                 << "correlation field '" << field.getValue()
                 << "' is missing from reachable offer/response payload";
        Type type = fieldType(declaration, *index);
        if (type != correlationType)
          return correlation.emitOpError()
                 << "correlation field '" << field.getValue() << "' has type "
                 << type << " but expected " << correlationType;
      }
    }
  }
  return success();
}

LogicalResult RoleOp::verify() {
  if (!isa_and_nonnull<InterfaceOp, ProtocolOp>(getOperation()->getParentOp()))
    return emitOpError(
        "role must be a direct child of ac.interface or ac.protocol");
  if (getCardinality() != "exclusive" && getCardinality() != "shared")
    return emitOpError() << "unsupported role cardinality '" << getCardinality()
                         << "'";
  return success();
}

LogicalResult StateOp::verify() {
  if (!isa_and_nonnull<ProtocolOp>(getOperation()->getParentOp()))
    return emitOpError("state must be a direct child of ac.protocol");
  return success();
}

LogicalResult EventOp::verify() {
  auto protocol = dyn_cast_or_null<ProtocolOp>(getOperation()->getParentOp());
  if (!protocol)
    return emitOpError("event must be a direct child of ac.protocol");
  if (failed(verifyRoleReference(*this, protocol, getFromAttr(),
                                 "event source")) ||
      failed(verifyRoleReference(*this, protocol, getToAttr(), "event target")))
    return failure();
  if (getFromAttr() == getToAttr())
    return emitOpError("event source and target roles must differ");
  if (!isProtocolPayloadType(getPayload()))
    return emitOpError(
        "event payload type must be a normative ACIR value type");
  if (failed(verifyNamedTypes(*this, getPayload())))
    return failure();
  static constexpr StringRef actions[] = {
      "offer", "accept", "cancel", "reject", "retry", "response", "notify"};
  if (!hasStringValue(getAction(), actions))
    return emitOpError() << "unsupported event action '" << getAction() << "'";
  return success();
}

LogicalResult TransitionOp::verify() {
  if (!isa_and_nonnull<ProtocolOp>(getOperation()->getParentOp()))
    return emitOpError("transition must be a direct child of ac.protocol");
  if (auto priority = getPriority(); priority && *priority > INT64_MAX)
    return emitOpError("transition priority must be a non-negative i64 value");
  WalkResult result = getGuard().walk([&](Operation *operation) {
    if (!isAllowedGuardExpression(operation)) {
      emitOpError() << "guard operation '" << operation->getName()
                    << "' is not in the pure expression allowlist";
      return WalkResult::interrupt();
    }
    auto effects = dyn_cast<MemoryEffectOpInterface>(operation);
    if (!effects) {
      emitOpError() << "allowed guard operation '" << operation->getName()
                    << "' must implement MemoryEffectOpInterface";
      return WalkResult::interrupt();
    }
    SmallVector<MemoryEffects::EffectInstance> instances;
    effects.getEffects(instances);
    if (!instances.empty()) {
      emitOpError() << "allowed guard operation '" << operation->getName()
                    << "' must have no memory effects";
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  if (result.wasInterrupted())
    return failure();
  return success();
}

LogicalResult GuaranteeOp::verify() {
  if (!isa_and_nonnull<ProtocolOp>(getOperation()->getParentOp()))
    return emitOpError("guarantee must be a direct child of ac.protocol");
  StringRef kind = getKind();
  if (kind == "backpressure")
    return verifyStringGuarantee(
        *this, {"none", "accept", "credit", "capacity", "custom"});
  if (kind == "ordering")
    return verifyStringGuarantee(*this, {"fifo", "per_key", "unordered"});
  if (kind == "delivery")
    return verifyStringGuarantee(
        *this, {"exactly_once", "at_most_once", "best_effort"});
  if (kind == "completion")
    return verifyStringGuarantee(
        *this, {"on_accept", "on_response", "on_terminal_phase"});
  if (kind == "stable_pending") {
    if (!isa<BoolAttr>(getValue()))
      return emitOpError("stable_pending requires a boolean value");
    return success();
  }
  if (kind == "max_inflight")
    return success();
  if (kind == "correlation") {
    auto value = dyn_cast<StringAttr>(getValue());
    if (!value || value.getValue().empty())
      return emitOpError("correlation requires a non-empty field name");
    return success();
  }
  if (kind == "custom_backpressure") {
    auto value = dyn_cast<StringAttr>(getValue());
    if (!value || value.getValue().empty())
      return emitOpError(
          "custom_backpressure requires a non-empty declarative contract");
    return success();
  }
  return emitOpError() << "unknown mandatory protocol guarantee '" << kind
                       << "'";
}

LogicalResult PortOp::verify() {
  auto interface = dyn_cast_or_null<InterfaceOp>(getOperation()->getParentOp());
  if (!interface)
    return emitOpError("port must be a direct child of ac.interface");
  auto channel = dyn_cast<ChannelType>(getType());
  if (!channel)
    return emitOpError("port type must be !ac.channel<T, Protocol>");
  if (failed(verifyRoleReference(*this, interface, getFromAttr(),
                                 "port source")) ||
      failed(verifyRoleReference(*this, interface, getToAttr(), "port target")))
    return failure();
  if (getFromAttr() == getToAttr())
    return emitOpError("port source and target roles must differ");
  RoleOp fromRole = lookupChild<RoleOp>(interface, getFromAttr());
  if (fromRole.getDualAttr() != getToAttr())
    return emitOpError("port source and target roles must be dual");
  if (!isProtocolPayloadType(channel.getElementType()))
    return emitOpError(
        "channel payload type must be a normative ACIR value type");
  if (failed(verifyNamedTypes(*this, channel.getElementType())))
    return failure();
  ProtocolOp protocol = lookupProtocol(*this, channel.getProtocol());
  if (!protocol)
    return emitOpError() << "unresolved channel protocol '@"
                         << channel.getProtocol().getValue() << "'";
  RoleOp protocolFrom = lookupChild<RoleOp>(protocol, getProtocolFromAttr());
  if (!protocolFrom)
    return emitOpError() << "unresolved mapped protocol source role '@"
                         << getProtocolFromAttr().getValue() << "'";
  RoleOp protocolTo = lookupChild<RoleOp>(protocol, getProtocolToAttr());
  if (!protocolTo)
    return emitOpError() << "unresolved mapped protocol target role '@"
                         << getProtocolToAttr().getValue() << "'";
  if (protocolFrom.getDualAttr() != getProtocolToAttr() ||
      protocolTo.getDualAttr() != getProtocolFromAttr())
    return emitOpError("mapped protocol roles must be dual");
  RoleOp toRole = lookupChild<RoleOp>(interface, getToAttr());
  if (fromRole.getCardinality() != protocolFrom.getCardinality() ||
      toRole.getCardinality() != protocolTo.getCardinality())
    return emitOpError(
        "interface and mapped protocol roles must have matching cardinality");
  if (!matchesCarrierEvent(protocol, channel.getElementType(),
                           getProtocolFromAttr(), getProtocolToAttr()))
    return emitOpError() << "channel payload " << channel.getElementType()
                         << " from mapped protocol role '@"
                         << getProtocolFromAttr().getValue() << "' to '@"
                         << getProtocolToAttr().getValue()
                         << "' does not match any carrier event in protocol '@"
                         << channel.getProtocol().getValue() << "'";
  return success();
}

namespace {

FunctionType graphSignature(Operation *op) {
  if (!op)
    return {};
  auto type = op->getAttrOfType<TypeAttr>("function_type");
  return type ? dyn_cast<FunctionType>(type.getValue()) : FunctionType();
}

Operation *lookupGraphSymbol(Operation *from, FlatSymbolRefAttr name) {
  auto file = from->getParentOfType<mlir::ModuleOp>();
  return file ? SymbolTable::lookupSymbolIn(file, name) : nullptr;
}

LogicalResult verifyConcreteDictionary(Operation *op, DictionaryAttr values,
                                       StringRef subject) {
  for (NamedAttribute value : values)
    if (!isConcreteStaticValue(value.getValue()))
      return op->emitOpError() << subject
                               << " must contain only concrete builtin static "
                                  "values";
  LogicalResult result = success();
  values.walk([&](SymbolRefAttr reference) {
    if (SymbolTable::lookupNearestSymbolFrom(op, reference))
      return WalkResult::advance();
    op->emitOpError() << "unresolved static symbol reference '" << reference
                      << "'";
    result = failure();
    return WalkResult::interrupt();
  });
  if (failed(result))
    return failure();
  return success();
}

LogicalResult verifyOuterPlacement(Operation *op) {
  auto outer = dyn_cast_or_null<mlir::ModuleOp>(op->getParentOp());
  if (outer &&
      (!outer->getParentOp() || (isa<mlir::ModuleOp>(outer->getParentOp()) &&
                                 !outer->getParentOp()->getParentOp())))
    return success();
  return op->emitOpError("must be a direct child of the outer builtin.module");
}

LogicalResult verifyStructuralPlacement(Operation *op) {
  auto module = dyn_cast_or_null<ModuleOp>(op->getParentOp());
  if (module && !module.getBody().empty() &&
      op->getBlock() == &module.getBody().front())
    return success();
  return op->emitOpError(
      "must be a direct child of the unique ac.module Graph block");
}

LogicalResult verifyExactBinding(Operation *op, DictionaryAttr binding,
                                 StringRef subject,
                                 StringRef requiredRegistry) {
  auto registry = binding.getAs<StringAttr>("registry");
  auto name = binding.getAs<StringAttr>("name");
  if (binding.size() != 2 || !registry || registry.getValue().empty() ||
      !name || name.getValue().empty() || registry.getValue() == "generic")
    return op->emitOpError()
           << subject << " requires exact registered {registry, name} metadata";
  if (registry.getValue() != requiredRegistry)
    return op->emitOpError() << subject << " requires registered registry '"
                             << requiredRegistry << "'";
  return success();
}

LogicalResult verifyCallShape(Operation *op, FunctionType signature,
                              TypeRange inputs, TypeRange outputs) {
  if (!signature)
    return op->emitOpError("definition has no canonical module signature");
  if (!llvm::equal(inputs, signature.getInputs()))
    return op->emitOpError("operand types do not match module signature");
  if (!llvm::equal(outputs, signature.getResults()))
    return op->emitOpError("result types do not match module signature");
  return success();
}

LogicalResult verifyStaticArgumentSet(Operation *op, DictionaryAttr arguments,
                                      Operation *definition = nullptr) {
  if (failed(verifyConcreteDictionary(op, arguments, "static arguments")))
    return failure();
  if (!definition)
    return success();
  auto parameters = definition->getAttrOfType<DictionaryAttr>("static_params");
  if (!parameters || parameters.size() != arguments.size())
    return op->emitOpError(
        "static argument names must exactly match definition parameters");
  for (NamedAttribute parameter : parameters) {
    Attribute argument = arguments.get(parameter.getName());
    if (!argument)
      return op->emitOpError(
          "static argument names must exactly match definition parameters");
    if (argument.getTypeID() != parameter.getValue().getTypeID())
      return op->emitOpError()
             << "static argument '" << parameter.getName().getValue()
             << "' must match parameter attribute kind";
    auto parameterInteger = dyn_cast<IntegerAttr>(parameter.getValue());
    auto argumentInteger = dyn_cast<IntegerAttr>(argument);
    if (parameterInteger &&
        parameterInteger.getType() != argumentInteger.getType())
      return op->emitOpError()
             << "static argument '" << parameter.getName().getValue()
             << "' must match parameter attribute type "
             << parameterInteger.getType();
    auto parameterUnit = dyn_cast<DictionaryAttr>(parameter.getValue());
    if (parameterUnit) {
      auto argumentUnit = cast<DictionaryAttr>(argument);
      if (parameterUnit.getAs<StringAttr>("unit") !=
          argumentUnit.getAs<StringAttr>("unit"))
        return op->emitOpError()
               << "static argument '" << parameter.getName().getValue()
               << "' must preserve unit '"
               << parameterUnit.getAs<StringAttr>("unit").getValue() << "'";
    }
  }
  return success();
}

bool isStructuralGraphChild(Operation &child) {
  auto file = child.getParentOp()->getParentOfType<mlir::ModuleOp>();
  auto kind =
      file ? file->getAttrOfType<StringAttr>("ac.model_kind") : StringAttr();
  const bool queueGraphChild =
      kind && kind.getValue() == "queue_graph" && isa<ScopeOp>(child);
  return isa<InstanceOp, ArrayOp, InstancesOp, ViewOp, QueueOp, EventQueueOp,
             ResourceOp, AddressSpaceOp, AddressMapOp, TimeDomainOp, ProcessOp,
             RequireOp, EnsureOp, StatOp, ReturnOp>(child) ||
         queueGraphChild || child.getName().getStringRef() == "arith.constant";
}

bool isStableHierarchySegment(StringRef segment) {
  return !segment.empty() && llvm::all_of(segment, [](char c) {
    return llvm::isAlnum(c) || c == '_' || c == '-';
  });
}

LogicalResult
verifyRuntimeReferences(ModuleOp module,
                        const llvm::StringMap<Operation *> &producerIndex) {
  LogicalResult result = success();
  auto lookupExpected = [&](Operation *operation, StringRef reference,
                            StringRef expectedName) -> Operation * {
    Operation *target = producerIndex.lookup(reference);
    if (!target) {
      operation->emitOpError()
          << "unresolved runtime target '@" << reference << "'";
      result = failure();
      return nullptr;
    }
    if (target->getName().getStringRef() != expectedName) {
      operation->emitOpError() << "runtime target '@" << reference
                               << "' must resolve to " << expectedName;
      result = failure();
      return nullptr;
    }
    return target;
  };
  auto lookupQueueRef = [&](Operation *operation,
                            SymbolRefAttr reference) -> QueueOp {
    Operation *target = lookupRuntimeSymbol(operation, reference);
    if (!target) {
      operation->emitOpError()
          << "unresolved runtime target '" << reference << "'";
      result = failure();
      return {};
    }
    auto queue = dyn_cast<QueueOp>(target);
    if (!queue) {
      operation->emitOpError()
          << "runtime target '" << reference << "' must resolve to ac.queue";
      result = failure();
      return {};
    }
    return queue;
  };
  for (ProcessOp process : module.getBody().front().getOps<ProcessOp>()) {
    WalkResult walk = process.getBody().walk([&](Operation *operation) {
      if (auto send = dyn_cast<TrySendOp>(operation)) {
        auto queue = lookupQueueRef(send, send.getQueue());
        if (queue && queue.getPayload() != send.getValue().getType()) {
          send.emitOpError()
              << "value type " << send.getValue().getType()
              << " does not match queue payload type " << queue.getPayload();
          result = failure();
        }
      } else if (auto recv = dyn_cast<TryRecvOp>(operation)) {
        auto queue = lookupQueueRef(recv, recv.getQueue());
        if (queue && queue.getPayload() != recv.getValue().getType()) {
          recv.emitOpError()
              << "result type " << recv.getValue().getType()
              << " does not match queue payload type " << queue.getPayload();
          result = failure();
        }
      } else if (auto schedule = dyn_cast<ScheduleOp>(operation)) {
        auto target = dyn_cast_or_null<ProcessOp>(lookupExpected(
            schedule, schedule.getTarget(), ProcessOp::getOperationName()));
        if (target && (target.getCaptures().size() != 1 ||
                       target.getCaptures().front().getType() !=
                           schedule.getValue().getType())) {
          schedule.emitOpError()
              << "scheduled value type " << schedule.getValue().getType()
              << " must match the target process's single capture type";
          result = failure();
        }
      } else if (auto wait = dyn_cast<WaitForOp>(operation)) {
        (void)lookupExpected(wait, wait.getResource(),
                             ResourceOp::getOperationName());
      } else if (auto await = dyn_cast<AwaitEventOp>(operation)) {
        (void)lookupExpected(await, await.getEventQueue(),
                             EventQueueOp::getOperationName());
      } else if (auto probe = dyn_cast<ProbeOp>(operation)) {
        StringRef expected =
            llvm::StringSwitch<StringRef>(probe.getKind())
                .Case("queue", QueueOp::getOperationName())
                .Case("resource", ResourceOp::getOperationName())
                .Case("module", ProcessOp::getOperationName())
                .Case("storage", AddressSpaceOp::getOperationName())
                .Case("protocol", QueueOp::getOperationName())
                .Case("trace", ProcessOp::getOperationName())
                .Case("event_queue", EventQueueOp::getOperationName())
                .Case("external_io", ProcessOp::getOperationName())
                .Case("statistics", StatOp::getOperationName())
                .Default(StringRef());
        Operation *target = lookupExpected(probe, probe.getTarget(), expected);
        if (auto queue = dyn_cast_or_null<QueueOp>(target);
            queue && probe.getKind() == "queue" &&
            queue.getPayload() != probe.getValue().getType()) {
          probe.emitOpError()
              << "result type " << probe.getValue().getType()
              << " does not match queue payload type " << queue.getPayload();
          result = failure();
        }
        if (auto eventQueue = dyn_cast_or_null<EventQueueOp>(target);
            eventQueue &&
            eventQueue.getPayload() != probe.getValue().getType()) {
          probe.emitOpError() << "result type " << probe.getValue().getType()
                              << " does not match event queue payload type "
                              << eventQueue.getPayload();
          result = failure();
        }
      } else if (auto stat = dyn_cast<StatAddOp>(operation)) {
        (void)lookupExpected(stat, stat.getStat(), StatOp::getOperationName());
      }
      return failed(result) ? WalkResult::interrupt() : WalkResult::advance();
    });
    if (walk.wasInterrupted())
      return failure();
  }
  return result;
}

LogicalResult verifyProcessOperations(ModuleOp module) {
  for (ProcessOp process : module.getBody().front().getOps<ProcessOp>()) {
    if (failed(process.verify()))
      return failure();
    LogicalResult result = success();
    process.getBody().walk([&](Operation *operation) {
      result =
          TypeSwitch<Operation *, LogicalResult>(operation)
              .Case<TrySendOp, TryRecvOp, ScheduleOp, WaitUntilOp, WaitForOp,
                    AwaitEventOp, YieldSimOp, TraceOpenOp, TraceNextOp,
                    TraceDecodeOp, TraceEofOp, TracePositionOp, RequireOp,
                    EnsureOp, AssertOp, ProbeOp, StatAddOp, InstrumentationOp>(
                  [](auto op) { return op.verify(); })
              .Default([](Operation *) { return success(); });
      return failed(result) ? WalkResult::interrupt() : WalkResult::advance();
    });
    if (failed(result))
      return failure();
  }
  return success();
}

Operation *resolveSystemMember(SystemOp system, SymbolRefAttr reference,
                               bool instrumentation) {
  if (reference.getRootReference() != system.getRootAttr().getValue())
    return nullptr;
  ArrayRef<FlatSymbolRefAttr> nested = reference.getNestedReferences();
  if (nested.size() != (instrumentation ? 2u : 1u))
    return nullptr;
  auto file = system->getParentOfType<mlir::ModuleOp>();
  if (!file)
    return nullptr;
  auto module = dyn_cast_or_null<ModuleOp>(
      SymbolTable::lookupSymbolIn(file, system.getRootAttr()));
  if (!module || module.getBody().empty())
    return nullptr;
  ProcessOp process;
  for (ProcessOp candidate : module.getBody().front().getOps<ProcessOp>())
    if (candidate.getSymName() == nested.front().getValue()) {
      process = candidate;
      break;
    }
  if (!process)
    return nullptr;
  if (!instrumentation)
    return process;
  Operation *found = nullptr;
  process.getBody().walk([&](InstrumentationOp candidate) {
    if (candidate.getSymName() == nested.back().getValue()) {
      found = candidate;
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return found;
}

} // namespace

LogicalResult SystemOp::verify() {
  if (failed(verifyOuterPlacement(*this)))
    return failure();
  if (!isStableHierarchySegment(getRootName()))
    return emitOpError(
        "root instance name must be one stable hierarchy segment");
  if (getTickEpoch() != 0)
    return emitOpError("global tick epoch must be exactly 0");
  if (!hasStringValue(getTickUnit(), {"cycle", "ps", "ns", "us", "ms", "s"}))
    return emitOpError() << "unsupported exact global tick unit '"
                         << getTickUnit() << "'";
  auto seedKind = getSeedPolicy().getAs<StringAttr>("kind");
  auto seedValue = getSeedPolicy().getAs<IntegerAttr>("value");
  if (getSeedPolicy().size() != 2 || !seedKind ||
      seedKind.getValue() != "fixed" || !seedValue ||
      !seedValue.getType().isSignlessInteger(64))
    return emitOpError(
        "seed policy requires exact {kind = \"fixed\", value = signless i64} "
        "schema");
  if (seedValue.getInt() < 0)
    return emitOpError("fixed seed value must be a non-negative signless i64");
  auto resultId = getResultSchema().getAs<StringAttr>("id");
  auto resultFormat = getResultSchema().getAs<StringAttr>("format");
  if (getResultSchema().size() != 2 || !resultId ||
      resultId.getValue().empty() || !resultFormat ||
      resultFormat.getValue() != "json")
    return emitOpError("result schema requires exact {id = non-empty string, "
                       "format = \"json\"}");
  if (SymbolRefAttr workload = getPrimaryWorkloadAttr()) {
    auto process = dyn_cast_or_null<ProcessOp>(
        resolveSystemMember(*this, workload, false));
    if (!process)
      return emitOpError() << "primary workload '" << workload
                           << "' is unresolved";
    if (process.getKind() != "workload")
      return emitOpError() << "primary workload '" << workload
                           << "' must reference a workload process";
  }
  for (Attribute value : getInstrumentation()) {
    auto reference = dyn_cast<SymbolRefAttr>(value);
    if (!reference)
      return emitOpError("instrumentation entries must be symbol references");
    Operation *target = resolveSystemMember(*this, reference, true);
    if (!target || target->getName().getStringRef() != "ac.instrumentation")
      return emitOpError() << "instrumentation reference '" << reference
                           << "' does not resolve to ac.instrumentation";
  }
  return success();
}

ParseResult ModuleOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  SmallVector<OpAsmParser::Argument> arguments;
  SmallVector<Type> results;
  SmallVector<DictionaryAttr> resultAttrs;
  bool isVariadic = false;
  if (parser.parseSymbolName(name, mlir::SymbolTable::getSymbolAttrName(),
                             result.attributes) ||
      function_interface_impl::parseFunctionSignatureWithArguments(
          parser, /*allowVariadic=*/false, arguments, isVariadic, results,
          resultAttrs))
    return failure();

  SmallVector<Attribute> argumentAttrs;
  argumentAttrs.reserve(arguments.size());
  bool hasArgumentAttrs = false;
  for (const OpAsmParser::Argument &argument : arguments) {
    DictionaryAttr attrs = argument.attrs;
    hasArgumentAttrs |= static_cast<bool>(attrs) && !attrs.empty();
    argumentAttrs.push_back(attrs ? attrs
                                  : parser.getBuilder().getDictionaryAttr({}));
  }
  if (hasArgumentAttrs)
    result.addAttribute("arg_attrs",
                        parser.getBuilder().getArrayAttr(argumentAttrs));
  if (llvm::any_of(resultAttrs, [](DictionaryAttr attrs) {
        return attrs && !attrs.empty();
      })) {
    SmallVector<Attribute> normalizedResultAttrs;
    normalizedResultAttrs.reserve(resultAttrs.size());
    for (DictionaryAttr attrs : resultAttrs)
      normalizedResultAttrs.push_back(
          attrs ? attrs : parser.getBuilder().getDictionaryAttr({}));
    result.addAttribute(
        "res_attrs", parser.getBuilder().getArrayAttr(normalizedResultAttrs));
  }

  DictionaryAttr staticParameters;
  if (succeeded(parser.parseOptionalKeyword("parameters"))) {
    if (parser.parseAttribute(staticParameters))
      return failure();
  } else {
    staticParameters = parser.getBuilder().getDictionaryAttr({});
  }
  result.addAttribute("static_params", staticParameters);
  if (parser.parseOptionalAttrDictWithKeyword(result.attributes) ||
      parser.parseKeyword("graph"))
    return failure();

  SmallVector<Type> inputs;
  inputs.reserve(arguments.size());
  for (const OpAsmParser::Argument &argument : arguments)
    inputs.push_back(argument.type);
  result.addAttribute(
      "function_type",
      TypeAttr::get(parser.getBuilder().getFunctionType(inputs, results)));
  Region *body = result.addRegion();
  return parser.parseRegion(*body, arguments, /*enableNameShadowing=*/false);
}

void ModuleOp::print(OpAsmPrinter &printer) {
  printer << ' ';
  printer.printSymbolName(getSymName());
  function_interface_impl::printFunctionSignature(
      printer, *this, getArgumentTypes(), /*isVariadic=*/false,
      getResultTypes());
  printer << " parameters " << getStaticParams();
  printer.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(),
      {mlir::SymbolTable::getSymbolAttrName(), "function_type", "static_params",
       "arg_attrs", "res_attrs"});
  printer << " graph ";
  printer.printRegion(getBody(), /*printEntryBlockArgs=*/false,
                      /*printBlockTerminators=*/true,
                      /*printEmptyBlock=*/true);
}

LogicalResult ModuleOp::verify() {
  if (failed(verifyOuterPlacement(*this)))
    return failure();
  if (failed(verifyConcreteDictionary(*this, getStaticParams(),
                                      "static parameters")))
    return failure();
  if (getBody().empty())
    return emitOpError("module requires one Graph body block");
  Block &entry = getBody().front();
  if (!llvm::equal(entry.getArgumentTypes(), getFunctionType().getInputs()))
    return emitOpError("Graph region arguments must match module signature");
  llvm::StringSet<> localNames;
  llvm::StringMap<Operation *> producerIndex;
  llvm::StringSet<> stableIds;
  llvm::StringSet<> paths;
  for (Operation &child : entry) {
    if (!isStructuralGraphChild(child))
      return child.emitOpError(
          "operation is not legal in an ac.module structural Graph region");
    StringAttr localName;
    StringAttr stableId;
    StringAttr path;
    if (auto instance = dyn_cast<InstanceOp>(child)) {
      localName = instance.getSymNameAttr();
      stableId = instance.getStableIdAttr();
      path = instance.getPathAttr();
    } else if (auto array = dyn_cast<ArrayOp>(child)) {
      localName = array.getSymNameAttr();
      stableId = array.getStableIdAttr();
      path = array.getPathAttr();
    } else if (auto instances = dyn_cast<InstancesOp>(child)) {
      localName = instances.getSymNameAttr();
      stableId = instances.getStableIdAttr();
      path = instances.getPathAttr();
    } else if (auto view = dyn_cast<ViewOp>(child)) {
      localName = view.getSymNameAttr();
    } else if (auto queue = dyn_cast<QueueOp>(child)) {
      localName = queue.getSymNameAttr();
      stableId = queue.getStableIdAttr();
      path = queue.getPathAttr();
    } else if (auto eventQueue = dyn_cast<EventQueueOp>(child)) {
      localName = eventQueue.getSymNameAttr();
      stableId = eventQueue.getStableIdAttr();
      path = eventQueue.getPathAttr();
    } else if (auto resource = dyn_cast<ResourceOp>(child)) {
      localName = resource.getSymNameAttr();
      stableId = resource.getStableIdAttr();
      path = resource.getPathAttr();
    } else if (auto addressSpace = dyn_cast<AddressSpaceOp>(child)) {
      localName = addressSpace.getSymNameAttr();
      stableId = addressSpace.getStableIdAttr();
      path = addressSpace.getPathAttr();
    } else if (auto addressMap = dyn_cast<AddressMapOp>(child)) {
      localName = addressMap.getSymNameAttr();
    } else if (auto timeDomain = dyn_cast<TimeDomainOp>(child)) {
      localName = timeDomain.getSymNameAttr();
    } else if (auto process = dyn_cast<ProcessOp>(child)) {
      localName = process.getSymNameAttr();
    } else if (auto stat = dyn_cast<StatOp>(child)) {
      localName = stat.getSymNameAttr();
    }
    if (localName && !localNames.insert(localName.getValue()).second)
      return child.emitOpError() << "duplicate local structural name '"
                                 << localName.getValue() << "'";
    if (localName)
      producerIndex.try_emplace(localName.getValue(), &child);
    if (stableId && !stableIds.insert(stableId.getValue()).second)
      return child.emitOpError() << "duplicate local structural stable id '"
                                 << stableId.getValue() << "'";
    if (path && !paths.insert(path.getValue()).second)
      return child.emitOpError()
             << "duplicate local structural path '" << path.getValue() << "'";
  }
  if (entry.empty() || !isa<ReturnOp>(entry.back()))
    return emitOpError("module Graph region must end with ac.return");
  llvm::StringMap<Operation *> traceSources;
  for (ProcessOp process : entry.getOps<ProcessOp>()) {
    WalkResult result = process.getBody().walk([&](TraceOpenOp trace) {
      if (!traceSources.try_emplace(trace.getSource(), trace).second) {
        trace.emitOpError() << "trace source '" << trace.getSource()
                            << "' must have exactly one cursor owner";
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    if (result.wasInterrupted())
      return failure();
  }
  for (Operation &child : entry) {
    LogicalResult local = TypeSwitch<Operation *, LogicalResult>(&child)
                              .Case<QueueOp, EventQueueOp, ResourceOp,
                                    AddressSpaceOp, AddressMapOp, TimeDomainOp>(
                                  [](auto op) { return op.verify(); })
                              .Default([](Operation *) { return success(); });
    if (failed(local))
      return failure();
  }
  if (failed(verifyModuleResourceReferences(*this, producerIndex)))
    return failure();
  if (failed(verifyProcessOperations(*this)))
    return failure();
  if (failed(verifyRuntimeReferences(*this, producerIndex)))
    return failure();
  for (ViewOp view : entry.getOps<ViewOp>())
    if (failed(view.verify()) ||
        failed(view.verifyWithProducerIndex(producerIndex)))
      return failure();
  return success();
}

LogicalResult ModuleExternOp::verify() {
  if (failed(verifyOuterPlacement(*this)))
    return failure();
  if (failed(verifyConcreteDictionary(*this, getStaticParams(),
                                      "static parameters")))
    return failure();
  if (failed(verifyExactBinding(*this, getImplementation(),
                                "external module implementation", "cpp")))
    return failure();
  StringRef name = getImplementation().getAs<StringAttr>("name").getValue();
  if (!getStructuralProviderRegistry(getContext()).hasExternal(name))
    return emitOpError() << "structural provider 'cpp:" << name
                         << "' is not registered";
  return success();
}

LogicalResult InstanceOp::verify() {
  if (failed(verifyStructuralPlacement(*this)))
    return failure();
  Operation *definition = lookupGraphSymbol(*this, getDefinitionAttr());
  if (!isa_and_nonnull<ModuleOp, ModuleExternOp>(definition))
    return emitOpError() << "unresolved module definition '"
                         << getDefinitionAttr() << "'";
  if (!isStableHierarchySegment(getSymName()) ||
      !isStableHierarchySegment(getStableId()) ||
      !isStableHierarchySegment(getPath()))
    return emitOpError(
        "instance name, stable id, and path must be stable local segments");
  if (failed(verifyStaticArgumentSet(*this, getStaticArgs(), definition)))
    return failure();
  return verifyCallShape(*this, graphSignature(definition),
                         getInputs().getTypes(), getOutputs().getTypes());
}

LogicalResult ArrayOp::verify() {
  if (failed(verifyStructuralPlacement(*this)))
    return failure();
  Operation *definition = lookupGraphSymbol(*this, getDefinitionAttr());
  if (!isa_and_nonnull<ModuleOp, ModuleExternOp>(definition))
    return emitOpError() << "unresolved array element definition '"
                         << getDefinitionAttr() << "'";
  if (!isStableHierarchySegment(getSymName()) ||
      !isStableHierarchySegment(getStableId()) ||
      !isStableHierarchySegment(getPath()))
    return emitOpError(
        "array name, stable id, and path must be stable local segments");
  if (getShape().empty())
    return emitOpError("array shape must have at least one dimension");
  uint64_t count = 1;
  for (int64_t extent : getShape()) {
    if (extent < 0)
      return emitOpError("array shape dimensions must be non-negative");
    if (extent != 0 && count > std::numeric_limits<uint64_t>::max() /
                                   static_cast<uint64_t>(extent))
      return emitOpError("array cardinality overflows 64 bits");
    count *= static_cast<uint64_t>(extent);
  }
  constexpr uint64_t maxStaticElements = 1U << 20;
  if (count > maxStaticElements)
    return emitOpError(
        "array cardinality exceeds static elaboration bound 1048576");
  FunctionType signature = graphSignature(definition);
  if (!signature)
    return emitOpError("array element definition has no signature");
  if (getStaticArgs().size() != count)
    return emitOpError("array requires one concrete static argument set per "
                       "lexicographically ordered element");
  for (Attribute value : getStaticArgs()) {
    auto arguments = dyn_cast<DictionaryAttr>(value);
    if (!arguments ||
        failed(verifyStaticArgumentSet(*this, arguments, definition)))
      return emitOpError(
          "array static arguments must be concrete dictionaries");
  }
  auto matchesRepeatedSignature = [count](TypeRange actual,
                                          TypeRange elementTypes) {
    if (elementTypes.empty())
      return actual.empty();
    if (count > std::numeric_limits<uint64_t>::max() / elementTypes.size() ||
        actual.size() != count * elementTypes.size())
      return false;
    for (auto [index, type] : llvm::enumerate(actual))
      if (type != elementTypes[index % elementTypes.size()])
        return false;
    return true;
  };
  if (!matchesRepeatedSignature(getInputs().getTypes(),
                                signature.getInputs()) ||
      !matchesRepeatedSignature(getOutputs().getTypes(),
                                signature.getResults()))
    return emitOpError("array flattened interface shape does not match element "
                       "signature and static cardinality");
  return success();
}

LogicalResult InstancesOp::verify() {
  if (failed(verifyStructuralPlacement(*this)))
    return failure();
  if (!isStableHierarchySegment(getSymName()) ||
      !isStableHierarchySegment(getStableId()) ||
      !isStableHierarchySegment(getPath()))
    return emitOpError("collection name, stable id, and path must be stable "
                       "local segments");
  size_t count = getDefinitions().size();
  if (count == 0 || getNames().size() != count ||
      getStableIds().size() != count || getPaths().size() != count ||
      getStaticArgs().size() != count)
    return emitOpError("ordered instance metadata arrays must have identical "
                       "non-zero cardinality");
  llvm::SmallDenseSet<StringRef> names;
  llvm::SmallDenseSet<StringRef> ids;
  llvm::SmallDenseSet<StringRef> paths;
  for (size_t index = 0; index < count; ++index) {
    auto definition = dyn_cast<FlatSymbolRefAttr>(getDefinitions()[index]);
    if (!definition)
      return emitOpError("definitions must contain flat module symbols");
    Operation *target = lookupGraphSymbol(*this, definition);
    if (!isa_and_nonnull<ModuleOp, ModuleExternOp>(target))
      return emitOpError() << "unresolved collection definition '" << definition
                           << "'";
    if (graphSignature(target) != getInterface())
      return emitOpError("collection element definition does not implement "
                         "the exact declared common interface");
    auto arguments = dyn_cast<DictionaryAttr>(getStaticArgs()[index]);
    if (!arguments || failed(verifyStaticArgumentSet(*this, arguments, target)))
      return emitOpError("collection static arguments must be concrete "
                         "dictionaries");
    StringRef name = cast<StringAttr>(getNames()[index]).getValue();
    StringRef id = cast<StringAttr>(getStableIds()[index]).getValue();
    StringRef path = cast<StringAttr>(getPaths()[index]).getValue();
    if (!isStableHierarchySegment(name) || !names.insert(name).second ||
        !isStableHierarchySegment(id) || !ids.insert(id).second)
      return emitOpError("collection names and stable ids must be non-empty "
                         "and unique in declared order");
    if (!isStableHierarchySegment(path) || !paths.insert(path).second)
      return emitOpError("collection paths must be stable, unique "
                         "parent-relative segments");
  }
  auto matchesRepeatedInterface = [count](TypeRange actual,
                                          TypeRange elementTypes) {
    if (elementTypes.empty())
      return actual.empty();
    if (count > std::numeric_limits<size_t>::max() / elementTypes.size() ||
        actual.size() != count * elementTypes.size())
      return false;
    for (auto [index, type] : llvm::enumerate(actual))
      if (type != elementTypes[index % elementTypes.size()])
        return false;
    return true;
  };
  if (!matchesRepeatedInterface(getInputs().getTypes(),
                                getInterface().getInputs()) ||
      !matchesRepeatedInterface(getOutputs().getTypes(),
                                getInterface().getResults()))
    return emitOpError("ordered collection IO does not match its common "
                       "interface shape");
  return success();
}

LogicalResult ViewOp::verify() {
  if (failed(verifyStructuralPlacement(*this)))
    return failure();
  if (!isStableHierarchySegment(getSymName()))
    return emitOpError("view name must be a stable local segment");
  return success();
}

LogicalResult ViewOp::verifyWithProducerIndex(
    const llvm::StringMap<Operation *> &producerIndex) {
  ArrayRef<int64_t> indices = getIndices();
  ArrayRef<int64_t> shape = getShape();
  if (llvm::any_of(shape, [](int64_t value) { return value < 0; }))
    return emitOpError("view shape must be fully static and non-negative");
  auto checkedProduct = [&](ArrayRef<int64_t> dimensions,
                            uint64_t &product) -> LogicalResult {
    product = 1;
    for (int64_t extent : dimensions) {
      if (extent < 0)
        return emitOpError(
            "source shapes must be fully static and non-negative");
      if (extent != 0 && product > std::numeric_limits<uint64_t>::max() /
                                       static_cast<uint64_t>(extent))
        return emitOpError("view cardinality overflows 64 bits");
      product *= static_cast<uint64_t>(extent);
    }
    return success();
  };

  SmallVector<SmallVector<int64_t>> sourceShapes;
  SmallVector<SmallVector<Value>> sources;
  llvm::SmallDenseSet<Operation *> sourceProducers;
  auto getProducerShape = [&](Operation *producer) {
    SmallVector<int64_t> producerShape;
    if (auto instance = dyn_cast<InstanceOp>(producer))
      producerShape.push_back(instance.getNumResults());
    else if (auto array = dyn_cast<ArrayOp>(producer)) {
      producerShape.append(array.getShape().begin(), array.getShape().end());
      producerShape.push_back(
          graphSignature(lookupGraphSymbol(array, array.getDefinitionAttr()))
              .getNumResults());
    } else if (auto instances = dyn_cast<InstancesOp>(producer)) {
      producerShape.push_back(instances.getDefinitions().size());
      producerShape.push_back(instances.getInterface().getNumResults());
    } else if (auto view = dyn_cast<ViewOp>(producer))
      producerShape.append(view.getShape().begin(), view.getShape().end());
    return producerShape;
  };
  if (getSourceProducers().size() != getSourceShapes().size())
    return emitOpError(
        "source_producers and source_shapes must have identical cardinality");
  size_t operandOffset = 0;
  for (auto [producerReference, attribute] :
       llvm::zip(getSourceProducers(), getSourceShapes())) {
    auto sourceShape = dyn_cast<DenseI64ArrayAttr>(attribute);
    if (!sourceShape)
      return emitOpError("source_shapes entries must be dense i64 arrays");
    uint64_t cardinality = 0;
    if (failed(checkedProduct(sourceShape.asArrayRef(), cardinality)))
      return failure();
    if (cardinality > getInputs().size() - operandOffset)
      return emitOpError("source_shapes do not partition the view operands");
    SmallVector<Value> source;
    source.append(getInputs().begin() + operandOffset,
                  getInputs().begin() + operandOffset + cardinality);
    operandOffset += cardinality;
    auto producerSymbol = cast<FlatSymbolRefAttr>(producerReference);
    if (!isStableHierarchySegment(producerSymbol.getValue()))
      return emitOpError("source producer IDs must be stable local segments");
    Operation *producer = producerIndex.lookup(producerSymbol.getValue());
    if (!producer)
      return emitOpError() << "source producer '" << producerReference
                           << "' is unresolved";
    if (producer == getOperation())
      return emitOpError("view cannot name itself as a source producer");
    if (!isa<InstanceOp, ArrayOp, InstancesOp, ViewOp>(producer) ||
        producer->getBlock() != getOperation()->getBlock())
      return emitOpError("source producer must resolve to a direct structural "
                         "producer in the same ac.module");
    if (!sourceProducers.insert(producer).second)
      return emitOpError("source producers must not repeat");
    if (!source.empty()) {
      if (source.front().getDefiningOp() != producer ||
          producer->getNumResults() != source.size())
        return emitOpError(
            "each source must be the complete result group of its declared "
            "structural producer");
      for (auto [index, value] : llvm::enumerate(source))
        if (value != producer->getResult(index))
          return emitOpError(
              "source operands must preserve producer result order");
    }
    SmallVector<int64_t> producerShape = getProducerShape(producer);
    if (producerShape != sourceShape.asArrayRef())
      return emitOpError("source_shapes must exactly match producer shapes");
    sourceShapes.emplace_back(sourceShape.asArrayRef().begin(),
                              sourceShape.asArrayRef().end());
    sources.push_back(std::move(source));
  }
  if (operandOffset != getInputs().size())
    return emitOpError("source_shapes do not partition the view operands");

  SmallVector<Value> expected;
  StringRef kind = getKind();
  if (kind == "select") {
    if (sources.size() != 1 || indices.size() != sourceShapes[0].size() ||
        !shape.empty() || getAxisAttr())
      return emitOpError("select requires one source, one coordinate per "
                         "dimension, scalar shape, and no axis");
    uint64_t ordinal = 0;
    for (auto [coordinate, extent] : llvm::zip(indices, sourceShapes[0])) {
      if (coordinate < 0 || coordinate >= extent)
        return emitOpError("select coordinate is out of bounds");
      ordinal = ordinal * static_cast<uint64_t>(extent) + coordinate;
    }
    expected.push_back(sources[0][ordinal]);
  } else if (kind == "slice") {
    if (sources.size() != 1 || getAxisAttr() ||
        indices.size() != 2 * sourceShapes[0].size())
      return emitOpError("slice requires one source and [lower, upper] bounds "
                         "for every dimension");
    SmallVector<int64_t> derivedShape;
    for (size_t dimension = 0; dimension < sourceShapes[0].size();
         ++dimension) {
      int64_t lower = indices[2 * dimension];
      int64_t upper = indices[2 * dimension + 1];
      if (lower < 0 || upper < lower || upper > sourceShapes[0][dimension])
        return emitOpError("slice bounds are invalid");
      derivedShape.push_back(upper - lower);
    }
    if (derivedShape != shape)
      return emitOpError("slice result shape must equal its bound extents");
    for (size_t ordinal = 0; ordinal < sources[0].size(); ++ordinal) {
      size_t remainder = ordinal;
      bool included = true;
      for (size_t reverse = sourceShapes[0].size(); reverse-- > 0;) {
        int64_t coordinate = remainder % sourceShapes[0][reverse];
        remainder /= sourceShapes[0][reverse];
        included &= coordinate >= indices[2 * reverse] &&
                    coordinate < indices[2 * reverse + 1];
      }
      if (included)
        expected.push_back(sources[0][ordinal]);
    }
  } else if (kind == "concat") {
    if (sources.size() < 2 || !indices.empty() || !getAxisAttr())
      return emitOpError("concat requires at least two sources, an axis, and "
                         "no index metadata");
    int64_t axis = getAxisAttr().getInt();
    size_t rank = sourceShapes.front().size();
    if (axis < 0 || static_cast<size_t>(axis) >= rank)
      return emitOpError("concat axis is out of bounds");
    SmallVector<int64_t> derivedShape = sourceShapes.front();
    derivedShape[axis] = 0;
    for (ArrayRef<int64_t> sourceShape : sourceShapes) {
      if (sourceShape.size() != rank)
        return emitOpError("concat source ranks must match");
      for (size_t dimension = 0; dimension < rank; ++dimension)
        if (dimension != static_cast<size_t>(axis) &&
            sourceShape[dimension] != derivedShape[dimension])
          return emitOpError("concat non-axis dimensions must match");
      if (sourceShape[axis] >
          std::numeric_limits<int64_t>::max() - derivedShape[axis])
        return emitOpError("concat axis extent overflows signed i64");
      derivedShape[axis] += sourceShape[axis];
    }
    if (derivedShape != shape)
      return emitOpError("concat result shape is not derived from its sources");
    uint64_t outer = 1, inner = 1;
    for (int64_t extent : ArrayRef<int64_t>(shape).take_front(axis))
      outer *= extent;
    for (int64_t extent : ArrayRef<int64_t>(shape).drop_front(axis + 1))
      inner *= extent;
    for (uint64_t outerIndex = 0; outerIndex < outer; ++outerIndex)
      for (auto [sourceIndex, source] : llvm::enumerate(sources)) {
        uint64_t chunk = sourceShapes[sourceIndex][axis] * inner;
        expected.append(source.begin() + outerIndex * chunk,
                        source.begin() + (outerIndex + 1) * chunk);
      }
  } else if (kind == "zip") {
    if (sources.size() != 2 || !indices.empty() || getAxisAttr() ||
        sourceShapes[0] != sourceShapes[1])
      return emitOpError("zip requires two equal-shaped sources and no axis or "
                         "index metadata");
    SmallVector<int64_t> derivedShape = sourceShapes[0];
    derivedShape.push_back(2);
    if (derivedShape != shape)
      return emitOpError("zip result shape must append source count");
    for (size_t index = 0; index < sources[0].size(); ++index) {
      expected.push_back(sources[0][index]);
      expected.push_back(sources[1][index]);
    }
  } else if (kind == "permutation") {
    if (sources.size() != 1 || getAxisAttr() ||
        !llvm::equal(shape, sourceShapes[0]) ||
        indices.size() != sources[0].size())
      return emitOpError("permutation requires one source, unchanged shape, "
                         "and one index per element");
    llvm::SmallDenseSet<int64_t> seen;
    for (int64_t index : indices) {
      if (index < 0 || static_cast<size_t>(index) >= sources[0].size() ||
          !seen.insert(index).second)
        return emitOpError(
            "permutation indices must be an in-bounds bijection");
      expected.push_back(sources[0][index]);
    }
  } else if (kind == "elementwise") {
    if (sources.size() < 2 || !indices.empty() || getAxisAttr())
      return emitOpError("elementwise requires at least two sources and no "
                         "axis or index metadata");
    if (llvm::any_of(sourceShapes, [&](const auto &sourceShape) {
          return !llvm::equal(sourceShape, sourceShapes.front());
        }))
      return emitOpError("elementwise source shapes must match");
    SmallVector<int64_t> derivedShape = sourceShapes.front();
    derivedShape.push_back(sources.size());
    if (derivedShape != shape)
      return emitOpError("elementwise result shape must append source count");
    for (size_t index = 0; index < sources.front().size(); ++index)
      for (ArrayRef<Value> source : sources)
        expected.push_back(source[index]);
  } else {
    return emitOpError() << "unsupported static view kind '" << kind << "'";
  }
  uint64_t cardinality = 0;
  if (failed(checkedProduct(shape, cardinality)))
    return failure();
  if (cardinality != expected.size() ||
      !llvm::equal(getOutputs().getTypes(),
                   llvm::map_range(
                       expected, [](Value value) { return value.getType(); })))
    return emitOpError("resolved view shape/order/types do not match outputs");
  return success();
}

LogicalResult ReturnOp::verify() {
  ModuleOp module = getOperation()->getParentOfType<ModuleOp>();
  if (!module)
    return emitOpError("must terminate an ac.module Graph region");
  if (!llvm::equal(getOperandTypes(), module.getFunctionType().getResults()))
    return emitOpError("operand types and count must exactly match module "
                       "results");
  if (llvm::any_of(getOperandTypes(),
                   [](Type type) { return isa<ResourceTokenType>(type); }))
    return emitOpError(
        "private ownership handle cannot be exported from ac.module");
  return success();
}

LogicalResult verifyTopologyTypeUses(Operation *operation) {
  if (failed(verifyGraphStructure(operation)))
    return failure();
  std::function<LogicalResult(Type, Value)> verifyType =
      [&](Type type, Value value) -> LogicalResult {
    if (auto function = dyn_cast<FunctionType>(type)) {
      for (Type input : function.getInputs())
        if (failed(verifyType(input, {})))
          return failure();
      for (Type result : function.getResults())
        if (failed(verifyType(result, {})))
          return failure();
      return success();
    }
    if (Type nested = findNestedTopologyLeaf(type))
      return operation->emitOpError() << "topology type " << nested
                                      << " cannot be nested inside " << type;
    if (auto flow = dyn_cast<FlowType>(type)) {
      if (!isProtocolPayloadType(flow.getElementType()))
        return operation->emitOpError(
            "flow payload type must be a normative ACIR value type");
      if (failed(verifyNamedTypes(operation, flow.getElementType())))
        return failure();
      ProtocolOp protocol = lookupProtocol(operation, flow.getProtocol());
      if (!protocol)
        return operation->emitOpError() << "unresolved flow protocol '@"
                                        << flow.getProtocol().getValue() << "'";
      if (!matchesCarrierEvent(protocol, flow.getElementType()))
        return operation->emitOpError()
               << "flow payload " << flow.getElementType()
               << " does not match any carrier event in protocol '@"
               << flow.getProtocol().getValue() << "'";
      if (value && !value.hasOneUse() && !value.use_empty())
        return operation->emitOpError(
            "flow value has more than one functional use");
    }
    if (auto endpoint = dyn_cast<EndpointType>(type)) {
      auto module = operation->getParentOfType<mlir::ModuleOp>();
      InterfaceOp interface =
          module ? dyn_cast_or_null<InterfaceOp>(SymbolTable::lookupSymbolIn(
                       module, endpoint.getInterface()))
                 : InterfaceOp();
      if (!interface)
        return operation->emitOpError()
               << "unresolved endpoint interface '@"
               << endpoint.getInterface().getValue() << "'";
      RoleOp role = lookupChild<RoleOp>(interface, endpoint.getRole());
      if (!role)
        return operation->emitOpError()
               << "endpoint role '@" << endpoint.getRole().getValue()
               << "' is not a member of interface '@"
               << endpoint.getInterface().getValue() << "'";
      if (role.getCardinality() == "exclusive" && value && !value.hasOneUse() &&
          !value.use_empty())
        return operation->emitOpError(
            "exclusive endpoint value has more than one structural use");
    }
    return success();
  };

  auto verifyAttribute = [&](Attribute attribute) -> LogicalResult {
    if (!attribute)
      return success();
    LogicalResult result = success();
    attribute.walk([&](TypeAttr type) {
      if (failed(verifyType(type.getValue(), {}))) {
        result = failure();
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    if (failed(result))
      return failure();
    attribute.walk([&](Type type) {
      if (failed(verifyType(type, {}))) {
        result = failure();
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    });
    return result;
  };

  for (Value result : operation->getResults())
    if (failed(verifyType(result.getType(), result)))
      return failure();
  for (OpOperand &operand : operation->getOpOperands())
    if (failed(verifyType(operand.get().getType(), operand.get())))
      return failure();
  for (Region &region : operation->getRegions())
    for (Block &block : region)
      for (BlockArgument argument : block.getArguments())
        if (failed(verifyType(argument.getType(), argument)))
          return failure();
  for (NamedAttribute attribute : operation->getAttrs())
    if (failed(verifyAttribute(attribute.getValue())))
      return failure();
  if (failed(verifyAttribute(operation->getPropertiesAsAttribute())) ||
      failed(verifyAttribute(LocationAttr(operation->getLoc()))))
    return failure();
  return success();
}

namespace {

SymbolRefAttr qualifiedRuntimeOwner(Operation *operation, StringRef local) {
  if (auto definition = operation->getParentOfType<ModuleOp>())
    return SymbolRefAttr::get(
        operation->getContext(), definition.getSymName(),
        {FlatSymbolRefAttr::get(operation->getContext(), local)});
  return SymbolRefAttr::get(operation->getContext(), local);
}

Operation *resolvedRuntimeTarget(Operation *operation, StringRef local) {
  ModuleOp module = operation->getParentOfType<ModuleOp>();
  if (!module || module.getBody().empty())
    return nullptr;
  for (Operation &candidate : module.getBody().front()) {
    auto name =
        candidate.getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
    if (name && name.getValue() == local)
      return &candidate;
  }
  return nullptr;
}

DictionaryAttr runtimeEffectParameters(Operation *operation, StringRef kind,
                                       StringRef identity) {
  Builder builder(operation->getContext());
  Operation *owner = resolvedRuntimeTarget(operation, identity);
  if (!owner)
    if (auto process = operation->getParentOfType<ProcessOp>())
      owner = process;
  if (owner)
    if (auto owners = owner->getAttrOfType<ArrayAttr>("ac.frozen_owners"))
      return builder.getDictionaryAttr({
          builder.getNamedAttr("identity_phase",
                               builder.getStringAttr("elaborated_absolute")),
          builder.getNamedAttr("owner_kind", builder.getStringAttr(kind)),
          builder.getNamedAttr("owners", owners),
      });
  return builder.getDictionaryAttr({
      builder.getNamedAttr("identity_phase",
                           builder.getStringAttr("definition_pre_freeze")),
      builder.getNamedAttr("owner_kind", builder.getStringAttr(kind)),
      builder.getNamedAttr("identity", builder.getStringAttr(identity)),
  });
}

void addEffect(SmallVectorImpl<MemoryEffects::EffectInstance> &effects,
               Operation *operation, MemoryEffects::Effect *effect,
               StringRef identity, StringRef kind,
               SideEffects::Resource *resource) {
  effects.emplace_back(effect, qualifiedRuntimeOwner(operation, identity),
                       runtimeEffectParameters(operation, kind, identity),
                       resource);
}

DictionaryAttr contractEffectParameters(Operation *operation, StringRef phase,
                                        StringRef identity) {
  Builder builder(operation->getContext());
  if (auto process = operation->getParentOfType<ProcessOp>())
    if (auto owners = process->getAttrOfType<ArrayAttr>("ac.frozen_owners"))
      return builder.getDictionaryAttr({
          builder.getNamedAttr("identity_phase",
                               builder.getStringAttr("elaborated_absolute")),
          builder.getNamedAttr("owner_kind", builder.getStringAttr("contract")),
          builder.getNamedAttr("owners", owners),
          builder.getNamedAttr("contract_phase", builder.getStringAttr(phase)),
      });
  if (operation->getAttrOfType<BoolAttr>("ac.freeze_proven"))
    return builder.getDictionaryAttr({
        builder.getNamedAttr("identity_phase",
                             builder.getStringAttr("elaborated_absolute")),
        builder.getNamedAttr("owner_kind", builder.getStringAttr("contract")),
        builder.getNamedAttr("identity", builder.getStringAttr(identity)),
        builder.getNamedAttr("contract_phase", builder.getStringAttr(phase)),
        builder.getNamedAttr("freeze_proven", builder.getBoolAttr(true)),
    });
  return builder.getDictionaryAttr({
      builder.getNamedAttr("identity_phase",
                           builder.getStringAttr("definition_pre_freeze")),
      builder.getNamedAttr("owner_kind", builder.getStringAttr("contract")),
      builder.getNamedAttr("identity", builder.getStringAttr(identity)),
      builder.getNamedAttr("contract_phase", builder.getStringAttr(phase)),
  });
}

ProcessOp enclosingProcess(Operation *operation) {
  return operation->getParentOfType<ProcessOp>();
}

LogicalResult requireProcess(Operation *operation) {
  if (enclosingProcess(operation))
    return success();
  return operation->emitOpError("must be nested in ac.process");
}

StringRef processIdentity(Operation *operation) {
  ProcessOp process = enclosingProcess(operation);
  return process ? process.getSymName() : StringRef("invalid_process");
}

void addContractEffect(SmallVectorImpl<MemoryEffects::EffectInstance> &effects,
                       Operation *operation) {
  if (!isa<AssertOp>(operation) &&
      isa_and_nonnull<ModuleOp>(operation->getParentOp())) {
    constexpr StringLiteral identity = "contracts";
    effects.emplace_back(
        MemoryEffects::Read::get(), qualifiedRuntimeOwner(operation, identity),
        contractEffectParameters(operation, "topology_freeze", identity),
        ModuleStateResource::get());
    return;
  }
  StringRef identity = processIdentity(operation);
  effects.emplace_back(MemoryEffects::Write::get(),
                       qualifiedRuntimeOwner(operation, identity),
                       contractEffectParameters(operation, "runtime", identity),
                       ExternalIOResource::get());
}

std::string traceOwnerIdentity(Operation *operation, StringRef trace) {
  return (processIdentity(operation) + "/" + trace).str();
}

bool isSuspension(Operation *operation) {
  return isa<WaitUntilOp, WaitForOp, AwaitEventOp, YieldSimOp>(operation);
}

bool isLinearAcrossSuspension(Type type) {
  return isa<FlowType, ResourceTokenType>(type);
}

bool isAllowedProcessOperation(Operation *operation) {
  StringRef name = operation->getName().getStringRef();
  if (name.starts_with("arith.") || name.starts_with("index.") ||
      name == "func.call" ||
      isa<scf::IfOp, scf::ForOp, scf::WhileOp, scf::ConditionOp, scf::YieldOp>(
          operation))
    return true;
  return isa<TrySendOp, TryRecvOp, ScheduleOp, WaitUntilOp, WaitForOp,
             AwaitEventOp, YieldSimOp, TraceOpenOp, TraceNextOp,
             TraceDecodeOp, TraceEofOp, TracePositionOp, RequireOp, EnsureOp,
             AssertOp, ProbeOp, StatAddOp, InstrumentationOp>(operation);
}

std::optional<bool> constantBool(Value value) {
  Operation *definition = value.getDefiningOp();
  if (!definition)
    return std::nullopt;
  Attribute attribute = definition->getAttr("value");
  if (auto boolean = dyn_cast_or_null<BoolAttr>(attribute))
    return boolean.getValue();
  if (auto integer = dyn_cast_or_null<IntegerAttr>(attribute);
      integer && integer.getType().isInteger(1))
    return integer.getValue().getBoolValue();
  return std::nullopt;
}

LogicalResult verifySupportedSCFShape(ProcessOp process) {
  LogicalResult result = success();
  process.getBody().walk([&](Operation *operation) {
    auto requireTerminator = [&](Region &region, StringRef owner,
                                 StringRef terminator) -> Operation * {
      if (region.empty() || !llvm::hasSingleElement(region) ||
          region.front().empty() ||
          region.front().back().getName().getStringRef() != terminator) {
        operation->emitOpError()
            << "malformed " << owner << " region must terminate with "
            << terminator;
        result = failure();
        return nullptr;
      }
      return &region.front().back();
    };
    auto sameTypes = [](auto left, auto right) {
      if (left.size() != right.size())
        return false;
      return llvm::all_of(llvm::zip(left, right), [](auto pair) {
        return std::get<0>(pair).getType() == std::get<1>(pair).getType();
      });
    };
    auto emitArityError = [&](StringRef owner) {
      operation->emitOpError()
          << "malformed " << owner
          << " operand/result/block argument/yield arity or type mismatch";
      result = failure();
      return WalkResult::interrupt();
    };
    if (isa<scf::IfOp>(operation)) {
      if (operation->getNumRegions() != 2) {
        operation->emitOpError(
            "malformed scf.if region must terminate with scf.yield");
        result = failure();
        return WalkResult::interrupt();
      }
      Region &thenRegion = operation->getRegion(0);
      Region &elseRegion = operation->getRegion(1);
      Operation *thenYield =
          requireTerminator(thenRegion, "scf.if", "scf.yield");
      Operation *elseYield =
          elseRegion.empty()
              ? nullptr
              : requireTerminator(elseRegion, "scf.if", "scf.yield");
      if (!thenYield || (!elseRegion.empty() && !elseYield))
        return WalkResult::interrupt();
      if (operation->getNumOperands() != 1 ||
          !operation->getOperand(0).getType().isInteger(1) ||
          !thenRegion.front().getArguments().empty() ||
          (!elseRegion.empty() && !elseRegion.front().getArguments().empty()) ||
          thenYield->getNumResults() != 0 ||
          (elseYield && elseYield->getNumResults() != 0) ||
          !sameTypes(thenYield->getOperands(), operation->getResults()) ||
          (elseRegion.empty()
               ? operation->getNumResults() != 0
               : !sameTypes(elseYield->getOperands(), operation->getResults())))
        return emitArityError("scf.if");
    } else if (isa<scf::ForOp>(operation)) {
      if (operation->getNumRegions() != 1)
        return emitArityError("scf.for");
      Region &body = operation->getRegion(0);
      Operation *yield = requireTerminator(body, "scf.for", "scf.yield");
      if (!yield)
        return WalkResult::interrupt();
      unsigned resultCount = operation->getNumResults();
      if (operation->getNumOperands() < 3 ||
          operation->getNumOperands() != resultCount + 3 ||
          body.front().getNumArguments() != resultCount + 1)
        return emitArityError("scf.for");
      Type inductionType = operation->getOperand(0).getType();
      if (yield->getNumResults() != 0 ||
          (!inductionType.isIndex() && !isa<IntegerType>(inductionType)) ||
          operation->getOperand(1).getType() != inductionType ||
          operation->getOperand(2).getType() != inductionType ||
          body.front().getArgument(0).getType() != inductionType ||
          !sameTypes(operation->getOperands().drop_front(3),
                     operation->getResults()) ||
          !sameTypes(body.front().getArguments().drop_front(),
                     operation->getResults()) ||
          !sameTypes(yield->getOperands(), operation->getResults()))
        return emitArityError("scf.for");
    } else if (isa<scf::WhileOp>(operation)) {
      if (operation->getNumRegions() != 2)
        return emitArityError("scf.while");
      Region &before = operation->getRegion(0);
      Region &after = operation->getRegion(1);
      Operation *condition =
          requireTerminator(before, "scf.while before", "scf.condition");
      Operation *yield =
          requireTerminator(after, "scf.while after", "scf.yield");
      if (!condition || !yield)
        return WalkResult::interrupt();
      if (condition->getNumResults() != 0 || yield->getNumResults() != 0 ||
          condition->getNumOperands() < 1 ||
          !condition->getOperand(0).getType().isInteger(1) ||
          !sameTypes(operation->getOperands(), before.front().getArguments()) ||
          !sameTypes(condition->getOperands().drop_front(),
                     operation->getResults()) ||
          !sameTypes(after.front().getArguments(), operation->getResults()) ||
          !sameTypes(yield->getOperands(), before.front().getArguments()))
        return emitArityError("scf.while");
    }
    return WalkResult::advance();
  });
  return result;
}

class StructuredSuspensionAnalysis {
public:
  explicit StructuredSuspensionAnalysis(ProcessOp process)
      : work(processLivenessWorkCollector) {
    buildSummaries(process.getBody());
    buildEpochs(process.getBody());
  }

  bool guaranteesSuspend(Region &region) const {
    return regionGuarantees.lookup(&region);
  }

  LogicalResult verifyLinearLiveness(ProcessOp process) const {
    LogicalResult result = success();
    process.getBody().walk([&](Block *block) {
      if (failed(result) || !reachableBlocks.contains(block))
        return failed(result) ? WalkResult::interrupt() : WalkResult::advance();
      auto verifyValue = [&](Value value,
                             uint64_t definitionEpoch) -> LogicalResult {
        if (work)
          ++work->valueVisits;
        if (!isLinearAcrossSuspension(value.getType()))
          return success();
        for (OpOperand &use : value.getUses()) {
          if (work)
            ++work->useVisits;
          if (!reachableBlocks.contains(use.getOwner()->getBlock()))
            continue;
          if (beforeEpoch.lookup(use.getOwner()) > definitionEpoch)
            return process.emitOpError()
                   << "value of type " << value.getType()
                   << " cannot remain live across suspension";
        }
        return success();
      };
      uint64_t entryEpoch = blockEntryEpoch.lookup(block);
      for (BlockArgument argument : block->getArguments())
        if (failed(verifyValue(argument, entryEpoch))) {
          result = failure();
          return WalkResult::interrupt();
        }
      for (Operation &operation : *block) {
        if (work)
          ++work->livenessOperationVisits;
        for (Value value : operation.getResults())
          if (failed(verifyValue(value, afterEpoch.lookup(&operation)))) {
            result = failure();
            return WalkResult::interrupt();
          }
      }
      return WalkResult::advance();
    });
    return result;
  }

private:
  void buildSummaries(Region &root) {
    SmallVector<std::pair<Operation *, bool>> worklist;
    for (Block &block : llvm::reverse(root))
      for (Operation &operation : llvm::reverse(block))
        worklist.emplace_back(&operation, false);
    while (!worklist.empty()) {
      auto [operation, visited] = worklist.pop_back_val();
      if (!visited) {
        worklist.emplace_back(operation, true);
        for (Region &region : llvm::reverse(operation->getRegions()))
          for (Block &block : llvm::reverse(region))
            for (Operation &nested : llvm::reverse(block))
              worklist.emplace_back(&nested, false);
        continue;
      }
      if (work)
        ++work->summaryOperationVisits;
      for (Region &region : operation->getRegions()) {
        bool may = false;
        bool guarantees = false;
        for (Block &block : region)
          for (Operation &nested : block) {
            may |= operationMaySuspend.lookup(&nested);
            guarantees |= operationGuaranteesSuspend.lookup(&nested);
          }
        regionMaySuspend.try_emplace(&region, may);
        regionGuarantees.try_emplace(&region, guarantees);
      }
      bool may = isSuspension(operation);
      bool guarantees = isSuspension(operation);
      if (auto ifOp = dyn_cast<scf::IfOp>(operation)) {
        if (std::optional<bool> condition = constantBool(ifOp.getCondition())) {
          Region &taken = operation->getRegion(*condition ? 0 : 1);
          may |= regionMaySuspend.lookup(&taken);
          guarantees |= regionGuarantees.lookup(&taken);
        } else {
          Region &thenRegion = operation->getRegion(0);
          Region &elseRegion = operation->getRegion(1);
          may |= regionMaySuspend.lookup(&thenRegion) ||
                 regionMaySuspend.lookup(&elseRegion);
          guarantees |= !elseRegion.empty() &&
                        regionGuarantees.lookup(&thenRegion) &&
                        regionGuarantees.lookup(&elseRegion);
        }
      } else {
        for (Region &region : operation->getRegions())
          may |= regionMaySuspend.lookup(&region);
      }
      operationMaySuspend.try_emplace(operation, may);
      operationGuaranteesSuspend.try_emplace(operation, guarantees);
    }
    bool may = false;
    bool guarantees = false;
    for (Block &block : root)
      for (Operation &operation : block) {
        may |= operationMaySuspend.lookup(&operation);
        guarantees |= operationGuaranteesSuspend.lookup(&operation);
      }
    regionMaySuspend.try_emplace(&root, may);
    regionGuarantees.try_emplace(&root, guarantees);
  }

  void buildEpochs(Region &root) {
    SmallVector<std::pair<Block *, uint64_t>> worklist;
    for (Block &block : root)
      worklist.emplace_back(&block, 0);
    while (!worklist.empty()) {
      auto [block, entryEpoch] = worklist.pop_back_val();
      if (!reachableBlocks.insert(block).second)
        continue;
      blockEntryEpoch.try_emplace(block, entryEpoch);
      uint64_t epoch = entryEpoch;
      for (Operation &operation : *block) {
        if (work)
          ++work->epochOperationVisits;
        beforeEpoch.try_emplace(&operation, epoch);
        SmallVector<unsigned> reachableRegions;
        if (auto ifOp = dyn_cast<scf::IfOp>(operation)) {
          if (std::optional<bool> condition = constantBool(ifOp.getCondition()))
            reachableRegions.push_back(*condition ? 0 : 1);
          else
            reachableRegions.append({0, 1});
        } else {
          for (unsigned index = 0; index < operation.getNumRegions(); ++index)
            reachableRegions.push_back(index);
        }
        for (unsigned index : llvm::reverse(reachableRegions))
          for (Block &nested : llvm::reverse(operation.getRegion(index)))
            worklist.emplace_back(&nested, epoch);
        epoch += operationMaySuspend.lookup(&operation) ? 1 : 0;
        afterEpoch.try_emplace(&operation, epoch);
      }
    }
  }

  llvm::DenseMap<Operation *, bool> operationMaySuspend;
  llvm::DenseMap<Operation *, bool> operationGuaranteesSuspend;
  llvm::DenseMap<Region *, bool> regionMaySuspend;
  llvm::DenseMap<Region *, bool> regionGuarantees;
  llvm::DenseMap<Block *, uint64_t> blockEntryEpoch;
  llvm::DenseMap<Operation *, uint64_t> beforeEpoch;
  llvm::DenseMap<Operation *, uint64_t> afterEpoch;
  llvm::DenseSet<Block *> reachableBlocks;
  detail::ProcessLivenessWork *work;
};

LogicalResult verifyTraceProvenance(ProcessOp process) {
  llvm::DenseMap<Value, SmallVector<Value>> forwarding;
  llvm::DenseSet<OpOperand *> forwardingUses;
  auto connect = [&](Value left, Value right, OpOperand *use = nullptr) {
    if (!left.getType().isIndex() || !right.getType().isIndex())
      return;
    forwarding[left].push_back(right);
    forwarding[right].push_back(left);
    if (use)
      forwardingUses.insert(use);
  };

  process.getBody().walk([&](Operation *operation) {
    if (auto ifOp = dyn_cast<scf::IfOp>(operation)) {
      for (Region *region : {&ifOp.getThenRegion(), &ifOp.getElseRegion()}) {
        if (region->empty())
          continue;
        auto yield = cast<scf::YieldOp>(region->front().getTerminator());
        for (auto [operand, result] :
             llvm::zip(yield->getOpOperands(), ifOp.getResults()))
          connect(operand.get(), result, &operand);
      }
    } else if (auto forOp = dyn_cast<scf::ForOp>(operation)) {
      auto yield = cast<scf::YieldOp>(forOp.getBody()->getTerminator());
      for (auto [index, init, iter, result, yielded] :
           llvm::enumerate(forOp.getInitArgs(), forOp.getRegionIterArgs(),
                           forOp.getResults(), yield->getOpOperands())) {
        connect(init, iter, &forOp->getOpOperand(index + 3));
        connect(iter, result);
        connect(yielded.get(), iter, &yielded);
      }
    } else if (auto whileOp = dyn_cast<scf::WhileOp>(operation)) {
      for (auto [index, init, argument] :
           llvm::enumerate(whileOp.getInits(), whileOp.getBeforeArguments()))
        connect(init, argument, &whileOp->getOpOperand(index));
      auto condition = whileOp.getConditionOp();
      for (auto [index, forwarded, afterArgument, result] :
           llvm::enumerate(condition.getArgs(), whileOp.getAfterArguments(),
                           whileOp.getResults())) {
        connect(forwarded, afterArgument, &condition->getOpOperand(index + 1));
        connect(afterArgument, result);
      }
      auto yield = whileOp.getYieldOp();
      for (auto [yielded, beforeArgument] :
           llvm::zip(yield->getOpOperands(), whileOp.getBeforeArguments()))
        connect(yielded.get(), beforeArgument, &yielded);
    }
    return WalkResult::advance();
  });

  llvm::DenseMap<Value, unsigned> component;
  unsigned nextComponent = 0;
  for (auto &entry : forwarding) {
    Value seed = entry.first;
    if (component.count(seed))
      continue;
    SmallVector<Value> worklist{seed};
    component.try_emplace(seed, nextComponent);
    while (!worklist.empty()) {
      Value current = worklist.pop_back_val();
      for (Value adjacent : forwarding[current])
        if (component.try_emplace(adjacent, nextComponent).second)
          worklist.push_back(adjacent);
    }
    ++nextComponent;
  }
  auto getComponent = [&](Value value) {
    auto [it, inserted] = component.try_emplace(value, nextComponent);
    if (inserted)
      ++nextComponent;
    return it->second;
  };

  SmallVector<TraceOpenOp> openOps;
  SmallVector<TraceNextOp> nextOps;
  process.getBody().walk([&](Operation *operation) {
    if (auto open = dyn_cast<TraceOpenOp>(operation)) {
      openOps.push_back(open);
      (void)getComponent(open.getCursor());
    } else if (auto next = dyn_cast<TraceNextOp>(operation)) {
      nextOps.push_back(next);
      (void)getComponent(next.getInputCursor());
      (void)getComponent(next.getCursor());
    } else if (auto eof = dyn_cast<TraceEofOp>(operation)) {
      (void)getComponent(eof.getInputCursor());
    } else if (auto position = dyn_cast<TracePositionOp>(operation)) {
      (void)getComponent(position.getInputCursor());
    }
    return WalkResult::advance();
  });

  enum class CursorLattice { Unknown, NonCursor, SingleSource, Conflict };
  struct CursorState {
    CursorLattice lattice = CursorLattice::Unknown;
    StringAttr source;
  };
  SmallVector<CursorState> states(nextComponent);
  auto isConcreteNonCursor = [](Value value) {
    if (isa_and_nonnull<TraceOpenOp>(value.getDefiningOp()))
      return false;
    if (auto next = dyn_cast_or_null<TraceNextOp>(value.getDefiningOp()))
      return value != next.getCursor();
    if (isa_and_nonnull<scf::IfOp, scf::ForOp, scf::WhileOp>(
            value.getDefiningOp()))
      return false;
    if (auto argument = dyn_cast<BlockArgument>(value)) {
      Operation *parent = argument.getOwner()->getParentOp();
      if (auto forOp = dyn_cast_or_null<scf::ForOp>(parent))
        return argument == forOp.getInductionVar();
      return !isa_and_nonnull<scf::IfOp, scf::ForOp, scf::WhileOp>(parent);
    }
    return true;
  };
  for (auto &[value, id] : component)
    if (isConcreteNonCursor(value))
      states[id].lattice = CursorLattice::NonCursor;

  for (TraceOpenOp open : openOps) {
    unsigned id = getComponent(open.getCursor());
    if (states[id].lattice == CursorLattice::NonCursor) {
      states[id].lattice = CursorLattice::Conflict;
      return open.emitOpError(
          "trace cursor forwarding merges cursor and non-cursor values");
    }
    if (states[id].lattice == CursorLattice::SingleSource &&
        states[id].source != open.getSourceAttr()) {
      states[id].lattice = CursorLattice::Conflict;
      return open.emitOpError(
          "trace cursor forwarding merges distinct provenance");
    }
    states[id] = {CursorLattice::SingleSource, open.getSourceAttr()};
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (TraceNextOp next : nextOps) {
      unsigned input = getComponent(next.getInputCursor());
      unsigned output = getComponent(next.getCursor());
      if (isa_and_nonnull<arith::IndexCastOp>(
              next.getInputCursor().getDefiningOp()) &&
          states[output].lattice == CursorLattice::Unknown) {
        states[output] = {CursorLattice::SingleSource, next.getSourceAttr()};
        changed = true;
        continue;
      }
      if (states[input].lattice != CursorLattice::SingleSource)
        continue;
      if (states[output].lattice == CursorLattice::NonCursor) {
        states[output].lattice = CursorLattice::Conflict;
        return next.emitOpError(
            "trace cursor forwarding merges cursor and non-cursor values");
      }
      if (states[output].lattice == CursorLattice::SingleSource &&
          states[output].source != states[input].source) {
        states[output].lattice = CursorLattice::Conflict;
        return next.emitOpError(
            "trace cursor forwarding merges distinct provenance");
      }
      if (states[output].lattice == CursorLattice::Unknown) {
        states[output] = states[input];
        changed = true;
      }
    }
  }

  SmallVector<unsigned> advancing(nextComponent);
  LogicalResult result = success();
  auto verifyConsumer = [&](Operation *operation, Value cursor,
                            StringRef source, bool advances) -> LogicalResult {
    // A generated timing model may checkpoint an index cursor in an integer
    // register between ticks.  The source remains explicit on every trace
    // operation and the runtime bounds-checks the restored cursor.
    if (isa_and_nonnull<arith::IndexCastOp>(cursor.getDefiningOp()))
      return success();
    unsigned id = getComponent(cursor);
    if (id >= states.size() ||
        states[id].lattice != CursorLattice::SingleSource)
      return operation->emitOpError(
          "trace cursor must originate from ac.trace.open or ac.trace.next");
    if (states[id].source.getValue() != source)
      return operation->emitOpError(
          "trace cursor owner does not match 'from source'");
    if (advances && ++advancing[id] > 1)
      return operation->emitOpError(
          "trace cursor provenance has more than one advancing consumer");
    return success();
  };
  for (TraceNextOp next : nextOps)
    if (failed(verifyConsumer(next, next.getInputCursor(), next.getSource(),
                              true)))
      return failure();
  process.getBody().walk([&](Operation *operation) {
    if (failed(result))
      return WalkResult::interrupt();
    if (auto eof = dyn_cast<TraceEofOp>(operation))
      result =
          verifyConsumer(eof, eof.getInputCursor(), eof.getSource(), false);
    else if (auto position = dyn_cast<TracePositionOp>(operation))
      result = verifyConsumer(position, position.getInputCursor(),
                              position.getSource(), false);
    return failed(result) ? WalkResult::interrupt() : WalkResult::advance();
  });
  if (failed(result))
    return failure();

  for (auto &[value, id] : component) {
    if (id >= states.size() ||
        states[id].lattice != CursorLattice::SingleSource)
      continue;
    for (OpOperand &use : value.getUses()) {
      if (forwardingUses.contains(&use) ||
          isa<TraceNextOp, TraceEofOp, TracePositionOp, arith::IndexCastOp>(
              use.getOwner()))
        continue;
      return use.getOwner()->emitOpError(
          "trace cursor may only feed trace cursor operations");
    }
  }
  return success();
}

template <typename Callback>
WalkResult walkOperationsIterative(Region &region, Callback callback) {
  SmallVector<Operation *> worklist;
  for (Block &block : llvm::reverse(region))
    for (Operation &operation : llvm::reverse(block))
      worklist.push_back(&operation);
  while (!worklist.empty()) {
    Operation *operation = worklist.pop_back_val();
    if (callback(operation).wasInterrupted())
      return WalkResult::interrupt();
    for (Region &nested : llvm::reverse(operation->getRegions()))
      for (Block &block : llvm::reverse(nested))
        for (Operation &child : llvm::reverse(block))
          worklist.push_back(&child);
  }
  return WalkResult::advance();
}

bool isObservationConsumer(Operation *operation) {
  return isa<ObservationOpInterface>(operation) ||
         operation->getParentOfType<InstrumentationOp>();
}

SideEffects::Resource *probeResource(StringRef kind) {
  return llvm::StringSwitch<SideEffects::Resource *>(kind)
      .Case("queue", QueueStateResource::get())
      .Case("resource", ReservationStateResource::get())
      .Case("module", ModuleStateResource::get())
      .Case("storage", StorageStateResource::get())
      .Case("protocol", ProtocolStateResource::get())
      .Case("trace", TracePositionResource::get())
      .Case("event_queue", EventQueueStateResource::get())
      .Case("external_io", ExternalIOResource::get())
      .Case("statistics", StatisticsResource::get())
      .Default(ExternalIOResource::get());
}

} // namespace

LogicalResult ProcessOp::verify() {
  if (!isa_and_nonnull<ModuleOp>((*this)->getParentOp()))
    return emitOpError("must be a direct child of ac.module");
  if (!isStableHierarchySegment(getSymName()))
    return emitOpError(
        "symbol name must be one stable hierarchy owner segment");
  if (getKind() != "control" && getKind() != "workload" &&
      getKind() != "monitor")
    return emitOpError("kind must be 'control', 'workload', or 'monitor'");
  if (getBody().empty())
    return emitOpError("requires one non-empty body block");
  if (!llvm::equal(getBody().front().getArgumentTypes(),
                   getCaptures().getTypes()))
    return emitOpError("body arguments must exactly match capture types");
  if (!isa<YieldSimOp>(getBody().front().back()))
    return emitOpError("body must terminate with ac.yield_sim");
  if (failed(verifyProcessLowerability(getOperation())))
    return failure();

  StructuredSuspensionAnalysis suspensionAnalysis(*this);

  LogicalResult result = success();
  walkOperationsIterative(getBody(), [&](Operation *operation) {
    if (getKind() == "monitor" &&
        isa<TrySendOp, TryRecvOp, ScheduleOp, WaitForOp>(operation)) {
      operation->emitOpError(
          "monitor process cannot perform functional state effects");
      result = failure();
      return WalkResult::interrupt();
    }
    if (auto whileOp = dyn_cast<scf::WhileOp>(operation)) {
      std::optional<bool> condition =
          constantBool(whileOp.getConditionOp().getCondition());
      if (condition != false &&
          !suspensionAnalysis.guaranteesSuspend(whileOp.getBefore()) &&
          !suspensionAnalysis.guaranteesSuspend(whileOp.getAfter())) {
        operation->emitOpError(
            "every scf.while backedge must suspend or prove bounded progress");
        result = failure();
        return WalkResult::interrupt();
      }
    }
    return WalkResult::advance();
  });
  if (failed(result))
    return failure();

  if (failed(suspensionAnalysis.verifyLinearLiveness(*this)))
    return failure();
  llvm::StringSet<> instrumentationNames;
  WalkResult instrumentationResult =
      getBody().walk([&](InstrumentationOp instrumentation) {
        if (!instrumentationNames.insert(instrumentation.getSymName()).second) {
          instrumentation.emitOpError()
              << "duplicate process-local instrumentation name '"
              << instrumentation.getSymName() << "'";
          return WalkResult::interrupt();
        }
        return WalkResult::advance();
      });
  if (instrumentationResult.wasInterrupted())
    return failure();
  return verifyTraceProvenance(*this);
}

LogicalResult TrySendOp::verify() { return requireProcess(*this); }
LogicalResult TryRecvOp::verify() { return requireProcess(*this); }

LogicalResult ScheduleOp::verify() {
  if (Operation *definition = getDelay().getDefiningOp();
      definition && definition->getName().getStringRef() == "arith.constant") {
    auto value = definition->getAttrOfType<IntegerAttr>("value");
    if (value && value.getInt() < 0)
      return emitOpError("schedule delay must be non-negative");
  }
  return requireProcess(*this);
}

LogicalResult WaitUntilOp::verify() { return requireProcess(*this); }
LogicalResult WaitForOp::verify() { return requireProcess(*this); }
LogicalResult AwaitEventOp::verify() { return requireProcess(*this); }

LogicalResult YieldSimOp::verify() {
  ProcessOp process = enclosingProcess(*this);
  if (!process || (*this)->getParentOp() != process)
    return emitOpError("must directly terminate an ac.process body");
  if (&(*this)->getBlock()->back() != getOperation())
    return emitOpError("must be the final operation in ac.process");
  return success();
}

LogicalResult TraceOpenOp::verify() {
  if (!isStableHierarchySegment(getSource()))
    return emitOpError(
        "trace source must be one stable logical identifier segment");
  return requireProcess(*this);
}

LogicalResult TraceNextOp::verify() { return requireProcess(*this); }

LogicalResult TraceDecodeOp::verify() {
  auto next = getEntry().getDefiningOp<TraceNextOp>();
  if ((!next || getEntry() != next.getEntry()) &&
      !getEntry().getType().isSignlessInteger(64))
    return emitOpError(
        "trace.decode input must be an ac.trace.next entry or an i64 handle");
  return requireProcess(*this);
}

LogicalResult TraceEofOp::verify() { return requireProcess(*this); }

LogicalResult TracePositionOp::verify() { return requireProcess(*this); }

LogicalResult RequireOp::verify() {
  if (isa_and_nonnull<ModuleOp>((*this)->getParentOp()))
    return success();
  return requireProcess(*this);
}

LogicalResult EnsureOp::verify() {
  if (isa_and_nonnull<ModuleOp>((*this)->getParentOp()))
    return success();
  return requireProcess(*this);
}

LogicalResult AssertOp::verify() { return requireProcess(*this); }

LogicalResult ProbeOp::verify() {
  if (!probeResource(getKind()) ||
      !hasStringValue(getKind(),
                      {"queue", "resource", "module", "storage", "protocol",
                       "trace", "event_queue", "external_io", "statistics"}))
    return emitOpError("unsupported probe resource kind '") << getKind() << "'";
  if (failed(requireProcess(*this)))
    return failure();
  for (Operation *user : getValue().getUsers())
    if (!isObservationConsumer(user))
      return emitOpError("probe result may only feed observation operations");
  return success();
}

LogicalResult StatOp::verify() {
  if (!isa_and_nonnull<ModuleOp>((*this)->getParentOp()))
    return emitOpError("must be a direct child of ac.module");
  if (!isStableHierarchySegment(getSymName()))
    return emitOpError(
        "symbol name must be one stable hierarchy owner segment");
  if (!hasStringValue(getKind(),
                      {"counter", "gauge", "histogram", "event_log"}))
    return emitOpError(
        "kind must be 'counter', 'gauge', 'histogram', or 'event_log'");
  return success();
}

LogicalResult StatAddOp::verify() { return requireProcess(*this); }

LogicalResult InstrumentationOp::verify() {
  if (!enclosingProcess(*this))
    return emitOpError("must be nested in ac.process");
  if (!isStableHierarchySegment(getSymName()))
    return emitOpError(
        "symbol name must be one stable hierarchy owner segment");
  LogicalResult result = success();
  walkOperationsIterative(getBody(), [&](Operation *operation) {
    if (isa<ObservationOpInterface>(operation) || isMemoryEffectFree(operation))
      if (!isa<TrySendOp, TryRecvOp, ScheduleOp, WaitUntilOp, WaitForOp,
               AwaitEventOp, YieldSimOp, TraceOpenOp, TraceNextOp, TraceEofOp,
               TracePositionOp>(operation))
        return WalkResult::advance();
    operation->emitOpError(
        "instrumentation may contain only removable observation operations");
    result = failure();
    return WalkResult::interrupt();
  });
  return result;
}

void TrySendOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  if (!isa_and_nonnull<QueueOp>(lookupRuntimeSymbol(*this, getQueue())))
    return;
  StringRef leaf = runtimeSymbolLeaf(getQueue());
  addEffect(effects, *this, MemoryEffects::Read::get(), leaf, "queue",
            QueueStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), leaf, "queue",
            QueueStateResource::get());
  addEffect(effects, *this, MemoryEffects::Read::get(), leaf, "protocol",
            ProtocolStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), leaf, "protocol",
            ProtocolStateResource::get());
}

void TryRecvOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  if (!isa_and_nonnull<QueueOp>(lookupRuntimeSymbol(*this, getQueue())))
    return;
  StringRef leaf = runtimeSymbolLeaf(getQueue());
  addEffect(effects, *this, MemoryEffects::Read::get(), leaf, "queue",
            QueueStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), leaf, "queue",
            QueueStateResource::get());
  addEffect(effects, *this, MemoryEffects::Read::get(), leaf, "protocol",
            ProtocolStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), leaf, "protocol",
            ProtocolStateResource::get());
}

void ScheduleOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  if (!isa_and_nonnull<ProcessOp>(resolvedRuntimeTarget(*this, getTarget())))
    return;
  addEffect(effects, *this, MemoryEffects::Write::get(), getTarget(), "module",
            ModuleStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), getTarget(),
            "event_queue", EventQueueStateResource::get());
}

void WaitUntilOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  addEffect(effects, *this, MemoryEffects::Read::get(), processIdentity(*this),
            "event_queue", EventQueueStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), processIdentity(*this),
            "module", ModuleStateResource::get());
}

void WaitForOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  if (!isa_and_nonnull<ResourceOp>(resolvedRuntimeTarget(*this, getResource())))
    return;
  addEffect(effects, *this, MemoryEffects::Read::get(), getResource(),
            "resource", ReservationStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), processIdentity(*this),
            "module", ModuleStateResource::get());
}

void AwaitEventOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  if (!isa_and_nonnull<EventQueueOp>(
          resolvedRuntimeTarget(*this, getEventQueue())))
    return;
  addEffect(effects, *this, MemoryEffects::Read::get(), getEventQueue(),
            "event_queue", EventQueueStateResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), processIdentity(*this),
            "module", ModuleStateResource::get());
}

void YieldSimOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  addEffect(effects, *this, MemoryEffects::Write::get(), processIdentity(*this),
            "module", ModuleStateResource::get());
}

void TraceOpenOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  std::string identity = traceOwnerIdentity(*this, getSource());
  addEffect(effects, *this, MemoryEffects::Read::get(), identity, "external_io",
            ExternalIOResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), identity, "trace",
            TracePositionResource::get());
}

void TraceNextOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  std::string identity = traceOwnerIdentity(*this, getSource());
  addEffect(effects, *this, MemoryEffects::Read::get(), identity, "trace",
            TracePositionResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), identity, "trace",
            TracePositionResource::get());
  addEffect(effects, *this, MemoryEffects::Read::get(), identity, "external_io",
            ExternalIOResource::get());
}

void TraceEofOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  std::string identity = traceOwnerIdentity(*this, getSource());
  addEffect(effects, *this, MemoryEffects::Read::get(), identity, "trace",
            TracePositionResource::get());
}

void TracePositionOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  std::string identity = traceOwnerIdentity(*this, getSource());
  addEffect(effects, *this, MemoryEffects::Read::get(), identity, "trace",
            TracePositionResource::get());
}

void RequireOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  addContractEffect(effects, *this);
}

void EnsureOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  addContractEffect(effects, *this);
}

void AssertOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  addContractEffect(effects, *this);
}

void ProbeOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  Operation *target = resolvedRuntimeTarget(*this, getTarget());
  bool matches = llvm::StringSwitch<bool>(getKind())
                     .Case("queue", isa_and_nonnull<QueueOp>(target))
                     .Case("resource", isa_and_nonnull<ResourceOp>(target))
                     .Case("module", isa_and_nonnull<ProcessOp>(target))
                     .Case("storage", isa_and_nonnull<AddressSpaceOp>(target))
                     .Case("protocol", isa_and_nonnull<QueueOp>(target))
                     .Case("trace", isa_and_nonnull<ProcessOp>(target))
                     .Case("event_queue", isa_and_nonnull<EventQueueOp>(target))
                     .Case("external_io", isa_and_nonnull<ProcessOp>(target))
                     .Case("statistics", isa_and_nonnull<StatOp>(target))
                     .Default(false);
  if (!matches)
    return;
  addEffect(effects, *this, MemoryEffects::Read::get(), getTarget(), getKind(),
            probeResource(getKind()));
}

void StatOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  addEffect(effects, *this, MemoryEffects::Write::get(), getSymName(),
            "statistics", StatisticsResource::get());
}

void StatAddOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  if (!isa_and_nonnull<StatOp>(resolvedRuntimeTarget(*this, getStat())))
    return;
  addEffect(effects, *this, MemoryEffects::Read::get(), getStat(), "statistics",
            StatisticsResource::get());
  addEffect(effects, *this, MemoryEffects::Write::get(), getStat(),
            "statistics", StatisticsResource::get());
}

Operation *lookupRuntimeSymbol(Operation *from, SymbolRefAttr ref) {
  if (!from || !ref)
    return nullptr;
  if (!ref.getNestedReferences().empty()) {
    if (ref.getNestedReferences().size() != 1)
      return nullptr;
    auto file = from->getParentOfType<mlir::ModuleOp>();
    if (!file)
      return nullptr;
    ModuleOp targetModule;
    for (ModuleOp candidate : file.getOps<ModuleOp>()) {
      if (candidate.getSymName() == ref.getRootReference()) {
        targetModule = candidate;
        break;
      }
    }
    if (!targetModule)
      return nullptr;
    StringRef leaf = ref.getNestedReferences().front().getValue();
    for (Operation &candidate : targetModule.getBody().front()) {
      if (auto name = SymbolTable::getSymbolName(&candidate);
          name && name.getValue() == leaf)
        return &candidate;
    }
    return nullptr;
  }
  if (auto owner = from->getParentOfType<ModuleOp>()) {
    StringRef name = ref.getRootReference();
    for (Operation &candidate : owner.getBody().front()) {
      if (auto symbol = SymbolTable::getSymbolName(&candidate);
          symbol && symbol.getValue() == name)
        return &candidate;
    }
  }
  return SymbolTable::lookupNearestSymbolFrom(from, ref);
}

StringRef runtimeSymbolLeaf(SymbolRefAttr ref) {
  if (!ref)
    return {};
  ArrayRef<FlatSymbolRefAttr> nested = ref.getNestedReferences();
  if (nested.empty())
    return ref.getRootReference();
  return nested.back().getValue();
}

} // namespace acir::ac

#define GET_OP_CLASSES
#include "acir/Dialect/ACIR/ACIROps.cpp.inc"
