#include "ProcessStatePlanInternal.h"

#include "acir/Bindings/Binding.h"

#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <cassert>
#include <limits>
#include <set>
#include <tuple>

namespace acir {
namespace {

thread_local std::string lastDiagnostic;

constexpr llvm::StringLiteral kWakeNextDeltaSpecialization =
    R"json({"contract_epoch":"0.5","effect":"stateful","inputs":[],"kind":"implementation","payload":{"wake_kind":"next_delta","wake_type":"@acir_wake_next_delta"},"results":["@acir_wake_next_delta"],"role":"wake_next_delta","schema":"acir-generated-implementation-0.1","source_paths":[]})json";
constexpr llvm::StringLiteral kWakeNextDeltaDigest =
    "a311590813b87bcc30389b814d751def9dfefb49dcd07a9c485b90701a83617e";

mlir::LogicalResult reject(const ProcessStatePlanSet &plans,
                           llvm::StringRef diagnostic) {
  lastDiagnostic = diagnostic.str();
  if (!plans.processes().empty() && plans.processes().front().process())
    plans.processes().front().process().emitError(diagnostic);
  return mlir::failure();
}

bool validDefinitionKey(llvm::StringRef key) {
  size_t separator = key.find("::");
  return separator > 1 && separator + 3 < key.size() && key.front() == '@' &&
         key[separator + 2] == '@' && key.find("::", separator + 2) == key.npos;
}

llvm::StringRef helperRoleSpelling(ProcessHelperRole role) {
  static constexpr llvm::StringLiteral names[] = {"trace_decode",
                                                  "queue_try_send",
                                                  "queue_try_recv",
                                                  "event_schedule",
                                                  "trace_open",
                                                  "trace_next",
                                                  "trace_eof",
                                                  "trace_position",
                                                  "contract_require",
                                                  "contract_ensure",
                                                  "contract_assert",
                                                  "probe",
                                                  "stat_add",
                                                  "wake_condition",
                                                  "wake_resource",
                                                  "wake_event_queue",
                                                  "wake_next_delta",
                                                  "scalar_wrap",
                                                  "scalar_unwrap"};
  return names[static_cast<unsigned>(role)];
}

llvm::StringRef wakeTypeKey(ProcessWakeKind kind) {
  static constexpr llvm::StringLiteral keys[] = {
      "@acir_wake_condition", "@acir_wake_resource", "@acir_wake_event_queue",
      "@acir_wake_next_delta"};
  return keys[static_cast<unsigned>(kind)];
}

bool validTypeKey(llvm::StringRef key) {
  auto validDigest = [](llvm::StringRef value) {
    return value.size() == 64 && llvm::all_of(value, [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
  };
  if (key.starts_with("mlir:"))
    return key.size() > 5;
  if (key.consume_front("storage:value:") ||
      key.consume_front("storage:packet:"))
    return validDigest(key);
  return key == "@acir_wake_condition" || key == "@acir_wake_resource" ||
         key == "@acir_wake_event_queue" || key == "@acir_wake_next_delta";
}

bool validCalleeSemantics(const ProcessGeneratedCalleePlan &callee) {
  auto inputs = callee.inputTypeKeys();
  auto results = callee.resultTypeKeys();
  const ProcessGeneratedCalleePayload &payload = callee.payload();
  switch (callee.role()) {
  case ProcessHelperRole::TraceDecode:
    return inputs.size() == 1 && results.size() == 1 &&
           inputs[0] == payload.traceDecode().entry() &&
           results[0] == payload.traceDecode().result();
  case ProcessHelperRole::QueueTrySend:
    return inputs.size() == 1 && results.size() == 1 &&
           inputs[0] == payload.queueTrySend().element();
  case ProcessHelperRole::QueueTryRecv:
    return inputs.empty() && results.size() == 2 &&
           results[0] == payload.queueTryRecv().element();
  case ProcessHelperRole::EventSchedule:
    return inputs.size() == 2 && results.empty() &&
           inputs[0] == payload.eventSchedule().value() &&
           inputs[1] == payload.eventSchedule().delay();
  case ProcessHelperRole::TraceOpen:
    return inputs.empty() && results.size() == 1;
  case ProcessHelperRole::TraceNext:
    return inputs.size() == 1 && results.size() == 3 &&
           inputs[0] == results[0] && results[1] == payload.traceNext().entry();
  case ProcessHelperRole::TraceEof:
  case ProcessHelperRole::TracePosition:
    return inputs.size() == 1 && results.size() == 1;
  case ProcessHelperRole::ContractRequire:
  case ProcessHelperRole::ContractEnsure:
  case ProcessHelperRole::ContractAssert:
    return inputs.size() == 1 && results.empty();
  case ProcessHelperRole::Probe:
    return inputs.empty() && results.size() == 1 &&
           results[0] == payload.probe().result();
  case ProcessHelperRole::StatAdd:
    return inputs.size() == 1 && results.empty() &&
           inputs[0] == payload.statAdd().valueType();
  case ProcessHelperRole::WakeCondition:
    return inputs.empty() && results.size() == 1 &&
           payload.wakeCondition().wakeKind() == ProcessWakeKind::Condition &&
           payload.wakeCondition().wakeType() ==
               wakeTypeKey(ProcessWakeKind::Condition) &&
           results[0] == payload.wakeCondition().wakeType();
  case ProcessHelperRole::WakeResource:
    return inputs.empty() && results.size() == 1 &&
           payload.wakeResource().wakeKind() == ProcessWakeKind::Resource &&
           payload.wakeResource().wakeType() ==
               wakeTypeKey(ProcessWakeKind::Resource) &&
           results[0] == payload.wakeResource().wakeType();
  case ProcessHelperRole::WakeEventQueue:
    return inputs.empty() && results.size() == 1 &&
           payload.wakeEventQueue().wakeKind() == ProcessWakeKind::EventQueue &&
           payload.wakeEventQueue().wakeType() ==
               wakeTypeKey(ProcessWakeKind::EventQueue) &&
           results[0] == payload.wakeEventQueue().wakeType();
  case ProcessHelperRole::WakeNextDelta:
    return inputs.empty() && results.size() == 1 &&
           payload.wakeNextDelta().wakeKind() == ProcessWakeKind::NextDelta &&
           payload.wakeNextDelta().wakeType() ==
               wakeTypeKey(ProcessWakeKind::NextDelta) &&
           results[0] == payload.wakeNextDelta().wakeType();
  case ProcessHelperRole::ScalarWrap:
    return inputs.size() == 1 && results.size() == 1 &&
           payload.scalarWrap().direction() == ProcessWrapperDirection::Wrap &&
           inputs[0] == payload.scalarWrap().scalar() &&
           results[0] == payload.scalarWrap().valueType();
  case ProcessHelperRole::ScalarUnwrap:
    return inputs.size() == 1 && results.size() == 1 &&
           payload.scalarUnwrap().direction() ==
               ProcessWrapperDirection::Unwrap &&
           inputs[0] == payload.scalarUnwrap().valueType() &&
           results[0] == payload.scalarUnwrap().scalar();
  }
  return false;
}

} // namespace

mlir::FailureOr<ProcessStatePlanSet>
detail::PlanSetBuilder::buildEmpty(mlir::ModuleOp module) {
  bool hasProcess = false;
  module.walk([&](ac::ProcessOp) { hasProcess = true; });
  if (hasProcess) {
    module.emitError(
        "empty process-state fixture requires zero ac.process operations");
    return mlir::failure();
  }
  auto epoch = module->getAttrOfType<mlir::StringAttr>("ac.contract_epoch");
  if (!epoch || epoch.getValue() != "0.5") {
    module.emitError("empty process-state fixture requires contract epoch 0.5");
    return mlir::failure();
  }
  return ProcessStatePlanSet(std::make_shared<ProcessStatePlanSet::Impl>());
}

mlir::FailureOr<ProcessStatePlanSet>
detail::PlanSetBuilder::buildProduction(mlir::ModuleOp module,
                                        const ProcessStateLimits &limits) {
  auto frozenEpoch = module->getAttrOfType<mlir::StringAttr>("ac.freeze_epoch");
  if (!frozenEpoch || frozenEpoch.getValue() != "0.5") {
    module.emitError("process-state planning requires a frozen model");
    return mlir::failure();
  }

  llvm::SmallVector<ac::ProcessOp> processes;
  module.walk([&](ac::ProcessOp process) { processes.push_back(process); });
  if (processes.empty()) {
    module.emitError("process-state planning requires at least one ac.process");
    return mlir::failure();
  }
  if (processes.size() > limits.maxProcesses) {
    module.emitError("process-state plan capability maxProcesses exceeded");
    return mlir::failure();
  }
  auto definitionKey = [](ac::ProcessOp process) {
    ac::ModuleOp owner = process->getParentOfType<ac::ModuleOp>();
    return ("@" + owner.getSymName() + "::@" + process.getSymName()).str();
  };
  llvm::sort(processes, [&](ac::ProcessOp lhs, ac::ProcessOp rhs) {
    return definitionKey(lhs) < definitionKey(rhs);
  });

  struct PendingProcess {
    ac::ProcessOp process;
    std::string definitionKey;
    std::unique_ptr<ControlPlan> control;
  };
  std::vector<PendingProcess> pending;
  ac::RawModelStructureLimits rawLimits;
  rawLimits.maxNodes = limits.maxPlannedOperations;
  rawLimits.maxEdges = limits.maxTransitions;
  rawLimits.maxNestedRegionDepth = limits.maxNestedRegionDepth;
  for (ac::ProcessOp process : processes) {
    auto expanded = expandProcess(process, rawLimits);
    if (mlir::failed(expanded))
      return mlir::failure();
    auto control = planProcessContinuation(*expanded, limits);
    if (mlir::failed(control))
      return mlir::failure();
    control = planProcessWakes(std::move(*control), limits);
    if (mlir::failed(control) ||
        mlir::failed(planProcessLiveness(**control, limits)) ||
        mlir::failed(planProcessCost(**control, limits)))
      return mlir::failure();
    pending.push_back({process, expanded->definitionKey, std::move(*control)});
  }

  auto makeWakePayload = [](ProcessWakeKind kind) {
    auto payload = std::make_shared<ProcessGeneratedCalleePayload::Impl>();
    payload->role = static_cast<ProcessHelperRole>(
        static_cast<unsigned>(ProcessHelperRole::WakeCondition) +
        static_cast<unsigned>(kind));
    switch (kind) {
    case ProcessWakeKind::Condition: {
      auto arm = std::make_shared<ProcessWakeConditionPayload::Impl>();
      arm->wakeKind = kind;
      arm->wakeType = wakeTypeKey(kind).str();
      payload->wakeCondition = ProcessWakeConditionPayload(arm);
      break;
    }
    case ProcessWakeKind::Resource: {
      auto arm = std::make_shared<ProcessWakeResourcePayload::Impl>();
      arm->wakeKind = kind;
      arm->wakeType = wakeTypeKey(kind).str();
      payload->wakeResource = ProcessWakeResourcePayload(arm);
      break;
    }
    case ProcessWakeKind::EventQueue: {
      auto arm = std::make_shared<ProcessWakeEventQueuePayload::Impl>();
      arm->wakeKind = kind;
      arm->wakeType = wakeTypeKey(kind).str();
      payload->wakeEventQueue = ProcessWakeEventQueuePayload(arm);
      break;
    }
    case ProcessWakeKind::NextDelta: {
      auto arm = std::make_shared<ProcessWakeNextDeltaPayload::Impl>();
      arm->wakeKind = kind;
      arm->wakeType = wakeTypeKey(kind).str();
      payload->wakeNextDelta = ProcessWakeNextDeltaPayload(arm);
      break;
    }
    }
    return ProcessGeneratedCalleePayload(payload);
  };

  llvm::SmallVector<bool, 4> usedWakeKinds(4, false);
  for (const PendingProcess &item : pending)
    for (const auto &wake : item.control->wakes)
      usedWakeKinds[static_cast<unsigned>(wake->kind)] = true;
  std::vector<std::shared_ptr<ProcessGeneratedCalleePlan::Impl>> callees;
  for (unsigned kindIndex = 0; kindIndex < usedWakeKinds.size(); ++kindIndex) {
    if (!usedWakeKinds[kindIndex])
      continue;
    ProcessWakeKind kind = static_cast<ProcessWakeKind>(kindIndex);
    auto callee = std::make_shared<ProcessGeneratedCalleePlan::Impl>();
    callee->effect = ProcessEffectKind::Stateful;
    callee->role = static_cast<ProcessHelperRole>(
        static_cast<unsigned>(ProcessHelperRole::WakeCondition) + kindIndex);
    callee->payload = makeWakePayload(kind);
    callee->resultTypeKeyStorage = {wakeTypeKey(kind).str()};
    callee->resultTypeKeys = {callee->resultTypeKeyStorage.front()};
    ProcessGeneratedCalleePlan value(callee);
    auto canonical = canonicalGeneratedCalleeSpecialization(value);
    if (!canonical) {
      llvm::consumeError(canonical.takeError());
      return mlir::failure();
    }
    callee->specializationBytes = std::move(*canonical);
    callee->fingerprint =
        bindings::sha256Fingerprint(callee->specializationBytes);
    llvm::StringRef digest = llvm::StringRef(callee->fingerprint).drop_front(7);
    callee->symbol =
        ("@acir_impl_" + helperRoleSpelling(callee->role) + "_" + digest).str();
    callee->cpp = ("acir::generated::impl_" + helperRoleSpelling(callee->role) +
                   "_" + digest)
                      .str();
    callees.push_back(std::move(callee));
  }
  auto typeSpelling = [](mlir::Type type) {
    std::string storage;
    llvm::raw_string_ostream stream(storage);
    stream << type;
    return storage;
  };
  llvm::SmallVector<mlir::Type> liveTypes;
  for (const PendingProcess &item : pending)
    for (const auto &slot : item.control->liveSlots)
      if (!llvm::is_contained(liveTypes, slot->type))
        liveTypes.push_back(slot->type);
  std::vector<std::shared_ptr<ProcessValueTypePlan::Impl>> valueTypes;
  for (mlir::Type type : liveTypes) {
    uint64_t width =
        type.isIndex() ? 64 : mlir::cast<mlir::IntegerType>(type).getWidth();
    std::string spelling = typeSpelling(type);
    auto storage = std::make_shared<ProcessStorageValuePayload::Impl>();
    storage->widthBits = width;
    storage->encoding = spelling;
    auto payload = std::make_shared<ProcessValueTypePayload::Impl>();
    payload->kind = ProcessValueTypeKind::Value;
    payload->value = ProcessStorageValuePayload(storage);
    auto value = std::make_shared<ProcessValueTypePlan::Impl>();
    value->id = ProcessValueTypeId(0);
    value->kind = ProcessValueTypeKind::Value;
    value->acirType = type;
    value->payload = ProcessValueTypePayload(payload);
    ProcessValueTypePlan plan(value);
    auto canonical = canonicalValueTypeSpecialization(plan);
    if (!canonical) {
      llvm::consumeError(canonical.takeError());
      return mlir::failure();
    }
    value->specializationBytes = std::move(*canonical);
    value->fingerprint =
        bindings::sha256Fingerprint(value->specializationBytes);
    llvm::StringRef digest = llvm::StringRef(value->fingerprint).drop_front(7);
    value->symbol = ("@acir_value_" + digest).str();
    value->cpp = ("acir::generated::value_" + digest).str();
    valueTypes.push_back(std::move(value));
  }
  llvm::sort(valueTypes, [](const auto &lhs, const auto &rhs) {
    return lhs->specializationBytes < rhs->specializationBytes;
  });
  for (auto [index, type] : llvm::enumerate(valueTypes))
    type->id = ProcessValueTypeId(index);

  auto addScalarCallee = [&](const auto &type,
                             ProcessHelperRole role) -> llvm::Error {
    std::string scalarKey = "mlir:" + typeSpelling(type->acirType);
    std::string valueKey =
        "storage:value:" +
        llvm::StringRef(type->fingerprint).drop_front(7).str();
    auto payload = std::make_shared<ProcessGeneratedCalleePayload::Impl>();
    payload->role = role;
    if (role == ProcessHelperRole::ScalarWrap) {
      auto arm = std::make_shared<ProcessScalarWrapPayload::Impl>();
      arm->direction = ProcessWrapperDirection::Wrap;
      arm->scalar = scalarKey;
      arm->valueType = valueKey;
      payload->scalarWrap = ProcessScalarWrapPayload(arm);
    } else {
      auto arm = std::make_shared<ProcessScalarUnwrapPayload::Impl>();
      arm->direction = ProcessWrapperDirection::Unwrap;
      arm->scalar = scalarKey;
      arm->valueType = valueKey;
      payload->scalarUnwrap = ProcessScalarUnwrapPayload(arm);
    }
    auto callee = std::make_shared<ProcessGeneratedCalleePlan::Impl>();
    callee->effect = ProcessEffectKind::Pure;
    callee->role = role;
    callee->payload = ProcessGeneratedCalleePayload(payload);
    if (role == ProcessHelperRole::ScalarWrap) {
      callee->inputTypeKeyStorage = {scalarKey};
      callee->resultTypeKeyStorage = {valueKey};
    } else {
      callee->inputTypeKeyStorage = {valueKey};
      callee->resultTypeKeyStorage = {scalarKey};
    }
    callee->inputTypeKeys = {callee->inputTypeKeyStorage.front()};
    callee->resultTypeKeys = {callee->resultTypeKeyStorage.front()};
    ProcessGeneratedCalleePlan value(callee);
    auto canonical = canonicalGeneratedCalleeSpecialization(value);
    if (!canonical)
      return canonical.takeError();
    callee->specializationBytes = std::move(*canonical);
    callee->fingerprint =
        bindings::sha256Fingerprint(callee->specializationBytes);
    llvm::StringRef digest = llvm::StringRef(callee->fingerprint).drop_front(7);
    callee->symbol =
        ("@acir_impl_" + helperRoleSpelling(role) + "_" + digest).str();
    callee->cpp =
        ("acir::generated::impl_" + helperRoleSpelling(role) + "_" + digest)
            .str();
    callees.push_back(std::move(callee));
    return llvm::Error::success();
  };
  for (const auto &type : valueTypes) {
    if (llvm::Error error =
            addScalarCallee(type, ProcessHelperRole::ScalarWrap)) {
      llvm::consumeError(std::move(error));
      return mlir::failure();
    }
    if (llvm::Error error =
            addScalarCallee(type, ProcessHelperRole::ScalarUnwrap)) {
      llvm::consumeError(std::move(error));
      return mlir::failure();
    }
  }

  llvm::sort(callees, [](const auto &lhs, const auto &rhs) {
    return lhs->specializationBytes < rhs->specializationBytes;
  });
  std::array<ProcessCalleeId, 4> wakeCalleeIds = {
      ProcessCalleeId(0), ProcessCalleeId(0), ProcessCalleeId(0),
      ProcessCalleeId(0)};
  for (auto [index, callee] : llvm::enumerate(callees)) {
    callee->id = ProcessCalleeId(index);
    if (callee->role >= ProcessHelperRole::WakeCondition &&
        callee->role <= ProcessHelperRole::WakeNextDelta) {
      unsigned kindIndex =
          static_cast<unsigned>(callee->role) -
          static_cast<unsigned>(ProcessHelperRole::WakeCondition);
      wakeCalleeIds[kindIndex] = ProcessCalleeId(index);
    }
  }

  auto set = std::make_shared<ProcessStatePlanSet::Impl>();
  for (auto &callee : callees)
    set->callees.push_back(ProcessGeneratedCalleePlan(callee));
  for (auto &type : valueTypes)
    set->valueTypes.push_back(ProcessValueTypePlan(type));
  for (PendingProcess &item : pending) {
    auto process = std::make_shared<ProcessStatePlan::Impl>();
    process->definitionKey = item.definitionKey;
    process->process = item.process;
    process->entryPc = ProcessPcId(0);
    for (auto &pc : item.control->pcs)
      process->pcs.push_back(ProcessPcPlan(pc));
    for (auto &block : item.control->blocks)
      process->blocks.push_back(ProcessBlockPlan(block));
    for (auto &wake : item.control->wakes) {
      wake->callee = wakeCalleeIds[static_cast<unsigned>(wake->kind)];
      process->wakes.push_back(ProcessWakePlan(wake));
    }
    for (auto &transition : item.control->transitions)
      process->transitions.push_back(ProcessTransitionPlan(transition));
    for (auto &slot : item.control->liveSlots) {
      auto found = llvm::find_if(valueTypes, [&](const auto &type) {
        return type->acirType == slot->type;
      });
      if (found == valueTypes.end())
        return mlir::failure();
      slot->storageType = (*found)->id;
      std::string scalarKey = "mlir:" + typeSpelling(slot->type);
      std::string valueKey =
          "storage:value:" +
          llvm::StringRef((*found)->fingerprint).drop_front(7).str();
      auto wrap = llvm::find_if(callees, [&](const auto &callee) {
        return callee->role == ProcessHelperRole::ScalarWrap &&
               callee->inputTypeKeys.front() == scalarKey &&
               callee->resultTypeKeys.front() == valueKey;
      });
      auto unwrap = llvm::find_if(callees, [&](const auto &callee) {
        return callee->role == ProcessHelperRole::ScalarUnwrap &&
               callee->inputTypeKeys.front() == valueKey &&
               callee->resultTypeKeys.front() == scalarKey;
      });
      if (wrap == callees.end() || unwrap == callees.end())
        return mlir::failure();
      slot->wrapCallee = (*wrap)->id;
      slot->unwrapCallee = (*unwrap)->id;
      process->liveSlots.push_back(ProcessLiveSlotPlan(slot));
    }
    for (auto &block : item.control->blocks) {
      for (const ProcessActionPlan &action : block->actions) {
        if (action.kind() != ProcessActionKind::ScalarWrap &&
            action.kind() != ProcessActionKind::ScalarUnwrap)
          continue;
        auto mutableAction =
            std::const_pointer_cast<ProcessActionPlan::Impl>(action.impl_);
        ProcessLiveSlotId slot = action.occurrence().syntheticWrapper().slot();
        if (slot.value() >= item.control->liveSlots.size())
          return mlir::failure();
        mutableAction->callee =
            action.kind() == ProcessActionKind::ScalarWrap
                ? item.control->liveSlots[slot.value()]->wrapCallee
                : item.control->liveSlots[slot.value()]->unwrapCallee;
      }
    }
    for (auto [index, operand] : llvm::enumerate(item.process->getOperands())) {
      auto capture = std::make_shared<ProcessCapturePlan::Impl>();
      capture->id = ProcessCaptureId(index);
      capture->name = llvm::formatv("capture{0:D8}", index).str();
      capture->operand = operand;
      capture->entryArgument =
          item.process.getBody().front().getArgument(index);
      capture->type = operand.getType();
      capture->operandPath =
          item.definitionKey + "/capture/operand/" + std::to_string(index);
      capture->argumentPath =
          item.definitionKey + "/capture/argument/" + std::to_string(index);
      process->captures.push_back(ProcessCapturePlan(capture));
    }
    process->pcBitWidth = 1;
    for (uint32_t largest = static_cast<uint32_t>(process->pcs.size() - 1);
         largest >>= 1;)
      ++process->pcBitWidth;
    process->fairnessWork = item.control->fairnessWork;
    set->processes.push_back(ProcessStatePlan(process));
  }
  ProcessStatePlanSet result(set);
  if (mlir::failed(verifyProcessStatePlan(result, limits)))
    return mlir::failure();
  return result;
}

mlir::FailureOr<ProcessStatePlanSet>
planProcessState(mlir::ModuleOp module, const ProcessStateLimits &limits) {
  return detail::PlanSetBuilder::buildProduction(module, limits);
}

mlir::FailureOr<ProcessStatePlanSet>
detail::PlanSetBuilder::buildYieldOnly(mlir::ModuleOp module) {
  return buildFrozenFixture(module, /*requireYieldOnly=*/true);
}

mlir::FailureOr<ProcessStatePlanSet>
detail::PlanSetBuilder::buildFrozenFixture(mlir::ModuleOp module,
                                           bool requireYieldOnly) {
  auto frozenEpoch = module->getAttrOfType<mlir::StringAttr>("ac.freeze_epoch");
  if (!frozenEpoch || frozenEpoch.getValue() != "0.5") {
    module.emitError(requireYieldOnly
                         ? "yield-only process-state fixture requires a frozen "
                           "model"
                         : "loop-action process-state fixture requires a "
                           "frozen model");
    return mlir::failure();
  }

  struct ProcessFixture {
    std::string definitionKey;
    ac::ProcessOp process;
    ac::YieldSimOp yield;
  };
  llvm::SmallVector<ProcessFixture> fixtures;
  module.walk([&](ac::ProcessOp process) {
    auto owner = process->getParentOfType<ac::ModuleOp>();
    auto moduleName = owner ? mlir::SymbolTable::getSymbolName(owner) : nullptr;
    auto processName = mlir::SymbolTable::getSymbolName(process);
    ac::YieldSimOp yield;
    unsigned yieldCount = 0;
    process.walk([&](ac::YieldSimOp candidate) {
      yield = candidate;
      ++yieldCount;
    });
    bool validBody = process.getBody().hasOneBlock() &&
                     (!requireYieldOnly ||
                      llvm::hasSingleElement(process.getBody().front()));
    if (!owner || !moduleName || !processName || yieldCount != 1 ||
        !validBody) {
      fixtures.push_back({{}, process, {}});
      return;
    }
    fixtures.push_back(
        {("@" + moduleName.str() + "::@" + processName.str()), process, yield});
  });
  if (fixtures.empty()) {
    module.emitError(requireYieldOnly
                         ? "yield-only process-state fixture requires at least "
                           "one ac.process"
                         : "loop-action process-state fixture requires exactly "
                           "one ac.process");
    return mlir::failure();
  }
  if (llvm::any_of(fixtures, [](const ProcessFixture &fixture) {
        return fixture.definitionKey.empty() || !fixture.yield;
      })) {
    module.emitError(
        requireYieldOnly
            ? "yield-only process-state fixture requires exactly "
              "one ac.yield_sim per process and no other operations"
            : "loop-action process-state fixture requires exactly "
              "one ac.yield_sim per process");
    return mlir::failure();
  }
  llvm::sort(fixtures,
             [](const ProcessFixture &lhs, const ProcessFixture &rhs) {
               return lhs.definitionKey < rhs.definitionKey;
             });
  for (auto [index, fixture] : llvm::enumerate(fixtures))
    if (index && fixtures[index - 1].definitionKey == fixture.definitionKey) {
      module.emitError(requireYieldOnly
                           ? "yield-only process-state fixture has duplicate "
                             "definition key"
                           : "loop-action process-state fixture has duplicate "
                             "definition key");
      return mlir::failure();
    }

  auto wakePayloadImpl = std::make_shared<ProcessWakeNextDeltaPayload::Impl>();
  wakePayloadImpl->wakeKind = ProcessWakeKind::NextDelta;
  wakePayloadImpl->wakeType = "@acir_wake_next_delta";
  ProcessWakeNextDeltaPayload wakePayload(wakePayloadImpl);
  auto payloadImpl = std::make_shared<ProcessGeneratedCalleePayload::Impl>();
  payloadImpl->role = ProcessHelperRole::WakeNextDelta;
  payloadImpl->wakeNextDelta = wakePayload;
  ProcessGeneratedCalleePayload payload(payloadImpl);

  auto calleeImpl = std::make_shared<ProcessGeneratedCalleePlan::Impl>();
  calleeImpl->id = ProcessCalleeId(0);
  calleeImpl->symbol =
      "@acir_impl_wake_next_delta_" + kWakeNextDeltaDigest.str();
  calleeImpl->cpp =
      "acir::generated::impl_wake_next_delta_" + kWakeNextDeltaDigest.str();
  calleeImpl->fingerprint = "sha256:" + kWakeNextDeltaDigest.str();
  calleeImpl->effect = ProcessEffectKind::Stateful;
  calleeImpl->resultTypeKeyStorage = {"@acir_wake_next_delta"};
  for (const std::string &key : calleeImpl->resultTypeKeyStorage)
    calleeImpl->resultTypeKeys.push_back(key);
  calleeImpl->role = ProcessHelperRole::WakeNextDelta;
  calleeImpl->payload = payload;
  calleeImpl->specializationBytes = kWakeNextDeltaSpecialization.str();
  llvm::json::Object descriptor;
  descriptor["cpp"] = calleeImpl->cpp;
  descriptor["effect"] = "stateful";
  descriptor["fingerprint"] = calleeImpl->fingerprint;
  descriptor["inputs"] = llvm::json::Array();
  descriptor["kind"] = "implementation";
  descriptor["ordinal"] = 0;
  llvm::json::Object descriptorPayload;
  descriptorPayload["wake_kind"] = "next_delta";
  descriptorPayload["wake_type"] = "@acir_wake_next_delta";
  descriptor["payload"] = std::move(descriptorPayload);
  llvm::json::Array results;
  results.push_back("@acir_wake_next_delta");
  descriptor["results"] = std::move(results);
  descriptor["role"] = "wake_next_delta";
  descriptor["source_paths"] = llvm::json::Array();
  descriptor["symbol"] = calleeImpl->symbol;
  if (auto canonical =
          bindings::canonicalizeJson(llvm::json::Value(std::move(descriptor))))
    calleeImpl->descriptorBytes = std::move(*canonical);
  ProcessGeneratedCalleePlan callee(calleeImpl);

  auto setImpl = std::make_shared<ProcessStatePlanSet::Impl>();
  setImpl->callees.push_back(callee);
  for (ProcessFixture &fixture : fixtures) {
    uint32_t yieldOrdinal = 0;
    for (mlir::Operation &operation : fixture.process.getBody().front()) {
      if (&operation == fixture.yield.getOperation())
        break;
      ++yieldOrdinal;
    }
    std::string operationPath =
        fixture.definitionKey + "/r0/b0/o" + std::to_string(yieldOrdinal);
    auto originalImpl = std::make_shared<ProcessOriginalOccurrence::Impl>();
    originalImpl->operation = fixture.yield;
    originalImpl->operationPath = operationPath;
    ProcessOriginalOccurrence original(originalImpl);
    auto occurrenceImpl = std::make_shared<ProcessOccurrenceId::Impl>();
    occurrenceImpl->kind = ProcessOccurrenceKind::Original;
    occurrenceImpl->original = original;
    ProcessOccurrenceId occurrence(occurrenceImpl);

    auto wakeImpl = std::make_shared<ProcessWakePlan::Impl>();
    wakeImpl->id = ProcessWakeId(0);
    wakeImpl->kind = ProcessWakeKind::NextDelta;
    wakeImpl->operation = fixture.yield;
    wakeImpl->callee = ProcessCalleeId(0);
    wakeImpl->typeKey = "@acir_wake_next_delta";
    wakeImpl->operationPath = operationPath;
    wakeImpl->target = "";
    wakeImpl->occurrence = occurrence;
    ProcessWakePlan wake(wakeImpl);

    auto transitionImpl = std::make_shared<ProcessTransitionPlan::Impl>();
    transitionImpl->id = ProcessTransitionId(0);
    transitionImpl->sourcePc = ProcessPcId(0);
    transitionImpl->targetPc = ProcessPcId(0);
    transitionImpl->wake = ProcessWakeId(0);
    ProcessTransitionPlan transition(transitionImpl);

    auto edgeImpl = std::make_shared<ProcessControlEdgePlan::Impl>();
    edgeImpl->kind = ProcessControlEdgeKind::Suspend;
    edgeImpl->transition = ProcessTransitionId(0);
    ProcessControlEdgePlan edge(edgeImpl);

    auto blockImpl = std::make_shared<ProcessBlockPlan::Impl>();
    blockImpl->id = ProcessBlockId(0);
    blockImpl->pc = ProcessPcId(0);
    blockImpl->originRegion = &fixture.process.getBody();
    blockImpl->originBlock = &fixture.process.getBody().front();
    blockImpl->path = fixture.definitionKey + "/plan/pc/entry/b00000000";
    blockImpl->edge = edge;
    blockImpl->cost = 2;
    ProcessBlockPlan block(blockImpl);

    auto pcImpl = std::make_shared<ProcessPcPlan::Impl>();
    pcImpl->id = ProcessPcId(0);
    pcImpl->name = "entry";
    pcImpl->entryPath = blockImpl->path;
    pcImpl->blocks.push_back(ProcessBlockId(0));
    ProcessPcPlan pc(pcImpl);

    auto processImpl = std::make_shared<ProcessStatePlan::Impl>();
    processImpl->definitionKey = fixture.definitionKey;
    processImpl->process = fixture.process;
    processImpl->entryPc = ProcessPcId(0);
    processImpl->pcs.push_back(pc);
    processImpl->blocks.push_back(block);
    processImpl->wakes.push_back(wake);
    processImpl->transitions.push_back(transition);
    processImpl->pcBitWidth = 1;
    processImpl->fairnessWork = 2;
    setImpl->processes.push_back(ProcessStatePlan(processImpl));
  }
  return ProcessStatePlanSet(setImpl);
}

mlir::FailureOr<ProcessStatePlanSet>
detail::PlanSetBuilder::buildLoopActionFixture(mlir::ModuleOp module) {
  auto built = buildFrozenFixture(module, /*requireYieldOnly=*/false);
  if (mlir::failed(built))
    return mlir::failure();
  if (built->impl_->processes.size() != 1) {
    module.emitError(
        "loop-action process-state fixture requires exactly one ac.process");
    return mlir::failure();
  }

  auto process = std::make_shared<ProcessStatePlan::Impl>(
      *built->impl_->processes.front().impl_);
  llvm::SmallVector<mlir::scf::ForOp> loops;
  process->process.walk([&](mlir::scf::ForOp loop) { loops.push_back(loop); });
  if (loops.size() != 2) {
    module.emitError(
        "loop-action process-state fixture requires exactly two scf.for "
        "operations");
    return mlir::failure();
  }

  uint32_t loopOrdinal = 0;
  for (mlir::Operation &operation : process->process.getBody().front()) {
    if (&operation == loops.front().getOperation())
      break;
    ++loopOrdinal;
  }
  auto original = std::make_shared<ProcessOriginalOccurrence::Impl>();
  original->operation = loops.front();
  original->operationPath =
      process->definitionKey + "/r0/b0/o" + std::to_string(loopOrdinal);
  auto anchor = std::make_shared<ProcessOccurrenceId::Impl>();
  anchor->kind = ProcessOccurrenceKind::Original;
  anchor->original = ProcessOriginalOccurrence(original);
  ProcessOccurrenceId loopAnchor(anchor);

  auto makeLoopOccurrence = [&](ProcessLoopPhase phase) {
    auto loop = std::make_shared<ProcessSyntheticLoopOccurrence::Impl>();
    loop->anchor = loopAnchor;
    loop->phase = phase;
    auto occurrence = std::make_shared<ProcessOccurrenceId::Impl>();
    occurrence->kind = ProcessOccurrenceKind::SyntheticLoop;
    occurrence->syntheticLoop = ProcessSyntheticLoopOccurrence(loop);
    return ProcessOccurrenceId(occurrence);
  };
  auto makeConstant = [](mlir::Type type, llvm::StringRef literal) {
    auto constant = std::make_shared<ProcessConstantPlannedValue::Impl>();
    constant->value = literal.str();
    auto value = std::make_shared<ProcessPlannedValue::Impl>();
    value->kind = ProcessPlannedValueKind::Constant;
    value->type = type;
    value->constant = ProcessConstantPlannedValue(constant);
    return ProcessPlannedValue(value);
  };

  mlir::MLIRContext *context = module.getContext();
  mlir::Type indexType = mlir::IndexType::get(context);
  ProcessPlannedValue lower = makeConstant(indexType, "0");
  ProcessPlannedValue upper = makeConstant(indexType, "4");
  ProcessPlannedValue step = makeConstant(indexType, "1");
  ProcessPlannedValue condition =
      makeConstant(mlir::IntegerType::get(context, 1), "true");

  auto predicate = std::make_shared<ProcessScalarAttribute::Impl>();
  predicate->name = "predicate";
  predicate->value = "2 : i64";
  auto comparison = std::make_shared<ProcessScalarOperationPlan::Impl>();
  comparison->name = "arith.cmpi";
  comparison->attributes.push_back(ProcessScalarAttribute(predicate));
  comparison->properties = "{}";
  auto increment = std::make_shared<ProcessScalarOperationPlan::Impl>();
  increment->name = "arith.addi";
  increment->properties = "{}";

  auto makeAction =
      [&](uint32_t id, ProcessActionKind kind, ProcessEmissionClass emission,
          ProcessLoopPhase phase, std::vector<ProcessPlannedValue> operands,
          std::vector<ProcessPlannedValue> results,
          std::shared_ptr<ProcessScalarOperationPlan::Impl> scalarOperation) {
        auto action = std::make_shared<ProcessActionPlan::Impl>();
        action->id = id;
        action->kind = kind;
        action->emission = emission;
        action->occurrence = makeLoopOccurrence(phase);
        action->sourceOperation = loops.front();
        action->operands = std::move(operands);
        action->results = std::move(results);
        action->cost = emission == ProcessEmissionClass::ForwardOnly ? 0 : 1;
        for (const ProcessPlannedValue &result : action->results)
          action->resultTypes.push_back(result.type());
        if (scalarOperation)
          action->scalarOp =
              ProcessScalarOperationPlan(std::move(scalarOperation));
        return ProcessActionPlan(action);
      };

  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  block->actions = {
      makeAction(0, ProcessActionKind::ForInitialize,
                 ProcessEmissionClass::ForwardOnly,
                 ProcessLoopPhase::Initialize, {lower}, {lower}, nullptr),
      makeAction(1, ProcessActionKind::ForCondition,
                 ProcessEmissionClass::CopyScalar, ProcessLoopPhase::Condition,
                 {lower, upper}, {condition}, comparison),
      makeAction(2, ProcessActionKind::ForIncrement,
                 ProcessEmissionClass::CopyScalar, ProcessLoopPhase::Increment,
                 {lower, step}, {step}, increment),
  };
  block->cost = 4;
  process->blocks.front() = ProcessBlockPlan(block);
  process->fairnessWork = 4;

  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*built->impl_);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithCorruption(
    const ProcessStatePlanSet &plans,
    ProcessStatePlanCorruptionForTest corruption) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto cloneProcess = [&]() {
    auto value = std::make_shared<ProcessStatePlan::Impl>(
        *impl->processes.front().impl_);
    impl->processes.front() = ProcessStatePlan(value);
    return value;
  };
  auto cloneBlock = [&]() {
    auto process = cloneProcess();
    auto value = std::make_shared<ProcessBlockPlan::Impl>(
        *process->blocks.front().impl_);
    process->blocks.front() = ProcessBlockPlan(value);
    return value;
  };
  auto cloneWake = [&]() {
    auto process = cloneProcess();
    auto value =
        std::make_shared<ProcessWakePlan::Impl>(*process->wakes.front().impl_);
    process->wakes.front() = ProcessWakePlan(value);
    return value;
  };
  auto cloneTransition = [&]() {
    auto process = cloneProcess();
    auto value = std::make_shared<ProcessTransitionPlan::Impl>(
        *process->transitions.front().impl_);
    process->transitions.front() = ProcessTransitionPlan(value);
    return value;
  };
  auto cloneCallee = [&]() {
    auto value = std::make_shared<ProcessGeneratedCalleePlan::Impl>(
        *impl->callees.front().impl_);
    impl->callees.front() = ProcessGeneratedCalleePlan(value);
    return value;
  };
  switch (corruption) {
  case ProcessStatePlanCorruptionForTest::DuplicateOrdinal:
    impl->callees.push_back(impl->callees.front());
    break;
  case ProcessStatePlanCorruptionForTest::NonDenseOrdinal:
    cloneCallee()->id = ProcessCalleeId(1);
    break;
  case ProcessStatePlanCorruptionForTest::DanglingReference:
    cloneTransition()->targetPc = ProcessPcId(99);
    break;
  case ProcessStatePlanCorruptionForTest::DuplicateIdentity: {
    auto duplicate = std::make_shared<ProcessGeneratedCalleePlan::Impl>(
        *impl->callees.front().impl_);
    duplicate->id = ProcessCalleeId(1);
    impl->callees.push_back(ProcessGeneratedCalleePlan(duplicate));
    break;
  }
  case ProcessStatePlanCorruptionForTest::UnsortedCanonicalOrder: {
    assert(impl->processes.size() > 1 &&
           "permutation fixture must contain multiple processes");
    std::swap(impl->processes[0], impl->processes[1]);
    break;
  }
  case ProcessStatePlanCorruptionForTest::CostMismatch:
    ++cloneBlock()->cost;
    break;
  case ProcessStatePlanCorruptionForTest::DefinitionKeyMismatch:
    cloneProcess()->definitionKey = "workload";
    break;
  case ProcessStatePlanCorruptionForTest::CalleeSpecializationMismatch:
    cloneCallee()->specializationBytes.push_back(' ');
    break;
  case ProcessStatePlanCorruptionForTest::ValueTypeSpecializationMismatch: {
    ProcessStatePlanSet seeded = cloneWithUnpairedLiveSlotCallee(plans);
    impl = std::make_shared<ProcessStatePlanSet::Impl>(*seeded.impl_);
    auto process = std::make_shared<ProcessStatePlan::Impl>(
        *impl->processes.front().impl_);
    process->liveSlots.clear();
    impl->processes.front() = ProcessStatePlan(process);
    auto type = std::make_shared<ProcessValueTypePlan::Impl>(
        *impl->valueTypes.front().impl_);
    type->fingerprint =
        "sha256:"
        "0000000000000000000000000000000000000000000000000000000000000000";
    impl->valueTypes.front() = ProcessValueTypePlan(type);
    break;
  }
  case ProcessStatePlanCorruptionForTest::EffectMismatch:
    cloneCallee()->effect = ProcessEffectKind::Pure;
    break;
  case ProcessStatePlanCorruptionForTest::IdKindMismatch: {
    auto process = cloneProcess();
    auto pc =
        std::make_shared<ProcessPcPlan::Impl>(*process->pcs.front().impl_);
    pc->blocks.front() = ProcessBlockId(1);
    process->pcs.front() = ProcessPcPlan(pc);
    break;
  }
  case ProcessStatePlanCorruptionForTest::WrongTypeKey:
    cloneWake()->typeKey = "mlir:!acsim.wake";
    break;
  case ProcessStatePlanCorruptionForTest::InvalidFramePhase: {
    auto frameImpl = std::make_shared<ProcessControlFramePlan::Impl>();
    frameImpl->kind = ProcessFrameKind::ScfIf;
    frameImpl->phase = ProcessFramePhase::Entry;
    cloneBlock()->frames.push_back(ProcessControlFramePlan(frameImpl));
    break;
  }
  case ProcessStatePlanCorruptionForTest::InvalidEdgeBinding: {
    auto edgeImpl = std::make_shared<ProcessControlEdgePlan::Impl>();
    edgeImpl->kind = ProcessControlEdgeKind::Branch;
    edgeImpl->trueBlock = ProcessBlockId(0);
    edgeImpl->falseBlock = ProcessBlockId(0);
    cloneBlock()->edge = ProcessControlEdgePlan(edgeImpl);
    break;
  }
  case ProcessStatePlanCorruptionForTest::InvalidWakeCallee:
    cloneWake()->callee = ProcessCalleeId(99);
    break;
  }
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithMissingWakeCallee(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto wake =
      std::make_shared<ProcessWakePlan::Impl>(*process->wakes.front().impl_);
  wake->callee.reset();
  process->wakes.front() = ProcessWakePlan(wake);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithDanglingSuspendTransition(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  auto edge =
      std::make_shared<ProcessControlEdgePlan::Impl>(*block->edge->impl_);
  edge->transition = ProcessTransitionId(99);
  block->edge = ProcessControlEdgePlan(edge);
  process->blocks.front() = ProcessBlockPlan(block);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithUnpairedLiveSlotCallee(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  mlir::MLIRContext *context = plans.processes().front().process().getContext();
  mlir::Type i32 = mlir::IntegerType::get(context, 32);

  auto storageImpl = std::make_shared<ProcessStorageValuePayload::Impl>();
  storageImpl->widthBits = 32;
  storageImpl->encoding = "i32";
  ProcessStorageValuePayload storage(storageImpl);
  auto payloadImpl = std::make_shared<ProcessValueTypePayload::Impl>();
  payloadImpl->kind = ProcessValueTypeKind::Value;
  payloadImpl->value = storage;
  ProcessValueTypePayload payload(payloadImpl);

  llvm::json::Object specialization;
  specialization["acir_type"] = "i32";
  specialization["contract_epoch"] = "0.5";
  specialization["kind"] = "value";
  llvm::json::Object payloadObject;
  payloadObject["encoding"] = "i32";
  payloadObject["members"] = llvm::json::Array();
  payloadObject["width_bits"] = 32;
  specialization["payload"] = std::move(payloadObject);
  specialization["schema"] = "acir-generated-value-type-0.1";
  auto canonical =
      bindings::canonicalizeJson(llvm::json::Value(std::move(specialization)));
  assert(canonical && "literal value-type specialization must canonicalize");
  std::string fingerprint = bindings::sha256Fingerprint(*canonical);
  llvm::StringRef digest = llvm::StringRef(fingerprint).drop_front(7);

  auto typeImpl = std::make_shared<ProcessValueTypePlan::Impl>();
  typeImpl->id = ProcessValueTypeId(0);
  typeImpl->symbol = ("@acir_value_" + digest).str();
  typeImpl->cpp = ("acir::generated::value_" + digest).str();
  typeImpl->kind = ProcessValueTypeKind::Value;
  typeImpl->fingerprint = fingerprint;
  typeImpl->acirType = i32;
  typeImpl->payload = payload;
  typeImpl->specializationBytes = std::move(*canonical);
  impl->valueTypes.push_back(ProcessValueTypePlan(typeImpl));

  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto slotImpl = std::make_shared<ProcessLiveSlotPlan::Impl>();
  slotImpl->id = ProcessLiveSlotId(0);
  slotImpl->name = "live00000000";
  slotImpl->type = i32;
  slotImpl->storageType = ProcessValueTypeId(0);
  slotImpl->wrapCallee = ProcessCalleeId(0);
  process->liveSlots.push_back(ProcessLiveSlotPlan(slotImpl));
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithMissingValueTypePayload(
    const ProcessStatePlanSet &plans) {
  ProcessStatePlanSet result = cloneWithUnpairedLiveSlotCallee(plans);
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*result.impl_);
  auto type = std::make_shared<ProcessValueTypePlan::Impl>(
      *impl->valueTypes.front().impl_);
  type->payload.reset();
  impl->valueTypes.front() = ProcessValueTypePlan(type);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  process->liveSlots.clear();
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithNullEdgeStorage(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  block->edge = ProcessControlEdgePlan(
      std::shared_ptr<const ProcessControlEdgePlan::Impl>());
  process->blocks.front() = ProcessBlockPlan(block);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithInactiveEdgeField(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  auto edge =
      std::make_shared<ProcessControlEdgePlan::Impl>(*block->edge->impl_);
  edge->targetBlock = ProcessBlockId(0);
  block->edge = ProcessControlEdgePlan(edge);
  process->blocks.front() = ProcessBlockPlan(block);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithDoubleValueTypePayload(
    const ProcessStatePlanSet &plans) {
  ProcessStatePlanSet seeded = cloneWithUnpairedLiveSlotCallee(plans);
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*seeded.impl_);
  auto type = std::make_shared<ProcessValueTypePlan::Impl>(
      *impl->valueTypes.front().impl_);
  auto payload =
      std::make_shared<ProcessValueTypePayload::Impl>(*type->payload->impl_);
  auto packet = std::make_shared<ProcessStoragePacketPayload::Impl>();
  packet->widthBits = 32;
  packet->bytes = 4;
  packet->encoding = "array<4xi8>";
  payload->packet = ProcessStoragePacketPayload(packet);
  type->payload = ProcessValueTypePayload(payload);
  impl->valueTypes.front() = ProcessValueTypePlan(type);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  process->liveSlots.clear();
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithMissingOriginalActionSource(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  auto action = std::make_shared<ProcessActionPlan::Impl>();
  action->id = 0;
  action->kind = ProcessActionKind::Original;
  action->emission = ProcessEmissionClass::ForwardOnly;
  action->occurrence = process->wakes.front().impl_->occurrence;
  action->sourceOperation = process->wakes.front().impl_->operation;
  action->cost = 0;
  action->sourceOperation = nullptr;
  block->actions.push_back(ProcessActionPlan(action));
  process->blocks.front() = ProcessBlockPlan(block);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithUnexpectedConstantActionSource(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  auto synthetic = std::make_shared<ProcessSyntheticConstantOccurrence::Impl>();
  synthetic->anchor = *process->wakes.front().impl_->occurrence;
  synthetic->constant = 7;
  auto occurrence = std::make_shared<ProcessOccurrenceId::Impl>();
  occurrence->kind = ProcessOccurrenceKind::SyntheticConstant;
  occurrence->syntheticConstant = ProcessSyntheticConstantOccurrence(synthetic);
  auto action = std::make_shared<ProcessActionPlan::Impl>();
  action->id = 0;
  action->kind = ProcessActionKind::Constant;
  action->emission = ProcessEmissionClass::ForwardOnly;
  action->occurrence = ProcessOccurrenceId(occurrence);
  action->cost = 0;
  action->sourceOperation = process->wakes.front().impl_->operation;
  block->actions.push_back(ProcessActionPlan(action));
  process->blocks.front() = ProcessBlockPlan(block);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithNonLoopForActionSource(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/1, LoopActionMutationForTest::NonLoopSource);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithForInitializeWrongOwningLoopSource(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/0,
      LoopActionMutationForTest::WrongOwningLoopSource);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithForConditionWrongOwningLoopSource(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/1,
      LoopActionMutationForTest::WrongOwningLoopSource);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithForIncrementWrongOwningLoopSource(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/2,
      LoopActionMutationForTest::WrongOwningLoopSource);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithForConditionWrongResultType(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/1, LoopActionMutationForTest::WrongResultType);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithForIncrementWrongResultType(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/2, LoopActionMutationForTest::WrongResultType);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithForConditionInactiveCallee(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/1, LoopActionMutationForTest::InactiveCallee);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithForInitializeInactiveScalar(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/0, LoopActionMutationForTest::InactiveScalar);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithForConditionWrongEmission(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/1, LoopActionMutationForTest::WrongEmission);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithForConditionWrongScalarOp(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/1, LoopActionMutationForTest::WrongScalarName);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithForIncrementWrongEmission(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/2, LoopActionMutationForTest::WrongEmission);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithForIncrementWrongScalarOp(
    const ProcessStatePlanSet &plans) {
  return cloneLoopActionWithMutationForTest(
      plans, /*actionIndex=*/2, LoopActionMutationForTest::WrongScalarName);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneLoopActionWithMutationForTest(
    const ProcessStatePlanSet &plans, uint32_t actionIndex,
    LoopActionMutationForTest mutation) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  assert(actionIndex < block->actions.size() &&
         "loop-action corruption requires a valid fixture action");
  auto action = std::make_shared<ProcessActionPlan::Impl>(
      *block->actions[actionIndex].impl_);
  auto cloneScalarOperation = [&]() {
    assert(action->scalarOp && action->scalarOp->impl_ &&
           "loop-action scalar corruption requires scalar storage");
    return std::make_shared<ProcessScalarOperationPlan::Impl>(
        *action->scalarOp->impl_);
  };

  switch (mutation) {
  case LoopActionMutationForTest::WrongOwningLoopSource: {
    llvm::SmallVector<mlir::scf::ForOp> loops;
    process->process.walk(
        [&](mlir::scf::ForOp loop) { loops.push_back(loop); });
    assert(loops.size() == 2 &&
           "loop-action source corruption requires two frozen loops");
    action->sourceOperation = loops.back();
    break;
  }
  case LoopActionMutationForTest::NonLoopSource:
    action->sourceOperation = process->wakes.front().impl_->operation;
    break;
  case LoopActionMutationForTest::WrongResultType: {
    assert(action->results.size() == 1 &&
           "loop-action result corruption requires one result");
    auto result = std::make_shared<ProcessPlannedValue::Impl>(
        *action->results.front().impl_);
    result->type =
        action->kind == ProcessActionKind::ForCondition
            ? mlir::Type(mlir::IndexType::get(process->process.getContext()))
            : mlir::Type(
                  mlir::IntegerType::get(process->process.getContext(), 1));
    action->results.front() = ProcessPlannedValue(result);
    break;
  }
  case LoopActionMutationForTest::InactiveCallee:
    action->callee = ProcessCalleeId(0);
    break;
  case LoopActionMutationForTest::InactiveScalar:
    assert(block->actions.size() > 1 && block->actions[1].impl_->scalarOp &&
           "loop-action scalar corruption requires a condition action");
    action->scalarOp = block->actions[1].impl_->scalarOp;
    break;
  case LoopActionMutationForTest::WrongEmission:
    action->emission = ProcessEmissionClass::Inline;
    break;
  case LoopActionMutationForTest::WrongScalarName: {
    auto scalar = cloneScalarOperation();
    scalar->name = action->kind == ProcessActionKind::ForCondition
                       ? "arith.addi"
                       : "arith.cmpi";
    action->scalarOp = ProcessScalarOperationPlan(scalar);
    break;
  }
  case LoopActionMutationForTest::MissingScalar:
    action->scalarOp.reset();
    break;
  case LoopActionMutationForTest::WrongScalarProperties: {
    auto scalar = cloneScalarOperation();
    scalar->properties = "{unexpected = true}";
    action->scalarOp = ProcessScalarOperationPlan(scalar);
    break;
  }
  case LoopActionMutationForTest::WrongScalarAttributeCount: {
    auto scalar = cloneScalarOperation();
    if (action->kind == ProcessActionKind::ForCondition) {
      scalar->attributes.clear();
    } else {
      auto unexpected = std::make_shared<ProcessScalarAttribute::Impl>();
      unexpected->name = "unexpected";
      unexpected->value = "true";
      scalar->attributes.push_back(ProcessScalarAttribute(unexpected));
    }
    action->scalarOp = ProcessScalarOperationPlan(scalar);
    break;
  }
  case LoopActionMutationForTest::WrongScalarAttributeName:
  case LoopActionMutationForTest::WrongScalarAttributeValue: {
    auto scalar = cloneScalarOperation();
    assert(scalar->attributes.size() == 1 && scalar->attributes.front().impl_ &&
           "loop-action attribute corruption requires one attribute");
    auto attribute = std::make_shared<ProcessScalarAttribute::Impl>(
        *scalar->attributes.front().impl_);
    if (mutation == LoopActionMutationForTest::WrongScalarAttributeName)
      attribute->name = "unexpected";
    else
      attribute->value = "3 : i64";
    scalar->attributes.front() = ProcessScalarAttribute(attribute);
    action->scalarOp = ProcessScalarOperationPlan(scalar);
    break;
  }
  case LoopActionMutationForTest::WrongOperandCount:
    assert(!action->operands.empty() &&
           "loop-action operand corruption requires an operand");
    action->operands.pop_back();
    break;
  case LoopActionMutationForTest::WrongOperandType: {
    assert(!action->operands.empty() && action->operands.front().impl_ &&
           "loop-action operand corruption requires an operand");
    auto operand = std::make_shared<ProcessPlannedValue::Impl>(
        *action->operands.front().impl_);
    operand->type = mlir::IntegerType::get(process->process.getContext(), 1);
    action->operands.front() = ProcessPlannedValue(operand);
    break;
  }
  case LoopActionMutationForTest::WrongResultCount:
    assert(!action->results.empty() &&
           "loop-action result corruption requires a result");
    action->results.pop_back();
    break;
  }

  block->actions[actionIndex] = ProcessActionPlan(action);
  process->blocks.front() = ProcessBlockPlan(block);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithLongLocalChain(
    const ProcessStatePlanSet &plans, uint32_t blockCount) {
  assert(blockCount > 0 && "long-chain fixture requires at least one block");
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  process->blocks.clear();
  process->wakes.clear();
  process->transitions.clear();
  auto pc = std::make_shared<ProcessPcPlan::Impl>(*process->pcs.front().impl_);
  pc->blocks.clear();
  for (uint32_t index = 0; index < blockCount; ++index) {
    auto edge = std::make_shared<ProcessControlEdgePlan::Impl>();
    if (index + 1 == blockCount) {
      edge->kind = ProcessControlEdgeKind::Terminate;
      edge->status = ProcessTerminateStatus::Success;
    } else {
      edge->kind = ProcessControlEdgeKind::LocalContinue;
      edge->targetBlock = ProcessBlockId(index + 1);
    }
    auto block = std::make_shared<ProcessBlockPlan::Impl>();
    block->id = ProcessBlockId(index);
    block->pc = ProcessPcId(0);
    block->path = process->definitionKey + "/plan/pc/entry/" +
                  llvm::formatv("b{0:D8}", index).str();
    block->edge = ProcessControlEdgePlan(edge);
    block->cost = 1;
    process->blocks.push_back(ProcessBlockPlan(block));
    pc->blocks.push_back(ProcessBlockId(index));
  }
  pc->entryPath = process->blocks.front().impl_->path;
  process->pcs.front() = ProcessPcPlan(pc);
  process->fairnessWork = blockCount;
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet
detail::PlanSetBuilder::cloneWithLocalCycle(const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.back().impl_);
  auto edge = std::make_shared<ProcessControlEdgePlan::Impl>();
  edge->kind = ProcessControlEdgeKind::LocalContinue;
  edge->targetBlock = ProcessBlockId(0);
  block->edge = ProcessControlEdgePlan(edge);
  process->blocks.back() = ProcessBlockPlan(block);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

ProcessStatePlanSet detail::PlanSetBuilder::cloneWithUnreachableBlock(
    const ProcessStatePlanSet &plans) {
  auto impl = std::make_shared<ProcessStatePlanSet::Impl>(*plans.impl_);
  auto process =
      std::make_shared<ProcessStatePlan::Impl>(*impl->processes.front().impl_);
  auto block =
      std::make_shared<ProcessBlockPlan::Impl>(*process->blocks.front().impl_);
  block->id = ProcessBlockId(1);
  block->path = process->definitionKey + "/plan/pc/entry/b00000001";
  auto edge = std::make_shared<ProcessControlEdgePlan::Impl>();
  edge->kind = ProcessControlEdgeKind::Terminate;
  edge->status = ProcessTerminateStatus::Success;
  block->edge = ProcessControlEdgePlan(edge);
  block->cost = 1;
  process->blocks.push_back(ProcessBlockPlan(block));
  auto pc = std::make_shared<ProcessPcPlan::Impl>(*process->pcs.front().impl_);
  pc->blocks.push_back(ProcessBlockId(1));
  process->pcs.front() = ProcessPcPlan(pc);
  impl->processes.front() = ProcessStatePlan(process);
  return ProcessStatePlanSet(impl);
}

bool detail::PlanSetBuilder::exerciseCompleteApiFixture(
    mlir::MLIRContext &context) {
  bool valid = true;
  auto expect = [&](bool condition) { valid &= condition; };
  mlir::Type i32 = mlir::IntegerType::get(&context, 32);
  mlir::Block values;
  mlir::Value operand =
      values.addArgument(i32, mlir::UnknownLoc::get(&context));
  mlir::Value argument =
      values.addArgument(i32, mlir::UnknownLoc::get(&context));

  auto siteImpl = std::make_shared<ProcessCallSitePlan::Impl>();
  siteImpl->operationPath = "@Top::@workload/r0/b0/o0";
  siteImpl->iterationVector = {3};
  ProcessCallSitePlan site(siteImpl);
  expect(site.operation() == nullptr);
  expect(site.operationPath() == "@Top::@workload/r0/b0/o0");
  expect(site.iterationVector() == llvm::ArrayRef<uint64_t>({3}));

  auto originalImpl = std::make_shared<ProcessOriginalOccurrence::Impl>();
  originalImpl->operationPath = "@Top::@workload/r0/b0/o1";
  originalImpl->callSites = {site};
  originalImpl->iterationVector = {5};
  ProcessOriginalOccurrence original(originalImpl);
  auto occurrenceImpl = std::make_shared<ProcessOccurrenceId::Impl>();
  occurrenceImpl->kind = ProcessOccurrenceKind::Original;
  occurrenceImpl->original = original;
  ProcessOccurrenceId anchor(occurrenceImpl);
  expect(original.operation() == nullptr);
  expect(original.operationPath() == "@Top::@workload/r0/b0/o1");
  expect(original.callSites().size() == 1);
  expect(original.iterationVector() == llvm::ArrayRef<uint64_t>({5}));
  expect(anchor.kind() == ProcessOccurrenceKind::Original);
  expect(anchor.original().operationPath() == original.operationPath());

  auto loopImpl = std::make_shared<ProcessSyntheticLoopOccurrence::Impl>();
  loopImpl->anchor = anchor;
  loopImpl->phase = ProcessLoopPhase::Condition;
  ProcessSyntheticLoopOccurrence loop(loopImpl);
  auto loopOccurrenceImpl = std::make_shared<ProcessOccurrenceId::Impl>();
  loopOccurrenceImpl->kind = ProcessOccurrenceKind::SyntheticLoop;
  loopOccurrenceImpl->syntheticLoop = loop;
  ProcessOccurrenceId loopOccurrence(loopOccurrenceImpl);
  expect(loop.anchor().kind() == ProcessOccurrenceKind::Original);
  expect(loop.phase() == ProcessLoopPhase::Condition);
  expect(loopOccurrence.syntheticLoop().phase() == ProcessLoopPhase::Condition);
  expect(loopOccurrence.kind() == ProcessOccurrenceKind::SyntheticLoop);

  auto wrapperImpl =
      std::make_shared<ProcessSyntheticWrapperOccurrence::Impl>();
  wrapperImpl->anchor = anchor;
  wrapperImpl->transition = ProcessTransitionId(2);
  wrapperImpl->slot = ProcessLiveSlotId(3);
  wrapperImpl->direction = ProcessWrapperDirection::Unwrap;
  ProcessSyntheticWrapperOccurrence wrapper(wrapperImpl);
  auto wrapperOccurrenceImpl = std::make_shared<ProcessOccurrenceId::Impl>();
  wrapperOccurrenceImpl->kind = ProcessOccurrenceKind::SyntheticWrapper;
  wrapperOccurrenceImpl->syntheticWrapper = wrapper;
  ProcessOccurrenceId wrapperOccurrence(wrapperOccurrenceImpl);
  expect(wrapper.anchor().kind() == ProcessOccurrenceKind::Original);
  expect(wrapper.transition().value() == 2);
  expect(wrapper.slot().value() == 3);
  expect(wrapper.direction() == ProcessWrapperDirection::Unwrap);
  expect(wrapperOccurrence.syntheticWrapper().slot().value() == 3);
  expect(wrapperOccurrence.kind() == ProcessOccurrenceKind::SyntheticWrapper);

  auto constantOccurrenceImpl =
      std::make_shared<ProcessSyntheticConstantOccurrence::Impl>();
  constantOccurrenceImpl->anchor = anchor;
  constantOccurrenceImpl->constant = 7;
  ProcessSyntheticConstantOccurrence constantOccurrencePayload(
      constantOccurrenceImpl);
  auto constantOccurrenceUnionImpl =
      std::make_shared<ProcessOccurrenceId::Impl>();
  constantOccurrenceUnionImpl->kind = ProcessOccurrenceKind::SyntheticConstant;
  constantOccurrenceUnionImpl->syntheticConstant = constantOccurrencePayload;
  ProcessOccurrenceId constantOccurrence(constantOccurrenceUnionImpl);
  expect(constantOccurrencePayload.anchor().kind() ==
         ProcessOccurrenceKind::Original);
  expect(constantOccurrencePayload.constant() == 7);
  expect(constantOccurrence.syntheticConstant().constant() == 7);
  expect(constantOccurrence.kind() == ProcessOccurrenceKind::SyntheticConstant);

  auto coordinateImpl = std::make_shared<ProcessValueCoordinate::Impl>();
  coordinateImpl->kind = ProcessValueCoordinateKind::BlockArgument;
  coordinateImpl->ownerPath = "@Top::@workload/r0/b0";
  coordinateImpl->index = 4;
  ProcessValueCoordinate coordinate(coordinateImpl);
  expect(coordinate.kind() == ProcessValueCoordinateKind::BlockArgument);
  expect(coordinate.ownerPath() == "@Top::@workload/r0/b0");
  expect(coordinate.index() == 4);

  auto originalValueImpl =
      std::make_shared<ProcessOriginalPlannedValue::Impl>();
  originalValueImpl->value = operand;
  originalValueImpl->occurrence = anchor;
  originalValueImpl->coordinate = coordinate;
  originalValueImpl->path = "@Top::@workload/r0/b0/a4";
  ProcessOriginalPlannedValue originalValue(originalValueImpl);
  auto captureValueImpl = std::make_shared<ProcessCapturePlannedValue::Impl>();
  captureValueImpl->capture = ProcessCaptureId(5);
  ProcessCapturePlannedValue captureValue(captureValueImpl);
  auto slotValueImpl = std::make_shared<ProcessLiveSlotPlannedValue::Impl>();
  slotValueImpl->slot = ProcessLiveSlotId(6);
  ProcessLiveSlotPlannedValue slotValue(slotValueImpl);
  auto syntheticValueImpl =
      std::make_shared<ProcessSyntheticPlannedValue::Impl>();
  syntheticValueImpl->occurrence = loopOccurrence;
  syntheticValueImpl->coordinate = coordinate;
  ProcessSyntheticPlannedValue syntheticValue(syntheticValueImpl);
  auto constantValueImpl =
      std::make_shared<ProcessConstantPlannedValue::Impl>();
  constantValueImpl->value = "42";
  ProcessConstantPlannedValue constantValue(constantValueImpl);

  auto makePlanned = [&](ProcessPlannedValueKind kind) {
    auto impl = std::make_shared<ProcessPlannedValue::Impl>();
    impl->kind = kind;
    impl->type = i32;
    switch (kind) {
    case ProcessPlannedValueKind::Original:
      impl->original = originalValue;
      break;
    case ProcessPlannedValueKind::Capture:
      impl->capture = captureValue;
      break;
    case ProcessPlannedValueKind::LiveSlot:
      impl->liveSlot = slotValue;
      break;
    case ProcessPlannedValueKind::Synthetic:
      impl->synthetic = syntheticValue;
      break;
    case ProcessPlannedValueKind::Constant:
      impl->constant = constantValue;
      break;
    }
    return ProcessPlannedValue(impl);
  };
  std::vector<ProcessPlannedValue> planned = {
      makePlanned(ProcessPlannedValueKind::Original),
      makePlanned(ProcessPlannedValueKind::Capture),
      makePlanned(ProcessPlannedValueKind::LiveSlot),
      makePlanned(ProcessPlannedValueKind::Synthetic),
      makePlanned(ProcessPlannedValueKind::Constant)};
  expect(originalValue.value() == operand);
  expect(originalValue.occurrence().kind() == ProcessOccurrenceKind::Original);
  expect(originalValue.coordinate().index() == 4);
  expect(originalValue.path() == "@Top::@workload/r0/b0/a4");
  expect(captureValue.capture().value() == 5);
  expect(slotValue.slot().value() == 6);
  expect(syntheticValue.occurrence().kind() ==
         ProcessOccurrenceKind::SyntheticLoop);
  expect(syntheticValue.coordinate().index() == 4);
  expect(constantValue.value() == "42");
  expect(planned[0].original().path() == originalValue.path());
  expect(planned[1].capture().capture().value() == 5);
  expect(planned[2].liveSlot().slot().value() == 6);
  expect(planned[3].synthetic().coordinate().index() == 4);
  expect(planned[4].constant().value() == "42");
  for (auto [index, value] : llvm::enumerate(planned)) {
    expect(static_cast<unsigned>(value.kind()) == index);
    expect(value.type() == i32);
  }

  auto scalarAttributeImpl = std::make_shared<ProcessScalarAttribute::Impl>();
  scalarAttributeImpl->name = "predicate";
  scalarAttributeImpl->value = "slt";
  ProcessScalarAttribute scalarAttribute(scalarAttributeImpl);
  auto scalarOpImpl = std::make_shared<ProcessScalarOperationPlan::Impl>();
  scalarOpImpl->name = "arith.cmpi";
  scalarOpImpl->attributes = {scalarAttribute};
  scalarOpImpl->properties = "{}";
  ProcessScalarOperationPlan scalarOp(scalarOpImpl);
  expect(scalarAttribute.name() == "predicate");
  expect(scalarAttribute.value() == "slt");
  expect(scalarOp.name() == "arith.cmpi");
  expect(scalarOp.attributes().size() == 1);
  expect(scalarOp.properties() == "{}");

  auto captureImpl = std::make_shared<ProcessCapturePlan::Impl>();
  captureImpl->id = ProcessCaptureId(0);
  captureImpl->name = "capture00000000";
  captureImpl->operand = operand;
  captureImpl->entryArgument = argument;
  captureImpl->type = i32;
  captureImpl->operandPath = "operand";
  captureImpl->argumentPath = "argument";
  ProcessCapturePlan capture(captureImpl);
  expect(capture.id().value() == 0);
  expect(capture.name() == "capture00000000");
  expect(capture.operand() == operand);
  expect(capture.entryArgument() == argument);
  expect(capture.type() == i32);
  expect(capture.operandPath() == "operand");
  expect(capture.argumentPath() == "argument");

  auto actionImpl = std::make_shared<ProcessActionPlan::Impl>();
  actionImpl->id = 9;
  actionImpl->kind = ProcessActionKind::ForCondition;
  actionImpl->emission = ProcessEmissionClass::CopyScalar;
  actionImpl->occurrence = loopOccurrence;
  actionImpl->iterationVector = {8};
  actionImpl->operands = {planned[0]};
  actionImpl->results = {planned[3]};
  actionImpl->cost = 1;
  actionImpl->resultTypes = {i32};
  actionImpl->scalarOp = scalarOp;
  ProcessActionPlan action(actionImpl);
  expect(action.id() == 9);
  expect(action.kind() == ProcessActionKind::ForCondition);
  expect(action.emission() == ProcessEmissionClass::CopyScalar);
  expect(action.occurrence().kind() == ProcessOccurrenceKind::SyntheticLoop);
  expect(action.sourceOperation() == nullptr);
  expect(action.iterationVector() == llvm::ArrayRef<uint64_t>({8}));
  expect(action.operands().size() == 1);
  expect(action.results().size() == 1);
  expect(action.cost() == 1);
  expect(action.resultTypes() == llvm::ArrayRef<mlir::Type>({i32}));
  expect(!action.callee());
  expect(action.scalarOp() && action.scalarOp()->name() == "arith.cmpi");

  auto liveSlotImpl = std::make_shared<ProcessLiveSlotPlan::Impl>();
  liveSlotImpl->id = ProcessLiveSlotId(1);
  liveSlotImpl->name = "live00000001";
  liveSlotImpl->type = i32;
  liveSlotImpl->storageType = ProcessValueTypeId(2);
  liveSlotImpl->memberValues = {planned[0]};
  liveSlotImpl->wrapCallee = ProcessCalleeId(3);
  liveSlotImpl->unwrapCallee = ProcessCalleeId(4);
  ProcessLiveSlotPlan liveSlot(liveSlotImpl);
  expect(liveSlot.id().value() == 1);
  expect(liveSlot.name() == "live00000001");
  expect(liveSlot.type() == i32);
  expect(liveSlot.storageType().value() == 2);
  expect(liveSlot.memberValues().size() == 1);
  expect(liveSlot.wrapCallee()->value() == 3);
  expect(liveSlot.unwrapCallee()->value() == 4);

  std::vector<ProcessSubscriptionSourcePlan> sources;
  for (ProcessSubscriptionSourceKind kind :
       {ProcessSubscriptionSourceKind::Capture,
        ProcessSubscriptionSourceKind::Value,
        ProcessSubscriptionSourceKind::Symbol}) {
    auto impl = std::make_shared<ProcessSubscriptionSourcePlan::Impl>();
    impl->kind = kind;
    impl->path = "source";
    if (kind == ProcessSubscriptionSourceKind::Capture)
      impl->capture = ProcessCaptureId(5);
    else if (kind == ProcessSubscriptionSourceKind::Value) {
      impl->value = operand;
      impl->ownerPath = "owner";
    } else {
      // NOLINTNEXTLINE(performance-no-int-to-ptr) sentinel test identity
      impl->declaration = reinterpret_cast<mlir::Operation *>(uintptr_t{1});
      impl->symbol = "@symbol";
      impl->ownerPath = "owner";
    }
    sources.push_back(ProcessSubscriptionSourcePlan(impl));
  }
  expect(sources[0].kind() == ProcessSubscriptionSourceKind::Capture);
  expect(sources[0].capture()->value() == 5);
  expect(sources[0].path() == "source");
  expect(sources[1].value() == operand);
  expect(sources[1].owner() == nullptr);
  expect(sources[1].ownerPath() == "owner");
  expect(sources[2].declaration() != nullptr);
  expect(sources[2].symbol() == "@symbol");
  for (auto [index, source] : llvm::enumerate(sources))
    expect(static_cast<unsigned>(source.kind()) == index);

  auto wakeImpl = std::make_shared<ProcessWakePlan::Impl>();
  wakeImpl->id = ProcessWakeId(6);
  wakeImpl->kind = ProcessWakeKind::Condition;
  wakeImpl->triggeringValue = operand;
  wakeImpl->callee = ProcessCalleeId(7);
  wakeImpl->typeKey = "@acir_wake_condition";
  wakeImpl->operationPath = "operation";
  wakeImpl->target = "condition";
  wakeImpl->occurrence = anchor;
  wakeImpl->iterationVector = {9};
  wakeImpl->sources = sources;
  ProcessWakePlan wake(wakeImpl);
  expect(wake.id().value() == 6);
  expect(wake.kind() == ProcessWakeKind::Condition);
  expect(wake.operation() == nullptr);
  expect(wake.triggeringValue() == operand);
  expect(wake.declaration() == nullptr);
  expect(wake.callee().value() == 7);
  expect(wake.typeKey() == "@acir_wake_condition");
  expect(wake.operationPath() == "operation");
  expect(wake.target() == "condition");
  expect(wake.occurrence().kind() == ProcessOccurrenceKind::Original);
  expect(wake.iterationVector() == llvm::ArrayRef<uint64_t>({9}));
  expect(wake.sources().size() == 3);

  auto storeImpl = std::make_shared<ProcessTransitionStorePlan::Impl>();
  storeImpl->slot = ProcessLiveSlotId(1);
  storeImpl->source = planned[0];
  storeImpl->sourceValue = operand;
  ProcessTransitionStorePlan store(storeImpl);
  auto loadImpl = std::make_shared<ProcessTransitionLoadPlan::Impl>();
  loadImpl->slot = ProcessLiveSlotId(1);
  loadImpl->replacements = {planned[1]};
  ProcessTransitionLoadPlan load(loadImpl);
  auto transitionImpl = std::make_shared<ProcessTransitionPlan::Impl>();
  transitionImpl->id = ProcessTransitionId(2);
  transitionImpl->sourcePc = ProcessPcId(3);
  transitionImpl->targetPc = ProcessPcId(4);
  transitionImpl->wake = ProcessWakeId(5);
  transitionImpl->iterationVector = {6};
  transitionImpl->stores = {store};
  transitionImpl->loads = {load};
  ProcessTransitionPlan transition(transitionImpl);
  expect(store.slot().value() == 1);
  expect(store.source().kind() == ProcessPlannedValueKind::Original);
  expect(store.sourceValue() == operand);
  expect(load.slot().value() == 1);
  expect(load.replacements().size() == 1);
  expect(transition.id().value() == 2);
  expect(transition.sourcePc().value() == 3);
  expect(transition.targetPc().value() == 4);
  expect(transition.wake().value() == 5);
  expect(transition.iterationVector() == llvm::ArrayRef<uint64_t>({6}));
  expect(transition.stores().size() == 1);
  expect(transition.loads().size() == 1);

  auto bindingImpl = std::make_shared<ProcessForwardingBindingPlan::Impl>();
  bindingImpl->from = planned[0];
  bindingImpl->to = planned[3];
  ProcessForwardingBindingPlan binding(bindingImpl);
  expect(binding.from().kind() == ProcessPlannedValueKind::Original);
  expect(binding.to().kind() == ProcessPlannedValueKind::Synthetic);

  std::vector<ProcessControlFramePlan> frames;
  for (auto [kind, phase] :
       {std::pair{ProcessFrameKind::Entry, ProcessFramePhase::Entry},
        std::pair{ProcessFrameKind::ScfIf, ProcessFramePhase::Then},
        std::pair{ProcessFrameKind::ScfIf, ProcessFramePhase::Else},
        std::pair{ProcessFrameKind::ScfIf, ProcessFramePhase::Merge},
        std::pair{ProcessFrameKind::ScfFor, ProcessFramePhase::Header},
        std::pair{ProcessFrameKind::ScfFor, ProcessFramePhase::Body},
        std::pair{ProcessFrameKind::ScfFor, ProcessFramePhase::Exit},
        std::pair{ProcessFrameKind::ScfWhile, ProcessFramePhase::Before},
        std::pair{ProcessFrameKind::ScfWhile, ProcessFramePhase::After},
        std::pair{ProcessFrameKind::ScfWhile, ProcessFramePhase::Exit}}) {
    auto impl = std::make_shared<ProcessControlFramePlan::Impl>();
    impl->kind = kind;
    impl->phase = phase;
    impl->operationPath = "frame";
    impl->bindings = {binding};
    frames.push_back(ProcessControlFramePlan(impl));
  }
  expect(frames.front().kind() == ProcessFrameKind::Entry);
  expect(frames.front().phase() == ProcessFramePhase::Entry);
  expect(frames.front().operation() == nullptr);
  expect(frames.front().operationPath() == "frame");
  expect(frames.front().bindings().size() == 1);

  std::vector<ProcessControlEdgePlan> edges;
  auto branch = std::make_shared<ProcessControlEdgePlan::Impl>();
  branch->kind = ProcessControlEdgeKind::Branch;
  branch->condition = planned[4];
  branch->trueBlock = ProcessBlockId(1);
  branch->falseBlock = ProcessBlockId(2);
  branch->trueBindings = {binding};
  branch->falseBindings = {binding};
  edges.push_back(ProcessControlEdgePlan(branch));
  auto local = std::make_shared<ProcessControlEdgePlan::Impl>();
  local->kind = ProcessControlEdgeKind::LocalContinue;
  local->targetBlock = ProcessBlockId(3);
  local->bindings = {binding};
  edges.push_back(ProcessControlEdgePlan(local));
  auto suspend = std::make_shared<ProcessControlEdgePlan::Impl>();
  suspend->kind = ProcessControlEdgeKind::Suspend;
  suspend->transition = ProcessTransitionId(4);
  edges.push_back(ProcessControlEdgePlan(suspend));
  auto terminate = std::make_shared<ProcessControlEdgePlan::Impl>();
  terminate->kind = ProcessControlEdgeKind::Terminate;
  terminate->status = ProcessTerminateStatus::Failure;
  edges.push_back(ProcessControlEdgePlan(terminate));
  expect(edges[0].kind() == ProcessControlEdgeKind::Branch);
  expect(edges[0].condition().kind() == ProcessPlannedValueKind::Constant);
  expect(edges[0].trueBlock().value() == 1);
  expect(edges[0].falseBlock().value() == 2);
  expect(edges[0].trueBindings().size() == 1);
  expect(edges[0].falseBindings().size() == 1);
  expect(edges[1].targetBlock().value() == 3);
  expect(edges[1].bindings().size() == 1);
  expect(edges[2].transition().value() == 4);
  expect(edges[3].status() == ProcessTerminateStatus::Failure);
  for (auto [index, edge] : llvm::enumerate(edges))
    expect(static_cast<unsigned>(edge.kind()) == index);

  auto blockImpl = std::make_shared<ProcessBlockPlan::Impl>();
  blockImpl->id = ProcessBlockId(1);
  blockImpl->pc = ProcessPcId(2);
  blockImpl->path = "block";
  blockImpl->frames = {frames.front()};
  blockImpl->loads = {load};
  blockImpl->actions = {action};
  blockImpl->edge = edges.back();
  blockImpl->cost = 3;
  ProcessBlockPlan block(blockImpl);
  expect(block.id().value() == 1);
  expect(block.pc().value() == 2);
  expect(block.originRegion() == nullptr);
  expect(block.originBlock() == nullptr);
  expect(block.path() == "block");
  expect(block.frames().size() == 1);
  expect(block.loads().size() == 1);
  expect(block.actions().size() == 1);
  expect(block.edge().kind() == ProcessControlEdgeKind::Terminate);
  expect(block.cost() == 3);

  auto pcImpl = std::make_shared<ProcessPcPlan::Impl>();
  pcImpl->id = ProcessPcId(2);
  pcImpl->name = "pc00000002";
  pcImpl->entryPath = "block";
  pcImpl->blocks = {ProcessBlockId(1)};
  ProcessPcPlan pc(pcImpl);
  expect(pc.id().value() == 2);
  expect(pc.name() == "pc00000002");
  expect(pc.entryPath() == "block");
  expect(pc.blocks().front().value() == 1);

  auto stateImpl = std::make_shared<ProcessStatePlan::Impl>();
  stateImpl->definitionKey = "@Top::@workload";
  stateImpl->captures = {capture};
  stateImpl->entryPc = ProcessPcId(2);
  stateImpl->pcs = {pc};
  stateImpl->blocks = {block};
  stateImpl->liveSlots = {liveSlot};
  stateImpl->wakes = {wake};
  stateImpl->transitions = {transition};
  stateImpl->pcBitWidth = 3;
  stateImpl->fairnessWork = 4;
  ProcessStatePlan state(stateImpl);
  expect(state.definitionKey() == "@Top::@workload");
  expect(!state.process());
  expect(state.captures().size() == 1);
  expect(state.entryPc().value() == 2);
  expect(state.pcs().size() == 1);
  expect(state.blocks().size() == 1);
  expect(state.liveSlots().size() == 1);
  expect(state.wakes().size() == 1);
  expect(state.transitions().size() == 1);
  expect(state.pcBitWidth() == 3);
  expect(state.fairnessWork() == 4);

  auto traceDecodeImpl = std::make_shared<ProcessTraceDecodePayload::Impl>();
  traceDecodeImpl->entry = "mlir:i32";
  traceDecodeImpl->result = "mlir:i64";
  traceDecodeImpl->source = "trace";
  ProcessTraceDecodePayload traceDecode(traceDecodeImpl);
  auto queueSendImpl = std::make_shared<ProcessQueueTrySendPayload::Impl>();
  queueSendImpl->element = "mlir:i32";
  queueSendImpl->queue = "@queue";
  ProcessQueueTrySendPayload queueSend(queueSendImpl);
  auto queueRecvImpl = std::make_shared<ProcessQueueTryRecvPayload::Impl>();
  queueRecvImpl->element = "mlir:i32";
  queueRecvImpl->queue = "@queue";
  ProcessQueueTryRecvPayload queueRecv(queueRecvImpl);
  auto eventImpl = std::make_shared<ProcessEventSchedulePayload::Impl>();
  eventImpl->delay = "mlir:i64";
  eventImpl->target = "@event";
  eventImpl->value = "mlir:i32";
  ProcessEventSchedulePayload event(eventImpl);
  expect(traceDecode.entry() == "mlir:i32");
  expect(traceDecode.result() == "mlir:i64");
  expect(traceDecode.source() == "trace");
  expect(queueSend.element() == "mlir:i32");
  expect(queueSend.queue() == "@queue");
  expect(queueRecv.element() == "mlir:i32");
  expect(queueRecv.queue() == "@queue");
  expect(event.delay() == "mlir:i64");
  expect(event.target() == "@event");
  expect(event.value() == "mlir:i32");

  auto traceOpenImpl = std::make_shared<ProcessTraceOpenPayload::Impl>();
  traceOpenImpl->source = "trace";
  ProcessTraceOpenPayload traceOpen(traceOpenImpl);
  auto traceNextImpl = std::make_shared<ProcessTraceNextPayload::Impl>();
  traceNextImpl->entry = "mlir:i32";
  traceNextImpl->source = "trace";
  ProcessTraceNextPayload traceNext(traceNextImpl);
  auto traceEofImpl = std::make_shared<ProcessTraceEofPayload::Impl>();
  traceEofImpl->source = "trace";
  ProcessTraceEofPayload traceEof(traceEofImpl);
  auto tracePositionImpl =
      std::make_shared<ProcessTracePositionPayload::Impl>();
  tracePositionImpl->source = "trace";
  ProcessTracePositionPayload tracePosition(tracePositionImpl);
  expect(traceOpen.source() == "trace");
  expect(traceNext.entry() == "mlir:i32");
  expect(traceNext.source() == "trace");
  expect(traceEof.source() == "trace");
  expect(tracePosition.source() == "trace");

  auto requireImpl = std::make_shared<ProcessContractRequirePayload::Impl>();
  requireImpl->message = "require";
  ProcessContractRequirePayload requirePayload(requireImpl);
  auto ensureImpl = std::make_shared<ProcessContractEnsurePayload::Impl>();
  ensureImpl->message = "ensure";
  ProcessContractEnsurePayload ensurePayload(ensureImpl);
  auto assertImpl = std::make_shared<ProcessContractAssertPayload::Impl>();
  assertImpl->message = "assert";
  ProcessContractAssertPayload assertPayload(assertImpl);
  auto probeImpl = std::make_shared<ProcessProbePayload::Impl>();
  probeImpl->kind = "counter";
  probeImpl->result = "mlir:i64";
  probeImpl->target = "@probe";
  ProcessProbePayload probe(probeImpl);
  auto statImpl = std::make_shared<ProcessStatAddPayload::Impl>();
  statImpl->stat = "@stat";
  statImpl->valueType = "mlir:i64";
  ProcessStatAddPayload stat(statImpl);
  expect(requirePayload.message() == "require");
  expect(ensurePayload.message() == "ensure");
  expect(assertPayload.message() == "assert");
  expect(probe.kind() == "counter");
  expect(probe.result() == "mlir:i64");
  expect(probe.target() == "@probe");
  expect(stat.stat() == "@stat");
  expect(stat.valueType() == "mlir:i64");

  auto wakeConditionImpl =
      std::make_shared<ProcessWakeConditionPayload::Impl>();
  wakeConditionImpl->wakeKind = ProcessWakeKind::Condition;
  wakeConditionImpl->wakeType = "@acir_wake_condition";
  ProcessWakeConditionPayload wakeCondition(wakeConditionImpl);
  auto wakeResourceImpl = std::make_shared<ProcessWakeResourcePayload::Impl>();
  wakeResourceImpl->wakeKind = ProcessWakeKind::Resource;
  wakeResourceImpl->wakeType = "@acir_wake_resource";
  ProcessWakeResourcePayload wakeResource(wakeResourceImpl);
  auto wakeEventImpl = std::make_shared<ProcessWakeEventQueuePayload::Impl>();
  wakeEventImpl->wakeKind = ProcessWakeKind::EventQueue;
  wakeEventImpl->wakeType = "@acir_wake_event_queue";
  ProcessWakeEventQueuePayload wakeEvent(wakeEventImpl);
  auto wakeNextImpl = std::make_shared<ProcessWakeNextDeltaPayload::Impl>();
  wakeNextImpl->wakeKind = ProcessWakeKind::NextDelta;
  wakeNextImpl->wakeType = "@acir_wake_next_delta";
  ProcessWakeNextDeltaPayload wakeNext(wakeNextImpl);
  expect(wakeCondition.wakeKind() == ProcessWakeKind::Condition);
  expect(wakeCondition.wakeType() == "@acir_wake_condition");
  expect(wakeResource.wakeKind() == ProcessWakeKind::Resource);
  expect(wakeResource.wakeType() == "@acir_wake_resource");
  expect(wakeEvent.wakeKind() == ProcessWakeKind::EventQueue);
  expect(wakeEvent.wakeType() == "@acir_wake_event_queue");
  expect(wakeNext.wakeKind() == ProcessWakeKind::NextDelta);
  expect(wakeNext.wakeType() == "@acir_wake_next_delta");

  auto scalarWrapImpl = std::make_shared<ProcessScalarWrapPayload::Impl>();
  scalarWrapImpl->direction = ProcessWrapperDirection::Wrap;
  scalarWrapImpl->scalar = "mlir:i32";
  scalarWrapImpl->valueType = "storage:value:digest";
  ProcessScalarWrapPayload scalarWrap(scalarWrapImpl);
  auto scalarUnwrapImpl = std::make_shared<ProcessScalarUnwrapPayload::Impl>();
  scalarUnwrapImpl->direction = ProcessWrapperDirection::Unwrap;
  scalarUnwrapImpl->scalar = "mlir:i32";
  scalarUnwrapImpl->valueType = "storage:value:digest";
  ProcessScalarUnwrapPayload scalarUnwrap(scalarUnwrapImpl);
  expect(scalarWrap.direction() == ProcessWrapperDirection::Wrap);
  expect(scalarWrap.scalar() == "mlir:i32");
  expect(scalarWrap.valueType() == "storage:value:digest");
  expect(scalarUnwrap.direction() == ProcessWrapperDirection::Unwrap);
  expect(scalarUnwrap.scalar() == "mlir:i32");
  expect(scalarUnwrap.valueType() == "storage:value:digest");

  std::vector<ProcessGeneratedCalleePayload> payloads;
  auto addPayload = [&](ProcessHelperRole role, auto value,
                        auto ProcessGeneratedCalleePayload::Impl::*member) {
    auto impl = std::make_shared<ProcessGeneratedCalleePayload::Impl>();
    impl->role = role;
    impl.get()->*member = value;
    payloads.push_back(ProcessGeneratedCalleePayload(impl));
  };
  addPayload(ProcessHelperRole::TraceDecode, traceDecode,
             &ProcessGeneratedCalleePayload::Impl::traceDecode);
  addPayload(ProcessHelperRole::QueueTrySend, queueSend,
             &ProcessGeneratedCalleePayload::Impl::queueTrySend);
  addPayload(ProcessHelperRole::QueueTryRecv, queueRecv,
             &ProcessGeneratedCalleePayload::Impl::queueTryRecv);
  addPayload(ProcessHelperRole::EventSchedule, event,
             &ProcessGeneratedCalleePayload::Impl::eventSchedule);
  addPayload(ProcessHelperRole::TraceOpen, traceOpen,
             &ProcessGeneratedCalleePayload::Impl::traceOpen);
  addPayload(ProcessHelperRole::TraceNext, traceNext,
             &ProcessGeneratedCalleePayload::Impl::traceNext);
  addPayload(ProcessHelperRole::TraceEof, traceEof,
             &ProcessGeneratedCalleePayload::Impl::traceEof);
  addPayload(ProcessHelperRole::TracePosition, tracePosition,
             &ProcessGeneratedCalleePayload::Impl::tracePosition);
  addPayload(ProcessHelperRole::ContractRequire, requirePayload,
             &ProcessGeneratedCalleePayload::Impl::contractRequire);
  addPayload(ProcessHelperRole::ContractEnsure, ensurePayload,
             &ProcessGeneratedCalleePayload::Impl::contractEnsure);
  addPayload(ProcessHelperRole::ContractAssert, assertPayload,
             &ProcessGeneratedCalleePayload::Impl::contractAssert);
  addPayload(ProcessHelperRole::Probe, probe,
             &ProcessGeneratedCalleePayload::Impl::probe);
  addPayload(ProcessHelperRole::StatAdd, stat,
             &ProcessGeneratedCalleePayload::Impl::statAdd);
  addPayload(ProcessHelperRole::WakeCondition, wakeCondition,
             &ProcessGeneratedCalleePayload::Impl::wakeCondition);
  addPayload(ProcessHelperRole::WakeResource, wakeResource,
             &ProcessGeneratedCalleePayload::Impl::wakeResource);
  addPayload(ProcessHelperRole::WakeEventQueue, wakeEvent,
             &ProcessGeneratedCalleePayload::Impl::wakeEventQueue);
  addPayload(ProcessHelperRole::WakeNextDelta, wakeNext,
             &ProcessGeneratedCalleePayload::Impl::wakeNextDelta);
  addPayload(ProcessHelperRole::ScalarWrap, scalarWrap,
             &ProcessGeneratedCalleePayload::Impl::scalarWrap);
  addPayload(ProcessHelperRole::ScalarUnwrap, scalarUnwrap,
             &ProcessGeneratedCalleePayload::Impl::scalarUnwrap);
  expect(payloads.size() == 19);
  for (auto [index, payload] : llvm::enumerate(payloads))
    expect(static_cast<unsigned>(payload.role()) == index);

  auto fieldMemberImpl = std::make_shared<ProcessValueTypeMemberPlan::Impl>();
  fieldMemberImpl->kind = ProcessValueTypeMemberKind::Field;
  fieldMemberImpl->name = "member";
  fieldMemberImpl->offsetBits = 0;
  fieldMemberImpl->widthBits = 32;
  fieldMemberImpl->signedness = ProcessStorageSignedness::Signed;
  fieldMemberImpl->encoding = "i32";
  fieldMemberImpl->typeKey = "mlir:i32";
  ProcessValueTypeMemberPlan fieldMember(fieldMemberImpl);
  auto elementMemberImpl = std::make_shared<ProcessValueTypeMemberPlan::Impl>();
  elementMemberImpl->kind = ProcessValueTypeMemberKind::Element;
  elementMemberImpl->index = 1;
  elementMemberImpl->offsetBits = 32;
  elementMemberImpl->widthBits = 32;
  elementMemberImpl->signedness = ProcessStorageSignedness::Unsigned;
  elementMemberImpl->encoding = "i32";
  elementMemberImpl->typeKey = "mlir:i32";
  ProcessValueTypeMemberPlan elementMember(elementMemberImpl);
  expect(fieldMember.kind() == ProcessValueTypeMemberKind::Field);
  expect(fieldMember.name() == "member");
  expect(!fieldMember.index());
  expect(fieldMember.offsetBits() == 0);
  expect(fieldMember.widthBits() == 32);
  expect(fieldMember.signedness() == ProcessStorageSignedness::Signed);
  expect(fieldMember.encoding() == "i32");
  expect(fieldMember.typeKey() == "mlir:i32");
  expect(elementMember.kind() == ProcessValueTypeMemberKind::Element);
  expect(elementMember.name().empty());
  expect(elementMember.index() == 1);

  auto storageValueImpl = std::make_shared<ProcessStorageValuePayload::Impl>();
  storageValueImpl->members = {fieldMember};
  storageValueImpl->widthBits = 32;
  storageValueImpl->encoding = "i32";
  ProcessStorageValuePayload storageValue(storageValueImpl);
  auto storagePacketImpl =
      std::make_shared<ProcessStoragePacketPayload::Impl>();
  storagePacketImpl->members = {fieldMember, elementMember};
  storagePacketImpl->widthBits = 64;
  storagePacketImpl->bytes = 8;
  storagePacketImpl->encoding = "array<8xi8>";
  ProcessStoragePacketPayload storagePacket(storagePacketImpl);
  expect(storageValue.members().size() == 1);
  expect(storageValue.widthBits() == 32);
  expect(storageValue.encoding() == "i32");
  expect(storagePacket.members().size() == 2);
  expect(storagePacket.widthBits() == 64);
  expect(storagePacket.bytes() == 8);
  expect(storagePacket.encoding() == "array<8xi8>");
  auto valuePayloadImpl = std::make_shared<ProcessValueTypePayload::Impl>();
  valuePayloadImpl->kind = ProcessValueTypeKind::Value;
  valuePayloadImpl->value = storageValue;
  ProcessValueTypePayload valuePayload(valuePayloadImpl);
  auto packetPayloadImpl = std::make_shared<ProcessValueTypePayload::Impl>();
  packetPayloadImpl->kind = ProcessValueTypeKind::Packet;
  packetPayloadImpl->packet = storagePacket;
  ProcessValueTypePayload packetPayload(packetPayloadImpl);
  expect(valuePayload.kind() == ProcessValueTypeKind::Value);
  expect(valuePayload.value().widthBits() == 32);
  expect(packetPayload.kind() == ProcessValueTypeKind::Packet);
  expect(packetPayload.packet().bytes() == 8);

  auto calleeImpl = std::make_shared<ProcessGeneratedCalleePlan::Impl>();
  calleeImpl->id = ProcessCalleeId(5);
  calleeImpl->symbol = "@callee";
  calleeImpl->cpp = "callee";
  calleeImpl->kind = "implementation";
  calleeImpl->fingerprint = "sha256:fingerprint";
  calleeImpl->effect = ProcessEffectKind::Stateful;
  calleeImpl->inputTypeKeyStorage = {"mlir:i32"};
  calleeImpl->inputTypeKeys = {calleeImpl->inputTypeKeyStorage[0]};
  calleeImpl->resultTypeKeyStorage = {"mlir:i64"};
  calleeImpl->resultTypeKeys = {calleeImpl->resultTypeKeyStorage[0]};
  calleeImpl->role = ProcessHelperRole::Probe;
  calleeImpl->payload = payloads[11];
  // NOLINTBEGIN(performance-no-int-to-ptr) sentinel test identities
  calleeImpl->sourceOperations = {
      reinterpret_cast<mlir::Operation *>(uintptr_t{1})};
  calleeImpl->declarations = {
      reinterpret_cast<mlir::Operation *>(uintptr_t{2})};
  // NOLINTEND(performance-no-int-to-ptr)
  calleeImpl->sourcePathStorage = {"source"};
  calleeImpl->sourcePaths = {calleeImpl->sourcePathStorage[0]};
  ProcessGeneratedCalleePlan callee(calleeImpl);
  expect(callee.id().value() == 5);
  expect(callee.symbol() == "@callee");
  expect(callee.cpp() == "callee");
  expect(callee.kind() == "implementation");
  expect(callee.fingerprint() == "sha256:fingerprint");
  expect(callee.effect() == ProcessEffectKind::Stateful);
  expect(callee.inputTypeKeys() ==
         llvm::ArrayRef<llvm::StringRef>({"mlir:i32"}));
  expect(callee.resultTypeKeys() ==
         llvm::ArrayRef<llvm::StringRef>({"mlir:i64"}));
  expect(callee.role() == ProcessHelperRole::Probe);
  expect(callee.payload().probe().kind() == "counter");
  expect(callee.sourceOperations().size() == 1);
  expect(callee.declarations().size() == 1);
  expect(callee.sourcePaths() == llvm::ArrayRef<llvm::StringRef>({"source"}));
  auto typeImpl = std::make_shared<ProcessValueTypePlan::Impl>();
  typeImpl->id = ProcessValueTypeId(6);
  typeImpl->symbol = "@type";
  typeImpl->cpp = "type";
  typeImpl->kind = ProcessValueTypeKind::Value;
  typeImpl->fingerprint = "sha256:type";
  typeImpl->acirType = i32;
  typeImpl->payload = valuePayload;
  ProcessValueTypePlan type(typeImpl);
  expect(type.id().value() == 6);
  expect(type.symbol() == "@type");
  expect(type.cpp() == "type");
  expect(type.kind() == ProcessValueTypeKind::Value);
  expect(type.fingerprint() == "sha256:type");
  expect(type.acirType() == i32);
  expect(type.payload().value().encoding() == "i32");

  auto setImpl = std::make_shared<ProcessStatePlanSet::Impl>();
  setImpl->processes = {state};
  setImpl->callees = {callee};
  setImpl->valueTypes = {type};
  ProcessStatePlanSet set(setImpl);
  expect(set.processes().size() == 1);
  expect(set.callees().size() == 1);
  expect(set.valueTypes().size() == 1);
  expect(set.lookupByDefinitionKey("@Top::@workload") != nullptr);
  expect(set.lookupByDefinitionKey("@Top::@missing") == nullptr);

  auto allEnumsSequential = [](auto values) {
    for (auto [index, value] : llvm::enumerate(values))
      if (static_cast<unsigned>(value) != index)
        return false;
    return true;
  };
  expect(allEnumsSequential(
      std::array{ProcessWakeKind::Condition, ProcessWakeKind::Resource,
                 ProcessWakeKind::EventQueue, ProcessWakeKind::NextDelta}));
  expect(allEnumsSequential(std::array{ProcessSubscriptionSourceKind::Capture,
                                       ProcessSubscriptionSourceKind::Value,
                                       ProcessSubscriptionSourceKind::Symbol}));
  expect(allEnumsSequential(std::array{
      ProcessActionKind::Original, ProcessActionKind::Constant,
      ProcessActionKind::ForInitialize, ProcessActionKind::ForCondition,
      ProcessActionKind::ForIncrement, ProcessActionKind::ScalarWrap,
      ProcessActionKind::ScalarUnwrap}));
  expect(allEnumsSequential(std::array{
      ProcessEmissionClass::CopyScalar, ProcessEmissionClass::Inline,
      ProcessEmissionClass::Invoke, ProcessEmissionClass::Wrap,
      ProcessEmissionClass::Unwrap, ProcessEmissionClass::ForwardOnly}));
  expect(allEnumsSequential(std::array{
      ProcessOccurrenceKind::Original, ProcessOccurrenceKind::SyntheticLoop,
      ProcessOccurrenceKind::SyntheticWrapper,
      ProcessOccurrenceKind::SyntheticConstant}));
  expect(allEnumsSequential(std::array{ProcessLoopPhase::Initialize,
                                       ProcessLoopPhase::Condition,
                                       ProcessLoopPhase::Increment}));
  expect(allEnumsSequential(std::array{ProcessWrapperDirection::Wrap,
                                       ProcessWrapperDirection::Unwrap}));
  expect(allEnumsSequential(
      std::array{ProcessFrameKind::Entry, ProcessFrameKind::ScfIf,
                 ProcessFrameKind::ScfFor, ProcessFrameKind::ScfWhile}));
  expect(allEnumsSequential(
      std::array{ProcessFramePhase::Entry, ProcessFramePhase::Then,
                 ProcessFramePhase::Else, ProcessFramePhase::Merge,
                 ProcessFramePhase::Header, ProcessFramePhase::Body,
                 ProcessFramePhase::Before, ProcessFramePhase::After,
                 ProcessFramePhase::Exit}));
  expect(allEnumsSequential(std::array{
      ProcessPlannedValueKind::Original, ProcessPlannedValueKind::Capture,
      ProcessPlannedValueKind::LiveSlot, ProcessPlannedValueKind::Synthetic,
      ProcessPlannedValueKind::Constant}));
  expect(allEnumsSequential(
      std::array{ProcessValueCoordinateKind::Result,
                 ProcessValueCoordinateKind::BlockArgument}));
  expect(allEnumsSequential(std::array{
      ProcessControlEdgeKind::Branch, ProcessControlEdgeKind::LocalContinue,
      ProcessControlEdgeKind::Suspend, ProcessControlEdgeKind::Terminate}));
  expect(allEnumsSequential(std::array{ProcessTerminateStatus::Success,
                                       ProcessTerminateStatus::Failure}));
  expect(allEnumsSequential(
      std::array{ProcessEffectKind::Pure, ProcessEffectKind::Stateful}));
  expect(allEnumsSequential(
      std::array{ProcessValueTypeKind::Value, ProcessValueTypeKind::Packet}));
  expect(allEnumsSequential(std::array{ProcessValueTypeMemberKind::Field,
                                       ProcessValueTypeMemberKind::Element}));
  expect(allEnumsSequential(std::array{ProcessStorageSignedness::Signless,
                                       ProcessStorageSignedness::Signed,
                                       ProcessStorageSignedness::Unsigned}));
  return valid;
}

bool detail::PlanSetBuilder::exerciseAllActionArmsFixture(
    mlir::MLIRContext &context) {
  bool valid = true;
  auto expect = [&](bool condition) { valid &= condition; };
  mlir::Type i1 = mlir::IntegerType::get(&context, 1);
  mlir::Type i32 = mlir::IntegerType::get(&context, 32);
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
  context.getOrLoadDialect<mlir::scf::SCFDialect>();
  mlir::OpBuilder builder(&context);
  builder.setInsertionPointToEnd(module->getBody());
  mlir::OperationState state(module->getLoc(),
                             mlir::scf::ForOp::getOperationName());
  state.addRegion();
  mlir::Operation *loopOperation = builder.create(state);

  auto makeOriginal = [&]() {
    auto original = std::make_shared<ProcessOriginalOccurrence::Impl>();
    original->operation = module->getOperation();
    original->operationPath = "@fixture/r0/b0/o0";
    auto occurrence = std::make_shared<ProcessOccurrenceId::Impl>();
    occurrence->kind = ProcessOccurrenceKind::Original;
    occurrence->original = ProcessOriginalOccurrence(original);
    return ProcessOccurrenceId(occurrence);
  };
  ProcessOccurrenceId original = makeOriginal();
  auto makeLoop = [&](ProcessLoopPhase phase) {
    auto loop = std::make_shared<ProcessSyntheticLoopOccurrence::Impl>();
    loop->anchor = original;
    loop->phase = phase;
    auto occurrence = std::make_shared<ProcessOccurrenceId::Impl>();
    occurrence->kind = ProcessOccurrenceKind::SyntheticLoop;
    occurrence->syntheticLoop = ProcessSyntheticLoopOccurrence(loop);
    return ProcessOccurrenceId(occurrence);
  };
  auto makeWrapper = [&](ProcessWrapperDirection direction) {
    auto wrapper = std::make_shared<ProcessSyntheticWrapperOccurrence::Impl>();
    wrapper->anchor = original;
    wrapper->transition = ProcessTransitionId(0);
    wrapper->slot = ProcessLiveSlotId(0);
    wrapper->direction = direction;
    auto occurrence = std::make_shared<ProcessOccurrenceId::Impl>();
    occurrence->kind = ProcessOccurrenceKind::SyntheticWrapper;
    occurrence->syntheticWrapper = ProcessSyntheticWrapperOccurrence(wrapper);
    return ProcessOccurrenceId(occurrence);
  };

  auto makeConstant = [&](mlir::Type type, llvm::StringRef literal) {
    auto constant = std::make_shared<ProcessConstantPlannedValue::Impl>();
    constant->value = literal.str();
    auto value = std::make_shared<ProcessPlannedValue::Impl>();
    value->kind = ProcessPlannedValueKind::Constant;
    value->type = type;
    value->constant = ProcessConstantPlannedValue(constant);
    return ProcessPlannedValue(value);
  };
  ProcessPlannedValue lhs = makeConstant(i32, "7");
  ProcessPlannedValue rhs = makeConstant(i32, "9");
  ProcessPlannedValue boolean = makeConstant(i1, "true");
  ProcessPlannedValue lower = makeConstant(mlir::IndexType::get(&context), "0");
  ProcessPlannedValue upper = makeConstant(mlir::IndexType::get(&context), "8");
  ProcessPlannedValue step = makeConstant(mlir::IndexType::get(&context), "1");

  auto makePayload = [](ProcessHelperRole role) {
    auto payload = std::make_shared<ProcessGeneratedCalleePayload::Impl>();
    payload->role = role;
    if (role == ProcessHelperRole::TraceDecode) {
      auto arm = std::make_shared<ProcessTraceDecodePayload::Impl>();
      arm->entry = "mlir:i32";
      arm->result = "mlir:i32";
      arm->source = "fixture";
      payload->traceDecode = ProcessTraceDecodePayload(arm);
    } else if (role == ProcessHelperRole::Probe) {
      auto arm = std::make_shared<ProcessProbePayload::Impl>();
      arm->kind = "fixture";
      arm->result = "mlir:i32";
      arm->target = "@fixture";
      payload->probe = ProcessProbePayload(arm);
    } else if (role == ProcessHelperRole::ScalarWrap) {
      auto arm = std::make_shared<ProcessScalarWrapPayload::Impl>();
      arm->direction = ProcessWrapperDirection::Wrap;
      arm->scalar = "mlir:i32";
      arm->valueType = "storage:value:fixture";
      payload->scalarWrap = ProcessScalarWrapPayload(arm);
    } else {
      auto arm = std::make_shared<ProcessScalarUnwrapPayload::Impl>();
      arm->direction = ProcessWrapperDirection::Unwrap;
      arm->scalar = "mlir:i32";
      arm->valueType = "storage:value:fixture";
      payload->scalarUnwrap = ProcessScalarUnwrapPayload(arm);
    }
    return ProcessGeneratedCalleePayload(payload);
  };
  const ProcessHelperRole calleeRoles[] = {
      ProcessHelperRole::TraceDecode, ProcessHelperRole::Probe,
      ProcessHelperRole::ScalarWrap, ProcessHelperRole::ScalarUnwrap};
  std::vector<ProcessGeneratedCalleePlan> callees;
  for (auto [index, role] : llvm::enumerate(calleeRoles)) {
    auto callee = std::make_shared<ProcessGeneratedCalleePlan::Impl>();
    callee->id = ProcessCalleeId(index);
    callee->role = role;
    callee->payload = makePayload(role);
    callees.push_back(ProcessGeneratedCalleePlan(callee));
  }

  auto cmpiAttribute = std::make_shared<ProcessScalarAttribute::Impl>();
  cmpiAttribute->name = "predicate";
  cmpiAttribute->value = "2 : i64";
  auto cmpi = std::make_shared<ProcessScalarOperationPlan::Impl>();
  cmpi->name = "arith.cmpi";
  cmpi->attributes = {ProcessScalarAttribute(cmpiAttribute)};
  cmpi->properties = "{}";
  auto addi = std::make_shared<ProcessScalarOperationPlan::Impl>();
  addi->name = "arith.addi";
  addi->properties = "{}";

  auto makeAction =
      [&](uint32_t id, ProcessActionKind kind, ProcessEmissionClass emission,
          ProcessOccurrenceId occurrence, mlir::Operation *source,
          std::vector<ProcessPlannedValue> operands,
          std::vector<ProcessPlannedValue> results,
          std::optional<ProcessCalleeId> callee,
          std::shared_ptr<ProcessScalarOperationPlan::Impl> scalar) {
        auto action = std::make_shared<ProcessActionPlan::Impl>();
        action->id = id;
        action->kind = kind;
        action->emission = emission;
        action->occurrence = std::move(occurrence);
        action->sourceOperation = source;
        action->iterationVector = {100 + id};
        action->operands = std::move(operands);
        action->results = std::move(results);
        action->cost = emission == ProcessEmissionClass::ForwardOnly ? 0 : 1;
        for (const ProcessPlannedValue &result : action->results)
          action->resultTypes.push_back(result.type());
        action->callee = callee;
        if (scalar)
          action->scalarOp = ProcessScalarOperationPlan(std::move(scalar));
        return ProcessActionPlan(action);
      };

  std::vector<ProcessActionPlan> actions;
  actions.push_back(makeAction(
      0, ProcessActionKind::ForCondition, ProcessEmissionClass::CopyScalar,
      makeLoop(ProcessLoopPhase::Condition), loopOperation, {lower, upper},
      {boolean}, std::nullopt, cmpi));
  actions.push_back(makeAction(
      1, ProcessActionKind::Original, ProcessEmissionClass::Inline, original,
      module->getOperation(), {lhs}, {rhs}, ProcessCalleeId(0), nullptr));
  actions.push_back(makeAction(
      2, ProcessActionKind::Original, ProcessEmissionClass::Invoke, original,
      module->getOperation(), {}, {lhs}, ProcessCalleeId(1), nullptr));
  actions.push_back(
      makeAction(3, ProcessActionKind::ScalarWrap, ProcessEmissionClass::Wrap,
                 makeWrapper(ProcessWrapperDirection::Wrap), nullptr, {lhs},
                 {rhs}, ProcessCalleeId(2), nullptr));
  actions.push_back(makeAction(
      4, ProcessActionKind::ScalarUnwrap, ProcessEmissionClass::Unwrap,
      makeWrapper(ProcessWrapperDirection::Unwrap), nullptr, {rhs}, {lhs},
      ProcessCalleeId(3), nullptr));
  actions.push_back(makeAction(
      5, ProcessActionKind::ForInitialize, ProcessEmissionClass::ForwardOnly,
      makeLoop(ProcessLoopPhase::Initialize), loopOperation, {lower}, {lower},
      std::nullopt, nullptr));
  actions.push_back(makeAction(
      6, ProcessActionKind::ForIncrement, ProcessEmissionClass::CopyScalar,
      makeLoop(ProcessLoopPhase::Increment), loopOperation, {lower, step},
      {step}, std::nullopt, addi));

  const ProcessEmissionClass emissions[] = {
      ProcessEmissionClass::CopyScalar, ProcessEmissionClass::Inline,
      ProcessEmissionClass::Invoke,     ProcessEmissionClass::Wrap,
      ProcessEmissionClass::Unwrap,     ProcessEmissionClass::ForwardOnly,
      ProcessEmissionClass::CopyScalar};
  const ProcessActionKind kinds[] = {
      ProcessActionKind::ForCondition, ProcessActionKind::Original,
      ProcessActionKind::Original,     ProcessActionKind::ScalarWrap,
      ProcessActionKind::ScalarUnwrap, ProcessActionKind::ForInitialize,
      ProcessActionKind::ForIncrement};
  for (auto [index, action] : llvm::enumerate(actions)) {
    expect(action.id() == index);
    expect(action.kind() == kinds[index]);
    expect(action.emission() == emissions[index]);
    expect(action.iterationVector() == llvm::ArrayRef<uint64_t>({100 + index}));
    expect(action.cost() ==
           (emissions[index] == ProcessEmissionClass::ForwardOnly ? 0U : 1U));
    expect(action.resultTypes().size() == action.results().size());
    expect(llvm::all_of(llvm::zip_equal(action.resultTypes(), action.results()),
                        [](auto pair) {
                          return std::get<0>(pair) == std::get<1>(pair).type();
                        }));
  }
  expect(actions[0].sourceOperation() == loopOperation);
  expect(actions[0].occurrence().syntheticLoop().phase() ==
         ProcessLoopPhase::Condition);
  expect(actions[0].operands().size() == 2);
  expect(actions[0].results()[0].type() == i1);
  expect(!actions[0].callee());
  expect(actions[0].scalarOp() &&
         actions[0].scalarOp()->name() == "arith.cmpi");
  expect(actions[0].scalarOp()->attributes()[0].name() == "predicate");
  expect(actions[0].scalarOp()->attributes()[0].value() == "2 : i64");
  expect(actions[0].scalarOp()->properties() == "{}");
  expect(actions[1].sourceOperation() == module->getOperation());
  expect(actions[1].callee() == ProcessCalleeId(0));
  expect(callees[actions[1].callee()->value()].role() ==
         ProcessHelperRole::TraceDecode);
  expect(!actions[1].scalarOp());
  expect(actions[2].sourceOperation() == module->getOperation());
  expect(actions[2].callee() == ProcessCalleeId(1));
  expect(callees[actions[2].callee()->value()].role() ==
         ProcessHelperRole::Probe);
  expect(!actions[2].scalarOp());
  expect(actions[3].sourceOperation() == nullptr);
  expect(actions[3].occurrence().syntheticWrapper().direction() ==
         ProcessWrapperDirection::Wrap);
  expect(actions[3].callee() == ProcessCalleeId(2));
  expect(callees[actions[3].callee()->value()].role() ==
         ProcessHelperRole::ScalarWrap);
  expect(!actions[3].scalarOp());
  expect(actions[4].sourceOperation() == nullptr);
  expect(actions[4].occurrence().syntheticWrapper().direction() ==
         ProcessWrapperDirection::Unwrap);
  expect(actions[4].callee() == ProcessCalleeId(3));
  expect(callees[actions[4].callee()->value()].role() ==
         ProcessHelperRole::ScalarUnwrap);
  expect(!actions[4].scalarOp());
  expect(actions[5].sourceOperation() == loopOperation);
  expect(actions[5].occurrence().syntheticLoop().phase() ==
         ProcessLoopPhase::Initialize);
  expect(!actions[5].callee());
  expect(!actions[5].scalarOp());
  expect(actions[6].sourceOperation() == loopOperation);
  expect(actions[6].occurrence().syntheticLoop().phase() ==
         ProcessLoopPhase::Increment);
  expect(actions[6].operands().size() == 2);
  expect(mlir::isa<mlir::IndexType>(actions[6].results()[0].type()));
  expect(!actions[6].callee());
  expect(actions[6].scalarOp() &&
         actions[6].scalarOp()->name() == "arith.addi");
  expect(actions[6].scalarOp()->attributes().empty());
  expect(actions[6].scalarOp()->properties() == "{}");
  return valid;
}

llvm::StringRef detail::PlanSetBuilder::specializationBytes(
    const ProcessGeneratedCalleePlan &callee) {
  return callee.impl_->specializationBytes;
}

llvm::StringRef detail::PlanSetBuilder::descriptorBytes(
    const ProcessGeneratedCalleePlan &callee) {
  return callee.impl_->descriptorBytes;
}

llvm::StringRef
detail::PlanSetBuilder::specializationBytes(const ProcessValueTypePlan &type) {
  return type.impl_->specializationBytes;
}

bool detail::PlanSetBuilder::validEdgeShape(
    const ProcessControlEdgePlan &edge) {
  if (!edge.impl_)
    return false;
  switch (edge.impl_->kind) {
  case ProcessControlEdgeKind::Branch:
    return edge.impl_->condition && edge.impl_->trueBlock &&
           edge.impl_->falseBlock && !edge.impl_->targetBlock &&
           !edge.impl_->transition && edge.impl_->bindings.empty();
  case ProcessControlEdgeKind::LocalContinue:
    return edge.impl_->targetBlock && !edge.impl_->condition &&
           !edge.impl_->trueBlock && !edge.impl_->falseBlock &&
           !edge.impl_->transition && edge.impl_->trueBindings.empty() &&
           edge.impl_->falseBindings.empty();
  case ProcessControlEdgeKind::Suspend:
    return edge.impl_->transition && !edge.impl_->condition &&
           !edge.impl_->trueBlock && !edge.impl_->falseBlock &&
           !edge.impl_->targetBlock && edge.impl_->trueBindings.empty() &&
           edge.impl_->falseBindings.empty() && edge.impl_->bindings.empty();
  case ProcessControlEdgeKind::Terminate:
    return !edge.impl_->condition && !edge.impl_->trueBlock &&
           !edge.impl_->falseBlock && !edge.impl_->targetBlock &&
           !edge.impl_->transition && edge.impl_->trueBindings.empty() &&
           edge.impl_->falseBindings.empty() && edge.impl_->bindings.empty();
  }
  return false;
}

llvm::StringRef
detail::PlanSetBuilder::structuralError(const ProcessStatePlanSet &plans) {
  if (!plans.impl_)
    return "process-state plan invariant violated: missing plan-set storage";

  auto validOccurrence = [](const ProcessOccurrenceId &root) {
    llvm::SmallVector<const ProcessOccurrenceId *> worklist{&root};
    llvm::SmallPtrSet<const ProcessOccurrenceId::Impl *, 16> visited;
    while (!worklist.empty()) {
      const ProcessOccurrenceId *occurrence = worklist.pop_back_val();
      if (!occurrence->impl_)
        return false;
      if (!visited.insert(occurrence->impl_.get()).second)
        return false;
      auto &impl = *occurrence->impl_;
      unsigned active =
          static_cast<unsigned>(impl.original.has_value()) +
          static_cast<unsigned>(impl.syntheticLoop.has_value()) +
          static_cast<unsigned>(impl.syntheticWrapper.has_value()) +
          static_cast<unsigned>(impl.syntheticConstant.has_value());
      if (active != 1)
        return false;
      switch (impl.kind) {
      case ProcessOccurrenceKind::Original:
        if (!impl.original || !impl.original->impl_)
          return false;
        for (const ProcessCallSitePlan &site : impl.original->impl_->callSites)
          if (!site.impl_)
            return false;
        break;
      case ProcessOccurrenceKind::SyntheticLoop:
        if (!impl.syntheticLoop || !impl.syntheticLoop->impl_ ||
            !impl.syntheticLoop->impl_->anchor)
          return false;
        worklist.push_back(&*impl.syntheticLoop->impl_->anchor);
        break;
      case ProcessOccurrenceKind::SyntheticWrapper:
        if (!impl.syntheticWrapper || !impl.syntheticWrapper->impl_ ||
            !impl.syntheticWrapper->impl_->anchor ||
            !impl.syntheticWrapper->impl_->transition ||
            !impl.syntheticWrapper->impl_->slot)
          return false;
        worklist.push_back(&*impl.syntheticWrapper->impl_->anchor);
        break;
      case ProcessOccurrenceKind::SyntheticConstant:
        if (!impl.syntheticConstant || !impl.syntheticConstant->impl_ ||
            !impl.syntheticConstant->impl_->anchor)
          return false;
        worklist.push_back(&*impl.syntheticConstant->impl_->anchor);
        break;
      }
    }
    return true;
  };

  auto validPlannedValue = [&](const ProcessPlannedValue &value) {
    if (!value.impl_ || !value.impl_->type)
      return false;
    unsigned active =
        static_cast<unsigned>(value.impl_->original.has_value()) +
        static_cast<unsigned>(value.impl_->capture.has_value()) +
        static_cast<unsigned>(value.impl_->liveSlot.has_value()) +
        static_cast<unsigned>(value.impl_->synthetic.has_value()) +
        static_cast<unsigned>(value.impl_->constant.has_value());
    if (active != 1)
      return false;
    switch (value.impl_->kind) {
    case ProcessPlannedValueKind::Original:
      return value.impl_->original && value.impl_->original->impl_ &&
             value.impl_->original->impl_->occurrence &&
             value.impl_->original->impl_->coordinate &&
             value.impl_->original->impl_->coordinate->impl_ &&
             validOccurrence(*value.impl_->original->impl_->occurrence);
    case ProcessPlannedValueKind::Capture:
      return value.impl_->capture && value.impl_->capture->impl_ &&
             value.impl_->capture->impl_->capture;
    case ProcessPlannedValueKind::LiveSlot:
      return value.impl_->liveSlot && value.impl_->liveSlot->impl_ &&
             value.impl_->liveSlot->impl_->slot;
    case ProcessPlannedValueKind::Synthetic:
      return value.impl_->synthetic && value.impl_->synthetic->impl_ &&
             value.impl_->synthetic->impl_->occurrence &&
             value.impl_->synthetic->impl_->coordinate &&
             value.impl_->synthetic->impl_->coordinate->impl_ &&
             validOccurrence(*value.impl_->synthetic->impl_->occurrence);
    case ProcessPlannedValueKind::Constant:
      return value.impl_->constant && value.impl_->constant->impl_;
    }
    return false;
  };

  auto validPayloadArm = [](const ProcessGeneratedCalleePayload &payload) {
    if (!payload.impl_)
      return false;
    auto &p = *payload.impl_;
    unsigned active = static_cast<unsigned>(p.traceDecode.has_value()) +
                      static_cast<unsigned>(p.queueTrySend.has_value()) +
                      static_cast<unsigned>(p.queueTryRecv.has_value()) +
                      static_cast<unsigned>(p.eventSchedule.has_value()) +
                      static_cast<unsigned>(p.traceOpen.has_value()) +
                      static_cast<unsigned>(p.traceNext.has_value()) +
                      static_cast<unsigned>(p.traceEof.has_value()) +
                      static_cast<unsigned>(p.tracePosition.has_value()) +
                      static_cast<unsigned>(p.contractRequire.has_value()) +
                      static_cast<unsigned>(p.contractEnsure.has_value()) +
                      static_cast<unsigned>(p.contractAssert.has_value()) +
                      static_cast<unsigned>(p.probe.has_value()) +
                      static_cast<unsigned>(p.statAdd.has_value()) +
                      static_cast<unsigned>(p.wakeCondition.has_value()) +
                      static_cast<unsigned>(p.wakeResource.has_value()) +
                      static_cast<unsigned>(p.wakeEventQueue.has_value()) +
                      static_cast<unsigned>(p.wakeNextDelta.has_value()) +
                      static_cast<unsigned>(p.scalarWrap.has_value()) +
                      static_cast<unsigned>(p.scalarUnwrap.has_value());
    if (active != 1)
      return false;
    auto present = [](const auto &arm) { return arm && arm->impl_; };
    switch (p.role) {
    case ProcessHelperRole::TraceDecode:
      return present(p.traceDecode);
    case ProcessHelperRole::QueueTrySend:
      return present(p.queueTrySend);
    case ProcessHelperRole::QueueTryRecv:
      return present(p.queueTryRecv);
    case ProcessHelperRole::EventSchedule:
      return present(p.eventSchedule);
    case ProcessHelperRole::TraceOpen:
      return present(p.traceOpen);
    case ProcessHelperRole::TraceNext:
      return present(p.traceNext);
    case ProcessHelperRole::TraceEof:
      return present(p.traceEof);
    case ProcessHelperRole::TracePosition:
      return present(p.tracePosition);
    case ProcessHelperRole::ContractRequire:
      return present(p.contractRequire);
    case ProcessHelperRole::ContractEnsure:
      return present(p.contractEnsure);
    case ProcessHelperRole::ContractAssert:
      return present(p.contractAssert);
    case ProcessHelperRole::Probe:
      return present(p.probe);
    case ProcessHelperRole::StatAdd:
      return present(p.statAdd);
    case ProcessHelperRole::WakeCondition:
      return present(p.wakeCondition);
    case ProcessHelperRole::WakeResource:
      return present(p.wakeResource);
    case ProcessHelperRole::WakeEventQueue:
      return present(p.wakeEventQueue);
    case ProcessHelperRole::WakeNextDelta:
      return present(p.wakeNextDelta);
    case ProcessHelperRole::ScalarWrap:
      return present(p.scalarWrap);
    case ProcessHelperRole::ScalarUnwrap:
      return present(p.scalarUnwrap);
    }
    return false;
  };

  for (const ProcessGeneratedCalleePlan &callee : plans.impl_->callees)
    if (!callee.impl_ || !callee.impl_->id || !callee.impl_->payload ||
        !validPayloadArm(*callee.impl_->payload))
      return "process-state plan invariant violated: callee specialization "
             "mismatch";
  for (const ProcessValueTypePlan &type : plans.impl_->valueTypes) {
    if (!type.impl_ || !type.impl_->id || !type.impl_->payload)
      return "process-state plan invariant violated: value-type specialization "
             "mismatch";
    if (!type.impl_->payload->impl_)
      return "process-state plan invariant violated: value-type specialization "
             "mismatch";
    auto &payload = *type.impl_->payload->impl_;
    unsigned active = static_cast<unsigned>(payload.value.has_value()) +
                      static_cast<unsigned>(payload.packet.has_value());
    if (active != 1 ||
        (payload.kind == ProcessValueTypeKind::Value &&
         (!payload.value || !payload.value->impl_)) ||
        (payload.kind == ProcessValueTypeKind::Packet &&
         (!payload.packet || !payload.packet->impl_)))
      return "process-state plan invariant violated: value-type specialization "
             "mismatch";
    auto members = payload.kind == ProcessValueTypeKind::Value
                       ? llvm::ArrayRef(payload.value->impl_->members)
                       : llvm::ArrayRef(payload.packet->impl_->members);
    if (llvm::any_of(members, [](const ProcessValueTypeMemberPlan &member) {
          return !member.impl_;
        }))
      return "process-state plan invariant violated: value-type specialization "
             "mismatch";
  }

  for (const ProcessStatePlan &plan : plans.impl_->processes) {
    if (!plan.impl_ || !plan.impl_->process || !plan.impl_->entryPc ||
        plan.impl_->entryPc->value() >= plan.impl_->pcs.size())
      return "process-state plan invariant violated: definition key mismatch";
    for (const ProcessCapturePlan &capture : plan.impl_->captures)
      if (!capture.impl_ || !capture.impl_->id || !capture.impl_->type ||
          !capture.impl_->operand || !capture.impl_->entryArgument)
        return "process-state plan invariant violated: dangling reference";
    for (const ProcessPcPlan &pc : plan.impl_->pcs) {
      if (!pc.impl_ || !pc.impl_->id)
        return "process-state plan invariant violated: non-dense ordinal";
      if (llvm::any_of(pc.impl_->blocks, [&](ProcessBlockId block) {
            return block.value() >= plan.impl_->blocks.size();
          }))
        return "process-state plan invariant violated: ID kind mismatch";
    }
    for (const ProcessBlockPlan &block : plan.impl_->blocks) {
      if (!block.impl_ || !block.impl_->id || !block.impl_->pc ||
          block.impl_->pc->value() >= plan.impl_->pcs.size() ||
          !block.impl_->edge)
        return "process-state plan invariant violated: dangling reference";
      if (!block.impl_->edge->impl_)
        return "process-state plan invariant violated: invalid edge binding";
      if (!validEdgeShape(*block.impl_->edge))
        return "process-state plan invariant violated: invalid edge binding";
      auto &edge = *block.impl_->edge->impl_;
      auto validBinding = [&](const ProcessForwardingBindingPlan &binding) {
        return binding.impl_ && binding.impl_->from && binding.impl_->to &&
               validPlannedValue(*binding.impl_->from) &&
               validPlannedValue(*binding.impl_->to);
      };
      if ((edge.condition && !validPlannedValue(*edge.condition)) ||
          llvm::any_of(
              edge.trueBindings,
              [&](const auto &binding) { return !validBinding(binding); }) ||
          llvm::any_of(
              edge.falseBindings,
              [&](const auto &binding) { return !validBinding(binding); }) ||
          llvm::any_of(edge.bindings, [&](const auto &binding) {
            return !validBinding(binding);
          }))
        return "process-state plan invariant violated: invalid edge binding";
      if (edge.kind == ProcessControlEdgeKind::Suspend &&
          edge.transition->value() >= plan.impl_->transitions.size())
        return "process-state plan invariant violated: dangling reference";
      if ((edge.kind == ProcessControlEdgeKind::Branch &&
           (edge.trueBlock->value() >= plan.impl_->blocks.size() ||
            edge.falseBlock->value() >= plan.impl_->blocks.size())) ||
          (edge.kind == ProcessControlEdgeKind::LocalContinue &&
           edge.targetBlock->value() >= plan.impl_->blocks.size()))
        return "process-state plan invariant violated: dangling reference";
      for (const ProcessActionPlan &action : block.impl_->actions) {
        if (!action.impl_ || !action.impl_->occurrence ||
            !validOccurrence(*action.impl_->occurrence))
          return "process-state plan invariant violated: invalid action arm";
        bool calleeRequired =
            action.impl_->emission == ProcessEmissionClass::Inline ||
            action.impl_->emission == ProcessEmissionClass::Invoke ||
            action.impl_->emission == ProcessEmissionClass::Wrap ||
            action.impl_->emission == ProcessEmissionClass::Unwrap;
        bool scalarRequired =
            action.impl_->emission == ProcessEmissionClass::CopyScalar;
        if (action.impl_->callee.has_value() != calleeRequired ||
            action.impl_->scalarOp.has_value() != scalarRequired ||
            (action.impl_->callee &&
             action.impl_->callee->value() >= plans.impl_->callees.size()) ||
            (action.impl_->scalarOp && !action.impl_->scalarOp->impl_))
          return "process-state plan invariant violated: invalid action arm";
        if (action.impl_->scalarOp) {
          auto &scalar = *action.impl_->scalarOp->impl_;
          if (scalar.name.empty() || scalar.properties.empty() ||
              llvm::any_of(scalar.attributes,
                           [](const ProcessScalarAttribute &attribute) {
                             return !attribute.impl_;
                           }))
            return "process-state plan invariant violated: invalid action arm";
          for (size_t index = 1; index < scalar.attributes.size(); ++index) {
            auto &previous = *scalar.attributes[index - 1].impl_;
            auto &current = *scalar.attributes[index].impl_;
            if (std::tie(previous.name, previous.value) >=
                std::tie(current.name, current.value))
              return "process-state plan invariant violated: invalid action "
                     "arm";
          }
        }
        bool sourceRequired =
            action.impl_->kind == ProcessActionKind::Original ||
            action.impl_->kind == ProcessActionKind::ForInitialize ||
            action.impl_->kind == ProcessActionKind::ForCondition ||
            action.impl_->kind == ProcessActionKind::ForIncrement;
        if (static_cast<bool>(action.impl_->sourceOperation) != sourceRequired)
          return "process-state plan invariant violated: invalid action arm";
        bool isLoopAction =
            action.impl_->kind == ProcessActionKind::ForInitialize ||
            action.impl_->kind == ProcessActionKind::ForCondition ||
            action.impl_->kind == ProcessActionKind::ForIncrement;
        if (isLoopAction) {
          if (!mlir::isa<mlir::scf::ForOp>(action.impl_->sourceOperation))
            return "process-state plan invariant violated: invalid action arm";
          const ProcessOccurrenceId::Impl &occurrence =
              *action.impl_->occurrence->impl_;
          if (occurrence.kind != ProcessOccurrenceKind::SyntheticLoop ||
              !occurrence.syntheticLoop ||
              !occurrence.syntheticLoop->impl_->anchor)
            return "process-state plan invariant violated: invalid action arm";
          const ProcessOccurrenceId &anchor =
              *occurrence.syntheticLoop->impl_->anchor;
          if (anchor.impl_->kind != ProcessOccurrenceKind::Original ||
              !anchor.impl_->original ||
              anchor.impl_->original->impl_->operation !=
                  action.impl_->sourceOperation)
            return "process-state plan invariant violated: invalid action arm";
        }
        for (const ProcessPlannedValue &value : action.impl_->operands)
          if (!validPlannedValue(value))
            return "process-state plan invariant violated: invalid planned "
                   "value arm";
        for (const ProcessPlannedValue &value : action.impl_->results)
          if (!validPlannedValue(value))
            return "process-state plan invariant violated: invalid planned "
                   "value arm";
      }
      for (const ProcessTransitionLoadPlan &load : block.impl_->loads) {
        if (!load.impl_ || !load.impl_->slot ||
            load.impl_->slot->value() >= plan.impl_->liveSlots.size())
          return "process-state plan invariant violated: dangling reference";
        for (const ProcessPlannedValue &value : load.impl_->replacements)
          if (!validPlannedValue(value))
            return "process-state plan invariant violated: invalid planned "
                   "value arm";
      }
      for (const ProcessControlFramePlan &frame : block.impl_->frames) {
        if (!frame.impl_)
          return "process-state plan invariant violated: invalid frame phase";
        for (const ProcessForwardingBindingPlan &binding :
             frame.impl_->bindings)
          if (!binding.impl_ || !binding.impl_->from || !binding.impl_->to ||
              !validPlannedValue(*binding.impl_->from) ||
              !validPlannedValue(*binding.impl_->to))
            return "process-state plan invariant violated: dangling reference";
      }
    }
    for (const ProcessLiveSlotPlan &slot : plan.impl_->liveSlots) {
      if (!slot.impl_ || !slot.impl_->id || !slot.impl_->storageType ||
          slot.impl_->storageType->value() >= plans.impl_->valueTypes.size() ||
          (slot.impl_->wrapCallee &&
           slot.impl_->wrapCallee->value() >= plans.impl_->callees.size()) ||
          (slot.impl_->unwrapCallee &&
           slot.impl_->unwrapCallee->value() >= plans.impl_->callees.size()))
        return "process-state plan invariant violated: dangling reference";
      if (slot.impl_->wrapCallee.has_value() !=
          slot.impl_->unwrapCallee.has_value())
        return "process-state plan invariant violated: invalid live-slot "
               "wrapper pair";
      for (const ProcessPlannedValue &value : slot.impl_->memberValues)
        if (!validPlannedValue(value))
          return "process-state plan invariant violated: invalid planned value "
                 "arm";
    }
    for (const ProcessWakePlan &wake : plan.impl_->wakes) {
      if (!wake.impl_ || !wake.impl_->id || !wake.impl_->callee ||
          wake.impl_->callee->value() >= plans.impl_->callees.size() ||
          !wake.impl_->occurrence || !validOccurrence(*wake.impl_->occurrence))
        return "process-state plan invariant violated: invalid wake callee";
      for (const ProcessSubscriptionSourcePlan &source : wake.impl_->sources) {
        if (!source.impl_)
          return "process-state plan invariant violated: dangling reference";
        switch (source.impl_->kind) {
        case ProcessSubscriptionSourceKind::Capture:
          if (!source.impl_->capture || source.impl_->value ||
              source.impl_->declaration || !source.impl_->symbol.empty() ||
              !source.impl_->ownerPath.empty())
            return "process-state plan invariant violated: dangling reference";
          break;
        case ProcessSubscriptionSourceKind::Value:
          if (!source.impl_->value || source.impl_->capture ||
              source.impl_->declaration || !source.impl_->symbol.empty())
            return "process-state plan invariant violated: dangling reference";
          break;
        case ProcessSubscriptionSourceKind::Symbol:
          if (!source.impl_->declaration || source.impl_->capture ||
              source.impl_->value || source.impl_->symbol.empty())
            return "process-state plan invariant violated: dangling reference";
          break;
        }
      }
    }
    for (const ProcessTransitionPlan &transition : plan.impl_->transitions) {
      if (!transition.impl_ || !transition.impl_->id ||
          !transition.impl_->sourcePc || !transition.impl_->targetPc ||
          !transition.impl_->wake ||
          transition.impl_->sourcePc->value() >= plan.impl_->pcs.size() ||
          transition.impl_->targetPc->value() >= plan.impl_->pcs.size() ||
          transition.impl_->wake->value() >= plan.impl_->wakes.size())
        return "process-state plan invariant violated: dangling reference";
      for (const ProcessTransitionStorePlan &store : transition.impl_->stores)
        if (!store.impl_ || !store.impl_->slot ||
            store.impl_->slot->value() >= plan.impl_->liveSlots.size() ||
            !store.impl_->source || !validPlannedValue(*store.impl_->source))
          return "process-state plan invariant violated: dangling reference";
      for (const ProcessTransitionLoadPlan &load : transition.impl_->loads)
        if (!load.impl_ || !load.impl_->slot ||
            load.impl_->slot->value() >= plan.impl_->liveSlots.size() ||
            llvm::any_of(load.impl_->replacements,
                         [&](const ProcessPlannedValue &value) {
                           return !validPlannedValue(value);
                         }))
          return "process-state plan invariant violated: dangling reference";
    }
  }
  return {};
}

ProcessStatePlanSet cloneProcessStatePlanWithCorruptionForTest(
    const ProcessStatePlanSet &plan,
    ProcessStatePlanCorruptionForTest corruption) {
  return detail::PlanSetBuilder::cloneWithCorruption(plan, corruption);
}

llvm::StringRef detail::generatedCalleeSpecializationBytes(
    const ProcessGeneratedCalleePlan &callee) {
  return PlanSetBuilder::specializationBytes(callee);
}

llvm::StringRef detail::generatedCalleeDescriptorBytes(
    const ProcessGeneratedCalleePlan &callee) {
  return PlanSetBuilder::descriptorBytes(callee);
}

llvm::StringRef detail::lastProcessStatePlanDiagnosticForTest() {
  return lastDiagnostic;
}

mlir::LogicalResult verifyProcessStatePlan(const ProcessStatePlanSet &plans,
                                           const ProcessStateLimits &limits) {
  lastDiagnostic.clear();
  if (llvm::StringRef error = detail::PlanSetBuilder::structuralError(plans);
      !error.empty())
    return reject(plans, error);
  if (plans.processes().size() > limits.maxProcesses)
    return reject(plans, "process-state plan capability maxProcesses exceeded");
  if (plans.callees().size() > limits.maxCalleeDescriptors)
    return reject(
        plans, "process-state plan capability maxCalleeDescriptors exceeded");
  llvm::StringRef previousKey;
  uint64_t pcs = 0, slots = 0, wakes = 0, transitions = 0, actions = 0;
  for (const ProcessStatePlan &plan : plans.processes()) {
    if (!validDefinitionKey(plan.definitionKey()))
      return reject(
          plans,
          "process-state plan invariant violated: definition key mismatch");
    ac::ModuleOp owner = plan.process()->getParentOfType<ac::ModuleOp>();
    auto ownerName = owner ? mlir::SymbolTable::getSymbolName(owner) : nullptr;
    auto processName = mlir::SymbolTable::getSymbolName(plan.process());
    if (!ownerName || !processName ||
        plan.definitionKey() !=
            ("@" + ownerName.str() + "::@" + processName.str()))
      return reject(
          plans,
          "process-state plan invariant violated: definition key mismatch");
    if (!previousKey.empty() && previousKey.compare(plan.definitionKey()) >= 0)
      return reject(
          plans,
          "process-state plan invariant violated: unsorted canonical order");
    previousKey = plan.definitionKey();
    auto occurrenceReferencesClose = [&](const ProcessOccurrenceId &root) {
      llvm::SmallVector<const ProcessOccurrenceId *> worklist{&root};
      while (!worklist.empty()) {
        const ProcessOccurrenceId &occurrence = *worklist.pop_back_val();
        switch (occurrence.kind()) {
        case ProcessOccurrenceKind::Original:
          break;
        case ProcessOccurrenceKind::SyntheticLoop:
          worklist.push_back(&occurrence.syntheticLoop().anchor());
          break;
        case ProcessOccurrenceKind::SyntheticWrapper:
          if (occurrence.syntheticWrapper().transition().value() >=
                  plan.transitions().size() ||
              occurrence.syntheticWrapper().slot().value() >=
                  plan.liveSlots().size())
            return false;
          worklist.push_back(&occurrence.syntheticWrapper().anchor());
          break;
        case ProcessOccurrenceKind::SyntheticConstant:
          worklist.push_back(&occurrence.syntheticConstant().anchor());
          break;
        }
      }
      return true;
    };
    auto plannedReferencesClose = [&](const ProcessPlannedValue &value) {
      switch (value.kind()) {
      case ProcessPlannedValueKind::Original:
        return occurrenceReferencesClose(value.original().occurrence());
      case ProcessPlannedValueKind::Capture:
        return value.capture().capture().value() < plan.captures().size();
      case ProcessPlannedValueKind::LiveSlot:
        return value.liveSlot().slot().value() < plan.liveSlots().size();
      case ProcessPlannedValueKind::Synthetic:
        return occurrenceReferencesClose(value.synthetic().occurrence());
      case ProcessPlannedValueKind::Constant:
        return true;
      }
      return false;
    };
    auto bindingsClose =
        [&](llvm::ArrayRef<ProcessForwardingBindingPlan> bindings) {
          return llvm::all_of(
              bindings, [&](const ProcessForwardingBindingPlan &binding) {
                return plannedReferencesClose(binding.from()) &&
                       plannedReferencesClose(binding.to()) &&
                       binding.from().type() == binding.to().type();
              });
        };
    if (plan.pcs().empty() || plan.entryPc().value() != 0 ||
        plan.pcs().front().id().value() != 0 ||
        plan.pcs().front().name() != "entry")
      return reject(plans,
                    "process-state plan invariant violated: non-dense ordinal");
    uint32_t expectedWidth = 1;
    uint32_t largestPc = static_cast<uint32_t>(plan.pcs().size() - 1);
    while (largestPc >>= 1)
      ++expectedWidth;
    if (plan.pcBitWidth() != expectedWidth)
      return reject(plans, "process-state plan invariant violated: PC width");
    for (auto [index, pc] : llvm::enumerate(plan.pcs()))
      if (pc.id().value() != index ||
          pc.name() !=
              (index == 0 ? "entry" : llvm::formatv("pc{0:D8}", index).str()))
        return reject(
            plans, "process-state plan invariant violated: non-dense ordinal");
    llvm::SmallVector<bool> listedBlocks(plan.blocks().size());
    for (const ProcessPcPlan &pc : plan.pcs()) {
      std::optional<uint32_t> previousBlock;
      for (ProcessBlockId block : pc.blocks()) {
        if (block.value() >= plan.blocks().size() ||
            plan.blocks()[block.value()].pc() != pc.id() ||
            listedBlocks[block.value()] ||
            (previousBlock && *previousBlock >= block.value()))
          return reject(
              plans, "process-state plan invariant violated: ID kind mismatch");
        listedBlocks[block.value()] = true;
        previousBlock = block.value();
      }
    }
    if (llvm::is_contained(listedBlocks, false))
      return reject(plans,
                    "process-state plan invariant violated: ID kind mismatch");
    for (auto [index, capture] : llvm::enumerate(plan.captures()))
      if (capture.id().value() != index || !capture.type() ||
          capture.name() != llvm::formatv("capture{0:D8}", index).str() ||
          capture.operand().getType() != capture.type() ||
          capture.entryArgument().getType() != capture.type())
        return reject(
            plans, "process-state plan invariant violated: non-dense ordinal");
    std::set<std::tuple<uint32_t, uint32_t, ProcessWrapperDirection>>
        wrapperActions;
    for (auto [index, slot] : llvm::enumerate(plan.liveSlots())) {
      if (slot.id().value() != index ||
          slot.storageType().value() >= plans.valueTypes().size() ||
          !slot.type() ||
          slot.name() != llvm::formatv("live{0:D8}", index).str())
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
      if (slot.wrapCallee()) {
        if (slot.wrapCallee()->value() >= plans.callees().size() ||
            slot.unwrapCallee()->value() >= plans.callees().size() ||
            plans.callees()[slot.wrapCallee()->value()].role() !=
                ProcessHelperRole::ScalarWrap ||
            plans.callees()[slot.unwrapCallee()->value()].role() !=
                ProcessHelperRole::ScalarUnwrap)
          return reject(plans, "process-state plan invariant violated: invalid "
                               "live-slot wrapper pair");
      }
      bool builtinScalar =
          mlir::isa<mlir::IntegerType, mlir::IndexType>(slot.type());
      if (builtinScalar != slot.wrapCallee().has_value())
        return reject(plans, "process-state plan invariant violated: invalid "
                             "live-slot wrapper pair");
      if (llvm::any_of(slot.memberValues(), [&](const auto &value) {
            return !plannedReferencesClose(value);
          }))
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
    }
    for (auto [index, block] : llvm::enumerate(plan.blocks())) {
      std::string expectedBlockPath =
          (plan.definitionKey() + "/plan/pc/" +
           plan.pcs()[block.pc().value()].name() + "/" +
           llvm::formatv("b{0:D8}", index).str())
              .str();
      if (block.id().value() != index ||
          block.pc().value() >= plan.pcs().size() ||
          block.path() != expectedBlockPath)
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
      actions += block.actions().size();
      for (auto [actionIndex, action] : llvm::enumerate(block.actions())) {
        if (action.id() != actionIndex)
          return reject(
              plans,
              "process-state plan invariant violated: non-dense ordinal");
        uint32_t expectedActionCost =
            action.emission() == ProcessEmissionClass::ForwardOnly ? 0 : 1;
        if (action.cost() != expectedActionCost)
          return reject(plans,
                        "process-state plan invariant violated: cost mismatch");
        if (action.callee() &&
            action.callee()->value() >= plans.callees().size())
          return reject(
              plans,
              "process-state plan invariant violated: dangling reference");
        if (action.callee()) {
          ProcessHelperRole role =
              plans.callees()[action.callee()->value()].role();
          ProcessEmissionClass expectedEmission =
              role <= ProcessHelperRole::TraceDecode
                  ? ProcessEmissionClass::Inline
              : role == ProcessHelperRole::ScalarWrap
                  ? ProcessEmissionClass::Wrap
              : role == ProcessHelperRole::ScalarUnwrap
                  ? ProcessEmissionClass::Unwrap
                  : ProcessEmissionClass::Invoke;
          if ((role >= ProcessHelperRole::WakeCondition &&
               role <= ProcessHelperRole::WakeNextDelta) ||
              action.emission() != expectedEmission)
            return reject(
                plans,
                "process-state plan invariant violated: invalid action arm");
        }
        if ((action.kind() == ProcessActionKind::ScalarWrap) !=
                (action.emission() == ProcessEmissionClass::Wrap) ||
            (action.kind() == ProcessActionKind::ScalarUnwrap) !=
                (action.emission() == ProcessEmissionClass::Unwrap) ||
            (action.kind() == ProcessActionKind::ForInitialize &&
             action.emission() != ProcessEmissionClass::ForwardOnly))
          return reject(
              plans,
              "process-state plan invariant violated: invalid action arm");
        if (action.kind() == ProcessActionKind::ForCondition) {
          const ProcessScalarOperationPlan *scalar = action.scalarOp();
          if (action.emission() != ProcessEmissionClass::CopyScalar ||
              !scalar || scalar->name() != "arith.cmpi" ||
              scalar->properties() != "{}" ||
              scalar->attributes().size() != 1 ||
              scalar->attributes()[0].name() != "predicate" ||
              scalar->attributes()[0].value() != "2 : i64" ||
              action.operands().size() != 2 ||
              !llvm::all_of(action.operands(),
                            [](const auto &operand) {
                              return mlir::isa<mlir::IndexType>(operand.type());
                            }) ||
              action.results().size() != 1 ||
              !action.results()[0].type().isInteger(1))
            return reject(
                plans,
                "process-state plan invariant violated: invalid action arm");
        }
        if (action.kind() == ProcessActionKind::ForIncrement) {
          const ProcessScalarOperationPlan *scalar = action.scalarOp();
          if (action.emission() != ProcessEmissionClass::CopyScalar ||
              !scalar || scalar->name() != "arith.addi" ||
              scalar->properties() != "{}" || !scalar->attributes().empty())
            return reject(
                plans,
                "process-state plan invariant violated: invalid action arm");
          if (action.operands().size() != 2 ||
              !llvm::all_of(action.operands(),
                            [](const auto &operand) {
                              return mlir::isa<mlir::IndexType>(operand.type());
                            }) ||
              action.results().size() != 1 ||
              !mlir::isa<mlir::IndexType>(action.results()[0].type()))
            return reject(
                plans,
                "process-state plan invariant violated: invalid action arm");
        }
        ProcessOccurrenceKind expectedOccurrence =
            ProcessOccurrenceKind::Original;
        std::optional<ProcessLoopPhase> expectedLoopPhase;
        std::optional<ProcessWrapperDirection> expectedWrapperDirection;
        switch (action.kind()) {
        case ProcessActionKind::Original:
          break;
        case ProcessActionKind::Constant:
          expectedOccurrence = ProcessOccurrenceKind::SyntheticConstant;
          break;
        case ProcessActionKind::ForInitialize:
          expectedOccurrence = ProcessOccurrenceKind::SyntheticLoop;
          expectedLoopPhase = ProcessLoopPhase::Initialize;
          break;
        case ProcessActionKind::ForCondition:
          expectedOccurrence = ProcessOccurrenceKind::SyntheticLoop;
          expectedLoopPhase = ProcessLoopPhase::Condition;
          break;
        case ProcessActionKind::ForIncrement:
          expectedOccurrence = ProcessOccurrenceKind::SyntheticLoop;
          expectedLoopPhase = ProcessLoopPhase::Increment;
          break;
        case ProcessActionKind::ScalarWrap:
          expectedOccurrence = ProcessOccurrenceKind::SyntheticWrapper;
          expectedWrapperDirection = ProcessWrapperDirection::Wrap;
          break;
        case ProcessActionKind::ScalarUnwrap:
          expectedOccurrence = ProcessOccurrenceKind::SyntheticWrapper;
          expectedWrapperDirection = ProcessWrapperDirection::Unwrap;
          break;
        }
        if (action.occurrence().kind() != expectedOccurrence ||
            (expectedLoopPhase && action.occurrence().syntheticLoop().phase() !=
                                      *expectedLoopPhase) ||
            (expectedWrapperDirection &&
             action.occurrence().syntheticWrapper().direction() !=
                 *expectedWrapperDirection))
          return reject(
              plans,
              "process-state plan invariant violated: invalid action arm");
        if (expectedWrapperDirection) {
          const ProcessSyntheticWrapperOccurrence &wrapper =
              action.occurrence().syntheticWrapper();
          const ProcessLiveSlotPlan &slot =
              plan.liveSlots()[wrapper.slot().value()];
          std::optional<ProcessCalleeId> expectedCallee =
              *expectedWrapperDirection == ProcessWrapperDirection::Wrap
                  ? slot.wrapCallee()
                  : slot.unwrapCallee();
          if (!expectedCallee || action.callee() != expectedCallee ||
              action.operands().size() != 1 || action.results().size() != 1)
            return reject(
                plans,
                "process-state plan invariant violated: invalid action arm");
          wrapperActions.emplace(wrapper.transition().value(),
                                 wrapper.slot().value(),
                                 *expectedWrapperDirection);
        }
        if (action.resultTypes().size() != action.results().size())
          return reject(
              plans, "process-state plan invariant violated: wrong type key");
        for (auto [type, result] :
             llvm::zip_equal(action.resultTypes(), action.results()))
          if (type != result.type())
            return reject(
                plans, "process-state plan invariant violated: wrong type key");
        if (llvm::any_of(action.operands(),
                         [&](const auto &value) {
                           return !plannedReferencesClose(value);
                         }) ||
            llvm::any_of(action.results(), [&](const auto &value) {
              return !plannedReferencesClose(value);
            }))
          return reject(
              plans,
              "process-state plan invariant violated: dangling reference");
      }
      for (const ProcessControlFramePlan &frame : block.frames()) {
        bool legal = (frame.kind() == ProcessFrameKind::Entry &&
                      frame.phase() == ProcessFramePhase::Entry) ||
                     (frame.kind() == ProcessFrameKind::ScfIf &&
                      (frame.phase() == ProcessFramePhase::Then ||
                       frame.phase() == ProcessFramePhase::Else ||
                       frame.phase() == ProcessFramePhase::Merge)) ||
                     (frame.kind() == ProcessFrameKind::ScfFor &&
                      (frame.phase() == ProcessFramePhase::Header ||
                       frame.phase() == ProcessFramePhase::Body ||
                       frame.phase() == ProcessFramePhase::Exit)) ||
                     (frame.kind() == ProcessFrameKind::ScfWhile &&
                      (frame.phase() == ProcessFramePhase::Before ||
                       frame.phase() == ProcessFramePhase::After ||
                       frame.phase() == ProcessFramePhase::Exit));
        if (!legal)
          return reject(
              plans,
              "process-state plan invariant violated: invalid frame phase");
        if (!bindingsClose(frame.bindings()))
          return reject(
              plans,
              "process-state plan invariant violated: dangling reference");
      }
      if (!detail::PlanSetBuilder::validEdgeShape(block.edge()))
        return reject(
            plans,
            "process-state plan invariant violated: invalid edge binding");
      const ProcessControlEdgePlan &edge = block.edge();
      if ((edge.kind() == ProcessControlEdgeKind::Branch &&
           (edge.trueBlock().value() >= plan.blocks().size() ||
            edge.falseBlock().value() >= plan.blocks().size() ||
            !plannedReferencesClose(edge.condition()))) ||
          (edge.kind() == ProcessControlEdgeKind::LocalContinue &&
           edge.targetBlock().value() >= plan.blocks().size()))
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
      if (!bindingsClose(edge.trueBindings()) ||
          !bindingsClose(edge.falseBindings()) ||
          !bindingsClose(edge.bindings()))
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
      for (const ProcessTransitionLoadPlan &load : block.loads())
        if (load.slot().value() >= plan.liveSlots().size() ||
            llvm::any_of(load.replacements(), [&](const auto &value) {
              return !plannedReferencesClose(value);
            }))
          return reject(
              plans,
              "process-state plan invariant violated: dangling reference");
      for (size_t i = 1; i < block.loads().size(); ++i)
        if (block.loads()[i - 1].slot().value() >=
            block.loads()[i].slot().value())
          return reject(plans, "process-state plan invariant violated: "
                               "unsorted canonical order");
      uint64_t expectedCost = block.loads().size();
      for (const ProcessActionPlan &action : block.actions())
        expectedCost += action.cost();
      if (block.edge().kind() == ProcessControlEdgeKind::Suspend) {
        const ProcessTransitionPlan &transition =
            plan.transitions()[block.edge().transition().value()];
        expectedCost += transition.stores().size() + 2;
      } else {
        ++expectedCost;
      }
      if (block.cost() != expectedCost)
        return reject(plans,
                      "process-state plan invariant violated: cost mismatch");
    }
    for (auto [index, wake] : llvm::enumerate(plan.wakes())) {
      if (wake.id().value() != index ||
          wake.callee().value() >= plans.callees().size())
        return reject(
            plans,
            "process-state plan invariant violated: invalid wake callee");
      if (wake.typeKey() != wakeTypeKey(wake.kind()))
        return reject(plans,
                      "process-state plan invariant violated: wrong type key");
      if (!occurrenceReferencesClose(wake.occurrence()) ||
          llvm::any_of(wake.sources(), [&](const auto &source) {
            return source.capture() &&
                   source.capture()->value() >= plan.captures().size();
          }))
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
      bool rawHandlesMatch = false;
      switch (wake.kind()) {
      case ProcessWakeKind::Condition:
        rawHandlesMatch = static_cast<bool>(wake.triggeringValue()) &&
                          wake.declaration() == nullptr &&
                          !wake.target().empty();
        break;
      case ProcessWakeKind::Resource:
      case ProcessWakeKind::EventQueue:
        rawHandlesMatch = !wake.triggeringValue() &&
                          wake.declaration() != nullptr &&
                          !wake.target().empty();
        break;
      case ProcessWakeKind::NextDelta:
        rawHandlesMatch = !wake.triggeringValue() &&
                          wake.declaration() == nullptr &&
                          wake.target().empty();
        break;
      }
      if (!rawHandlesMatch)
        return reject(plans,
                      "process-state plan invariant violated: wrong type key");
      const ProcessGeneratedCalleePlan &callee =
          plans.callees()[wake.callee().value()];
      ProcessHelperRole expectedRole = static_cast<ProcessHelperRole>(
          static_cast<unsigned>(ProcessHelperRole::WakeCondition) +
          static_cast<unsigned>(wake.kind()));
      if (callee.role() != expectedRole || !callee.inputTypeKeys().empty() ||
          callee.resultTypeKeys().size() != 1 ||
          callee.resultTypeKeys().front() != wake.typeKey())
        return reject(
            plans,
            "process-state plan invariant violated: invalid wake callee");
    }
    for (auto [index, transition] : llvm::enumerate(plan.transitions())) {
      if (transition.id().value() != index ||
          transition.sourcePc().value() >= plan.pcs().size() ||
          transition.targetPc().value() >= plan.pcs().size() ||
          transition.wake().value() >= plan.wakes().size())
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
      for (const ProcessTransitionStorePlan &store : transition.stores())
        if (store.slot().value() >= plan.liveSlots().size() ||
            !plannedReferencesClose(store.source()))
          return reject(
              plans,
              "process-state plan invariant violated: dangling reference");
        else if (plan.liveSlots()[store.slot().value()].wrapCallee() &&
                 !wrapperActions.contains({transition.id().value(),
                                           store.slot().value(),
                                           ProcessWrapperDirection::Wrap}))
          return reject(plans, "process-state plan invariant violated: missing "
                               "scalar wrapper action");
      for (const ProcessTransitionLoadPlan &load : transition.loads())
        if (load.slot().value() >= plan.liveSlots().size() ||
            llvm::any_of(load.replacements(), [&](const auto &value) {
              return !plannedReferencesClose(value);
            }))
          return reject(
              plans,
              "process-state plan invariant violated: dangling reference");
        else if (plan.liveSlots()[load.slot().value()].unwrapCallee() &&
                 !wrapperActions.contains({transition.id().value(),
                                           load.slot().value(),
                                           ProcessWrapperDirection::Unwrap}))
          return reject(plans, "process-state plan invariant violated: missing "
                               "scalar wrapper action");
      for (size_t i = 1; i < transition.stores().size(); ++i)
        if (transition.stores()[i - 1].slot().value() >=
            transition.stores()[i].slot().value())
          return reject(plans, "process-state plan invariant violated: "
                               "unsorted canonical order");
      for (size_t i = 1; i < transition.loads().size(); ++i)
        if (transition.loads()[i - 1].slot().value() >=
            transition.loads()[i].slot().value())
          return reject(plans, "process-state plan invariant violated: "
                               "unsorted canonical order");
    }
    uint64_t expectedFairness = 0;
    for (const ProcessPcPlan &pc : plan.pcs()) {
      if (pc.blocks().empty() ||
          plan.blocks()[pc.blocks().front().value()].path() != pc.entryPath())
        return reject(
            plans, "process-state plan invariant violated: dangling reference");
      llvm::SmallVector<uint32_t> indegree(plan.blocks().size());
      llvm::SmallVector<std::optional<uint64_t>> distance(plan.blocks().size());
      bool invalidGraph = false;
      auto successors = [](const ProcessControlEdgePlan &edge) {
        llvm::SmallVector<ProcessBlockId, 2> result;
        if (edge.kind() == ProcessControlEdgeKind::Branch) {
          result.push_back(edge.trueBlock());
          result.push_back(edge.falseBlock());
        } else if (edge.kind() == ProcessControlEdgeKind::LocalContinue) {
          result.push_back(edge.targetBlock());
        }
        return result;
      };
      for (ProcessBlockId id : pc.blocks()) {
        for (ProcessBlockId successor :
             successors(plan.blocks()[id.value()].edge())) {
          if (plan.blocks()[successor.value()].pc() != pc.id()) {
            invalidGraph = true;
            continue;
          }
          ++indegree[successor.value()];
        }
      }
      llvm::SmallVector<ProcessBlockId> ready;
      for (ProcessBlockId id : pc.blocks())
        if (indegree[id.value()] == 0)
          ready.push_back(id);
      distance[pc.blocks().front().value()] =
          plan.blocks()[pc.blocks().front().value()].cost();
      size_t processed = 0;
      uint64_t pcWork = 0;
      for (size_t cursor = 0; cursor < ready.size(); ++cursor) {
        ProcessBlockId id = ready[cursor];
        ++processed;
        if (distance[id.value()])
          pcWork = std::max(pcWork, *distance[id.value()]);
        for (ProcessBlockId successor :
             successors(plan.blocks()[id.value()].edge())) {
          if (distance[id.value()]) {
            uint64_t cost = plan.blocks()[successor.value()].cost();
            if (*distance[id.value()] >
                std::numeric_limits<uint64_t>::max() - cost) {
              invalidGraph = true;
            } else {
              uint64_t candidate = *distance[id.value()] + cost;
              if (!distance[successor.value()] ||
                  candidate > *distance[successor.value()])
                distance[successor.value()] = candidate;
            }
          }
          if (--indegree[successor.value()] == 0)
            ready.push_back(successor);
        }
      }
      if (processed != pc.blocks().size())
        invalidGraph = true;
      for (ProcessBlockId id : pc.blocks())
        if (!distance[id.value()])
          invalidGraph = true;
      if (invalidGraph)
        return reject(plans,
                      "process-state plan invariant violated: cost mismatch");
      expectedFairness = std::max(expectedFairness, pcWork);
    }
    if (plan.fairnessWork() != expectedFairness || expectedFairness == 0)
      return reject(plans,
                    "process-state plan invariant violated: cost mismatch");
    if (plan.fairnessWork() > limits.maxFairnessWork)
      return reject(plans,
                    "process-state plan capability maxFairnessWork exceeded");
    pcs += plan.pcs().size();
    slots += plan.liveSlots().size();
    wakes += plan.wakes().size();
    transitions += plan.transitions().size();
  }
  if (pcs > limits.maxProgramCounters)
    return reject(plans,
                  "process-state plan capability maxProgramCounters exceeded");
  if (slots > limits.maxLiveSlots)
    return reject(plans, "process-state plan capability maxLiveSlots exceeded");
  if (wakes > limits.maxWakeRecords)
    return reject(plans,
                  "process-state plan capability maxWakeRecords exceeded");
  if (transitions > limits.maxTransitions)
    return reject(plans,
                  "process-state plan capability maxTransitions exceeded");
  if (actions > limits.maxPlannedOperations)
    return reject(
        plans, "process-state plan capability maxPlannedOperations exceeded");
  llvm::StringRef previousSpecialization;
  for (auto [index, callee] : llvm::enumerate(plans.callees())) {
    if (callee.id().value() < index)
      return reject(plans,
                    "process-state plan invariant violated: duplicate ordinal");
    if (callee.id().value() > index)
      return reject(plans,
                    "process-state plan invariant violated: non-dense ordinal");
    if (!previousSpecialization.empty() &&
        previousSpecialization.compare(
            detail::generatedCalleeSpecializationBytes(callee)) >= 0)
      return reject(
          plans,
          previousSpecialization ==
                  detail::generatedCalleeSpecializationBytes(callee)
              ? "process-state plan invariant violated: duplicate identity"
              : "process-state plan invariant violated: unsorted canonical "
                "order");
    previousSpecialization = detail::generatedCalleeSpecializationBytes(callee);
    ProcessEffectKind expectedEffect =
        callee.role() <= ProcessHelperRole::TraceDecode ||
                callee.role() >= ProcessHelperRole::ScalarWrap
            ? ProcessEffectKind::Pure
            : ProcessEffectKind::Stateful;
    if (callee.effect() != expectedEffect)
      return reject(plans,
                    "process-state plan invariant violated: effect mismatch");
    if (callee.kind() != "implementation")
      return reject(plans,
                    "process-state plan invariant violated: callee kind");
    if (!validCalleeSemantics(callee))
      return reject(plans,
                    "process-state plan invariant violated: callee semantics");
    if (llvm::any_of(callee.inputTypeKeys(),
                     [](llvm::StringRef key) { return !validTypeKey(key); }) ||
        llvm::any_of(callee.resultTypeKeys(),
                     [](llvm::StringRef key) { return !validTypeKey(key); }))
      return reject(plans,
                    "process-state plan invariant violated: callee type key");
    if (!llvm::is_sorted(callee.sourcePaths()) ||
        std::adjacent_find(callee.sourcePaths().begin(),
                           callee.sourcePaths().end()) !=
            callee.sourcePaths().end() ||
        callee.sourceOperations().size() != callee.sourcePaths().size())
      return reject(plans,
                    "process-state plan invariant violated: callee sources");
    bool ownsDeclaration = callee.role() == ProcessHelperRole::QueueTrySend ||
        callee.role() == ProcessHelperRole::QueueTryRecv ||
        callee.role() == ProcessHelperRole::EventSchedule ||
        callee.role() == ProcessHelperRole::Probe ||
        callee.role() == ProcessHelperRole::StatAdd;
    llvm::SmallPtrSet<mlir::Operation *, 8> uniqueDeclarations;
    if ((ownsDeclaration && callee.declarations().empty()) ||
        (!ownsDeclaration && !callee.declarations().empty()) ||
        llvm::any_of(callee.sourceOperations(),
                     [](mlir::Operation *op) { return op == nullptr; }) ||
        llvm::any_of(callee.declarations(), [&](mlir::Operation *op) {
          return op == nullptr || !uniqueDeclarations.insert(op).second;
        }))
      return reject(plans,
                    "process-state plan invariant violated: callee declarations");
    auto canonical = detail::canonicalGeneratedCalleeSpecialization(callee);
    if (!canonical) {
      llvm::consumeError(canonical.takeError());
      return reject(plans,
                    "process-state plan invariant violated: callee canonicalization");
    }
    std::string fingerprint = bindings::sha256Fingerprint(*canonical);
    llvm::StringRef digest = llvm::StringRef(fingerprint).drop_front(7);
    std::string expectedSymbol =
        ("@acir_impl_" + helperRoleSpelling(callee.role()) + "_" + digest)
            .str();
    std::string expectedCpp = ("acir::generated::impl_" +
                               helperRoleSpelling(callee.role()) + "_" + digest)
                                  .str();
    if (*canonical != detail::generatedCalleeSpecializationBytes(callee))
      return reject(plans, "process-state plan invariant violated: callee "
                           "specialization mismatch");
    if (fingerprint != callee.fingerprint())
      return reject(plans, "process-state plan invariant violated: callee "
                           "specialization mismatch");
    if (callee.symbol() != expectedSymbol || callee.cpp() != expectedCpp)
      return reject(plans,
                    "process-state plan invariant violated: callee symbol");
  }
  previousSpecialization = {};
  llvm::StringSet<> generatedTypeKeys;
  for (auto [index, type] : llvm::enumerate(plans.valueTypes())) {
    if (type.id().value() != index)
      return reject(plans,
                    "process-state plan invariant violated: non-dense ordinal");
    llvm::StringRef specialization =
        detail::PlanSetBuilder::specializationBytes(type);
    if (!previousSpecialization.empty() &&
        previousSpecialization.compare(specialization) >= 0)
      return reject(
          plans,
          previousSpecialization == specialization
              ? "process-state plan invariant violated: duplicate identity"
              : "process-state plan invariant violated: unsorted canonical "
                "order");
    previousSpecialization = specialization;
    auto canonical = detail::canonicalValueTypeSpecialization(type);
    if (!canonical) {
      llvm::consumeError(canonical.takeError());
      return reject(plans, "process-state plan invariant violated: value-type "
                           "specialization mismatch");
    }
    std::string fingerprint = bindings::sha256Fingerprint(*canonical);
    llvm::StringRef digest = llvm::StringRef(fingerprint).drop_front(7);
    llvm::StringRef stem =
        type.kind() == ProcessValueTypeKind::Value ? "value" : "packet";
    std::string expectedSymbol = ("@acir_" + stem + "_" + digest).str();
    std::string expectedCpp = ("acir::generated::" + stem + "_" + digest).str();
    if (type.kind() != type.payload().kind() || !type.acirType() ||
        *canonical != specialization || fingerprint != type.fingerprint() ||
        type.symbol() != expectedSymbol || type.cpp() != expectedCpp)
      return reject(
          plans,
          "process-state plan invariant violated: value-type specialization "
          "mismatch");
    generatedTypeKeys.insert(("storage:" + stem + ":" + digest).str());
    uint64_t width = type.kind() == ProcessValueTypeKind::Value
                         ? type.payload().value().widthBits()
                         : type.payload().packet().widthBits();
    auto members = type.kind() == ProcessValueTypeKind::Value
                       ? type.payload().value().members()
                       : type.payload().packet().members();
    if ((type.kind() == ProcessValueTypeKind::Packet &&
         (type.payload().packet().bytes() > UINT64_MAX / 8 ||
          type.payload().packet().bytes() * 8 != width)) ||
        llvm::any_of(members, [&](const ProcessValueTypeMemberPlan &member) {
          bool shape =
              member.kind() == ProcessValueTypeMemberKind::Field
                  ? !member.name().empty() && !member.index()
                  : member.name().empty() && member.index().has_value();
          return !shape || member.offsetBits() > width ||
                 member.widthBits() > width - member.offsetBits() ||
                 member.encoding().empty() || !validTypeKey(member.typeKey());
        }))
      return reject(plans, "process-state plan invariant violated: value-type "
                           "specialization mismatch");
  }
  auto referenceCloses = [&](llvm::StringRef key) {
    return !key.starts_with("storage:") || generatedTypeKeys.contains(key);
  };
  for (const ProcessGeneratedCalleePlan &callee : plans.callees())
    if (llvm::any_of(
            callee.inputTypeKeys(),
            [&](llvm::StringRef key) { return !referenceCloses(key); }) ||
        llvm::any_of(callee.resultTypeKeys(), [&](llvm::StringRef key) {
          return !referenceCloses(key);
        }))
      return reject(
          plans, "process-state plan invariant violated: dangling reference");
  for (const ProcessValueTypePlan &type : plans.valueTypes()) {
    auto members = type.kind() == ProcessValueTypeKind::Value
                       ? type.payload().value().members()
                       : type.payload().packet().members();
    if (llvm::any_of(members, [&](const ProcessValueTypeMemberPlan &member) {
          return !referenceCloses(member.typeKey());
        }))
      return reject(
          plans, "process-state plan invariant violated: dangling reference");
  }
  return mlir::success();
}

} // namespace acir
