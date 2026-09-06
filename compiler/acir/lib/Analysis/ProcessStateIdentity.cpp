#include "ProcessStatePlanInternal.h"

#include <cassert>

namespace acir {

#define FIELD_GET(Class, Type, Method, Field)                                  \
  Type Class::Method() const { return impl_->Field; }
#define REF_GET(Class, Type, Method, Field)                                    \
  const Type &Class::Method() const {                                          \
    assert(impl_->Field);                                                      \
    return *impl_->Field;                                                      \
  }
#define ID_GET(Class, Type, Method, Field)                                     \
  Type Class::Method() const {                                                 \
    assert(impl_->Field);                                                      \
    return *impl_->Field;                                                      \
  }
#define ARRAY_GET(Class, Type, Method, Field)                                  \
  llvm::ArrayRef<Type> Class::Method() const { return impl_->Field; }
#define STR_GET(Class, Method, Field)                                          \
  llvm::StringRef Class::Method() const { return impl_->Field; }

FIELD_GET(ProcessCallSitePlan, mlir::Operation *, operation, operation)
STR_GET(ProcessCallSitePlan, operationPath, operationPath)
ARRAY_GET(ProcessCallSitePlan, uint64_t, iterationVector, iterationVector)
FIELD_GET(ProcessOriginalOccurrence, mlir::Operation *, operation, operation)
STR_GET(ProcessOriginalOccurrence, operationPath, operationPath)
ARRAY_GET(ProcessOriginalOccurrence, ProcessCallSitePlan, callSites, callSites)
ARRAY_GET(ProcessOriginalOccurrence, uint64_t, iterationVector, iterationVector)
REF_GET(ProcessSyntheticLoopOccurrence, ProcessOccurrenceId, anchor, anchor)
FIELD_GET(ProcessSyntheticLoopOccurrence, ProcessLoopPhase, phase, phase)
REF_GET(ProcessSyntheticWrapperOccurrence, ProcessOccurrenceId, anchor, anchor)
ID_GET(ProcessSyntheticWrapperOccurrence, ProcessTransitionId, transition,
       transition)
ID_GET(ProcessSyntheticWrapperOccurrence, ProcessLiveSlotId, slot, slot)
FIELD_GET(ProcessSyntheticWrapperOccurrence, ProcessWrapperDirection, direction,
          direction)
REF_GET(ProcessSyntheticConstantOccurrence, ProcessOccurrenceId, anchor, anchor)
FIELD_GET(ProcessSyntheticConstantOccurrence, uint32_t, constant, constant)
FIELD_GET(ProcessOccurrenceId, ProcessOccurrenceKind, kind, kind)
REF_GET(ProcessOccurrenceId, ProcessOriginalOccurrence, original, original)
REF_GET(ProcessOccurrenceId, ProcessSyntheticLoopOccurrence, syntheticLoop,
        syntheticLoop)
REF_GET(ProcessOccurrenceId, ProcessSyntheticWrapperOccurrence,
        syntheticWrapper, syntheticWrapper)
REF_GET(ProcessOccurrenceId, ProcessSyntheticConstantOccurrence,
        syntheticConstant, syntheticConstant)
FIELD_GET(ProcessValueCoordinate, ProcessValueCoordinateKind, kind, kind)
STR_GET(ProcessValueCoordinate, ownerPath, ownerPath)
FIELD_GET(ProcessValueCoordinate, uint32_t, index, index)
FIELD_GET(ProcessOriginalPlannedValue, mlir::Value, value, value)
REF_GET(ProcessOriginalPlannedValue, ProcessOccurrenceId, occurrence,
        occurrence)
REF_GET(ProcessOriginalPlannedValue, ProcessValueCoordinate, coordinate,
        coordinate)
STR_GET(ProcessOriginalPlannedValue, path, path)
ID_GET(ProcessCapturePlannedValue, ProcessCaptureId, capture, capture)
ID_GET(ProcessLiveSlotPlannedValue, ProcessLiveSlotId, slot, slot)
REF_GET(ProcessSyntheticPlannedValue, ProcessOccurrenceId, occurrence,
        occurrence)
REF_GET(ProcessSyntheticPlannedValue, ProcessValueCoordinate, coordinate,
        coordinate)
STR_GET(ProcessConstantPlannedValue, value, value)
FIELD_GET(ProcessPlannedValue, ProcessPlannedValueKind, kind, kind)
FIELD_GET(ProcessPlannedValue, mlir::Type, type, type)
REF_GET(ProcessPlannedValue, ProcessOriginalPlannedValue, original, original)
REF_GET(ProcessPlannedValue, ProcessCapturePlannedValue, capture, capture)
REF_GET(ProcessPlannedValue, ProcessLiveSlotPlannedValue, liveSlot, liveSlot)
REF_GET(ProcessPlannedValue, ProcessSyntheticPlannedValue, synthetic, synthetic)
REF_GET(ProcessPlannedValue, ProcessConstantPlannedValue, constant, constant)
STR_GET(ProcessScalarAttribute, name, name)
STR_GET(ProcessScalarAttribute, value, value)
STR_GET(ProcessScalarOperationPlan, name, name)
ARRAY_GET(ProcessScalarOperationPlan, ProcessScalarAttribute, attributes,
          attributes)
STR_GET(ProcessScalarOperationPlan, properties, properties)
ID_GET(ProcessCapturePlan, ProcessCaptureId, id, id)
STR_GET(ProcessCapturePlan, name, name)
FIELD_GET(ProcessCapturePlan, mlir::Value, operand, operand)
FIELD_GET(ProcessCapturePlan, mlir::Value, entryArgument, entryArgument)
FIELD_GET(ProcessCapturePlan, mlir::Type, type, type)
STR_GET(ProcessCapturePlan, operandPath, operandPath)
STR_GET(ProcessCapturePlan, argumentPath, argumentPath)
FIELD_GET(ProcessActionPlan, uint32_t, id, id)
FIELD_GET(ProcessActionPlan, ProcessActionKind, kind, kind)
FIELD_GET(ProcessActionPlan, ProcessEmissionClass, emission, emission)
REF_GET(ProcessActionPlan, ProcessOccurrenceId, occurrence, occurrence)
FIELD_GET(ProcessActionPlan, mlir::Operation *, sourceOperation,
          sourceOperation)
ARRAY_GET(ProcessActionPlan, uint64_t, iterationVector, iterationVector)
ARRAY_GET(ProcessActionPlan, ProcessPlannedValue, operands, operands)
ARRAY_GET(ProcessActionPlan, ProcessPlannedValue, results, results)
FIELD_GET(ProcessActionPlan, uint32_t, cost, cost)
ARRAY_GET(ProcessActionPlan, mlir::Type, resultTypes, resultTypes)
FIELD_GET(ProcessActionPlan, std::optional<ProcessCalleeId>, callee, callee)
const ProcessScalarOperationPlan *ProcessActionPlan::scalarOp() const {
  return impl_->scalarOp ? &*impl_->scalarOp : nullptr;
}
ID_GET(ProcessLiveSlotPlan, ProcessLiveSlotId, id, id)
STR_GET(ProcessLiveSlotPlan, name, name)
FIELD_GET(ProcessLiveSlotPlan, mlir::Type, type, type)
ID_GET(ProcessLiveSlotPlan, ProcessValueTypeId, storageType, storageType)
ARRAY_GET(ProcessLiveSlotPlan, ProcessPlannedValue, memberValues, memberValues)
FIELD_GET(ProcessLiveSlotPlan, std::optional<ProcessCalleeId>, wrapCallee,
          wrapCallee)
FIELD_GET(ProcessLiveSlotPlan, std::optional<ProcessCalleeId>, unwrapCallee,
          unwrapCallee)
FIELD_GET(ProcessSubscriptionSourcePlan, ProcessSubscriptionSourceKind, kind,
          kind)
FIELD_GET(ProcessSubscriptionSourcePlan, mlir::Value, value, value)
FIELD_GET(ProcessSubscriptionSourcePlan, mlir::Operation *, owner, owner)
FIELD_GET(ProcessSubscriptionSourcePlan, mlir::Operation *, declaration,
          declaration)
FIELD_GET(ProcessSubscriptionSourcePlan, std::optional<ProcessCaptureId>,
          capture, capture)
STR_GET(ProcessSubscriptionSourcePlan, symbol, symbol)
STR_GET(ProcessSubscriptionSourcePlan, path, path)
STR_GET(ProcessSubscriptionSourcePlan, ownerPath, ownerPath)
ID_GET(ProcessWakePlan, ProcessWakeId, id, id)
FIELD_GET(ProcessWakePlan, ProcessWakeKind, kind, kind)
FIELD_GET(ProcessWakePlan, mlir::Operation *, operation, operation)
FIELD_GET(ProcessWakePlan, mlir::Value, triggeringValue, triggeringValue)
FIELD_GET(ProcessWakePlan, mlir::Operation *, declaration, declaration)
ID_GET(ProcessWakePlan, ProcessCalleeId, callee, callee)
STR_GET(ProcessWakePlan, typeKey, typeKey)
STR_GET(ProcessWakePlan, operationPath, operationPath)
STR_GET(ProcessWakePlan, target, target)
REF_GET(ProcessWakePlan, ProcessOccurrenceId, occurrence, occurrence)
ARRAY_GET(ProcessWakePlan, uint64_t, iterationVector, iterationVector)
ARRAY_GET(ProcessWakePlan, ProcessSubscriptionSourcePlan, sources, sources)
ID_GET(ProcessTransitionStorePlan, ProcessLiveSlotId, slot, slot)
REF_GET(ProcessTransitionStorePlan, ProcessPlannedValue, source, source)
FIELD_GET(ProcessTransitionStorePlan, mlir::Value, sourceValue, sourceValue)
ID_GET(ProcessTransitionLoadPlan, ProcessLiveSlotId, slot, slot)
ARRAY_GET(ProcessTransitionLoadPlan, ProcessPlannedValue, replacements,
          replacements)
ID_GET(ProcessTransitionPlan, ProcessTransitionId, id, id)
ID_GET(ProcessTransitionPlan, ProcessPcId, sourcePc, sourcePc)
ID_GET(ProcessTransitionPlan, ProcessPcId, targetPc, targetPc)
ID_GET(ProcessTransitionPlan, ProcessWakeId, wake, wake)
ARRAY_GET(ProcessTransitionPlan, uint64_t, iterationVector, iterationVector)
ARRAY_GET(ProcessTransitionPlan, ProcessTransitionStorePlan, stores, stores)
ARRAY_GET(ProcessTransitionPlan, ProcessTransitionLoadPlan, loads, loads)
REF_GET(ProcessForwardingBindingPlan, ProcessPlannedValue, from, from)
REF_GET(ProcessForwardingBindingPlan, ProcessPlannedValue, to, to)
FIELD_GET(ProcessControlFramePlan, ProcessFrameKind, kind, kind)
FIELD_GET(ProcessControlFramePlan, ProcessFramePhase, phase, phase)
FIELD_GET(ProcessControlFramePlan, mlir::Operation *, operation, operation)
STR_GET(ProcessControlFramePlan, operationPath, operationPath)
ARRAY_GET(ProcessControlFramePlan, ProcessForwardingBindingPlan, bindings,
          bindings)
FIELD_GET(ProcessControlEdgePlan, ProcessControlEdgeKind, kind, kind)
REF_GET(ProcessControlEdgePlan, ProcessPlannedValue, condition, condition)
ID_GET(ProcessControlEdgePlan, ProcessBlockId, trueBlock, trueBlock)
ID_GET(ProcessControlEdgePlan, ProcessBlockId, falseBlock, falseBlock)
ARRAY_GET(ProcessControlEdgePlan, ProcessForwardingBindingPlan, trueBindings,
          trueBindings)
ARRAY_GET(ProcessControlEdgePlan, ProcessForwardingBindingPlan, falseBindings,
          falseBindings)
ID_GET(ProcessControlEdgePlan, ProcessBlockId, targetBlock, targetBlock)
ARRAY_GET(ProcessControlEdgePlan, ProcessForwardingBindingPlan, bindings,
          bindings)
ID_GET(ProcessControlEdgePlan, ProcessTransitionId, transition, transition)
FIELD_GET(ProcessControlEdgePlan, ProcessTerminateStatus, status, status)
ID_GET(ProcessBlockPlan, ProcessBlockId, id, id)
ID_GET(ProcessBlockPlan, ProcessPcId, pc, pc)
FIELD_GET(ProcessBlockPlan, mlir::Region *, originRegion, originRegion)
FIELD_GET(ProcessBlockPlan, mlir::Block *, originBlock, originBlock)
STR_GET(ProcessBlockPlan, path, path)
ARRAY_GET(ProcessBlockPlan, ProcessControlFramePlan, frames, frames)
ARRAY_GET(ProcessBlockPlan, ProcessTransitionLoadPlan, loads, loads)
ARRAY_GET(ProcessBlockPlan, ProcessActionPlan, actions, actions)
REF_GET(ProcessBlockPlan, ProcessControlEdgePlan, edge, edge)
FIELD_GET(ProcessBlockPlan, uint64_t, cost, cost)
ID_GET(ProcessPcPlan, ProcessPcId, id, id)
STR_GET(ProcessPcPlan, name, name)
STR_GET(ProcessPcPlan, entryPath, entryPath)
ARRAY_GET(ProcessPcPlan, ProcessBlockId, blocks, blocks)
STR_GET(ProcessStatePlan, definitionKey, definitionKey)
FIELD_GET(ProcessStatePlan, ac::ProcessOp, process, process)
ARRAY_GET(ProcessStatePlan, ProcessCapturePlan, captures, captures)
ID_GET(ProcessStatePlan, ProcessPcId, entryPc, entryPc)
ARRAY_GET(ProcessStatePlan, ProcessPcPlan, pcs, pcs)
ARRAY_GET(ProcessStatePlan, ProcessBlockPlan, blocks, blocks)
ARRAY_GET(ProcessStatePlan, ProcessLiveSlotPlan, liveSlots, liveSlots)
ARRAY_GET(ProcessStatePlan, ProcessWakePlan, wakes, wakes)
ARRAY_GET(ProcessStatePlan, ProcessTransitionPlan, transitions, transitions)
FIELD_GET(ProcessStatePlan, uint32_t, pcBitWidth, pcBitWidth)
FIELD_GET(ProcessStatePlan, uint64_t, fairnessWork, fairnessWork)
STR_GET(ProcessTraceDecodePayload, entry, entry)
STR_GET(ProcessTraceDecodePayload, result, result)
STR_GET(ProcessTraceDecodePayload, source, source)
STR_GET(ProcessQueueTrySendPayload, element, element)
STR_GET(ProcessQueueTrySendPayload, queue, queue)
STR_GET(ProcessQueueTryRecvPayload, element, element)
STR_GET(ProcessQueueTryRecvPayload, queue, queue)
STR_GET(ProcessEventSchedulePayload, delay, delay)
STR_GET(ProcessEventSchedulePayload, target, target)
STR_GET(ProcessEventSchedulePayload, value, value)
STR_GET(ProcessTraceOpenPayload, source, source)
STR_GET(ProcessTraceNextPayload, entry, entry)
STR_GET(ProcessTraceNextPayload, source, source)
STR_GET(ProcessTraceEofPayload, source, source)
STR_GET(ProcessTracePositionPayload, source, source)
STR_GET(ProcessContractRequirePayload, message, message)
STR_GET(ProcessContractEnsurePayload, message, message)
STR_GET(ProcessContractAssertPayload, message, message)
STR_GET(ProcessProbePayload, kind, kind)
STR_GET(ProcessProbePayload, result, result)
STR_GET(ProcessProbePayload, target, target)
STR_GET(ProcessStatAddPayload, stat, stat)
STR_GET(ProcessStatAddPayload, valueType, valueType)
#define WAKE_GETS(Class)                                                       \
  FIELD_GET(Class, ProcessWakeKind, wakeKind, wakeKind)                        \
  STR_GET(Class, wakeType, wakeType)
WAKE_GETS(ProcessWakeConditionPayload)
WAKE_GETS(ProcessWakeResourcePayload)
WAKE_GETS(ProcessWakeEventQueuePayload)
WAKE_GETS(ProcessWakeNextDeltaPayload)
#undef WAKE_GETS
FIELD_GET(ProcessScalarWrapPayload, ProcessWrapperDirection, direction,
          direction)
STR_GET(ProcessScalarWrapPayload, scalar, scalar)
STR_GET(ProcessScalarWrapPayload, valueType, valueType)
FIELD_GET(ProcessScalarUnwrapPayload, ProcessWrapperDirection, direction,
          direction)
STR_GET(ProcessScalarUnwrapPayload, scalar, scalar)
STR_GET(ProcessScalarUnwrapPayload, valueType, valueType)
FIELD_GET(ProcessGeneratedCalleePayload, ProcessHelperRole, role, role)
#define PAYLOAD_REF(Type, Method, Field)                                       \
  REF_GET(ProcessGeneratedCalleePayload, Type, Method, Field)
PAYLOAD_REF(ProcessTraceDecodePayload, traceDecode, traceDecode)
PAYLOAD_REF(ProcessQueueTrySendPayload, queueTrySend, queueTrySend)
PAYLOAD_REF(ProcessQueueTryRecvPayload, queueTryRecv, queueTryRecv)
PAYLOAD_REF(ProcessEventSchedulePayload, eventSchedule, eventSchedule)
PAYLOAD_REF(ProcessTraceOpenPayload, traceOpen, traceOpen)
PAYLOAD_REF(ProcessTraceNextPayload, traceNext, traceNext)
PAYLOAD_REF(ProcessTraceEofPayload, traceEof, traceEof)
PAYLOAD_REF(ProcessTracePositionPayload, tracePosition, tracePosition)
PAYLOAD_REF(ProcessContractRequirePayload, contractRequire, contractRequire)
PAYLOAD_REF(ProcessContractEnsurePayload, contractEnsure, contractEnsure)
PAYLOAD_REF(ProcessContractAssertPayload, contractAssert, contractAssert)
PAYLOAD_REF(ProcessProbePayload, probe, probe)
PAYLOAD_REF(ProcessStatAddPayload, statAdd, statAdd)
PAYLOAD_REF(ProcessWakeConditionPayload, wakeCondition, wakeCondition)
PAYLOAD_REF(ProcessWakeResourcePayload, wakeResource, wakeResource)
PAYLOAD_REF(ProcessWakeEventQueuePayload, wakeEventQueue, wakeEventQueue)
PAYLOAD_REF(ProcessWakeNextDeltaPayload, wakeNextDelta, wakeNextDelta)
PAYLOAD_REF(ProcessScalarWrapPayload, scalarWrap, scalarWrap)
PAYLOAD_REF(ProcessScalarUnwrapPayload, scalarUnwrap, scalarUnwrap)
#undef PAYLOAD_REF
FIELD_GET(ProcessValueTypeMemberPlan, ProcessValueTypeMemberKind, kind, kind)
STR_GET(ProcessValueTypeMemberPlan, name, name)
FIELD_GET(ProcessValueTypeMemberPlan, std::optional<uint32_t>, index, index)
FIELD_GET(ProcessValueTypeMemberPlan, uint64_t, offsetBits, offsetBits)
FIELD_GET(ProcessValueTypeMemberPlan, uint64_t, widthBits, widthBits)
FIELD_GET(ProcessValueTypeMemberPlan, std::optional<ProcessStorageSignedness>,
          signedness, signedness)
STR_GET(ProcessValueTypeMemberPlan, encoding, encoding)
STR_GET(ProcessValueTypeMemberPlan, typeKey, typeKey)
ARRAY_GET(ProcessStorageValuePayload, ProcessValueTypeMemberPlan, members,
          members)
FIELD_GET(ProcessStorageValuePayload, uint64_t, widthBits, widthBits)
STR_GET(ProcessStorageValuePayload, encoding, encoding)
ARRAY_GET(ProcessStoragePacketPayload, ProcessValueTypeMemberPlan, members,
          members)
FIELD_GET(ProcessStoragePacketPayload, uint64_t, widthBits, widthBits)
FIELD_GET(ProcessStoragePacketPayload, uint64_t, bytes, bytes)
STR_GET(ProcessStoragePacketPayload, encoding, encoding)
FIELD_GET(ProcessValueTypePayload, ProcessValueTypeKind, kind, kind)
REF_GET(ProcessValueTypePayload, ProcessStorageValuePayload, value, value)
REF_GET(ProcessValueTypePayload, ProcessStoragePacketPayload, packet, packet)
ID_GET(ProcessGeneratedCalleePlan, ProcessCalleeId, id, id)
STR_GET(ProcessGeneratedCalleePlan, symbol, symbol)
STR_GET(ProcessGeneratedCalleePlan, cpp, cpp)
STR_GET(ProcessGeneratedCalleePlan, kind, kind)
STR_GET(ProcessGeneratedCalleePlan, fingerprint, fingerprint)
FIELD_GET(ProcessGeneratedCalleePlan, ProcessEffectKind, effect, effect)
ARRAY_GET(ProcessGeneratedCalleePlan, llvm::StringRef, inputTypeKeys,
          inputTypeKeys)
ARRAY_GET(ProcessGeneratedCalleePlan, llvm::StringRef, resultTypeKeys,
          resultTypeKeys)
FIELD_GET(ProcessGeneratedCalleePlan, ProcessHelperRole, role, role)
REF_GET(ProcessGeneratedCalleePlan, ProcessGeneratedCalleePayload, payload,
        payload)
ARRAY_GET(ProcessGeneratedCalleePlan, mlir::Operation *, sourceOperations,
          sourceOperations)
ARRAY_GET(ProcessGeneratedCalleePlan, mlir::Operation *, declarations,
          declarations)
ARRAY_GET(ProcessGeneratedCalleePlan, llvm::StringRef, sourcePaths, sourcePaths)
ID_GET(ProcessValueTypePlan, ProcessValueTypeId, id, id)
STR_GET(ProcessValueTypePlan, symbol, symbol)
STR_GET(ProcessValueTypePlan, cpp, cpp)
FIELD_GET(ProcessValueTypePlan, ProcessValueTypeKind, kind, kind)
STR_GET(ProcessValueTypePlan, fingerprint, fingerprint)
FIELD_GET(ProcessValueTypePlan, mlir::Type, acirType, acirType)
REF_GET(ProcessValueTypePlan, ProcessValueTypePayload, payload, payload)
ARRAY_GET(ProcessStatePlanSet, ProcessStatePlan, processes, processes)
ARRAY_GET(ProcessStatePlanSet, ProcessGeneratedCalleePlan, callees, callees)
ARRAY_GET(ProcessStatePlanSet, ProcessValueTypePlan, valueTypes, valueTypes)

const ProcessStatePlan *ProcessStatePlanSet::lookupByDefinitionKey(
    llvm::StringRef definitionKey) const {
  auto processes = this->processes();
  auto iterator =
      llvm::lower_bound(processes, definitionKey,
                        [](const ProcessStatePlan &plan, llvm::StringRef key) {
                          return plan.definitionKey().compare(key) < 0;
                        });
  return iterator != processes.end() &&
                 iterator->definitionKey() == definitionKey
             ? &*iterator
             : nullptr;
}

#undef FIELD_GET
#undef REF_GET
#undef ID_GET
#undef ARRAY_GET
#undef STR_GET

} // namespace acir
