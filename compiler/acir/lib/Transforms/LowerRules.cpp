#include "acir/Transforms/Passes.h"

#include "acir/Analysis/VariableAnalysis.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

#include <optional>

using namespace mlir;

namespace acir {
namespace {

template <typename Marker> SmallVector<Marker> collectMarkers(ModuleOp model) {
  SmallVector<Marker> markers;
  model.walk([&](Marker marker) { markers.push_back(marker); });
  return markers;
}

template <typename Marker> void dischargeMarker(Marker marker) {
  marker.getResult().replaceAllUsesWith(marker.getInput());
  marker.erase();
}

LogicalResult requireNoTypeMarkers(ModuleOp model, StringRef stage) {
  LogicalResult result = success();
  model.walk([&](ac::TypeConstraintMarkerOp marker) {
    if (failed(result))
      return;
    result = marker.emitOpError() << "must be resolved before " << stage;
  });
  return result;
}

LogicalResult requireRuleAttribute(ac::RuleOp rule, StringRef name,
                                   StringRef stage) {
  if (rule->hasAttr(name))
    return success();
  return rule.emitOpError() << "requires '" << name << "' before " << stage;
}

std::string ruleHandshake(ac::RuleOp rule, bool hasTableProposal) {
  return "ready_valid_" + std::to_string(rule.getInputs().size()) + "x" +
         std::to_string(rule.getOutputs().size()) +
         (hasTableProposal ? "_table" : "");
}

std::string firingHandshake(ac::FiringOp firing) {
  return "ready_valid_" + std::to_string(firing.getInputs().size()) + "x" +
         std::to_string(firing.getOutputs().size());
}

std::optional<bool> constantBool(Value value) {
  auto constant = value.getDefiningOp<ac::VarConstantOp>();
  auto integer =
      constant ? dyn_cast<IntegerAttr>(constant.getValue()) : IntegerAttr();
  if (!integer)
    return std::nullopt;
  return !integer.getValue().isZero();
}

ac::RuleGuardKind guardKind(Value value) {
  std::optional<bool> constant = constantBool(value);
  return constant == true ? ac::RuleGuardKind::Always
                          : ac::RuleGuardKind::Predicate;
}

bool presenceImpliesCandidate(Value present, Value candidate) {
  return present == candidate || constantBool(candidate) == true;
}

bool isBooleanComplement(Value candidate, Value base) {
  auto compare = candidate.getDefiningOp<ac::VarCmpOp>();
  if (!compare || compare.getPredicate() != "eq")
    return false;
  return (compare.getLhs() == base &&
          constantBool(compare.getRhs()) == false) ||
         (compare.getRhs() == base && constantBool(compare.getLhs()) == false);
}

bool areBooleanComplements(Value left, Value right) {
  return isBooleanComplement(left, right) || isBooleanComplement(right, left);
}

ac::RuleGuardKind inferredGuardKind(Operation *scope) {
  bool hasPredicate = false;
  scope->walk([&](Operation *operation) {
    Value condition;
    if (auto ruleCondition = dyn_cast<ac::RuleConditionOp>(operation))
      condition = ruleCondition.getCondition();
    else if (auto firingCondition = dyn_cast<ac::FiringConditionOp>(operation))
      condition = firingCondition.getCondition();
    if (!condition)
      return;
    hasPredicate |= guardKind(condition) == ac::RuleGuardKind::Predicate;
  });
  return hasPredicate ? ac::RuleGuardKind::Predicate
                      : ac::RuleGuardKind::Always;
}

ac::RuleIndexKind typedIndexKind(StringRef value) {
  return llvm::StringSwitch<ac::RuleIndexKind>(value)
      .Case("static", ac::RuleIndexKind::Static)
      .Case("dynamic", ac::RuleIndexKind::Dynamic)
      .Default(ac::RuleIndexKind::All);
}

DictionaryAttr queueRuleFact(Builder &builder, StringRef kindName,
                             Attribute kind, size_t ordinal,
                             ac::RuleGuardKind path) {
  NamedAttrList fields;
  fields.set(kindName, kind);
  fields.set("ordinal", builder.getI64IntegerAttr(ordinal));
  fields.set("guard_kind",
             ac::RuleGuardKindAttr::get(builder.getContext(), path));
  return builder.getDictionaryAttr(fields);
}

DictionaryAttr stateRuleEffect(Builder &builder, ac::RuleEffectKind kind,
                               StringRef resource, ac::RuleGuardKind path) {
  NamedAttrList fields;
  fields.set("kind", ac::RuleEffectKindAttr::get(builder.getContext(), kind));
  fields.set("resource",
             FlatSymbolRefAttr::get(builder.getContext(), resource));
  fields.set("guard_kind",
             ac::RuleGuardKindAttr::get(builder.getContext(), path));
  return builder.getDictionaryAttr(fields);
}

LogicalResult inferRuleTypes(ModuleOp model) {
  for (ac::TypeConstraintMarkerOp marker :
       collectMarkers<ac::TypeConstraintMarkerOp>(model)) {
    if (marker.getConstraint() != ac::TypeConstraintKind::QueuePayload)
      return marker.emitOpError("unsupported type-constraint kind");
    ac::RuleOp rule = marker->getParentOfType<ac::RuleOp>();
    if (!rule || !llvm::is_contained(rule.getBody().front().getArguments(),
                                     marker.getInput()))
      return marker.emitOpError("phase-one Queue payload inference must refine "
                                "a rule input");
    // The ACIR Var wrapper already carries the exact element type.  This first
    // inference lane therefore refines unknown/constrained Queue payload facts
    // monotonically to exact before removing the marker.
    dischargeMarker(marker);
  }
  return success();
}

LogicalResult inferRuleEffects(ModuleOp model) {
  if (failed(requireNoTypeMarkers(model, "rule effect inference")))
    return failure();
  ACDataFlowAnalyzer dataFlow(model.getOperation());
  if (failed(dataFlow.run()))
    return model.emitError("AC dataflow analysis failed before rule effects");
  LogicalResult variableResult = success();
  model.walk([&](ac::RuleOp rule) {
    if (failed(variableResult))
      return;
    for (BlockArgument argument : rule.getBody().front().getArguments()) {
      VariableProperties properties = dataFlow.lookup(argument);
      if (properties.lifetime != VariableLifetime::Temporary ||
          properties.update != VariableUpdate::Immutable) {
        variableResult = rule.emitOpError(
            "rule inputs must be temporary immutable ac.var snapshots");
        return;
      }
    }
  });
  if (failed(variableResult))
    return failure();
  for (ac::ValueFactMarkerOp marker :
       collectMarkers<ac::ValueFactMarkerOp>(model)) {
    if (marker.getFact() != ac::ValueFactKind::CommittedInput)
      return marker.emitOpError("unsupported value-fact kind");
    ac::RuleOp rule = marker->getParentOfType<ac::RuleOp>();
    if (!rule ||
        !llvm::is_contained(rule.getBody().front().getArguments(),
                            marker.getInput()) ||
        marker.getPathPredicate() != "true")
      return marker.emitOpError(
          "phase-one value inference requires a committed rule input on path "
          "true");
    const size_t inputIndex =
        cast<BlockArgument>(marker.getInput()).getArgNumber();
    const std::string expectedIdentity =
        rule.getInputs().size() == 1
            ? "input"
            : "input[" + std::to_string(inputIndex) + "]";
    if (marker.getIdentity() != expectedIdentity)
      return marker.emitOpError()
             << "committed input " << inputIndex << " requires identity '"
             << expectedIdentity << "'";
    dischargeMarker(marker);
  }
  Builder builder(model.getContext());
  model.walk([&](ac::RuleOp rule) {
    const ac::RuleGuardKind candidateGuard =
        inferredGuardKind(rule.getOperation());
    SmallVector<ac::TableProposeOp> proposals;
    rule.getBody().walk(
        [&](ac::TableProposeOp proposal) { proposals.push_back(proposal); });
    SmallVector<StringRef> effectNames;
    if (!rule.getInputs().empty())
      effectNames.push_back("input.consume");
    if (!rule.getOutputs().empty())
      effectNames.push_back("output.produce");
    SmallVector<std::string> tableEffects;
    if (!proposals.empty()) {
      llvm::StringSet<> seenTables;
      for (ac::TableProposeOp proposal : proposals)
        if (seenTables.insert(proposal.getTable()).second)
          tableEffects.push_back("table.replace:" + proposal.getTable().str());
      for (const std::string &effect : tableEffects)
        effectNames.push_back(effect);
    }
    rule->setAttr("ac.rule.effects", builder.getStrArrayAttr(effectNames));
    SmallVector<Attribute> typedEffects;
    SmallVector<Value> outputPresences(rule.getOutputs().size());
    rule.getBody().walk([&](ac::RuleOutputOp output) {
      if (output.getOrdinal() >= 0 &&
          static_cast<size_t>(output.getOrdinal()) < outputPresences.size())
        outputPresences[output.getOrdinal()] = output.getWhen();
    });
    for (size_t index = 0; index < rule.getInputs().size(); ++index)
      typedEffects.push_back(queueRuleFact(
          builder, "kind",
          ac::RuleEffectKindAttr::get(builder.getContext(),
                                      ac::RuleEffectKind::InputConsume),
          index, candidateGuard));
    for (size_t index = 0; index < rule.getOutputs().size(); ++index) {
      const ac::RuleGuardKind outputGuard =
          outputPresences[index] ? guardKind(outputPresences[index])
                                 : candidateGuard;
      typedEffects.push_back(queueRuleFact(
          builder, "kind",
          ac::RuleEffectKindAttr::get(builder.getContext(),
                                      ac::RuleEffectKind::OutputProduce),
          index, outputGuard));
    }
    SmallVector<Attribute> footprintAttrs;
    SmallVector<Attribute> conflictAttrs;
    for (const StateAccessFootprint &footprint :
         dataFlow.stateFootprints(rule.getOperation())) {
      if (footprint.indexKind == "unknown") {
        variableResult = rule.emitOpError(
            "state footprint index must be a temporary immutable ac.var");
        return;
      }
      NamedAttrList fields;
      fields.set("access", builder.getStringAttr(footprint.access));
      fields.set("resource", FlatSymbolRefAttr::get(model.getContext(),
                                                    footprint.resource));
      fields.set("index_kind", builder.getStringAttr(footprint.indexKind));
      const bool read = footprint.access == "read";
      const ac::RuleGuardKind footprintGuard =
          read ? ac::RuleGuardKind::Always
               : (footprint.present ? guardKind(footprint.present)
                                    : candidateGuard);
      fields.set("guard_kind", ac::RuleGuardKindAttr::get(builder.getContext(),
                                                          footprintGuard));
      if (!footprint.fields.empty()) {
        SmallVector<StringRef> fieldNames;
        for (const std::string &field : footprint.fields)
          fieldNames.push_back(field);
        fields.set("fields", builder.getStrArrayAttr(fieldNames));
      }
      footprintAttrs.push_back(builder.getDictionaryAttr(fields));
      typedEffects.push_back(stateRuleEffect(
          builder,
          read ? ac::RuleEffectKind::StateRead : ac::RuleEffectKind::StateWrite,
          footprint.resource, footprintGuard));
      NamedAttrList conflict;
      conflict.set("kind",
                   ac::RuleStateAccessKindAttr::get(
                       builder.getContext(),
                       read ? ac::RuleStateAccessKind::Read
                            : (footprint.access == "replace"
                                   ? ac::RuleStateAccessKind::Replace
                                   : ac::RuleStateAccessKind::FieldWrite)));
      conflict.set("resource", FlatSymbolRefAttr::get(builder.getContext(),
                                                      footprint.resource));
      conflict.set("index_kind", ac::RuleIndexKindAttr::get(
                                     builder.getContext(),
                                     typedIndexKind(footprint.indexKind)));
      conflict.set("guard_kind", ac::RuleGuardKindAttr::get(
                                     builder.getContext(), footprintGuard));
      if (!footprint.fields.empty())
        conflict.set("fields", fields.get("fields"));
      conflictAttrs.push_back(builder.getDictionaryAttr(conflict));
    }
    rule->setAttr("ac.rule.footprints", builder.getArrayAttr(footprintAttrs));
    rule->setAttr("ac.rule.effects_typed", builder.getArrayAttr(typedEffects));
    rule->setAttr("ac.rule.state_accesses",
                  builder.getArrayAttr(conflictAttrs));
  });
  return variableResult;
}

DictionaryAttr queueActivationResource(Builder &builder,
                                       ac::ActivationResourceKind kind,
                                       size_t ordinal) {
  NamedAttrList fields;
  fields.set("kind",
             ac::ActivationResourceKindAttr::get(builder.getContext(), kind));
  fields.set("ordinal", builder.getI64IntegerAttr(ordinal));
  return builder.getDictionaryAttr(fields);
}

DictionaryAttr stateActivationResource(Builder &builder, StringRef resource) {
  NamedAttrList fields;
  fields.set("kind",
             ac::ActivationResourceKindAttr::get(
                 builder.getContext(), ac::ActivationResourceKind::State));
  fields.set("resource",
             FlatSymbolRefAttr::get(builder.getContext(), resource));
  return builder.getDictionaryAttr(fields);
}

LogicalResult inferRuleActivation(ModuleOp model) {
  Builder builder(model.getContext());
  LogicalResult result = success();
  model.walk([&](ac::RuleOp rule) {
    if (failed(result))
      return;
    auto footprints = rule->getAttrOfType<ArrayAttr>("ac.rule.footprints");
    if (!footprints) {
      result = rule.emitOpError(
          "requires state footprints before activation inference");
      return;
    }
    SmallVector<Attribute> sources;
    SmallVector<Attribute> transaction;
    for (size_t index = 0; index < rule.getInputs().size(); ++index) {
      DictionaryAttr resource = queueActivationResource(
          builder, ac::ActivationResourceKind::InputQueue, index);
      sources.push_back(resource);
      transaction.push_back(resource);
    }
    for (size_t index = 0; index < rule.getOutputs().size(); ++index) {
      DictionaryAttr resource = queueActivationResource(
          builder, ac::ActivationResourceKind::OutputQueue, index);
      sources.push_back(resource);
      transaction.push_back(resource);
    }
    llvm::StringSet<> sourceState;
    for (Attribute attribute : footprints) {
      auto footprint = dyn_cast<DictionaryAttr>(attribute);
      auto resource = footprint ? footprint.getAs<FlatSymbolRefAttr>("resource")
                                : FlatSymbolRefAttr();
      if (!resource) {
        result = rule.emitOpError(
            "state footprint lacks a typed activation resource");
        return;
      }
      if (sourceState.insert(resource.getValue()).second)
        sources.push_back(
            stateActivationResource(builder, resource.getValue()));
    }
    llvm::StringSet<> transactionState;
    rule.getBody().walk([&](ac::TableProposeOp proposal) {
      if (transactionState.insert(proposal.getTable()).second)
        transaction.push_back(
            stateActivationResource(builder, proposal.getTable()));
    });
    rule->setAttr("ac.rule.activation_sources", builder.getArrayAttr(sources));
    rule->setAttr("ac.rule.transaction_resources",
                  builder.getArrayAttr(transaction));
    rule->setAttr("ac.rule.initially_active",
                  builder.getBoolAttr(rule.getInputs().empty()));
  });
  return result;
}

LogicalResult materializeRuleChecks(ModuleOp model) {
  Builder builder(model.getContext());
  LogicalResult result = success();
  model.walk([&](ac::RuleOp rule) {
    if (failed(result))
      return;
    if (failed(requireRuleAttribute(rule, "ac.rule.effects",
                                    "check materialization"))) {
      result = failure();
      return;
    }
    SmallVector<Attribute> checks;
    for (ac::PendingObligationMarkerOp marker :
         collectMarkers<ac::PendingObligationMarkerOp>(model)) {
      if (marker->getParentOfType<ac::RuleOp>() != rule ||
          marker.getResolver() != ac::ObligationResolver::Checks)
        continue;
      if (marker.getState() != ac::ObligationState::Pending) {
        marker.emitOpError("check resolver requires a pending obligation");
        result = failure();
        return;
      }
      marker.emitOpError("dynamic checks are not executable in the phase-one "
                         "pure rule subset");
      result = failure();
      return;
    }
    rule->setAttr("ac.rule.checks", builder.getArrayAttr(checks));
    const ac::RuleGuardKind candidateGuard =
        inferredGuardKind(rule.getOperation());
    SmallVector<Value> outputPresences(rule.getOutputs().size());
    rule.getBody().walk([&](ac::RuleOutputOp output) {
      if (output.getOrdinal() >= 0 &&
          static_cast<size_t>(output.getOrdinal()) < outputPresences.size())
        outputPresences[output.getOrdinal()] = output.getWhen();
    });
    SmallVector<Attribute> typedChecks;
    for (size_t index = 0; index < rule.getInputs().size(); ++index)
      typedChecks.push_back(queueRuleFact(
          builder, "kind",
          ac::RuleCheckKindAttr::get(builder.getContext(),
                                     ac::RuleCheckKind::InputAvailable),
          index, ac::RuleGuardKind::Always));
    SmallVector<Attribute> outputPresence;
    for (size_t index = 0; index < rule.getOutputs().size(); ++index) {
      const ac::RuleGuardKind outputGuard =
          outputPresences[index] ? guardKind(outputPresences[index])
                                 : candidateGuard;
      typedChecks.push_back(queueRuleFact(
          builder, "kind",
          ac::RuleCheckKindAttr::get(builder.getContext(),
                                     ac::RuleCheckKind::OutputCapacity),
          index, outputGuard));
      NamedAttrList presence;
      presence.set("ordinal", builder.getI64IntegerAttr(index));
      presence.set("presence_kind",
                   ac::RuleOutputPresenceKindAttr::get(
                       builder.getContext(),
                       outputGuard == ac::RuleGuardKind::Always
                           ? ac::RuleOutputPresenceKind::Always
                           : ac::RuleOutputPresenceKind::Predicate));
      outputPresence.push_back(builder.getDictionaryAttr(presence));
    }
    rule->setAttr("ac.rule.checks_typed", builder.getArrayAttr(typedChecks));
    rule->setAttr("ac.rule.output_presence",
                  builder.getArrayAttr(outputPresence));
  });
  return result;
}

LogicalResult materializeRuleHandshake(ModuleOp model) {
  LogicalResult result = success();
  model.walk([&](ac::RuleOp rule) {
    if (failed(result))
      return;
    if (failed(requireRuleAttribute(rule, "ac.rule.checks",
                                    "handshake materialization"))) {
      result = failure();
      return;
    }
    size_t found = 0;
    for (ac::PendingObligationMarkerOp marker :
         collectMarkers<ac::PendingObligationMarkerOp>(model)) {
      if (marker->getParentOfType<ac::RuleOp>() != rule ||
          marker.getResolver() != ac::ObligationResolver::Handshake)
        continue;
      if (marker.getState() != ac::ObligationState::Pending ||
          marker.getPathPredicate() != "true") {
        marker.emitOpError("handshake requires pending unconditional output "
                           "obligations");
        result = failure();
        return;
      }
      if (!marker.getResult().hasOneUse() ||
          !isa<ac::RuleReturnOp>(marker.getResult().use_begin()->getOwner())) {
        marker.emitOpError("handshake obligation must wrap the value returned "
                           "by ac.rule.return");
        result = failure();
        return;
      }
      ++found;
      marker.setStateAttr(ac::ObligationStateAttr::get(
          model.getContext(), ac::ObligationState::Materialized));
    }
    if (found != rule.getOutputs().size()) {
      rule.emitOpError("requires one handshake obligation per output Queue");
      result = failure();
      return;
    }
    bool hasTableProposal = false;
    rule.getBody().walk([&](ac::TableProposeOp) { hasTableProposal = true; });
    rule->setAttr("ac.rule.handshake",
                  StringAttr::get(model.getContext(),
                                  ruleHandshake(rule, hasTableProposal)));
  });
  return result;
}

LogicalResult dischargeRuleObligations(ModuleOp model) {
  LogicalResult result = success();
  for (ac::PendingObligationMarkerOp marker :
       collectMarkers<ac::PendingObligationMarkerOp>(model)) {
    if (marker.getState() != ac::ObligationState::Materialized) {
      marker.emitOpError(
          "must be materialized by its named resolver before discharge");
      result = failure();
      continue;
    }
    ac::RuleOp rule = marker->getParentOfType<ac::RuleOp>();
    if (!rule) {
      marker.emitOpError("has no owning rule");
      result = failure();
      continue;
    }
    if (marker.getResolver() == ac::ObligationResolver::Handshake) {
      auto handshake = rule->getAttrOfType<StringAttr>("ac.rule.handshake");
      bool hasTableProposal = false;
      rule.getBody().walk([&](ac::TableProposeOp) { hasTableProposal = true; });
      const std::string expected = ruleHandshake(rule, hasTableProposal);
      if (!handshake || handshake.getValue() != expected ||
          !marker.getResult().hasOneUse() ||
          !isa<ac::RuleReturnOp>(marker.getResult().use_begin()->getOwner())) {
        marker.emitOpError(
            "handshake discharge requires returned-value handshake evidence");
        result = failure();
        continue;
      }
    } else if (marker.getResolver() == ac::ObligationResolver::Checks) {
      auto checks = rule->getAttrOfType<ArrayAttr>("ac.rule.checks");
      bool found = checks && llvm::any_of(checks, [&](Attribute attribute) {
                     auto record = dyn_cast<DictionaryAttr>(attribute);
                     return record &&
                            record.getAs<StringAttr>("origin") ==
                                marker.getOriginAttr() &&
                            record.getAs<StringAttr>("path") ==
                                marker.getPathPredicateAttr();
                   });
      if (!found) {
        marker.emitOpError(
            "check discharge requires matching materialized check evidence");
        result = failure();
        continue;
      }
    } else {
      marker.emitOpError("has no implemented discharge verifier");
      result = failure();
      continue;
    }
    marker.setStateAttr(ac::ObligationStateAttr::get(
        model.getContext(), ac::ObligationState::Discharged));
    dischargeMarker(marker);
  }
  return result;
}

LogicalResult resolveRuleSchedule(ModuleOp model) {
  LogicalResult result = success();
  llvm::StringSet<> stableIds;
  int64_t lexicalPriority = 0;
  Builder builder(model.getContext());
  ACDataFlowAnalyzer dataFlow(model.getOperation());
  if (failed(dataFlow.run()))
    return model.emitError("AC dataflow analysis failed before rule schedule");
  model.walk([&](ac::RuleOp rule) {
    if (failed(result))
      return;
    if (failed(requireRuleAttribute(rule, "ac.rule.handshake",
                                    "schedule resolution"))) {
      result = failure();
      return;
    }
    if (failed(requireRuleAttribute(rule, "ac.rule.footprints",
                                    "schedule resolution"))) {
      result = failure();
      return;
    }
    if (!stableIds.insert(rule.getStableId()).second) {
      rule.emitOpError() << "duplicate stable rule identity '"
                         << rule.getStableId() << "'";
      result = failure();
      return;
    }
    bool unresolved = false;
    rule.getBody().walk([&](ac::PendingObligationMarkerOp marker) {
      marker.emitOpError("has no implemented named resolver");
      unresolved = true;
    });
    if (unresolved) {
      result = failure();
      return;
    }
    if (rule.getOutputs().size() > 1) {
      rule.emitOpError(
          "scheduling currently supports at most one output Queue");
      result = failure();
      return;
    }
    for (Value input : rule.getInputs()) {
      size_t consumingUses = 0;
      for (OpOperand &use : input.getUses())
        if (!isa<ac::ObserveOp, ac::ExpectOp>(use.getOwner()))
          ++consumingUses;
      if (consumingUses != 1) {
        rule.emitOpError("independent scheduling requires every input Queue to "
                         "be exclusive");
        result = failure();
        return;
      }
    }
    SmallVector<ac::TableProposeOp> proposals;
    rule.getBody().walk(
        [&](ac::TableProposeOp proposal) { proposals.push_back(proposal); });
    SmallVector<ac::RuleConditionOp> conditions;
    rule.getBody().walk([&](ac::RuleConditionOp condition) {
      conditions.push_back(condition);
    });
    if (conditions.empty()) {
      // The synthesized unconditional presence is consumed by every proposal
      // and output proof in the rule.  Materialize it at block entry so it
      // dominates proposals authored before the return terminator.
      OpBuilder bodyBuilder = OpBuilder::atBlockBegin(&rule.getBody().front());
      Type conditionType = IntegerType::get(model.getContext(), 1);
      OperationState constantState(rule.getLoc(),
                                   ac::VarConstantOp::getOperationName());
      constantState.addTypes(
          ac::VarType::get(model.getContext(), conditionType));
      constantState.addAttribute("value",
                                 bodyBuilder.getIntegerAttr(conditionType, 1));
      auto constant =
          cast<ac::VarConstantOp>(bodyBuilder.create(constantState));
      OperationState conditionState(rule.getLoc(),
                                    ac::RuleConditionOp::getOperationName());
      conditionState.addOperands(constant.getResult());
      conditions.push_back(
          cast<ac::RuleConditionOp>(bodyBuilder.create(conditionState)));
    }
    if (conditions.size() != 1) {
      rule.emitOpError("schedule resolution requires one functional condition");
      result = failure();
      return;
    }
    auto constant =
        conditions.front().getCondition().getDefiningOp<ac::VarConstantOp>();
    auto constantValue =
        constant ? dyn_cast<IntegerAttr>(constant.getValue()) : IntegerAttr();
    const bool always = constantValue && !constantValue.getValue().isZero();
    Value presence = conditions.front().getCondition();
    SmallVector<Value> divergentPresences;
    for (ac::TableProposeOp proposal : proposals) {
      if (proposal.getWhen()) {
        if (!presenceImpliesCandidate(proposal.getWhen(), presence)) {
          result = proposal.emitOpError(
              "proposal presence must imply the rule condition");
          return;
        }
        if (proposal.getWhen() != presence) {
          if (rule.getInputs().size() != 1) {
            result = proposal.emitOpError(
                "conditional-effect presence requires one input");
            return;
          }
          if (!llvm::is_contained(divergentPresences, proposal.getWhen())) {
            if (divergentPresences.size() >= 2 ||
                (!divergentPresences.empty() &&
                 !areBooleanComplements(divergentPresences.front(),
                                        proposal.getWhen()))) {
              result = proposal.emitOpError(
                  "conditional-effect presences must share one predicate or "
                  "one complementary pair");
              return;
            }
            divergentPresences.push_back(proposal.getWhen());
          }
        }
      } else {
        proposal->insertOperands(2, presence);
      }
    }
    SmallVector<ac::RuleOutputOp> existingOutputs;
    rule.getBody().walk(
        [&](ac::RuleOutputOp output) { existingOutputs.push_back(output); });
    auto returned =
        dyn_cast<ac::RuleReturnOp>(rule.getBody().front().getTerminator());
    if (!returned) {
      result = rule.emitOpError(
          "requires ac.rule.return before path materialization");
      return;
    }
    OpBuilder pathBuilder(returned);
    if (existingOutputs.empty()) {
      for (auto [ordinal, value] : llvm::enumerate(returned.getValues())) {
        OperationState outputState(rule.getLoc(),
                                   ac::RuleOutputOp::getOperationName());
        outputState.addOperands({value, presence});
        outputState.addAttribute("ordinal",
                                 pathBuilder.getI64IntegerAttr(ordinal));
        pathBuilder.create(outputState);
      }
    } else {
      if (existingOutputs.size() != returned.getValues().size()) {
        result = rule.emitOpError(
            "preexisting output presence must cover every rule result");
        return;
      }
      for (ac::RuleOutputOp output : existingOutputs)
        if (output.getOrdinal() < 0 ||
            static_cast<size_t>(output.getOrdinal()) >=
                returned.getValues().size() ||
            output.getValue() != returned.getValues()[output.getOrdinal()] ||
            !presenceImpliesCandidate(output.getWhen(), presence)) {
          result = output.emitOpError(
              "preexisting output presence is not a valid rule path");
          return;
        }
    }
    bool hasExistingSnapshotEvidence = false;
    rule.getBody().walk([&](Operation *operation) {
      hasExistingSnapshotEvidence |=
          isa<ac::StateSnapshotOp, ac::StateSnapshotSetOp>(operation);
    });
    if (hasExistingSnapshotEvidence) {
      result = rule.emitOpError(
          "state snapshot evidence must be materialized exactly once");
      return;
    }
    for (const StateSnapshotFootprint &snapshot :
         dataFlow.stateSnapshots(rule.getOperation())) {
      if (snapshot.indexKind == "unknown") {
        result = rule.emitOpError(
            "state snapshot index must be a temporary immutable ac.var");
        return;
      }
      if (snapshot.fields.empty()) {
        result = rule.emitOpError(
            "state snapshot fields must be derived before materialization");
        return;
      }
      SmallVector<StringRef> readFields;
      for (const std::string &field : snapshot.fields)
        readFields.push_back(field);
      ArrayAttr readFieldsAttr = builder.getStrArrayAttr(readFields);
      if (snapshot.indexKind == "set") {
        ac::StateSnapshotSetOp::create(
            pathBuilder, rule.getLoc(),
            FlatSymbolRefAttr::get(model.getContext(), snapshot.resource),
            snapshot.source, snapshot.predicate, readFieldsAttr);
        continue;
      }
      ac::StateSnapshotOp::create(
          pathBuilder, rule.getLoc(),
          FlatSymbolRefAttr::get(model.getContext(), snapshot.resource),
          snapshot.index, snapshot.predicate,
          ac::RuleIndexKindAttr::get(model.getContext(),
                                     typedIndexKind(snapshot.indexKind)),
          readFieldsAttr);
    }
    const int64_t priority = lexicalPriority++;
    rule->setAttr(
        "ac.rule.guard",
        StringAttr::get(model.getContext(), always ? "true" : "dynamic"));
    rule->setAttr("ac.rule.priority", builder.getI64IntegerAttr(priority));
    rule->setAttr(
        "ac.rule.guard_kind",
        ac::RuleGuardKindAttr::get(model.getContext(),
                                   always ? ac::RuleGuardKind::Always
                                          : ac::RuleGuardKind::Predicate));
    rule->setAttr(
        "ac.rule.schedule_kind",
        ac::RuleScheduleKindAttr::get(
            model.getContext(), proposals.empty()
                                    ? ac::RuleScheduleKind::Independent
                                    : ac::RuleScheduleKind::LexicalPriority));
    SmallVector<Attribute> arbitration;
    llvm::StringSet<> arbitrated;
    for (ac::TableProposeOp proposal : proposals) {
      if (!arbitrated.insert(proposal.getTable()).second)
        continue;
      NamedAttrList record;
      record.set("kind", ac::RuleArbitrationKindAttr::get(
                             model.getContext(),
                             ac::RuleArbitrationKind::LexicalPriority));
      record.set("resource", proposal.getTableAttr());
      record.set("priority", builder.getI64IntegerAttr(priority));
      arbitration.push_back(builder.getDictionaryAttr(record));
    }
    rule->setAttr("ac.rule.arbitration_membership",
                  builder.getArrayAttr(arbitration));
    rule->setAttr(
        "ac.rule.schedule",
        StringAttr::get(model.getContext(), proposals.empty()
                                                ? "independent"
                                                : "table_lexical_priority"));
  });
  return result;
}

LogicalResult lowerRulesToFiring(ModuleOp model) {
  SmallVector<ac::RuleOp> rules;
  model.walk([&](ac::RuleOp rule) { rules.push_back(rule); });
  for (ac::RuleOp rule : rules) {
    for (StringRef attribute :
         {"ac.rule.effects", "ac.rule.checks", "ac.rule.handshake",
          "ac.rule.guard", "ac.rule.schedule", "ac.rule.priority",
          "ac.rule.footprints", "ac.rule.activation_sources",
          "ac.rule.transaction_resources", "ac.rule.initially_active",
          "ac.rule.effects_typed", "ac.rule.checks_typed",
          "ac.rule.output_presence", "ac.rule.state_accesses",
          "ac.rule.guard_kind", "ac.rule.schedule_kind",
          "ac.rule.arbitration_membership"})
      if (failed(requireRuleAttribute(rule, attribute, "rule lowering")))
        return failure();
    bool hasMarker = false;
    rule.getBody().walk([&](Operation *operation) {
      hasMarker |= isa<ac::TypeConstraintMarkerOp, ac::ValueFactMarkerOp,
                       ac::PendingObligationMarkerOp>(operation);
    });
    if (hasMarker)
      return rule.emitOpError("cannot lower while typed markers remain");

    auto returned =
        dyn_cast<ac::RuleReturnOp>(rule.getBody().front().getTerminator());
    if (!returned)
      return rule.emitOpError("requires ac.rule.return before rule lowering");
    SmallVector<ac::RuleConditionOp> conditions;
    rule.getBody().walk([&](ac::RuleConditionOp condition) {
      conditions.push_back(condition);
    });
    for (ac::RuleConditionOp condition : conditions) {
      OpBuilder conditionBuilder(condition);
      OperationState conditionState(condition.getLoc(),
                                    ac::FiringConditionOp::getOperationName());
      conditionState.addOperands(condition.getCondition());
      conditionBuilder.create(conditionState);
      condition.erase();
    }
    SmallVector<ac::RuleOutputOp> outputs;
    rule.getBody().walk(
        [&](ac::RuleOutputOp output) { outputs.push_back(output); });
    for (ac::RuleOutputOp output : outputs) {
      OpBuilder outputBuilder(output);
      OperationState outputState(output.getLoc(),
                                 ac::FiringOutputOp::getOperationName());
      outputState.addOperands({output.getValue(), output.getWhen()});
      outputState.addAttribute("ordinal", output.getOrdinalAttr());
      outputBuilder.create(outputState);
      output.erase();
    }
    OpBuilder bodyBuilder(returned);
    OperationState yieldState(returned.getLoc(),
                              ac::FiringYieldOp::getOperationName());
    yieldState.addOperands(returned.getValues());
    bodyBuilder.create(yieldState);
    returned.erase();

    OpBuilder builder(rule);
    OperationState state(rule.getLoc(), ac::FiringOp::getOperationName());
    state.addOperands(rule.getInputs());
    state.addTypes(rule.getResultTypes());
    state.addAttribute("output_depths", rule.getOutputDepthsAttr());
    state.addAttribute("output_latencies", rule.getOutputLatenciesAttr());
    state.addAttribute("stable_id", rule.getStableIdAttr());
    state.addAttribute("time_domain", rule.getTimeDomainAttr());
    state.addAttribute("functional_guard", rule->getAttr("ac.rule.guard"));
    state.addAttribute("checks", rule->getAttr("ac.rule.checks"));
    state.addAttribute("handshake", rule->getAttr("ac.rule.handshake"));
    state.addAttribute("schedule", rule->getAttr("ac.rule.schedule"));
    state.addAttribute("effects", rule->getAttr("ac.rule.effects"));
    state.addAttribute("ac.rule_priority", rule->getAttr("ac.rule.priority"));
    state.addAttribute("ac.rule_footprints",
                       rule->getAttr("ac.rule.footprints"));
    state.addAttribute("ac.activation_sources",
                       rule->getAttr("ac.rule.activation_sources"));
    state.addAttribute("ac.transaction_resources",
                       rule->getAttr("ac.rule.transaction_resources"));
    state.addAttribute("ac.initially_active",
                       rule->getAttr("ac.rule.initially_active"));
    state.addAttribute("ac.effects_typed",
                       rule->getAttr("ac.rule.effects_typed"));
    state.addAttribute("ac.checks_typed",
                       rule->getAttr("ac.rule.checks_typed"));
    state.addAttribute("ac.output_presence",
                       rule->getAttr("ac.rule.output_presence"));
    state.addAttribute("ac.state_accesses",
                       rule->getAttr("ac.rule.state_accesses"));
    state.addAttribute("ac.guard_kind", rule->getAttr("ac.rule.guard_kind"));
    state.addAttribute("ac.schedule_kind",
                       rule->getAttr("ac.rule.schedule_kind"));
    state.addAttribute("ac.arbitration_membership",
                       rule->getAttr("ac.rule.arbitration_membership"));
    state.addAttribute("ac.rule_definition", rule.getNameAttr());
    if (Attribute name = rule->getAttr("ac.name"))
      state.addAttribute("ac.name", name);
    state.addRegion();
    auto firing = cast<ac::FiringOp>(builder.create(state));
    firing.getBody().takeBody(rule.getBody());
    rule.replaceAllUsesWith(firing.getResults());
    rule.erase();
  }
  return success();
}

LogicalResult canonicalizePureFirings(ModuleOp model) {
  SmallVector<ac::FiringOp> firings;
  model.walk([&](ac::FiringOp firing) { firings.push_back(firing); });
  Builder attrBuilder(model.getContext());
  for (ac::FiringOp firing : firings) {
    const ArrayAttr pureEffects =
        attrBuilder.getStrArrayAttr({"input.consume", "output.produce"});
    bool hasTableProposal = false;
    firing.getBody().walk([&](ac::TableProposeOp) { hasTableProposal = true; });
    if (hasTableProposal)
      continue;
    if (firing.getInputs().empty() || firing.getOutputs().size() != 1 ||
        firing.getFunctionalGuard() != "true" || !firing.getChecks().empty() ||
        firing.getHandshake() != firingHandshake(firing) ||
        firing.getSchedule() != "independent" ||
        firing.getTimeDomain() != "cycle" || firing.getEffects() != pureEffects)
      return firing.emitOpError(
          "is not proven equivalent to the phase-one pure transform subset");

    SmallVector<ac::FiringConditionOp> conditions;
    firing.getBody().walk([&](ac::FiringConditionOp condition) {
      conditions.push_back(condition);
    });
    if (conditions.size() != 1)
      return firing.emitOpError(
          "pure firing requires one proven functional condition");
    SmallVector<ac::FiringOutputOp> outputs;
    firing.getBody().walk(
        [&](ac::FiringOutputOp output) { outputs.push_back(output); });
    if (outputs.size() != 1 || outputs.front().getOrdinal() != 0 ||
        outputs.front().getWhen() != conditions.front().getCondition())
      return firing.emitOpError(
          "pure firing requires one output on its proven condition");
    outputs.front().erase();
    conditions.front().erase();

    auto yielded =
        dyn_cast<ac::FiringYieldOp>(firing.getBody().front().getTerminator());
    if (!yielded)
      return firing.emitOpError(
          "requires ac.firing.yield before pure-firing canonicalization");
    OpBuilder bodyBuilder(yielded);
    OperationState yieldState(yielded.getLoc(),
                              ac::TransformYieldOp::getOperationName());
    yieldState.addOperands(yielded.getValues());
    bodyBuilder.create(yieldState);
    yielded.erase();

    OpBuilder builder(firing);
    OperationState state(firing.getLoc(), ac::TransformOp::getOperationName());
    state.addOperands(firing.getInputs());
    state.addTypes(firing.getResultTypes());
    state.addAttribute("output_depths", firing.getOutputDepthsAttr());
    state.addAttribute("output_latencies", firing.getOutputLatenciesAttr());
    for (StringRef name : {"ac.name", "ac.rule_definition"})
      if (Attribute attribute = firing->getAttr(name))
        state.addAttribute(name, attribute);
    state.addAttribute("ac.rule_stable_id", firing.getStableIdAttr());
    state.addAttribute("ac.rule_time_domain", firing.getTimeDomainAttr());
    state.addAttribute("ac.rule_guard", firing.getFunctionalGuardAttr());
    state.addAttribute("ac.rule_checks", firing.getChecksAttr());
    state.addAttribute("ac.rule_handshake", firing.getHandshakeAttr());
    state.addAttribute("ac.rule_schedule", firing.getScheduleAttr());
    state.addAttribute("ac.rule_effects", firing.getEffectsAttr());
    state.addAttribute("ac.rule_priority", firing->getAttr("ac.rule_priority"));
    state.addAttribute("ac.rule_footprints",
                       firing->getAttr("ac.rule_footprints"));
    state.addAttribute("ac.activation_sources",
                       firing->getAttr("ac.activation_sources"));
    state.addAttribute("ac.transaction_resources",
                       firing->getAttr("ac.transaction_resources"));
    state.addAttribute("ac.initially_active",
                       firing->getAttr("ac.initially_active"));
    for (StringRef name :
         {"effects_typed", "checks_typed", "output_presence", "state_accesses",
          "guard_kind", "schedule_kind", "arbitration_membership"})
      state.addAttribute("ac.rule_" + name.str(),
                         firing->getAttr("ac." + name.str()));
    state.addRegion();
    auto transform = cast<ac::TransformOp>(builder.create(state));
    transform.getBody().takeBody(firing.getBody());
    firing.replaceAllUsesWith(transform.getResults());
    firing.erase();
  }
  return success();
}

#define GEN_PASS_DEF_INFERRULETYPESPASS
#define GEN_PASS_DEF_INFERRULEEFFECTSPASS
#define GEN_PASS_DEF_INFERRULEACTIVATIONPASS
#define GEN_PASS_DEF_MATERIALIZERULECHECKSPASS
#define GEN_PASS_DEF_MATERIALIZERULEHANDSHAKEPASS
#define GEN_PASS_DEF_DISCHARGERULEOBLIGATIONSPASS
#define GEN_PASS_DEF_RESOLVERULESCHEDULEPASS
#define GEN_PASS_DEF_LOWERRULESTOFIRINGPASS
#define GEN_PASS_DEF_CANONICALIZEPUREFIRINGSPASS
#define GEN_PASS_DEF_VERIFYRULECLOSUREPASS
#include "acir/Transforms/Passes.h.inc"

template <typename Base, LogicalResult (*Implementation)(ModuleOp)>
struct RulePass : Base {
  void runOnOperation() override {
    if (failed(Implementation(this->getOperation())))
      this->signalPassFailure();
  }
};

struct InferRuleTypesPass
    : RulePass<impl::InferRuleTypesPassBase<InferRuleTypesPass>,
               inferRuleTypes> {};
struct InferRuleEffectsPass
    : RulePass<impl::InferRuleEffectsPassBase<InferRuleEffectsPass>,
               inferRuleEffects> {};
struct InferRuleActivationPass
    : RulePass<impl::InferRuleActivationPassBase<InferRuleActivationPass>,
               inferRuleActivation> {};
struct MaterializeRuleChecksPass
    : RulePass<impl::MaterializeRuleChecksPassBase<MaterializeRuleChecksPass>,
               materializeRuleChecks> {};
struct MaterializeRuleHandshakePass
    : RulePass<
          impl::MaterializeRuleHandshakePassBase<MaterializeRuleHandshakePass>,
          materializeRuleHandshake> {};
struct DischargeRuleObligationsPass
    : RulePass<
          impl::DischargeRuleObligationsPassBase<DischargeRuleObligationsPass>,
          dischargeRuleObligations> {};
struct ResolveRuleSchedulePass
    : RulePass<impl::ResolveRuleSchedulePassBase<ResolveRuleSchedulePass>,
               resolveRuleSchedule> {};
struct LowerRulesToFiringPass
    : RulePass<impl::LowerRulesToFiringPassBase<LowerRulesToFiringPass>,
               lowerRulesToFiring> {};
struct CanonicalizePureFiringsPass
    : RulePass<
          impl::CanonicalizePureFiringsPassBase<CanonicalizePureFiringsPass>,
          canonicalizePureFirings> {};

struct VerifyRuleClosurePass
    : impl::VerifyRuleClosurePassBase<VerifyRuleClosurePass> {
  void runOnOperation() override {
    if (failed(verifyRuleClosure(getOperation())))
      signalPassFailure();
  }
};

} // namespace

LogicalResult verifyRuleClosure(ModuleOp model) {
  LogicalResult result = success();
  llvm::StringSet<> stableIds;
  ACDataFlowAnalyzer dataFlow(model.getOperation());
  if (failed(dataFlow.run()))
    return model.emitError("AC dataflow analysis failed during rule closure");
  model.walk([&](Operation *operation) {
    if (failed(result))
      return;
    if (isa<ac::RuleOp, ac::TypeConstraintMarkerOp, ac::ValueFactMarkerOp,
            ac::PendingObligationMarkerOp, ac::VarDeclOp, ac::VarReadOp,
            ac::VarAssignOp, ac::VarReadElementOp, ac::VarAssignElementOp,
            ac::RuleConditionOp>(operation)) {
      result = operation->emitError(
          "unresolved transient rule or typed marker before Frozen ACIR");
      return;
    }
    StringAttr identity;
    if (auto firing = dyn_cast<ac::FiringOp>(operation)) {
      if (failed(firing.verify())) {
        result = failure();
        return;
      }
      SmallVector<StateSnapshotFootprint> actualSnapshots;
      firing.getBody().walk([&](Operation *nested) {
        if (auto snapshot = dyn_cast<ac::StateSnapshotOp>(nested)) {
          StringRef kind =
              snapshot.getIndexKind() == ac::RuleIndexKind::Static ? "static"
              : snapshot.getIndexKind() == ac::RuleIndexKind::Dynamic
                  ? "dynamic"
                  : "all";
          StateSnapshotFootprint footprint{snapshot.getTable().str(),
                                           snapshot.getIndex(),
                                           {},
                                           kind.str(),
                                           snapshot.getPredicate(),
                                           {}};
          for (Attribute field : snapshot.getReadFields())
            footprint.fields.push_back(
                cast<StringAttr>(field).getValue().str());
          actualSnapshots.push_back(std::move(footprint));
        } else if (auto snapshotSet =
                       dyn_cast<ac::StateSnapshotSetOp>(nested)) {
          StateSnapshotFootprint footprint{snapshotSet.getTable().str(), {},
                                           snapshotSet.getSource(),      "set",
                                           snapshotSet.getPredicate(),   {}};
          for (Attribute field : snapshotSet.getReadFields())
            footprint.fields.push_back(
                cast<StringAttr>(field).getValue().str());
          actualSnapshots.push_back(std::move(footprint));
        }
      });
      const SmallVector<StateSnapshotFootprint> expectedSnapshots =
          dataFlow.stateSnapshots(firing.getOperation());
      if (actualSnapshots.size() != expectedSnapshots.size()) {
        result = firing.emitOpError(
            "state snapshot evidence must exactly match predicate reads");
        return;
      }
      for (const auto &[actual, expected] :
           llvm::zip_equal(actualSnapshots, expectedSnapshots)) {
        if (actual.resource != expected.resource ||
            actual.index != expected.index ||
            actual.source != expected.source ||
            actual.predicate != expected.predicate ||
            actual.indexKind != expected.indexKind ||
            actual.fields != expected.fields) {
          result = firing.emitOpError(
              "state snapshot evidence does not match predicate dataflow");
          return;
        }
      }
      identity = firing.getStableIdAttr();
    } else if (auto transform = dyn_cast<ac::TransformOp>(operation)) {
      if (failed(ac::verifyLoweredRuleTransformContract(transform))) {
        result = failure();
        return;
      }
      identity = transform->getAttrOfType<StringAttr>("ac.rule_stable_id");
    }
    if (identity && !stableIds.insert(identity.getValue()).second)
      result = operation->emitError() << "duplicate lowered rule identity '"
                                      << identity.getValue() << "'";
  });
  return result;
}

std::unique_ptr<Pass> createInferRuleTypesPass() {
  return std::make_unique<InferRuleTypesPass>();
}
std::unique_ptr<Pass> createInferRuleEffectsPass() {
  return std::make_unique<InferRuleEffectsPass>();
}
std::unique_ptr<Pass> createInferRuleActivationPass() {
  return std::make_unique<InferRuleActivationPass>();
}
std::unique_ptr<Pass> createMaterializeRuleChecksPass() {
  return std::make_unique<MaterializeRuleChecksPass>();
}
std::unique_ptr<Pass> createMaterializeRuleHandshakePass() {
  return std::make_unique<MaterializeRuleHandshakePass>();
}
std::unique_ptr<Pass> createDischargeRuleObligationsPass() {
  return std::make_unique<DischargeRuleObligationsPass>();
}
std::unique_ptr<Pass> createResolveRuleSchedulePass() {
  return std::make_unique<ResolveRuleSchedulePass>();
}
std::unique_ptr<Pass> createLowerRulesToFiringPass() {
  return std::make_unique<LowerRulesToFiringPass>();
}
std::unique_ptr<Pass> createCanonicalizePureFiringsPass() {
  return std::make_unique<CanonicalizePureFiringsPass>();
}
std::unique_ptr<Pass> createVerifyRuleClosurePass() {
  return std::make_unique<VerifyRuleClosurePass>();
}

void addRuleLoweringPipeline(mlir::OpPassManager &manager) {
  manager.addPass(createVerifyValueConstraintsPass());
  manager.addPass(createLowerVariableStatePass());
  manager.addPass(createVerifyValueConstraintsPass());
  manager.addPass(std::make_unique<InferRuleTypesPass>());
  manager.addPass(std::make_unique<InferRuleEffectsPass>());
  manager.addPass(std::make_unique<InferRuleActivationPass>());
  manager.addPass(std::make_unique<MaterializeRuleChecksPass>());
  manager.addPass(std::make_unique<MaterializeRuleHandshakePass>());
  manager.addPass(std::make_unique<DischargeRuleObligationsPass>());
  manager.addPass(std::make_unique<ResolveRuleSchedulePass>());
  manager.addPass(std::make_unique<LowerRulesToFiringPass>());
  manager.addPass(std::make_unique<CanonicalizePureFiringsPass>());
  manager.addPass(std::make_unique<VerifyRuleClosurePass>());
}

void registerRuleLoweringPipeline() {
  static mlir::PassPipelineRegistration<> registration(
      "ac-lower-rules",
      "Infer and materialize rules into marker-free internal ACIR",
      [](mlir::OpPassManager &manager) { addRuleLoweringPipeline(manager); });
  (void)registration;
}

} // namespace acir
