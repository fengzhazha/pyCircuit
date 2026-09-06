#ifndef ACIR_ANALYSIS_PROCESSSTATEPLANINTERNAL_H
#define ACIR_ANALYSIS_PROCESSSTATEPLANINTERNAL_H

#include "Dialect/ACIR/ProcessLowerability.h"
#include "ProcessStatePlanTestHooks.h"

#include <vector>

namespace acir {

struct ProcessCallSitePlan::Impl {
  mlir::Operation *operation = nullptr;
  std::string operationPath;
  std::vector<uint64_t> iterationVector;
};
struct ProcessOriginalOccurrence::Impl {
  mlir::Operation *operation = nullptr;
  std::string operationPath;
  std::vector<ProcessCallSitePlan> callSites;
  std::vector<uint64_t> iterationVector;
};
struct ProcessSyntheticLoopOccurrence::Impl {
  std::optional<ProcessOccurrenceId> anchor;
  ProcessLoopPhase phase = ProcessLoopPhase::Initialize;
};
struct ProcessSyntheticWrapperOccurrence::Impl {
  std::optional<ProcessOccurrenceId> anchor;
  std::optional<ProcessTransitionId> transition;
  std::optional<ProcessLiveSlotId> slot;
  ProcessWrapperDirection direction = ProcessWrapperDirection::Wrap;
};
struct ProcessSyntheticConstantOccurrence::Impl {
  std::optional<ProcessOccurrenceId> anchor;
  uint32_t constant = 0;
};
struct ProcessOccurrenceId::Impl {
  ProcessOccurrenceKind kind = ProcessOccurrenceKind::Original;
  std::optional<ProcessOriginalOccurrence> original;
  std::optional<ProcessSyntheticLoopOccurrence> syntheticLoop;
  std::optional<ProcessSyntheticWrapperOccurrence> syntheticWrapper;
  std::optional<ProcessSyntheticConstantOccurrence> syntheticConstant;
};
struct ProcessValueCoordinate::Impl {
  ProcessValueCoordinateKind kind = ProcessValueCoordinateKind::Result;
  std::string ownerPath;
  uint32_t index = 0;
};
struct ProcessOriginalPlannedValue::Impl {
  mlir::Value value;
  std::optional<ProcessOccurrenceId> occurrence;
  std::optional<ProcessValueCoordinate> coordinate;
  std::string path;
};
struct ProcessCapturePlannedValue::Impl {
  std::optional<ProcessCaptureId> capture;
};
struct ProcessLiveSlotPlannedValue::Impl {
  std::optional<ProcessLiveSlotId> slot;
};
struct ProcessSyntheticPlannedValue::Impl {
  std::optional<ProcessOccurrenceId> occurrence;
  std::optional<ProcessValueCoordinate> coordinate;
};
struct ProcessConstantPlannedValue::Impl {
  std::string value;
};
struct ProcessPlannedValue::Impl {
  ProcessPlannedValueKind kind = ProcessPlannedValueKind::Constant;
  mlir::Type type;
  std::optional<ProcessOriginalPlannedValue> original;
  std::optional<ProcessCapturePlannedValue> capture;
  std::optional<ProcessLiveSlotPlannedValue> liveSlot;
  std::optional<ProcessSyntheticPlannedValue> synthetic;
  std::optional<ProcessConstantPlannedValue> constant;
};
struct ProcessScalarAttribute::Impl {
  std::string name;
  std::string value;
};
struct ProcessScalarOperationPlan::Impl {
  std::string name;
  std::vector<ProcessScalarAttribute> attributes;
  std::string properties;
};
struct ProcessCapturePlan::Impl {
  std::optional<ProcessCaptureId> id;
  std::string name;
  mlir::Value operand;
  mlir::Value entryArgument;
  mlir::Type type;
  std::string operandPath;
  std::string argumentPath;
};
struct ProcessActionPlan::Impl {
  uint32_t id = 0;
  ProcessActionKind kind = ProcessActionKind::Original;
  ProcessEmissionClass emission = ProcessEmissionClass::ForwardOnly;
  std::optional<ProcessOccurrenceId> occurrence;
  mlir::Operation *sourceOperation = nullptr;
  std::vector<uint64_t> iterationVector;
  std::vector<ProcessPlannedValue> operands;
  std::vector<ProcessPlannedValue> results;
  uint32_t cost = 0;
  std::vector<mlir::Type> resultTypes;
  std::optional<ProcessCalleeId> callee;
  std::optional<ProcessScalarOperationPlan> scalarOp;
};
struct ProcessLiveSlotPlan::Impl {
  std::optional<ProcessLiveSlotId> id;
  std::string name;
  mlir::Type type;
  std::optional<ProcessValueTypeId> storageType;
  std::vector<ProcessPlannedValue> memberValues;
  std::optional<ProcessCalleeId> wrapCallee;
  std::optional<ProcessCalleeId> unwrapCallee;
};
struct ProcessSubscriptionSourcePlan::Impl {
  ProcessSubscriptionSourceKind kind = ProcessSubscriptionSourceKind::Value;
  mlir::Value value;
  mlir::Operation *owner = nullptr;
  mlir::Operation *declaration = nullptr;
  std::optional<ProcessCaptureId> capture;
  std::string symbol;
  std::string path;
  std::string ownerPath;
};
struct ProcessWakePlan::Impl {
  std::optional<ProcessWakeId> id;
  ProcessWakeKind kind = ProcessWakeKind::NextDelta;
  mlir::Operation *operation = nullptr;
  mlir::Value triggeringValue;
  mlir::Operation *declaration = nullptr;
  std::optional<ProcessCalleeId> callee;
  std::string typeKey;
  std::string operationPath;
  std::string target;
  std::optional<ProcessOccurrenceId> occurrence;
  std::vector<uint64_t> iterationVector;
  std::vector<ProcessSubscriptionSourcePlan> sources;
};
struct ProcessTransitionStorePlan::Impl {
  std::optional<ProcessLiveSlotId> slot;
  std::optional<ProcessPlannedValue> source;
  mlir::Value sourceValue;
};
struct ProcessTransitionLoadPlan::Impl {
  std::optional<ProcessLiveSlotId> slot;
  std::vector<ProcessPlannedValue> replacements;
};
struct ProcessTransitionPlan::Impl {
  std::optional<ProcessTransitionId> id;
  std::optional<ProcessPcId> sourcePc;
  std::optional<ProcessPcId> targetPc;
  std::optional<ProcessWakeId> wake;
  std::vector<uint64_t> iterationVector;
  std::vector<ProcessTransitionStorePlan> stores;
  std::vector<ProcessTransitionLoadPlan> loads;
};
struct ProcessForwardingBindingPlan::Impl {
  std::optional<ProcessPlannedValue> from;
  std::optional<ProcessPlannedValue> to;
};
struct ProcessControlFramePlan::Impl {
  ProcessFrameKind kind = ProcessFrameKind::Entry;
  ProcessFramePhase phase = ProcessFramePhase::Entry;
  mlir::Operation *operation = nullptr;
  std::string operationPath;
  std::vector<ProcessForwardingBindingPlan> bindings;
};
struct ProcessControlEdgePlan::Impl {
  ProcessControlEdgeKind kind = ProcessControlEdgeKind::Terminate;
  std::optional<ProcessPlannedValue> condition;
  std::optional<ProcessBlockId> trueBlock;
  std::optional<ProcessBlockId> falseBlock;
  std::vector<ProcessForwardingBindingPlan> trueBindings;
  std::vector<ProcessForwardingBindingPlan> falseBindings;
  std::optional<ProcessBlockId> targetBlock;
  std::vector<ProcessForwardingBindingPlan> bindings;
  std::optional<ProcessTransitionId> transition;
  ProcessTerminateStatus status = ProcessTerminateStatus::Success;
};
struct ProcessBlockPlan::Impl {
  std::optional<ProcessBlockId> id;
  std::optional<ProcessPcId> pc;
  mlir::Region *originRegion = nullptr;
  mlir::Block *originBlock = nullptr;
  std::string path;
  std::vector<ProcessControlFramePlan> frames;
  std::vector<ProcessTransitionLoadPlan> loads;
  std::vector<ProcessActionPlan> actions;
  std::optional<ProcessControlEdgePlan> edge;
  uint64_t cost = 0;
};
struct ProcessPcPlan::Impl {
  std::optional<ProcessPcId> id;
  std::string name;
  std::string entryPath;
  std::vector<ProcessBlockId> blocks;
};
struct ProcessStatePlan::Impl {
  std::string definitionKey;
  ac::ProcessOp process;
  std::vector<ProcessCapturePlan> captures;
  std::optional<ProcessPcId> entryPc;
  std::vector<ProcessPcPlan> pcs;
  std::vector<ProcessBlockPlan> blocks;
  std::vector<ProcessLiveSlotPlan> liveSlots;
  std::vector<ProcessWakePlan> wakes;
  std::vector<ProcessTransitionPlan> transitions;
  uint32_t pcBitWidth = 1;
  uint64_t fairnessWork = 1;
};
struct ProcessTraceDecodePayload::Impl {
  std::string entry, result, source;
};
struct ProcessQueueTrySendPayload::Impl {
  std::string element, queue;
};
struct ProcessQueueTryRecvPayload::Impl {
  std::string element, queue;
};
struct ProcessEventSchedulePayload::Impl {
  std::string delay, target, value;
};
struct ProcessTraceOpenPayload::Impl {
  std::string source;
};
struct ProcessTraceNextPayload::Impl {
  std::string entry, source;
};
struct ProcessTraceEofPayload::Impl {
  std::string source;
};
struct ProcessTracePositionPayload::Impl {
  std::string source;
};
struct ProcessContractRequirePayload::Impl {
  std::string message;
};
struct ProcessContractEnsurePayload::Impl {
  std::string message;
};
struct ProcessContractAssertPayload::Impl {
  std::string message;
};
struct ProcessProbePayload::Impl {
  std::string kind, result, target;
};
struct ProcessStatAddPayload::Impl {
  std::string stat, valueType;
};
struct ProcessWakeConditionPayload::Impl {
  ProcessWakeKind wakeKind = ProcessWakeKind::Condition;
  std::string wakeType;
};
struct ProcessWakeResourcePayload::Impl {
  ProcessWakeKind wakeKind = ProcessWakeKind::Resource;
  std::string wakeType;
};
struct ProcessWakeEventQueuePayload::Impl {
  ProcessWakeKind wakeKind = ProcessWakeKind::EventQueue;
  std::string wakeType;
};
struct ProcessWakeNextDeltaPayload::Impl {
  ProcessWakeKind wakeKind = ProcessWakeKind::NextDelta;
  std::string wakeType;
};
struct ProcessScalarWrapPayload::Impl {
  ProcessWrapperDirection direction = ProcessWrapperDirection::Wrap;
  std::string scalar, valueType;
};
struct ProcessScalarUnwrapPayload::Impl {
  ProcessWrapperDirection direction = ProcessWrapperDirection::Unwrap;
  std::string scalar, valueType;
};
struct ProcessGeneratedCalleePayload::Impl {
  ProcessHelperRole role = ProcessHelperRole::WakeNextDelta;
  std::optional<ProcessTraceDecodePayload> traceDecode;
  std::optional<ProcessQueueTrySendPayload> queueTrySend;
  std::optional<ProcessQueueTryRecvPayload> queueTryRecv;
  std::optional<ProcessEventSchedulePayload> eventSchedule;
  std::optional<ProcessTraceOpenPayload> traceOpen;
  std::optional<ProcessTraceNextPayload> traceNext;
  std::optional<ProcessTraceEofPayload> traceEof;
  std::optional<ProcessTracePositionPayload> tracePosition;
  std::optional<ProcessContractRequirePayload> contractRequire;
  std::optional<ProcessContractEnsurePayload> contractEnsure;
  std::optional<ProcessContractAssertPayload> contractAssert;
  std::optional<ProcessProbePayload> probe;
  std::optional<ProcessStatAddPayload> statAdd;
  std::optional<ProcessWakeConditionPayload> wakeCondition;
  std::optional<ProcessWakeResourcePayload> wakeResource;
  std::optional<ProcessWakeEventQueuePayload> wakeEventQueue;
  std::optional<ProcessWakeNextDeltaPayload> wakeNextDelta;
  std::optional<ProcessScalarWrapPayload> scalarWrap;
  std::optional<ProcessScalarUnwrapPayload> scalarUnwrap;
};
struct ProcessValueTypeMemberPlan::Impl {
  ProcessValueTypeMemberKind kind = ProcessValueTypeMemberKind::Field;
  std::string name;
  std::optional<uint32_t> index;
  uint64_t offsetBits = 0;
  uint64_t widthBits = 0;
  std::optional<ProcessStorageSignedness> signedness;
  std::string encoding;
  std::string typeKey;
};
struct ProcessStorageValuePayload::Impl {
  std::vector<ProcessValueTypeMemberPlan> members;
  uint64_t widthBits = 0;
  std::string encoding;
};
struct ProcessStoragePacketPayload::Impl {
  std::vector<ProcessValueTypeMemberPlan> members;
  uint64_t widthBits = 0;
  uint64_t bytes = 0;
  std::string encoding;
};
struct ProcessValueTypePayload::Impl {
  ProcessValueTypeKind kind = ProcessValueTypeKind::Value;
  std::optional<ProcessStorageValuePayload> value;
  std::optional<ProcessStoragePacketPayload> packet;
};
struct ProcessGeneratedCalleePlan::Impl {
  std::optional<ProcessCalleeId> id;
  std::string symbol, cpp, kind = "implementation", fingerprint;
  ProcessEffectKind effect = ProcessEffectKind::Pure;
  std::vector<std::string> inputTypeKeyStorage;
  std::vector<llvm::StringRef> inputTypeKeys;
  std::vector<std::string> resultTypeKeyStorage;
  std::vector<llvm::StringRef> resultTypeKeys;
  ProcessHelperRole role = ProcessHelperRole::WakeNextDelta;
  std::optional<ProcessGeneratedCalleePayload> payload;
  std::vector<mlir::Operation *> sourceOperations;
  std::vector<mlir::Operation *> declarations;
  std::vector<std::string> sourcePathStorage;
  std::vector<llvm::StringRef> sourcePaths;
  std::string specializationBytes;
  std::string descriptorBytes;
};
struct ProcessValueTypePlan::Impl {
  std::optional<ProcessValueTypeId> id;
  std::string symbol, cpp, fingerprint;
  ProcessValueTypeKind kind = ProcessValueTypeKind::Value;
  mlir::Type acirType;
  std::optional<ProcessValueTypePayload> payload;
  std::string specializationBytes;
};
struct ProcessStatePlanSet::Impl {
  std::vector<ProcessStatePlan> processes;
  std::vector<ProcessGeneratedCalleePlan> callees;
  std::vector<ProcessValueTypePlan> valueTypes;
};

namespace detail {

struct ExpandedForwarding {
  ProcessPlannedValue from;
  ProcessPlannedValue to;
};

struct ExpandedAction {
  ProcessActionKind kind = ProcessActionKind::Original;
  mlir::Operation *operation = nullptr;
  std::string operationPath;
  std::optional<ProcessOccurrenceId> occurrence;
  std::vector<ProcessCallSitePlan> callSites;
  std::vector<uint64_t> iterationVector;
  std::vector<ProcessPlannedValue> operands;
  std::vector<ProcessPlannedValue> results;
  std::optional<ProcessScalarOperationPlan> scalarOperation;
};

struct ExpandedProcess {
  ac::ProcessOp process;
  std::string definitionKey;
  std::vector<ExpandedAction> actions;
  std::vector<ExpandedForwarding> forwarding;
  uint64_t expandedNodes = 0;
  uint64_t expandedEdges = 0;
  uint64_t valueLookupProbes = 0;
  uint64_t maxValueLookupProbes = 0;
};

mlir::FailureOr<ExpandedProcess> expandProcessForPlanning(
    ac::ProcessOp process,
    const ac::RawModelStructureLimits &limits = ac::RawModelStructureLimits());

llvm::Expected<std::string>
canonicalProcessOccurrenceJSON(const ProcessOccurrenceId &occurrence);
llvm::Expected<std::string>
hashProcessOccurrence(const ProcessOccurrenceId &occurrence);

class PlanSetBuilder {
public:
  enum class LoopActionMutationForTest {
    WrongOwningLoopSource,
    NonLoopSource,
    WrongResultType,
    InactiveCallee,
    InactiveScalar,
    WrongEmission,
    WrongScalarName,
    MissingScalar,
    WrongScalarProperties,
    WrongScalarAttributeCount,
    WrongScalarAttributeName,
    WrongScalarAttributeValue,
    WrongOperandCount,
    WrongOperandType,
    WrongResultCount,
  };

  static mlir::FailureOr<ProcessStatePlanSet> buildEmpty(mlir::ModuleOp module);
  static mlir::FailureOr<ProcessStatePlanSet>
  buildYieldOnly(mlir::ModuleOp module);
  static mlir::FailureOr<ProcessStatePlanSet>
  buildFrozenFixture(mlir::ModuleOp module, bool requireYieldOnly);
  static mlir::FailureOr<ProcessStatePlanSet>
  buildLoopActionFixture(mlir::ModuleOp module);
  static ProcessStatePlanSet
  cloneWithCorruption(const ProcessStatePlanSet &plans,
                      ProcessStatePlanCorruptionForTest corruption);
  static ProcessStatePlanSet
  cloneWithMissingWakeCallee(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithDanglingSuspendTransition(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithUnpairedLiveSlotCallee(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithMissingValueTypePayload(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithNullEdgeStorage(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithInactiveEdgeField(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithDoubleValueTypePayload(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithMissingOriginalActionSource(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithUnexpectedConstantActionSource(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithNonLoopForActionSource(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForInitializeWrongOwningLoopSource(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForConditionWrongOwningLoopSource(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForIncrementWrongOwningLoopSource(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForConditionWrongResultType(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForIncrementWrongResultType(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForConditionInactiveCallee(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForInitializeInactiveScalar(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForConditionWrongEmission(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForConditionWrongScalarOp(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForIncrementWrongEmission(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithForIncrementWrongScalarOp(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneLoopActionWithMutationForTest(const ProcessStatePlanSet &plans,
                                     uint32_t actionIndex,
                                     LoopActionMutationForTest mutation);
  static bool exerciseCompleteApiFixture(mlir::MLIRContext &context);
  static bool exerciseAllActionArmsFixture(mlir::MLIRContext &context);
  static ProcessStatePlanSet
  cloneWithLongLocalChain(const ProcessStatePlanSet &plans, uint32_t blocks);
  static ProcessStatePlanSet
  cloneWithLocalCycle(const ProcessStatePlanSet &plans);
  static ProcessStatePlanSet
  cloneWithUnreachableBlock(const ProcessStatePlanSet &plans);
  static llvm::StringRef
  specializationBytes(const ProcessGeneratedCalleePlan &callee);
  static llvm::StringRef
  descriptorBytes(const ProcessGeneratedCalleePlan &callee);
  static llvm::StringRef specializationBytes(const ProcessValueTypePlan &type);
  static bool validEdgeShape(const ProcessControlEdgePlan &edge);
  static llvm::StringRef structuralError(const ProcessStatePlanSet &plans);
  static mlir::FailureOr<ExpandedProcess>
  expandProcess(ac::ProcessOp process,
                const ac::RawModelStructureLimits &limits);

  struct ControlPlan {
    std::vector<std::shared_ptr<ProcessCapturePlan::Impl>> captures;
    std::vector<std::shared_ptr<ProcessPcPlan::Impl>> pcs;
    std::vector<std::shared_ptr<ProcessBlockPlan::Impl>> blocks;
    std::vector<std::shared_ptr<ProcessWakePlan::Impl>> wakes;
    std::vector<std::shared_ptr<ProcessTransitionPlan::Impl>> transitions;
    std::vector<std::shared_ptr<ProcessLiveSlotPlan::Impl>> liveSlots;
    uint64_t fairnessWork = 0;
  };
  static mlir::FailureOr<ProcessStatePlanSet>
  buildProduction(mlir::ModuleOp module, const ProcessStateLimits &limits);
  static mlir::FailureOr<std::unique_ptr<ControlPlan>>
  planProcessContinuation(const ExpandedProcess &expanded,
                          const ProcessStateLimits &limits);
  static mlir::FailureOr<std::unique_ptr<ControlPlan>>
  planProcessWakes(std::unique_ptr<ControlPlan> control,
                   const ProcessStateLimits &limits);
  static mlir::LogicalResult
  planProcessLiveness(ControlPlan &control, const ProcessStateLimits &limits);
  static mlir::LogicalResult planProcessCost(ControlPlan &control,
                                             const ProcessStateLimits &limits);
};

llvm::StringRef
generatedCalleeSpecializationBytes(const ProcessGeneratedCalleePlan &callee);
llvm::StringRef
generatedCalleeDescriptorBytes(const ProcessGeneratedCalleePlan &callee);
llvm::StringRef lastProcessStatePlanDiagnosticForTest();
llvm::Expected<std::string> canonicalGeneratedCalleeSpecialization(
    const ProcessGeneratedCalleePlan &callee);
llvm::Expected<std::string>
canonicalValueTypeSpecialization(const ProcessValueTypePlan &type);

} // namespace detail
} // namespace acir

#endif
