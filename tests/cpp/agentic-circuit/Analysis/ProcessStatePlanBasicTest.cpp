#include "Analysis/ProcessStatePlanInternal.h"
#include "Analysis/ProcessStatePlanTestHooks.h"
#include "ProcessStatePlanTestSupport.h"
#include "acir/Analysis/ProcessStatePlan.h"

#include "gtest/gtest.h"

#include <concepts>
#include <type_traits>

namespace acir {
namespace {

template <typename T>
concept HasId = requires(const T &value) { value.id(); };
template <typename T>
concept HasOrdinal = requires(const T &value) { value.ordinal(); };
template <typename T>
concept HasSetter = requires(T &value) { value.setId(value.entryPc()); };
template <typename T>
concept HasMutableProcesses = requires(T &value) {
  {
    value.processes()
  } -> std::same_as<llvm::MutableArrayRef<ProcessStatePlan>>;
};
template <typename T>
concept HasComponentLookup =
    requires(const T &value) { value.lookupByComponentName("workload"); };
template <typename T>
concept HasHierarchyLookup =
    requires(const T &value) { value.lookupByHierarchy("Top.workload"); };
template <typename T>
concept HasFallbackLookup =
    requires(const T &value) { value.lookup("workload"); };

template <typename = void>
concept CompleteProcessStatePlanApi = requires(
    const ProcessCallSitePlan &callSite,
    const ProcessOriginalOccurrence &originalOccurrence,
    const ProcessSyntheticLoopOccurrence &loopOccurrence,
    const ProcessSyntheticWrapperOccurrence &wrapperOccurrence,
    const ProcessSyntheticConstantOccurrence &constantOccurrence,
    const ProcessOccurrenceId &occurrence,
    const ProcessValueCoordinate &coordinate,
    const ProcessOriginalPlannedValue &originalValue,
    const ProcessCapturePlannedValue &captureValue,
    const ProcessLiveSlotPlannedValue &slotValue,
    const ProcessSyntheticPlannedValue &syntheticValue,
    const ProcessConstantPlannedValue &constantValue,
    const ProcessPlannedValue &plannedValue,
    const ProcessScalarAttribute &scalarAttribute,
    const ProcessScalarOperationPlan &scalarOp,
    const ProcessCapturePlan &capture, const ProcessActionPlan &action,
    const ProcessLiveSlotPlan &slot,
    const ProcessSubscriptionSourcePlan &source, const ProcessWakePlan &wake,
    const ProcessTransitionStorePlan &store,
    const ProcessTransitionLoadPlan &load,
    const ProcessTransitionPlan &transition,
    const ProcessForwardingBindingPlan &binding,
    const ProcessControlFramePlan &frame, const ProcessControlEdgePlan &edge,
    const ProcessBlockPlan &block, const ProcessPcPlan &pc,
    const ProcessStatePlan &plan,
    const ProcessTraceDecodePayload &traceDecode,
    const ProcessQueueTrySendPayload &queueSend,
    const ProcessQueueTryRecvPayload &queueRecv,
    const ProcessEventSchedulePayload &eventSchedule,
    const ProcessTraceOpenPayload &traceOpen,
    const ProcessTraceNextPayload &traceNext,
    const ProcessTraceEofPayload &traceEof,
    const ProcessTracePositionPayload &tracePosition,
    const ProcessContractRequirePayload &requirePayload,
    const ProcessContractEnsurePayload &ensurePayload,
    const ProcessContractAssertPayload &assertPayload,
    const ProcessProbePayload &probe, const ProcessStatAddPayload &stat,
    const ProcessWakeConditionPayload &wakeCondition,
    const ProcessWakeResourcePayload &wakeResource,
    const ProcessWakeEventQueuePayload &wakeEvent,
    const ProcessWakeNextDeltaPayload &wakeNext,
    const ProcessScalarWrapPayload &wrap,
    const ProcessScalarUnwrapPayload &unwrap,
    const ProcessGeneratedCalleePayload &calleePayload,
    const ProcessValueTypeMemberPlan &member,
    const ProcessStorageValuePayload &storageValue,
    const ProcessStoragePacketPayload &storagePacket,
    const ProcessValueTypePayload &valuePayload,
    const ProcessGeneratedCalleePlan &callee,
    const ProcessValueTypePlan &valueType, const ProcessStatePlanSet &set) {
  callSite.operation();
  callSite.operationPath();
  callSite.iterationVector();
  originalOccurrence.operation();
  originalOccurrence.operationPath();
  originalOccurrence.callSites();
  originalOccurrence.iterationVector();
  loopOccurrence.anchor();
  loopOccurrence.phase();
  wrapperOccurrence.anchor();
  wrapperOccurrence.transition();
  wrapperOccurrence.slot();
  wrapperOccurrence.direction();
  constantOccurrence.anchor();
  constantOccurrence.constant();
  occurrence.kind();
  occurrence.original();
  occurrence.syntheticLoop();
  occurrence.syntheticWrapper();
  occurrence.syntheticConstant();
  coordinate.kind();
  coordinate.ownerPath();
  coordinate.index();
  originalValue.value();
  originalValue.occurrence();
  originalValue.coordinate();
  originalValue.path();
  captureValue.capture();
  slotValue.slot();
  syntheticValue.occurrence();
  syntheticValue.coordinate();
  constantValue.value();
  plannedValue.kind();
  plannedValue.type();
  plannedValue.original();
  plannedValue.capture();
  plannedValue.liveSlot();
  plannedValue.synthetic();
  plannedValue.constant();
  scalarAttribute.name();
  scalarAttribute.value();
  scalarOp.name();
  scalarOp.attributes();
  scalarOp.properties();
  capture.id();
  capture.name();
  capture.operand();
  capture.entryArgument();
  capture.type();
  capture.operandPath();
  capture.argumentPath();
  action.id();
  action.kind();
  action.emission();
  action.occurrence();
  action.sourceOperation();
  action.iterationVector();
  action.operands();
  action.results();
  action.cost();
  action.resultTypes();
  action.callee();
  action.scalarOp();
  slot.id();
  slot.name();
  slot.type();
  slot.storageType();
  slot.memberValues();
  slot.wrapCallee();
  slot.unwrapCallee();
  source.kind();
  source.value();
  source.owner();
  source.declaration();
  source.capture();
  source.symbol();
  source.path();
  source.ownerPath();
  wake.id();
  wake.kind();
  wake.operation();
  wake.triggeringValue();
  wake.declaration();
  wake.callee();
  wake.typeKey();
  wake.operationPath();
  wake.target();
  wake.occurrence();
  wake.iterationVector();
  wake.sources();
  store.slot();
  store.source();
  store.sourceValue();
  load.slot();
  load.replacements();
  transition.id();
  transition.sourcePc();
  transition.targetPc();
  transition.wake();
  transition.iterationVector();
  transition.stores();
  transition.loads();
  binding.from();
  binding.to();
  frame.kind();
  frame.phase();
  frame.operation();
  frame.operationPath();
  frame.bindings();
  edge.kind();
  edge.condition();
  edge.trueBlock();
  edge.falseBlock();
  edge.trueBindings();
  edge.falseBindings();
  edge.targetBlock();
  edge.bindings();
  edge.transition();
  edge.status();
  block.id();
  block.pc();
  block.originRegion();
  block.originBlock();
  block.path();
  block.frames();
  block.loads();
  block.actions();
  block.edge();
  block.cost();
  pc.id();
  pc.name();
  pc.entryPath();
  pc.blocks();
  plan.definitionKey();
  plan.process();
  plan.captures();
  plan.entryPc();
  plan.pcs();
  plan.blocks();
  plan.liveSlots();
  plan.wakes();
  plan.transitions();
  plan.pcBitWidth();
  plan.fairnessWork();
  traceDecode.entry();
  traceDecode.result();
  traceDecode.source();
  queueSend.element();
  queueSend.queue();
  queueRecv.element();
  queueRecv.queue();
  eventSchedule.delay();
  eventSchedule.target();
  eventSchedule.value();
  traceOpen.source();
  traceNext.entry();
  traceNext.source();
  traceEof.source();
  tracePosition.source();
  requirePayload.message();
  ensurePayload.message();
  assertPayload.message();
  probe.kind();
  probe.result();
  probe.target();
  stat.stat();
  stat.valueType();
  wakeCondition.wakeKind();
  wakeCondition.wakeType();
  wakeResource.wakeKind();
  wakeResource.wakeType();
  wakeEvent.wakeKind();
  wakeEvent.wakeType();
  wakeNext.wakeKind();
  wakeNext.wakeType();
  wrap.direction();
  wrap.scalar();
  wrap.valueType();
  unwrap.direction();
  unwrap.scalar();
  unwrap.valueType();
  calleePayload.role();
  calleePayload.traceDecode();
  calleePayload.queueTrySend();
  calleePayload.queueTryRecv();
  calleePayload.eventSchedule();
  calleePayload.traceOpen();
  calleePayload.traceNext();
  calleePayload.traceEof();
  calleePayload.tracePosition();
  calleePayload.contractRequire();
  calleePayload.contractEnsure();
  calleePayload.contractAssert();
  calleePayload.probe();
  calleePayload.statAdd();
  calleePayload.wakeCondition();
  calleePayload.wakeResource();
  calleePayload.wakeEventQueue();
  calleePayload.wakeNextDelta();
  calleePayload.scalarWrap();
  calleePayload.scalarUnwrap();
  member.kind();
  member.name();
  member.index();
  member.offsetBits();
  member.widthBits();
  member.signedness();
  member.encoding();
  member.typeKey();
  storageValue.members();
  storageValue.widthBits();
  storageValue.encoding();
  storagePacket.members();
  storagePacket.widthBits();
  storagePacket.bytes();
  storagePacket.encoding();
  valuePayload.kind();
  valuePayload.value();
  valuePayload.packet();
  callee.id();
  callee.symbol();
  callee.cpp();
  callee.kind();
  callee.fingerprint();
  callee.effect();
  callee.inputTypeKeys();
  callee.resultTypeKeys();
  callee.role();
  callee.payload();
  callee.sourceOperations();
  callee.declarations();
  callee.sourcePaths();
  valueType.id();
  valueType.symbol();
  valueType.cpp();
  valueType.kind();
  valueType.fingerprint();
  valueType.acirType();
  valueType.payload();
  set.processes();
  set.callees();
  set.valueTypes();
  set.lookupByDefinitionKey("@Top::@workload");
};

static_assert(HasId<ProcessGeneratedCalleePlan>);
static_assert(HasId<ProcessValueTypePlan>);
static_assert(HasId<ProcessCapturePlan>);
static_assert(HasId<ProcessPcPlan>);
static_assert(HasId<ProcessBlockPlan>);
static_assert(HasId<ProcessLiveSlotPlan>);
static_assert(HasId<ProcessWakePlan>);
static_assert(HasId<ProcessTransitionPlan>);
static_assert(CompleteProcessStatePlanApi<>);
#define CHECK_GETTER(Class, Method, Return)                                    \
  static_assert(                                                               \
      std::same_as<decltype(&Class::Method), Return (Class::*)() const>)
#define CHECK_STRING(Class, Method) CHECK_GETTER(Class, Method, llvm::StringRef)
#define CHECK_U32(Class, Method) CHECK_GETTER(Class, Method, uint32_t)
#define CHECK_U64(Class, Method) CHECK_GETTER(Class, Method, uint64_t)

CHECK_GETTER(ProcessCallSitePlan, operation, mlir::Operation *);
CHECK_STRING(ProcessCallSitePlan, operationPath);
CHECK_GETTER(ProcessCallSitePlan, iterationVector, llvm::ArrayRef<uint64_t>);
CHECK_GETTER(ProcessOriginalOccurrence, operation, mlir::Operation *);
CHECK_STRING(ProcessOriginalOccurrence, operationPath);
CHECK_GETTER(ProcessOriginalOccurrence, callSites,
             llvm::ArrayRef<ProcessCallSitePlan>);
CHECK_GETTER(ProcessOriginalOccurrence, iterationVector,
             llvm::ArrayRef<uint64_t>);
CHECK_GETTER(ProcessSyntheticLoopOccurrence, anchor,
             const ProcessOccurrenceId &);
CHECK_GETTER(ProcessSyntheticLoopOccurrence, phase, ProcessLoopPhase);
CHECK_GETTER(ProcessSyntheticWrapperOccurrence, anchor,
             const ProcessOccurrenceId &);
CHECK_GETTER(ProcessSyntheticWrapperOccurrence, transition,
             ProcessTransitionId);
CHECK_GETTER(ProcessSyntheticWrapperOccurrence, slot, ProcessLiveSlotId);
CHECK_GETTER(ProcessSyntheticWrapperOccurrence, direction,
             ProcessWrapperDirection);
CHECK_GETTER(ProcessSyntheticConstantOccurrence, anchor,
             const ProcessOccurrenceId &);
CHECK_U32(ProcessSyntheticConstantOccurrence, constant);
CHECK_GETTER(ProcessOccurrenceId, kind, ProcessOccurrenceKind);
CHECK_GETTER(ProcessOccurrenceId, original, const ProcessOriginalOccurrence &);
CHECK_GETTER(ProcessOccurrenceId, syntheticLoop,
             const ProcessSyntheticLoopOccurrence &);
CHECK_GETTER(ProcessOccurrenceId, syntheticWrapper,
             const ProcessSyntheticWrapperOccurrence &);
CHECK_GETTER(ProcessOccurrenceId, syntheticConstant,
             const ProcessSyntheticConstantOccurrence &);
CHECK_GETTER(ProcessValueCoordinate, kind, ProcessValueCoordinateKind);
CHECK_STRING(ProcessValueCoordinate, ownerPath);
CHECK_U32(ProcessValueCoordinate, index);
CHECK_GETTER(ProcessOriginalPlannedValue, value, mlir::Value);
CHECK_GETTER(ProcessOriginalPlannedValue, occurrence,
             const ProcessOccurrenceId &);
CHECK_GETTER(ProcessOriginalPlannedValue, coordinate,
             const ProcessValueCoordinate &);
CHECK_STRING(ProcessOriginalPlannedValue, path);
CHECK_GETTER(ProcessCapturePlannedValue, capture, ProcessCaptureId);
CHECK_GETTER(ProcessLiveSlotPlannedValue, slot, ProcessLiveSlotId);
CHECK_GETTER(ProcessSyntheticPlannedValue, occurrence,
             const ProcessOccurrenceId &);
CHECK_GETTER(ProcessSyntheticPlannedValue, coordinate,
             const ProcessValueCoordinate &);
CHECK_STRING(ProcessConstantPlannedValue, value);
CHECK_GETTER(ProcessPlannedValue, kind, ProcessPlannedValueKind);
CHECK_GETTER(ProcessPlannedValue, type, mlir::Type);
CHECK_GETTER(ProcessPlannedValue, original,
             const ProcessOriginalPlannedValue &);
CHECK_GETTER(ProcessPlannedValue, capture, const ProcessCapturePlannedValue &);
CHECK_GETTER(ProcessPlannedValue, liveSlot,
             const ProcessLiveSlotPlannedValue &);
CHECK_GETTER(ProcessPlannedValue, synthetic,
             const ProcessSyntheticPlannedValue &);
CHECK_GETTER(ProcessPlannedValue, constant,
             const ProcessConstantPlannedValue &);
CHECK_STRING(ProcessScalarAttribute, name);
CHECK_STRING(ProcessScalarAttribute, value);
CHECK_STRING(ProcessScalarOperationPlan, name);
CHECK_GETTER(ProcessScalarOperationPlan, attributes,
             llvm::ArrayRef<ProcessScalarAttribute>);
CHECK_STRING(ProcessScalarOperationPlan, properties);
CHECK_GETTER(ProcessCapturePlan, id, ProcessCaptureId);
CHECK_STRING(ProcessCapturePlan, name);
CHECK_GETTER(ProcessCapturePlan, operand, mlir::Value);
CHECK_GETTER(ProcessCapturePlan, entryArgument, mlir::Value);
CHECK_GETTER(ProcessCapturePlan, type, mlir::Type);
CHECK_STRING(ProcessCapturePlan, operandPath);
CHECK_STRING(ProcessCapturePlan, argumentPath);
CHECK_U32(ProcessActionPlan, id);
CHECK_GETTER(ProcessActionPlan, kind, ProcessActionKind);
CHECK_GETTER(ProcessActionPlan, emission, ProcessEmissionClass);
CHECK_GETTER(ProcessActionPlan, occurrence, const ProcessOccurrenceId &);
CHECK_GETTER(ProcessActionPlan, sourceOperation, mlir::Operation *);
CHECK_GETTER(ProcessActionPlan, iterationVector, llvm::ArrayRef<uint64_t>);
CHECK_GETTER(ProcessActionPlan, operands, llvm::ArrayRef<ProcessPlannedValue>);
CHECK_GETTER(ProcessActionPlan, results, llvm::ArrayRef<ProcessPlannedValue>);
CHECK_U32(ProcessActionPlan, cost);
CHECK_GETTER(ProcessActionPlan, resultTypes, llvm::ArrayRef<mlir::Type>);
CHECK_GETTER(ProcessActionPlan, callee, std::optional<ProcessCalleeId>);
CHECK_GETTER(ProcessActionPlan, scalarOp, const ProcessScalarOperationPlan *);
CHECK_GETTER(ProcessLiveSlotPlan, id, ProcessLiveSlotId);
CHECK_STRING(ProcessLiveSlotPlan, name);
CHECK_GETTER(ProcessLiveSlotPlan, type, mlir::Type);
CHECK_GETTER(ProcessLiveSlotPlan, storageType, ProcessValueTypeId);
CHECK_GETTER(ProcessLiveSlotPlan, memberValues,
             llvm::ArrayRef<ProcessPlannedValue>);
CHECK_GETTER(ProcessLiveSlotPlan, wrapCallee, std::optional<ProcessCalleeId>);
CHECK_GETTER(ProcessLiveSlotPlan, unwrapCallee, std::optional<ProcessCalleeId>);
CHECK_GETTER(ProcessSubscriptionSourcePlan, kind,
             ProcessSubscriptionSourceKind);
CHECK_GETTER(ProcessSubscriptionSourcePlan, value, mlir::Value);
CHECK_GETTER(ProcessSubscriptionSourcePlan, owner, mlir::Operation *);
CHECK_GETTER(ProcessSubscriptionSourcePlan, declaration, mlir::Operation *);
CHECK_GETTER(ProcessSubscriptionSourcePlan, capture,
             std::optional<ProcessCaptureId>);
CHECK_STRING(ProcessSubscriptionSourcePlan, symbol);
CHECK_STRING(ProcessSubscriptionSourcePlan, path);
CHECK_STRING(ProcessSubscriptionSourcePlan, ownerPath);
CHECK_GETTER(ProcessWakePlan, id, ProcessWakeId);
CHECK_GETTER(ProcessWakePlan, kind, ProcessWakeKind);
CHECK_GETTER(ProcessWakePlan, operation, mlir::Operation *);
CHECK_GETTER(ProcessWakePlan, triggeringValue, mlir::Value);
CHECK_GETTER(ProcessWakePlan, declaration, mlir::Operation *);
CHECK_GETTER(ProcessWakePlan, callee, ProcessCalleeId);
CHECK_STRING(ProcessWakePlan, typeKey);
CHECK_STRING(ProcessWakePlan, operationPath);
CHECK_STRING(ProcessWakePlan, target);
CHECK_GETTER(ProcessWakePlan, occurrence, const ProcessOccurrenceId &);
CHECK_GETTER(ProcessWakePlan, iterationVector, llvm::ArrayRef<uint64_t>);
CHECK_GETTER(ProcessWakePlan, sources,
             llvm::ArrayRef<ProcessSubscriptionSourcePlan>);
CHECK_GETTER(ProcessTransitionStorePlan, slot, ProcessLiveSlotId);
CHECK_GETTER(ProcessTransitionStorePlan, source, const ProcessPlannedValue &);
CHECK_GETTER(ProcessTransitionStorePlan, sourceValue, mlir::Value);
CHECK_GETTER(ProcessTransitionLoadPlan, slot, ProcessLiveSlotId);
CHECK_GETTER(ProcessTransitionLoadPlan, replacements,
             llvm::ArrayRef<ProcessPlannedValue>);
CHECK_GETTER(ProcessTransitionPlan, id, ProcessTransitionId);
CHECK_GETTER(ProcessTransitionPlan, sourcePc, ProcessPcId);
CHECK_GETTER(ProcessTransitionPlan, targetPc, ProcessPcId);
CHECK_GETTER(ProcessTransitionPlan, wake, ProcessWakeId);
CHECK_GETTER(ProcessTransitionPlan, iterationVector, llvm::ArrayRef<uint64_t>);
CHECK_GETTER(ProcessTransitionPlan, stores,
             llvm::ArrayRef<ProcessTransitionStorePlan>);
CHECK_GETTER(ProcessTransitionPlan, loads,
             llvm::ArrayRef<ProcessTransitionLoadPlan>);
CHECK_GETTER(ProcessForwardingBindingPlan, from, const ProcessPlannedValue &);
CHECK_GETTER(ProcessForwardingBindingPlan, to, const ProcessPlannedValue &);
CHECK_GETTER(ProcessControlFramePlan, kind, ProcessFrameKind);
CHECK_GETTER(ProcessControlFramePlan, phase, ProcessFramePhase);
CHECK_GETTER(ProcessControlFramePlan, operation, mlir::Operation *);
CHECK_STRING(ProcessControlFramePlan, operationPath);
CHECK_GETTER(ProcessControlFramePlan, bindings,
             llvm::ArrayRef<ProcessForwardingBindingPlan>);
CHECK_GETTER(ProcessControlEdgePlan, kind, ProcessControlEdgeKind);
CHECK_GETTER(ProcessControlEdgePlan, condition, const ProcessPlannedValue &);
CHECK_GETTER(ProcessControlEdgePlan, trueBlock, ProcessBlockId);
CHECK_GETTER(ProcessControlEdgePlan, falseBlock, ProcessBlockId);
CHECK_GETTER(ProcessControlEdgePlan, trueBindings,
             llvm::ArrayRef<ProcessForwardingBindingPlan>);
CHECK_GETTER(ProcessControlEdgePlan, falseBindings,
             llvm::ArrayRef<ProcessForwardingBindingPlan>);
CHECK_GETTER(ProcessControlEdgePlan, targetBlock, ProcessBlockId);
CHECK_GETTER(ProcessControlEdgePlan, bindings,
             llvm::ArrayRef<ProcessForwardingBindingPlan>);
CHECK_GETTER(ProcessControlEdgePlan, transition, ProcessTransitionId);
CHECK_GETTER(ProcessControlEdgePlan, status, ProcessTerminateStatus);
CHECK_GETTER(ProcessBlockPlan, id, ProcessBlockId);
CHECK_GETTER(ProcessBlockPlan, pc, ProcessPcId);
CHECK_GETTER(ProcessBlockPlan, originRegion, mlir::Region *);
CHECK_GETTER(ProcessBlockPlan, originBlock, mlir::Block *);
CHECK_STRING(ProcessBlockPlan, path);
CHECK_GETTER(ProcessBlockPlan, frames, llvm::ArrayRef<ProcessControlFramePlan>);
CHECK_GETTER(ProcessBlockPlan, loads,
             llvm::ArrayRef<ProcessTransitionLoadPlan>);
CHECK_GETTER(ProcessBlockPlan, actions, llvm::ArrayRef<ProcessActionPlan>);
CHECK_GETTER(ProcessBlockPlan, edge, const ProcessControlEdgePlan &);
CHECK_U64(ProcessBlockPlan, cost);
CHECK_GETTER(ProcessPcPlan, id, ProcessPcId);
CHECK_STRING(ProcessPcPlan, name);
CHECK_STRING(ProcessPcPlan, entryPath);
CHECK_GETTER(ProcessPcPlan, blocks, llvm::ArrayRef<ProcessBlockId>);
CHECK_STRING(ProcessStatePlan, definitionKey);
CHECK_GETTER(ProcessStatePlan, process, ac::ProcessOp);
CHECK_GETTER(ProcessStatePlan, captures, llvm::ArrayRef<ProcessCapturePlan>);
CHECK_GETTER(ProcessStatePlan, entryPc, ProcessPcId);
CHECK_GETTER(ProcessStatePlan, pcs, llvm::ArrayRef<ProcessPcPlan>);
CHECK_GETTER(ProcessStatePlan, blocks, llvm::ArrayRef<ProcessBlockPlan>);
CHECK_GETTER(ProcessStatePlan, liveSlots, llvm::ArrayRef<ProcessLiveSlotPlan>);
CHECK_GETTER(ProcessStatePlan, wakes, llvm::ArrayRef<ProcessWakePlan>);
CHECK_GETTER(ProcessStatePlan, transitions,
             llvm::ArrayRef<ProcessTransitionPlan>);
CHECK_U32(ProcessStatePlan, pcBitWidth);
CHECK_U64(ProcessStatePlan, fairnessWork);

#define CHECK_PAYLOAD_STRING(Class, Method) CHECK_STRING(Class, Method)
#define CHECK_TWO_STRINGS(Class, First, Second)                                \
  CHECK_STRING(Class, First);                                                  \
  CHECK_STRING(Class, Second)
#define CHECK_THREE_STRINGS(Class, First, Second, Third)                       \
  CHECK_STRING(Class, First);                                                  \
  CHECK_STRING(Class, Second);                                                 \
  CHECK_STRING(Class, Third)
CHECK_THREE_STRINGS(ProcessTraceDecodePayload, entry, result, source);
CHECK_TWO_STRINGS(ProcessQueueTrySendPayload, element, queue);
CHECK_TWO_STRINGS(ProcessQueueTryRecvPayload, element, queue);
CHECK_THREE_STRINGS(ProcessEventSchedulePayload, delay, target, value);
CHECK_STRING(ProcessTraceOpenPayload, source);
CHECK_TWO_STRINGS(ProcessTraceNextPayload, entry, source);
CHECK_STRING(ProcessTraceEofPayload, source);
CHECK_STRING(ProcessTracePositionPayload, source);
CHECK_STRING(ProcessContractRequirePayload, message);
CHECK_STRING(ProcessContractEnsurePayload, message);
CHECK_STRING(ProcessContractAssertPayload, message);
CHECK_THREE_STRINGS(ProcessProbePayload, kind, result, target);
CHECK_TWO_STRINGS(ProcessStatAddPayload, stat, valueType);
#define CHECK_WAKE_PAYLOAD(Class)                                              \
  CHECK_GETTER(Class, wakeKind, ProcessWakeKind);                              \
  CHECK_STRING(Class, wakeType)
CHECK_WAKE_PAYLOAD(ProcessWakeConditionPayload);
CHECK_WAKE_PAYLOAD(ProcessWakeResourcePayload);
CHECK_WAKE_PAYLOAD(ProcessWakeEventQueuePayload);
CHECK_WAKE_PAYLOAD(ProcessWakeNextDeltaPayload);
#define CHECK_SCALAR_PAYLOAD(Class)                                            \
  CHECK_GETTER(Class, direction, ProcessWrapperDirection);                     \
  CHECK_STRING(Class, scalar);                                                 \
  CHECK_STRING(Class, valueType)
CHECK_SCALAR_PAYLOAD(ProcessScalarWrapPayload);
CHECK_SCALAR_PAYLOAD(ProcessScalarUnwrapPayload);
CHECK_GETTER(ProcessGeneratedCalleePayload, role, ProcessHelperRole);
#define CHECK_PAYLOAD_ARM(Method, Type)                                        \
  CHECK_GETTER(ProcessGeneratedCalleePayload, Method, const Type &)
CHECK_PAYLOAD_ARM(traceDecode, ProcessTraceDecodePayload);
CHECK_PAYLOAD_ARM(queueTrySend, ProcessQueueTrySendPayload);
CHECK_PAYLOAD_ARM(queueTryRecv, ProcessQueueTryRecvPayload);
CHECK_PAYLOAD_ARM(eventSchedule, ProcessEventSchedulePayload);
CHECK_PAYLOAD_ARM(traceOpen, ProcessTraceOpenPayload);
CHECK_PAYLOAD_ARM(traceNext, ProcessTraceNextPayload);
CHECK_PAYLOAD_ARM(traceEof, ProcessTraceEofPayload);
CHECK_PAYLOAD_ARM(tracePosition, ProcessTracePositionPayload);
CHECK_PAYLOAD_ARM(contractRequire, ProcessContractRequirePayload);
CHECK_PAYLOAD_ARM(contractEnsure, ProcessContractEnsurePayload);
CHECK_PAYLOAD_ARM(contractAssert, ProcessContractAssertPayload);
CHECK_PAYLOAD_ARM(probe, ProcessProbePayload);
CHECK_PAYLOAD_ARM(statAdd, ProcessStatAddPayload);
CHECK_PAYLOAD_ARM(wakeCondition, ProcessWakeConditionPayload);
CHECK_PAYLOAD_ARM(wakeResource, ProcessWakeResourcePayload);
CHECK_PAYLOAD_ARM(wakeEventQueue, ProcessWakeEventQueuePayload);
CHECK_PAYLOAD_ARM(wakeNextDelta, ProcessWakeNextDeltaPayload);
CHECK_PAYLOAD_ARM(scalarWrap, ProcessScalarWrapPayload);
CHECK_PAYLOAD_ARM(scalarUnwrap, ProcessScalarUnwrapPayload);
CHECK_GETTER(ProcessValueTypeMemberPlan, kind, ProcessValueTypeMemberKind);
CHECK_STRING(ProcessValueTypeMemberPlan, name);
CHECK_GETTER(ProcessValueTypeMemberPlan, index, std::optional<uint32_t>);
CHECK_U64(ProcessValueTypeMemberPlan, offsetBits);
CHECK_U64(ProcessValueTypeMemberPlan, widthBits);
CHECK_GETTER(ProcessValueTypeMemberPlan, signedness,
             std::optional<ProcessStorageSignedness>);
CHECK_STRING(ProcessValueTypeMemberPlan, encoding);
CHECK_STRING(ProcessValueTypeMemberPlan, typeKey);
CHECK_GETTER(ProcessStorageValuePayload, members,
             llvm::ArrayRef<ProcessValueTypeMemberPlan>);
CHECK_U64(ProcessStorageValuePayload, widthBits);
CHECK_STRING(ProcessStorageValuePayload, encoding);
CHECK_GETTER(ProcessStoragePacketPayload, members,
             llvm::ArrayRef<ProcessValueTypeMemberPlan>);
CHECK_U64(ProcessStoragePacketPayload, widthBits);
CHECK_U64(ProcessStoragePacketPayload, bytes);
CHECK_STRING(ProcessStoragePacketPayload, encoding);
CHECK_GETTER(ProcessValueTypePayload, kind, ProcessValueTypeKind);
CHECK_GETTER(ProcessValueTypePayload, value,
             const ProcessStorageValuePayload &);
CHECK_GETTER(ProcessValueTypePayload, packet,
             const ProcessStoragePacketPayload &);
CHECK_GETTER(ProcessGeneratedCalleePlan, id, ProcessCalleeId);
CHECK_STRING(ProcessGeneratedCalleePlan, symbol);
CHECK_STRING(ProcessGeneratedCalleePlan, cpp);
CHECK_STRING(ProcessGeneratedCalleePlan, kind);
CHECK_STRING(ProcessGeneratedCalleePlan, fingerprint);
CHECK_GETTER(ProcessGeneratedCalleePlan, effect, ProcessEffectKind);
CHECK_GETTER(ProcessGeneratedCalleePlan, inputTypeKeys,
             llvm::ArrayRef<llvm::StringRef>);
CHECK_GETTER(ProcessGeneratedCalleePlan, resultTypeKeys,
             llvm::ArrayRef<llvm::StringRef>);
CHECK_GETTER(ProcessGeneratedCalleePlan, role, ProcessHelperRole);
CHECK_GETTER(ProcessGeneratedCalleePlan, payload,
             const ProcessGeneratedCalleePayload &);
CHECK_GETTER(ProcessGeneratedCalleePlan, sourceOperations,
             llvm::ArrayRef<mlir::Operation *>);
CHECK_GETTER(ProcessGeneratedCalleePlan, declarations,
             llvm::ArrayRef<mlir::Operation *>);
CHECK_GETTER(ProcessGeneratedCalleePlan, sourcePaths,
             llvm::ArrayRef<llvm::StringRef>);
CHECK_GETTER(ProcessValueTypePlan, id, ProcessValueTypeId);
CHECK_STRING(ProcessValueTypePlan, symbol);
CHECK_STRING(ProcessValueTypePlan, cpp);
CHECK_GETTER(ProcessValueTypePlan, kind, ProcessValueTypeKind);
CHECK_STRING(ProcessValueTypePlan, fingerprint);
CHECK_GETTER(ProcessValueTypePlan, acirType, mlir::Type);
CHECK_GETTER(ProcessValueTypePlan, payload, const ProcessValueTypePayload &);
CHECK_GETTER(ProcessStatePlanSet, processes, llvm::ArrayRef<ProcessStatePlan>);
CHECK_GETTER(ProcessStatePlanSet, callees,
             llvm::ArrayRef<ProcessGeneratedCalleePlan>);
CHECK_GETTER(ProcessStatePlanSet, valueTypes,
             llvm::ArrayRef<ProcessValueTypePlan>);
static_assert(
    std::same_as<decltype(&ProcessStatePlanSet::lookupByDefinitionKey),
                 const ProcessStatePlan *(
                     ProcessStatePlanSet::*)(llvm::StringRef) const>);

#undef CHECK_PAYLOAD_ARM
#undef CHECK_SCALAR_PAYLOAD
#undef CHECK_WAKE_PAYLOAD
#undef CHECK_PACKET
#undef CHECK_THREE_STRINGS
#undef CHECK_TWO_STRINGS
#undef CHECK_PAYLOAD_STRING
#undef CHECK_U64
#undef CHECK_U32
#undef CHECK_STRING
#undef CHECK_GETTER
static_assert(!HasOrdinal<ProcessGeneratedCalleePlan>);
static_assert(!HasSetter<ProcessStatePlan>);
static_assert(!HasMutableProcesses<ProcessStatePlanSet>);
static_assert(!HasComponentLookup<ProcessStatePlanSet>);
static_assert(!HasHierarchyLookup<ProcessStatePlanSet>);
static_assert(!HasFallbackLookup<ProcessStatePlanSet>);
static_assert(!std::default_initializable<ProcessCalleeId>);
static_assert(!std::constructible_from<ProcessCalleeId, uint32_t>);
#define CHECK_ID_VALUE(Type)                                                   \
  static_assert(                                                               \
      std::same_as<decltype(&Type::value), uint32_t (Type::*)() const>)
CHECK_ID_VALUE(ProcessCalleeId);
CHECK_ID_VALUE(ProcessValueTypeId);
CHECK_ID_VALUE(ProcessCaptureId);
CHECK_ID_VALUE(ProcessPcId);
CHECK_ID_VALUE(ProcessBlockId);
CHECK_ID_VALUE(ProcessLiveSlotId);
CHECK_ID_VALUE(ProcessWakeId);
CHECK_ID_VALUE(ProcessTransitionId);
#undef CHECK_ID_VALUE

constexpr llvm::StringLiteral kEmptyBytes =
    R"json({"callees":[],"contract_epoch":"0.5","processes":[],"schema":"acir-process-state-plan-0.1","value_types":[]})json";
constexpr llvm::StringLiteral kSpecialization =
    R"json({"contract_epoch":"0.5","effect":"stateful","inputs":[],"kind":"implementation","payload":{"wake_kind":"next_delta","wake_type":"@acir_wake_next_delta"},"results":["@acir_wake_next_delta"],"role":"wake_next_delta","schema":"acir-generated-implementation-0.1","source_paths":[]})json";
constexpr llvm::StringLiteral kDescriptor =
    R"json({"cpp":"acir::generated::impl_wake_next_delta_a311590813b87bcc30389b814d751def9dfefb49dcd07a9c485b90701a83617e","effect":"stateful","fingerprint":"sha256:a311590813b87bcc30389b814d751def9dfefb49dcd07a9c485b90701a83617e","inputs":[],"kind":"implementation","ordinal":0,"payload":{"wake_kind":"next_delta","wake_type":"@acir_wake_next_delta"},"results":["@acir_wake_next_delta"],"role":"wake_next_delta","source_paths":[],"symbol":"@acir_impl_wake_next_delta_a311590813b87bcc30389b814d751def9dfefb49dcd07a9c485b90701a83617e"})json";

TEST(ProcessStatePlanApiTest, EmptyFrozenModelHasLiteralCanonicalBytes) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = test::parseEmptyModel(context);
  ASSERT_TRUE(module);
  auto built = detail::PlanSetBuilder::buildEmpty(*module);
  ASSERT_TRUE(mlir::succeeded(built));
  ProcessStatePlanSet plans = std::move(*built);
  ASSERT_TRUE(mlir::succeeded(verifyProcessStatePlan(plans)));
  auto bytes = serializeProcessStatePlan(plans);
  ASSERT_TRUE(static_cast<bool>(bytes)) << test::takeError(bytes.takeError());
  EXPECT_EQ(*bytes, kEmptyBytes);
}

TEST(ProcessStatePlanBasicTest, YieldOnlyBaselineIsExactAndImmutable) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = test::parseAndFreezeYieldOnly(context);
  ASSERT_TRUE(module);
  auto built = detail::PlanSetBuilder::buildYieldOnly(*module);
  ASSERT_TRUE(mlir::succeeded(built));
  ProcessStatePlanSet plans = std::move(*built);
  ASSERT_TRUE(mlir::succeeded(verifyProcessStatePlan(plans)));
  ASSERT_EQ(plans.processes().size(), 1U);
  ASSERT_EQ(plans.callees().size(), 1U);
  EXPECT_TRUE(plans.valueTypes().empty());

  const ProcessStatePlan *plan = plans.lookupByDefinitionKey("@Top::@workload");
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(plans.lookupByDefinitionKey("@workload"), nullptr);
  EXPECT_EQ(plans.lookupByDefinitionKey("workload"), nullptr);
  EXPECT_EQ(plans.lookupByDefinitionKey("Top.workload"), nullptr);
  EXPECT_EQ(plan->entryPc().value(), 0U);
  EXPECT_EQ(plan->pcBitWidth(), 1U);
  EXPECT_EQ(plan->pcs().size(), 1U);
  EXPECT_EQ(plan->pcs()[0].name(), "entry");
  EXPECT_EQ(plan->pcs()[0].id().value(), 0U);
  EXPECT_TRUE(plan->liveSlots().empty());
  ASSERT_EQ(plan->wakes().size(), 1U);
  EXPECT_EQ(plan->wakes()[0].kind(), ProcessWakeKind::NextDelta);
  EXPECT_EQ(plan->wakes()[0].typeKey(), "@acir_wake_next_delta");
  EXPECT_EQ(plan->wakes()[0].callee().value(), 0U);
  ASSERT_EQ(plan->transitions().size(), 1U);
  EXPECT_EQ(plan->transitions()[0].targetPc().value(), 0U);

  const auto &callee = plans.callees()[0];
  EXPECT_EQ(callee.id().value(), 0U);
  EXPECT_EQ(callee.kind(), "implementation");
  EXPECT_EQ(callee.effect(), ProcessEffectKind::Stateful);
  EXPECT_EQ(callee.role(), ProcessHelperRole::WakeNextDelta);
  EXPECT_EQ(callee.payload().wakeNextDelta().wakeKind(),
            ProcessWakeKind::NextDelta);
  EXPECT_EQ(callee.payload().wakeNextDelta().wakeType(),
            "@acir_wake_next_delta");
  EXPECT_EQ(detail::generatedCalleeSpecializationBytes(callee),
            kSpecialization);
  EXPECT_EQ(detail::generatedCalleeDescriptorBytes(callee), kDescriptor);
}

TEST(ProcessStatePlanBasicTest, FrozenDeclarationPermutationsAreByteIdentical) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto first = test::parseAndFreezeYieldPermutation(context, false);
  auto second = test::parseAndFreezeYieldPermutation(context, true);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  auto firstBuilt = detail::PlanSetBuilder::buildYieldOnly(*first);
  auto secondBuilt = detail::PlanSetBuilder::buildYieldOnly(*second);
  ASSERT_TRUE(mlir::succeeded(firstBuilt));
  ASSERT_TRUE(mlir::succeeded(secondBuilt));
  auto firstPlan = std::move(*firstBuilt);
  auto secondPlan = std::move(*secondBuilt);
  auto firstBytes = serializeProcessStatePlan(firstPlan);
  auto secondBytes = serializeProcessStatePlan(secondPlan);
  ASSERT_TRUE(static_cast<bool>(firstBytes));
  ASSERT_TRUE(static_cast<bool>(secondBytes));
  EXPECT_EQ(*firstBytes, *secondBytes);
  ASSERT_EQ(firstPlan.processes().size(), 2U);
  ASSERT_EQ(secondPlan.processes().size(), 2U);
  EXPECT_EQ(firstPlan.processes()[0].definitionKey(), "@Top::@alpha");
  EXPECT_EQ(firstPlan.processes()[1].definitionKey(), "@Top::@workload");
  for (auto [firstProcess, secondProcess] :
       llvm::zip_equal(firstPlan.processes(), secondPlan.processes())) {
    EXPECT_EQ(firstProcess.definitionKey(), secondProcess.definitionKey());
    EXPECT_EQ(firstProcess.entryPc(), secondProcess.entryPc());
    EXPECT_EQ(firstProcess.pcBitWidth(), secondProcess.pcBitWidth());
    EXPECT_EQ(firstProcess.fairnessWork(), secondProcess.fairnessWork());
    EXPECT_EQ(firstProcess.pcs().size(), secondProcess.pcs().size());
    EXPECT_EQ(firstProcess.blocks().size(), secondProcess.blocks().size());
    EXPECT_EQ(firstProcess.wakes().size(), secondProcess.wakes().size());
    EXPECT_EQ(firstProcess.transitions().size(),
              secondProcess.transitions().size());
  }
  EXPECT_EQ(detail::generatedCalleeDescriptorBytes(firstPlan.callees()[0]),
            detail::generatedCalleeDescriptorBytes(secondPlan.callees()[0]));
}

TEST(ProcessStatePlanBasicTest, EveryFrozenSemanticCorruptionIsRejected) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = test::parseAndFreezeYieldOnly(context);
  ASSERT_TRUE(module);
  auto built = detail::PlanSetBuilder::buildYieldOnly(*module);
  ASSERT_TRUE(mlir::succeeded(built));
  auto plan = std::move(*built);
  auto permutationModule = test::parseAndFreezeYieldPermutation(context, false);
  ASSERT_TRUE(permutationModule);
  auto permutationBuilt =
      detail::PlanSetBuilder::buildYieldOnly(*permutationModule);
  ASSERT_TRUE(mlir::succeeded(permutationBuilt));
  auto permutationPlan = std::move(*permutationBuilt);
  mlir::ScopedDiagnosticHandler handler(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });
  const ProcessStatePlanCorruptionForTest corruptions[] = {
      ProcessStatePlanCorruptionForTest::DuplicateOrdinal,
      ProcessStatePlanCorruptionForTest::NonDenseOrdinal,
      ProcessStatePlanCorruptionForTest::DanglingReference,
      ProcessStatePlanCorruptionForTest::DuplicateIdentity,
      ProcessStatePlanCorruptionForTest::UnsortedCanonicalOrder,
      ProcessStatePlanCorruptionForTest::CostMismatch,
      ProcessStatePlanCorruptionForTest::DefinitionKeyMismatch,
      ProcessStatePlanCorruptionForTest::CalleeSpecializationMismatch,
      ProcessStatePlanCorruptionForTest::ValueTypeSpecializationMismatch,
      ProcessStatePlanCorruptionForTest::EffectMismatch,
      ProcessStatePlanCorruptionForTest::IdKindMismatch,
      ProcessStatePlanCorruptionForTest::WrongTypeKey,
      ProcessStatePlanCorruptionForTest::InvalidFramePhase,
      ProcessStatePlanCorruptionForTest::InvalidEdgeBinding,
      ProcessStatePlanCorruptionForTest::InvalidWakeCallee,
  };
  constexpr llvm::StringLiteral diagnostics[] = {
      "process-state plan invariant violated: duplicate ordinal",
      "process-state plan invariant violated: non-dense ordinal",
      "process-state plan invariant violated: dangling reference",
      "process-state plan invariant violated: duplicate identity",
      "process-state plan invariant violated: unsorted canonical order",
      "process-state plan invariant violated: cost mismatch",
      "process-state plan invariant violated: definition key mismatch",
      "process-state plan invariant violated: callee specialization mismatch",
      // NOLINTNEXTLINE(bugprone-suspicious-missing-comma) intentional split
      "process-state plan invariant violated: value-type specialization "
      "mismatch",
      "process-state plan invariant violated: effect mismatch",
      "process-state plan invariant violated: ID kind mismatch",
      "process-state plan invariant violated: wrong type key",
      "process-state plan invariant violated: invalid frame phase",
      "process-state plan invariant violated: invalid edge binding",
      "process-state plan invariant violated: invalid wake callee",
  };
  for (auto [index, corruption] : llvm::enumerate(corruptions)) {
    SCOPED_TRACE(static_cast<int>(corruption));
    const ProcessStatePlanSet &source =
        corruption == ProcessStatePlanCorruptionForTest::UnsortedCanonicalOrder
            ? permutationPlan
            : plan;
    auto corrupted =
        cloneProcessStatePlanWithCorruptionForTest(source, corruption);
    EXPECT_TRUE(mlir::failed(verifyProcessStatePlan(corrupted)));
    EXPECT_EQ(diagnostics[index],
              detail::lastProcessStatePlanDiagnosticForTest());
  }
}

TEST(ProcessStatePlanBasicTest,
     MalformedPrivateShapesReturnDiagnosticsWithoutDereferencingInvalidArms) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = test::parseAndFreezeYieldOnly(context);
  ASSERT_TRUE(module);
  auto built = detail::PlanSetBuilder::buildYieldOnly(*module);
  ASSERT_TRUE(mlir::succeeded(built));
  auto plan = std::move(*built);
  mlir::ScopedDiagnosticHandler handler(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });

  struct Case {
    ProcessStatePlanSet (*build)(const ProcessStatePlanSet &);
    llvm::StringLiteral diagnostic;
  };
  const Case cases[] = {
      {detail::PlanSetBuilder::cloneWithMissingWakeCallee,
       "process-state plan invariant violated: invalid wake callee"},
      {detail::PlanSetBuilder::cloneWithDanglingSuspendTransition,
       "process-state plan invariant violated: dangling reference"},
      {detail::PlanSetBuilder::cloneWithUnpairedLiveSlotCallee,
       "process-state plan invariant violated: invalid live-slot wrapper pair"},
      {detail::PlanSetBuilder::cloneWithMissingValueTypePayload,
       "process-state plan invariant violated: value-type specialization "
       "mismatch"},
      {detail::PlanSetBuilder::cloneWithNullEdgeStorage,
       "process-state plan invariant violated: invalid edge binding"},
      {detail::PlanSetBuilder::cloneWithInactiveEdgeField,
       "process-state plan invariant violated: invalid edge binding"},
      {detail::PlanSetBuilder::cloneWithDoubleValueTypePayload,
       "process-state plan invariant violated: value-type specialization "
       "mismatch"},
      {detail::PlanSetBuilder::cloneWithMissingOriginalActionSource,
       "process-state plan invariant violated: invalid action arm"},
      {detail::PlanSetBuilder::cloneWithUnexpectedConstantActionSource,
       "process-state plan invariant violated: invalid action arm"},
  };
  for (auto [index, testCase] : llvm::enumerate(cases)) {
    SCOPED_TRACE(index);
    ProcessStatePlanSet corrupted = testCase.build(plan);
    EXPECT_TRUE(mlir::failed(verifyProcessStatePlan(corrupted)));
    EXPECT_EQ(testCase.diagnostic.str(),
              detail::lastProcessStatePlanDiagnosticForTest().str());
    auto serialized = serializeProcessStatePlan(corrupted);
    EXPECT_FALSE(static_cast<bool>(serialized));
    llvm::consumeError(serialized.takeError());
  }
}

TEST(ProcessStatePlanBasicTest,
     FrozenLoopActionFixtureBuildsWithoutMutatingItsInputModule) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = test::parseAndFreezeLoopActions(context);
  ASSERT_TRUE(module);
  const std::string textBefore = test::moduleText(*module);
  const std::string bytecodeBefore = test::moduleBytecode(*module);
  const std::string freezeSealBefore = test::moduleFreezeSeal(*module);
  ASSERT_FALSE(bytecodeBefore.empty());
  ASSERT_FALSE(freezeSealBefore.empty());

  auto built = detail::PlanSetBuilder::buildLoopActionFixture(*module);
  ASSERT_TRUE(mlir::succeeded(built));
  ASSERT_TRUE(mlir::succeeded(verifyProcessStatePlan(*built)));
  auto serialized = serializeProcessStatePlan(*built);
  ASSERT_TRUE(static_cast<bool>(serialized));
  ASSERT_EQ(built->processes().size(), 1U);
  const auto &actions = built->processes().front().blocks().front().actions();
  ASSERT_EQ(actions.size(), 3U);
  EXPECT_EQ(actions[0].kind(), ProcessActionKind::ForInitialize);
  EXPECT_EQ(actions[1].kind(), ProcessActionKind::ForCondition);
  EXPECT_EQ(actions[2].kind(), ProcessActionKind::ForIncrement);
  EXPECT_EQ(actions[0].sourceOperation(), actions[1].sourceOperation());
  EXPECT_EQ(actions[1].sourceOperation(), actions[2].sourceOperation());
  for (const ProcessActionPlan &action : actions) {
    const ProcessOccurrenceId &anchor =
        action.occurrence().syntheticLoop().anchor();
    ASSERT_EQ(anchor.kind(), ProcessOccurrenceKind::Original);
    EXPECT_EQ(anchor.original().operation(), action.sourceOperation());
    EXPECT_EQ(anchor.original().operationPath(), "@Top::@workload/r0/b0/o3");
    EXPECT_TRUE(anchor.original().iterationVector().empty());
  }

  EXPECT_EQ(test::moduleText(*module), textBefore);
  EXPECT_EQ(test::moduleBytecode(*module), bytecodeBefore);
  EXPECT_EQ(test::moduleFreezeSeal(*module), freezeSealBefore);
}

TEST(ProcessStatePlanBasicTest,
     IsolatedLoopActionCorruptionsRejectWithoutMutatingFrozenInput) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = test::parseAndFreezeLoopActions(context);
  ASSERT_TRUE(module);
  const std::string textBefore = test::moduleText(*module);
  const std::string bytecodeBefore = test::moduleBytecode(*module);
  const std::string freezeSealBefore = test::moduleFreezeSeal(*module);
  ASSERT_FALSE(bytecodeBefore.empty());
  ASSERT_FALSE(freezeSealBefore.empty());

  auto built = detail::PlanSetBuilder::buildLoopActionFixture(*module);
  ASSERT_TRUE(mlir::succeeded(built));
  const ProcessStatePlanSet plan = std::move(*built);
  ASSERT_TRUE(mlir::succeeded(verifyProcessStatePlan(plan)));

  struct Case {
    ProcessStatePlanSet (*corrupt)(const ProcessStatePlanSet &);
    const char *name;
    bool rejectedByStructuralPreflight;
  };
  const Case cases[] = {
      {detail::PlanSetBuilder::cloneWithForInitializeWrongOwningLoopSource,
       "for_initialize wrong owning-loop source", true},
      {detail::PlanSetBuilder::cloneWithForConditionWrongOwningLoopSource,
       "for_condition wrong owning-loop source", true},
      {detail::PlanSetBuilder::cloneWithForIncrementWrongOwningLoopSource,
       "for_increment wrong owning-loop source", true},
      {detail::PlanSetBuilder::cloneWithNonLoopForActionSource,
       "for_condition non-loop source", true},
      {detail::PlanSetBuilder::cloneWithForConditionWrongResultType,
       "for_condition wrong result type", false},
      {detail::PlanSetBuilder::cloneWithForIncrementWrongResultType,
       "for_increment wrong result type", false},
      {detail::PlanSetBuilder::cloneWithForConditionInactiveCallee,
       "for_condition inactive callee", true},
      {detail::PlanSetBuilder::cloneWithForInitializeInactiveScalar,
       "for_initialize inactive scalar", true},
      {detail::PlanSetBuilder::cloneWithForConditionWrongEmission,
       "for_condition wrong emission", true},
      {detail::PlanSetBuilder::cloneWithForConditionWrongScalarOp,
       "for_condition wrong scalar operation", false},
      {detail::PlanSetBuilder::cloneWithForIncrementWrongEmission,
       "for_increment wrong emission", true},
      {detail::PlanSetBuilder::cloneWithForIncrementWrongScalarOp,
       "for_increment wrong scalar operation", false},
  };
  mlir::ScopedDiagnosticHandler handler(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });
  auto expectRejected = [&](ProcessStatePlanSet corrupted, const char *name,
                            bool rejectedByStructuralPreflight) {
    SCOPED_TRACE(name);
    EXPECT_EQ(detail::PlanSetBuilder::structuralError(corrupted),
              rejectedByStructuralPreflight
                  ? "process-state plan invariant violated: invalid action arm"
                  : "");
    EXPECT_TRUE(mlir::failed(verifyProcessStatePlan(corrupted)));
    EXPECT_EQ(detail::lastProcessStatePlanDiagnosticForTest(),
              "process-state plan invariant violated: invalid action arm");
    auto serialized = serializeProcessStatePlan(corrupted);
    if (serialized) {
      ADD_FAILURE() << "serializer accepted isolated corruption";
      return;
    }
    EXPECT_EQ(test::takeError(serialized.takeError()),
              "process-state plan verification failed");
  };
  for (const Case &testCase : cases) {
    expectRejected(testCase.corrupt(plan), testCase.name,
                   testCase.rejectedByStructuralPreflight);
  }

  using Mutation = detail::PlanSetBuilder::LoopActionMutationForTest;
  struct RelationshipCase {
    uint32_t actionIndex;
    Mutation mutation;
    const char *name;
    bool rejectedByStructuralPreflight;
  };
  const RelationshipCase relationshipCases[] = {
      {0, Mutation::InactiveCallee, "for_initialize inactive callee", true},
      {2, Mutation::InactiveCallee, "for_increment inactive callee", true},
      {1, Mutation::MissingScalar, "for_condition missing scalar operation",
       true},
      {1, Mutation::WrongScalarProperties,
       "for_condition wrong scalar properties", false},
      {1, Mutation::WrongScalarAttributeCount,
       "for_condition wrong scalar attribute count", false},
      {1, Mutation::WrongScalarAttributeName,
       "for_condition wrong predicate name", false},
      {1, Mutation::WrongScalarAttributeValue,
       "for_condition wrong predicate value", false},
      {1, Mutation::WrongOperandCount, "for_condition wrong operand count",
       false},
      {1, Mutation::WrongOperandType, "for_condition wrong operand type",
       false},
      {1, Mutation::WrongResultCount, "for_condition wrong result count",
       false},
      {2, Mutation::MissingScalar, "for_increment missing scalar operation",
       true},
      {2, Mutation::WrongScalarProperties,
       "for_increment wrong scalar properties", false},
      {2, Mutation::WrongScalarAttributeCount,
       "for_increment forbidden scalar attribute", false},
      {2, Mutation::WrongOperandCount, "for_increment wrong operand count",
       false},
      {2, Mutation::WrongOperandType, "for_increment wrong operand type",
       false},
      {2, Mutation::WrongResultCount, "for_increment wrong result count",
       false},
  };
  for (const RelationshipCase &testCase : relationshipCases) {
    expectRejected(detail::PlanSetBuilder::cloneLoopActionWithMutationForTest(
                       plan, testCase.actionIndex, testCase.mutation),
                   testCase.name, testCase.rejectedByStructuralPreflight);
  }

  EXPECT_EQ(test::moduleText(*module), textBefore);
  EXPECT_EQ(test::moduleBytecode(*module), bytecodeBefore);
  EXPECT_EQ(test::moduleFreezeSeal(*module), freezeSealBefore);
}

TEST(ProcessStatePlanBasicTest,
     PrivateFixtureConstructionRejectsUnexpectedProcessStructure) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto empty = test::parseEmptyModel(context);
  auto yieldOnly = test::parseAndFreezeYieldOnly(context);
  ASSERT_TRUE(empty);
  ASSERT_TRUE(yieldOnly);
  mlir::ScopedDiagnosticHandler handler(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });
  EXPECT_TRUE(mlir::failed(detail::PlanSetBuilder::buildYieldOnly(*empty)));
  EXPECT_TRUE(mlir::failed(detail::PlanSetBuilder::buildEmpty(*yieldOnly)));
}

TEST(ProcessStatePlanBasicTest,
     CompletePrivateFixtureExercisesEveryPublicGetterAndUnionArm) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  EXPECT_TRUE(detail::PlanSetBuilder::exerciseCompleteApiFixture(context));
  EXPECT_TRUE(detail::PlanSetBuilder::exerciseAllActionArmsFixture(context));
}

TEST(ProcessStatePlanBasicTest,
     LongProcessGraphUsesBoundedIterativeFairnessVerification) {
  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = test::parseAndFreezeYieldOnly(context);
  ASSERT_TRUE(module);
  auto baseline = detail::PlanSetBuilder::buildYieldOnly(*module);
  ASSERT_TRUE(mlir::succeeded(baseline));
  auto longChain =
      detail::PlanSetBuilder::cloneWithLongLocalChain(*baseline, 32768);
  EXPECT_TRUE(mlir::succeeded(verifyProcessStatePlan(longChain)));
  EXPECT_EQ(longChain.processes().front().blocks().size(), 32768U);
  EXPECT_EQ(longChain.processes().front().fairnessWork(), 32768U);
  mlir::ScopedDiagnosticHandler handler(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });
  auto cycle = detail::PlanSetBuilder::cloneWithLocalCycle(longChain);
  EXPECT_TRUE(mlir::failed(verifyProcessStatePlan(cycle)));
  EXPECT_EQ(detail::lastProcessStatePlanDiagnosticForTest(),
            "process-state plan invariant violated: cost mismatch");
  auto unreachable =
      detail::PlanSetBuilder::cloneWithUnreachableBlock(*baseline);
  EXPECT_TRUE(mlir::failed(verifyProcessStatePlan(unreachable)));
  EXPECT_EQ(detail::lastProcessStatePlanDiagnosticForTest(),
            "process-state plan invariant violated: cost mismatch");
}

} // namespace
} // namespace acir
