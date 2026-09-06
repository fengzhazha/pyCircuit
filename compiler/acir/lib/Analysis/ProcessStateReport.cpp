#include "ProcessStatePlanInternal.h"

#include "acir/Bindings/Binding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

namespace acir {
namespace {

using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;

template <typename Range, typename Convert>
Array mapArray(Range range, Convert convert) {
  Array result;
  for (const auto &item : range)
    result.push_back(convert(item));
  return result;
}

Array integers(llvm::ArrayRef<uint64_t> values) {
  return mapArray(values, [](uint64_t value) { return Value(value); });
}

std::string typeSpelling(mlir::Type type) {
  std::string storage;
  llvm::raw_string_ostream stream(storage);
  stream << type;
  return storage;
}

llvm::StringRef spelling(ProcessWakeKind value) {
  switch (value) {
  case ProcessWakeKind::Condition:
    return "condition";
  case ProcessWakeKind::Resource:
    return "resource";
  case ProcessWakeKind::EventQueue:
    return "event_queue";
  case ProcessWakeKind::NextDelta:
    return "next_delta";
  }
  llvm_unreachable("unknown wake kind");
}
llvm::StringRef spelling(ProcessSubscriptionSourceKind value) {
  switch (value) {
  case ProcessSubscriptionSourceKind::Capture:
    return "capture";
  case ProcessSubscriptionSourceKind::Value:
    return "value";
  case ProcessSubscriptionSourceKind::Symbol:
    return "symbol";
  }
  llvm_unreachable("unknown source kind");
}
llvm::StringRef spelling(ProcessActionKind value) {
  switch (value) {
  case ProcessActionKind::Original:
    return "original";
  case ProcessActionKind::Constant:
    return "constant";
  case ProcessActionKind::ForInitialize:
    return "for_initialize";
  case ProcessActionKind::ForCondition:
    return "for_condition";
  case ProcessActionKind::ForIncrement:
    return "for_increment";
  case ProcessActionKind::ScalarWrap:
    return "scalar_wrap";
  case ProcessActionKind::ScalarUnwrap:
    return "scalar_unwrap";
  }
  llvm_unreachable("unknown action kind");
}
llvm::StringRef spelling(ProcessEmissionClass value) {
  switch (value) {
  case ProcessEmissionClass::CopyScalar:
    return "copy_scalar";
  case ProcessEmissionClass::Inline:
    return "inline";
  case ProcessEmissionClass::Invoke:
    return "invoke";
  case ProcessEmissionClass::Wrap:
    return "wrap";
  case ProcessEmissionClass::Unwrap:
    return "unwrap";
  case ProcessEmissionClass::ForwardOnly:
    return "forward_only";
  }
  llvm_unreachable("unknown emission class");
}
llvm::StringRef spelling(ProcessLoopPhase value) {
  switch (value) {
  case ProcessLoopPhase::Initialize:
    return "initialize";
  case ProcessLoopPhase::Condition:
    return "condition";
  case ProcessLoopPhase::Increment:
    return "increment";
  }
  llvm_unreachable("unknown loop phase");
}
llvm::StringRef spelling(ProcessWrapperDirection value) {
  return value == ProcessWrapperDirection::Wrap ? "wrap" : "unwrap";
}
llvm::StringRef spelling(ProcessFrameKind value) {
  switch (value) {
  case ProcessFrameKind::Entry:
    return "entry";
  case ProcessFrameKind::ScfIf:
    return "scf.if";
  case ProcessFrameKind::ScfFor:
    return "scf.for";
  case ProcessFrameKind::ScfWhile:
    return "scf.while";
  }
  llvm_unreachable("unknown frame kind");
}
llvm::StringRef spelling(ProcessFramePhase value) {
  switch (value) {
  case ProcessFramePhase::Entry:
    return "entry";
  case ProcessFramePhase::Then:
    return "then";
  case ProcessFramePhase::Else:
    return "else";
  case ProcessFramePhase::Merge:
    return "merge";
  case ProcessFramePhase::Header:
    return "header";
  case ProcessFramePhase::Body:
    return "body";
  case ProcessFramePhase::Before:
    return "before";
  case ProcessFramePhase::After:
    return "after";
  case ProcessFramePhase::Exit:
    return "exit";
  }
  llvm_unreachable("unknown frame phase");
}
llvm::StringRef spelling(ProcessValueCoordinateKind value) {
  return value == ProcessValueCoordinateKind::Result ? "result"
                                                     : "block_argument";
}
llvm::StringRef spelling(ProcessControlEdgeKind value) {
  switch (value) {
  case ProcessControlEdgeKind::Branch:
    return "branch";
  case ProcessControlEdgeKind::LocalContinue:
    return "local_continue";
  case ProcessControlEdgeKind::Suspend:
    return "suspend";
  case ProcessControlEdgeKind::Terminate:
    return "terminate";
  }
  llvm_unreachable("unknown edge kind");
}
llvm::StringRef spelling(ProcessTerminateStatus value) {
  return value == ProcessTerminateStatus::Success ? "success" : "failure";
}
llvm::StringRef spelling(ProcessEffectKind value) {
  return value == ProcessEffectKind::Pure ? "pure" : "stateful";
}
llvm::StringRef spelling(ProcessValueTypeKind value) {
  return value == ProcessValueTypeKind::Value ? "value" : "packet";
}
llvm::StringRef spelling(ProcessValueTypeMemberKind value) {
  return value == ProcessValueTypeMemberKind::Field ? "field" : "element";
}
llvm::StringRef spelling(ProcessStorageSignedness value) {
  switch (value) {
  case ProcessStorageSignedness::Signless:
    return "signless";
  case ProcessStorageSignedness::Signed:
    return "signed";
  case ProcessStorageSignedness::Unsigned:
    return "unsigned";
  }
  llvm_unreachable("unknown signedness");
}
llvm::StringRef spelling(ProcessHelperRole value) {
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
  return names[static_cast<unsigned>(value)];
}

Value json(const ProcessOccurrenceId &occurrence);
Value json(const ProcessPlannedValue &planned);

Value json(const ProcessValueCoordinate &coordinate) {
  Object object;
  object["index"] = coordinate.index();
  object["kind"] = spelling(coordinate.kind());
  object["owner_path"] = coordinate.ownerPath();
  return object;
}
Value json(const ProcessCallSitePlan &site) {
  Object object;
  object["iteration_vector"] = integers(site.iterationVector());
  object["operation_path"] = site.operationPath();
  return object;
}
Value json(const ProcessOccurrenceId &occurrence) {
  Object object;
  switch (occurrence.kind()) {
  case ProcessOccurrenceKind::Original:
    object["call_sites"] = mapArray(occurrence.original().callSites(),
                                    [](const auto &x) { return json(x); });
    object["iteration_vector"] =
        integers(occurrence.original().iterationVector());
    object["kind"] = "original";
    object["operation_path"] = occurrence.original().operationPath();
    break;
  case ProcessOccurrenceKind::SyntheticLoop:
    object["anchor"] = json(occurrence.syntheticLoop().anchor());
    object["kind"] = "synthetic";
    object["phase"] = spelling(occurrence.syntheticLoop().phase());
    break;
  case ProcessOccurrenceKind::SyntheticWrapper:
    object["anchor"] = json(occurrence.syntheticWrapper().anchor());
    object["direction"] = spelling(occurrence.syntheticWrapper().direction());
    object["kind"] = "synthetic";
    object["slot"] = occurrence.syntheticWrapper().slot().value();
    object["transition"] = occurrence.syntheticWrapper().transition().value();
    break;
  case ProcessOccurrenceKind::SyntheticConstant:
    object["anchor"] = json(occurrence.syntheticConstant().anchor());
    object["constant"] = occurrence.syntheticConstant().constant();
    object["kind"] = "synthetic";
    break;
  }
  return object;
}
Value json(const ProcessPlannedValue &planned) {
  Object object;
  switch (planned.kind()) {
  case ProcessPlannedValueKind::Original:
    object["coordinate"] = json(planned.original().coordinate());
    object["kind"] = "original";
    object["occurrence"] = json(planned.original().occurrence());
    object["path"] = planned.original().path();
    break;
  case ProcessPlannedValueKind::Capture:
    object["capture"] = planned.capture().capture().value();
    object["kind"] = "capture";
    break;
  case ProcessPlannedValueKind::LiveSlot:
    object["kind"] = "live_slot";
    object["slot"] = planned.liveSlot().slot().value();
    break;
  case ProcessPlannedValueKind::Synthetic:
    object["coordinate"] = json(planned.synthetic().coordinate());
    object["kind"] = "synthetic";
    object["occurrence"] = json(planned.synthetic().occurrence());
    break;
  case ProcessPlannedValueKind::Constant:
    object["kind"] = "constant";
    object["value"] = planned.constant().value();
    break;
  }
  object["type"] = typeSpelling(planned.type());
  return object;
}
Value json(const ProcessForwardingBindingPlan &binding) {
  Object object;
  object["from"] = json(binding.from());
  object["to"] = json(binding.to());
  return object;
}
Value json(const ProcessControlEdgePlan &edge) {
  Object object;
  object["kind"] = spelling(edge.kind());
  switch (edge.kind()) {
  case ProcessControlEdgeKind::Branch:
    object["condition"] = json(edge.condition());
    object["false_bindings"] =
        mapArray(edge.falseBindings(), [](const auto &x) { return json(x); });
    object["false_block"] = edge.falseBlock().value();
    object["true_bindings"] =
        mapArray(edge.trueBindings(), [](const auto &x) { return json(x); });
    object["true_block"] = edge.trueBlock().value();
    break;
  case ProcessControlEdgeKind::LocalContinue:
    object["bindings"] =
        mapArray(edge.bindings(), [](const auto &x) { return json(x); });
    object["target_block"] = edge.targetBlock().value();
    break;
  case ProcessControlEdgeKind::Suspend:
    object["transition"] = edge.transition().value();
    break;
  case ProcessControlEdgeKind::Terminate:
    object["status"] = spelling(edge.status());
    break;
  }
  return object;
}
Value json(const ProcessTransitionLoadPlan &load) {
  Object object;
  object["replacements"] =
      mapArray(load.replacements(), [](const auto &x) { return json(x); });
  object["slot"] = load.slot().value();
  return object;
}
Value json(const ProcessTransitionStorePlan &store) {
  Object object;
  object["slot"] = store.slot().value();
  object["source"] = json(store.source());
  return object;
}
Value json(const ProcessGeneratedCalleePayload &payload) {
  Object object;
  auto two = [&](llvm::StringRef a, llvm::StringRef av, llvm::StringRef b,
                 llvm::StringRef bv) {
    object[a] = av;
    object[b] = bv;
  };
  switch (payload.role()) {
  case ProcessHelperRole::TraceDecode:
    object["entry"] = payload.traceDecode().entry();
    object["result"] = payload.traceDecode().result();
    object["source"] = payload.traceDecode().source();
    break;
  case ProcessHelperRole::QueueTrySend:
    two("element", payload.queueTrySend().element(), "queue",
        payload.queueTrySend().queue());
    break;
  case ProcessHelperRole::QueueTryRecv:
    two("element", payload.queueTryRecv().element(), "queue",
        payload.queueTryRecv().queue());
    break;
  case ProcessHelperRole::EventSchedule:
    object["delay"] = payload.eventSchedule().delay();
    object["target"] = payload.eventSchedule().target();
    object["value"] = payload.eventSchedule().value();
    break;
  case ProcessHelperRole::TraceOpen:
    object["source"] = payload.traceOpen().source();
    break;
  case ProcessHelperRole::TraceNext:
    two("entry", payload.traceNext().entry(), "source",
        payload.traceNext().source());
    break;
  case ProcessHelperRole::TraceEof:
    object["source"] = payload.traceEof().source();
    break;
  case ProcessHelperRole::TracePosition:
    object["source"] = payload.tracePosition().source();
    break;
  case ProcessHelperRole::ContractRequire:
    object["message"] = payload.contractRequire().message();
    break;
  case ProcessHelperRole::ContractEnsure:
    object["message"] = payload.contractEnsure().message();
    break;
  case ProcessHelperRole::ContractAssert:
    object["message"] = payload.contractAssert().message();
    break;
  case ProcessHelperRole::Probe:
    object["kind"] = payload.probe().kind();
    object["result"] = payload.probe().result();
    object["target"] = payload.probe().target();
    break;
  case ProcessHelperRole::StatAdd:
    two("stat", payload.statAdd().stat(), "value_type",
        payload.statAdd().valueType());
    break;
  case ProcessHelperRole::WakeCondition:
    two("wake_kind", spelling(payload.wakeCondition().wakeKind()), "wake_type",
        payload.wakeCondition().wakeType());
    break;
  case ProcessHelperRole::WakeResource:
    two("wake_kind", spelling(payload.wakeResource().wakeKind()), "wake_type",
        payload.wakeResource().wakeType());
    break;
  case ProcessHelperRole::WakeEventQueue:
    two("wake_kind", spelling(payload.wakeEventQueue().wakeKind()), "wake_type",
        payload.wakeEventQueue().wakeType());
    break;
  case ProcessHelperRole::WakeNextDelta:
    two("wake_kind", spelling(payload.wakeNextDelta().wakeKind()), "wake_type",
        payload.wakeNextDelta().wakeType());
    break;
  case ProcessHelperRole::ScalarWrap:
    object["direction"] = spelling(payload.scalarWrap().direction());
    object["scalar"] = payload.scalarWrap().scalar();
    object["value_type"] = payload.scalarWrap().valueType();
    break;
  case ProcessHelperRole::ScalarUnwrap:
    object["direction"] = spelling(payload.scalarUnwrap().direction());
    object["scalar"] = payload.scalarUnwrap().scalar();
    object["value_type"] = payload.scalarUnwrap().valueType();
    break;
  }
  return object;
}
Value json(const ProcessGeneratedCalleePlan &callee) {
  Object object;
  object["cpp"] = callee.cpp();
  object["effect"] = spelling(callee.effect());
  object["fingerprint"] = callee.fingerprint();
  object["inputs"] = mapArray(callee.inputTypeKeys(),
                              [](llvm::StringRef x) { return Value(x); });
  object["kind"] = callee.kind();
  object["ordinal"] = callee.id().value();
  object["payload"] = json(callee.payload());
  object["results"] = mapArray(callee.resultTypeKeys(),
                               [](llvm::StringRef x) { return Value(x); });
  object["role"] = spelling(callee.role());
  object["source_paths"] = mapArray(callee.sourcePaths(),
                                    [](llvm::StringRef x) { return Value(x); });
  object["symbol"] = callee.symbol();
  return object;
}
Value json(const ProcessValueTypeMemberPlan &member) {
  Object object;
  object["encoding"] = member.encoding();
  object["kind"] = spelling(member.kind());
  if (member.kind() == ProcessValueTypeMemberKind::Field)
    object["name"] = member.name();
  else
    object["index"] = *member.index();
  object["offset_bits"] = member.offsetBits();
  if (member.signedness())
    object["signedness"] = spelling(*member.signedness());
  object["type_key"] = member.typeKey();
  object["width_bits"] = member.widthBits();
  return object;
}
Value json(const ProcessValueTypePlan &type) {
  Object payload;
  const auto &value = type.payload();
  if (value.kind() == ProcessValueTypeKind::Value) {
    payload["encoding"] = value.value().encoding();
    payload["members"] = mapArray(value.value().members(),
                                  [](const auto &x) { return json(x); });
    payload["width_bits"] = value.value().widthBits();
  } else {
    payload["bytes"] = value.packet().bytes();
    payload["encoding"] = value.packet().encoding();
    payload["members"] = mapArray(value.packet().members(),
                                  [](const auto &x) { return json(x); });
    payload["width_bits"] = value.packet().widthBits();
  }
  Object object;
  object["acir_type"] = typeSpelling(type.acirType());
  object["cpp"] = type.cpp();
  object["fingerprint"] = type.fingerprint();
  object["kind"] = spelling(type.kind());
  object["ordinal"] = type.id().value();
  object["payload"] = std::move(payload);
  object["symbol"] = type.symbol();
  return object;
}
Value json(const ProcessStatePlan &plan) {
  Object object;
  object["blocks"] = mapArray(plan.blocks(), [](const ProcessBlockPlan &block) {
    Object value;
    value["actions"] =
        mapArray(block.actions(), [](const ProcessActionPlan &action) {
          Object a;
          a["cost"] = action.cost();
          a["emission"] = spelling(action.emission());
          a["iteration_vector"] = integers(action.iterationVector());
          a["kind"] = spelling(action.kind());
          a["occurrence"] = json(action.occurrence());
          a["operands"] = mapArray(action.operands(),
                                   [](const auto &x) { return json(x); });
          a["ordinal"] = action.id();
          a["result_types"] = mapArray(action.resultTypes(), [](mlir::Type x) {
            return Value(typeSpelling(x));
          });
          a["results"] =
              mapArray(action.results(), [](const auto &x) { return json(x); });
          if (action.callee())
            a["callee"] = action.callee()->value();
          if (action.scalarOp()) {
            Object scalar;
            scalar["attributes"] =
                mapArray(action.scalarOp()->attributes(), [](const auto &x) {
                  Object attr;
                  attr["name"] = x.name();
                  attr["value"] = x.value();
                  return Value(std::move(attr));
                });
            scalar["name"] = action.scalarOp()->name();
            scalar["properties"] = action.scalarOp()->properties();
            a["scalar_op"] = std::move(scalar);
          }
          return Value(std::move(a));
        });
    value["cost"] = block.cost();
    value["edge"] = json(block.edge());
    value["frames"] =
        mapArray(block.frames(), [](const ProcessControlFramePlan &frame) {
          Object f;
          f["bindings"] =
              mapArray(frame.bindings(), [](const auto &x) { return json(x); });
          f["kind"] = spelling(frame.kind());
          f["operation_path"] = frame.operationPath();
          f["phase"] = spelling(frame.phase());
          return Value(std::move(f));
        });
    value["loads"] =
        mapArray(block.loads(), [](const auto &x) { return json(x); });
    value["ordinal"] = block.id().value();
    value["path"] = block.path();
    value["pc"] = block.pc().value();
    return Value(std::move(value));
  });
  object["captures"] =
      mapArray(plan.captures(), [](const ProcessCapturePlan &capture) {
        Object value;
        value["argument_path"] = capture.argumentPath();
        value["name"] = capture.name();
        value["operand_path"] = capture.operandPath();
        value["ordinal"] = capture.id().value();
        value["type"] = typeSpelling(capture.type());
        return Value(std::move(value));
      });
  object["definition_key"] = plan.definitionKey();
  object["entry_pc"] = plan.entryPc().value();
  object["fairness_work"] = plan.fairnessWork();
  object["live_slots"] =
      mapArray(plan.liveSlots(), [](const ProcessLiveSlotPlan &slot) {
        Object value;
        value["member_values"] = mapArray(
            slot.memberValues(), [](const auto &x) { return json(x); });
        value["name"] = slot.name();
        value["ordinal"] = slot.id().value();
        value["storage_type"] = slot.storageType().value();
        value["type"] = typeSpelling(slot.type());
        if (slot.wrapCallee()) {
          value["wrap_callee"] = slot.wrapCallee()->value();
          value["unwrap_callee"] = slot.unwrapCallee()->value();
        }
        return Value(std::move(value));
      });
  object["pc_bit_width"] = plan.pcBitWidth();
  object["pcs"] = mapArray(plan.pcs(), [](const ProcessPcPlan &pc) {
    Object value;
    value["blocks"] = mapArray(
        pc.blocks(), [](ProcessBlockId id) { return Value(id.value()); });
    value["entry_path"] = pc.entryPath();
    value["name"] = pc.name();
    value["ordinal"] = pc.id().value();
    return Value(std::move(value));
  });
  object["transitions"] =
      mapArray(plan.transitions(), [](const ProcessTransitionPlan &transition) {
        Object value;
        value["iteration_vector"] = integers(transition.iterationVector());
        value["loads"] =
            mapArray(transition.loads(), [](const auto &x) { return json(x); });
        value["ordinal"] = transition.id().value();
        value["source_pc"] = transition.sourcePc().value();
        value["stores"] = mapArray(transition.stores(),
                                   [](const auto &x) { return json(x); });
        value["target_pc"] = transition.targetPc().value();
        value["wake"] = transition.wake().value();
        return Value(std::move(value));
      });
  object["wakes"] = mapArray(plan.wakes(), [](const ProcessWakePlan &wake) {
    Object value;
    value["callee"] = wake.callee().value();
    value["iteration_vector"] = integers(wake.iterationVector());
    value["kind"] = spelling(wake.kind());
    value["occurrence"] = json(wake.occurrence());
    value["operation_path"] = wake.operationPath();
    value["ordinal"] = wake.id().value();
    value["sources"] = mapArray(
        wake.sources(), [](const ProcessSubscriptionSourcePlan &source) {
          Object s;
          if (source.capture())
            s["capture"] = source.capture()->value();
          s["kind"] = spelling(source.kind());
          if (!source.ownerPath().empty())
            s["owner_path"] = source.ownerPath();
          s["path"] = source.path();
          if (!source.symbol().empty())
            s["symbol"] = source.symbol();
          return Value(std::move(s));
        });
    value["target"] = wake.target();
    value["type_key"] = wake.typeKey();
    return Value(std::move(value));
  });
  return object;
}

} // namespace

llvm::Expected<std::string>
detail::canonicalProcessOccurrenceJSON(const ProcessOccurrenceId &occurrence) {
  return bindings::canonicalizeJson(json(occurrence));
}

llvm::Expected<std::string>
detail::hashProcessOccurrence(const ProcessOccurrenceId &occurrence) {
  auto canonical = canonicalProcessOccurrenceJSON(occurrence);
  if (!canonical)
    return canonical.takeError();
  llvm::SHA256 hasher;
  hasher.update(*canonical);
  return llvm::toHex(hasher.final(), /*LowerCase=*/true);
}

llvm::Expected<std::string> detail::canonicalGeneratedCalleeSpecialization(
    const ProcessGeneratedCalleePlan &callee) {
  Object object;
  object["contract_epoch"] = "0.5";
  object["effect"] = spelling(callee.effect());
  object["inputs"] = mapArray(callee.inputTypeKeys(),
                              [](llvm::StringRef key) { return Value(key); });
  object["kind"] = callee.kind();
  object["payload"] = json(callee.payload());
  object["results"] = mapArray(callee.resultTypeKeys(),
                               [](llvm::StringRef key) { return Value(key); });
  object["role"] = spelling(callee.role());
  object["schema"] = "acir-generated-implementation-0.1";
  object["source_paths"] = mapArray(
      callee.sourcePaths(), [](llvm::StringRef path) { return Value(path); });
  return bindings::canonicalizeJson(Value(std::move(object)));
}

llvm::Expected<std::string>
detail::canonicalValueTypeSpecialization(const ProcessValueTypePlan &type) {
  Object descriptor = *json(type).getAsObject();
  descriptor.erase("cpp");
  descriptor.erase("fingerprint");
  descriptor.erase("ordinal");
  descriptor.erase("symbol");
  descriptor["contract_epoch"] = "0.5";
  descriptor["schema"] = "acir-generated-value-type-0.1";
  return bindings::canonicalizeJson(Value(std::move(descriptor)));
}

llvm::Expected<std::string>
serializeProcessStatePlan(const ProcessStatePlanSet &plans,
                          const ProcessStateLimits &limits) {
  if (mlir::failed(verifyProcessStatePlan(plans, limits)))
    return llvm::createStringError("process-state plan verification failed");
  Object report;
  report["callees"] =
      mapArray(plans.callees(), [](const auto &x) { return json(x); });
  report["contract_epoch"] = "0.5";
  report["processes"] =
      mapArray(plans.processes(), [](const auto &x) { return json(x); });
  report["schema"] = "acir-process-state-plan-0.1";
  report["value_types"] =
      mapArray(plans.valueTypes(), [](const auto &x) { return json(x); });
  auto canonical = bindings::canonicalizeJson(Value(std::move(report)));
  if (!canonical)
    return canonical.takeError();
  if (canonical->size() > limits.maxCanonicalReportBytes)
    return llvm::createStringError(
        "process-state plan capability maxCanonicalReportBytes exceeded");
  return canonical;
}

} // namespace acir
