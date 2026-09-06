#ifndef ACIR_ANALYSIS_PROCESSSTATEPLAN_H
#define ACIR_ANALYSIS_PROCESSSTATEPLAN_H

#include "acir/Dialect/ACIR/ACIROps.h"

#include "mlir/IR/Block.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace acir {
namespace detail {
class PlanSetBuilder;
}

struct ProcessStateLimits {
  uint64_t maxProcesses = 1U << 20;
  uint64_t maxProgramCounters = 1U << 20;
  uint64_t maxLiveSlots = 1U << 20;
  uint64_t maxWakeRecords = 1U << 20;
  uint64_t maxCalleeDescriptors = 1U << 20;
  uint64_t maxPlannedOperations = 1U << 20;
  uint64_t maxFairnessWork = 1U << 20;
  uint64_t maxTransitions = 1U << 22;
  uint64_t maxNestedRegionDepth = 512;
  uint64_t maxCanonicalReportBytes = 1U << 24;
};

#define ACIR_DECLARE_PROCESS_ID(Name)                                          \
  class Name {                                                                 \
  public:                                                                      \
    uint32_t value() const { return value_; }                                  \
    auto operator<=>(const Name &) const = default;                            \
                                                                               \
  private:                                                                     \
    explicit Name(uint32_t value) : value_(value) {}                           \
    uint32_t value_;                                                           \
    friend class detail::PlanSetBuilder;                                       \
  }

ACIR_DECLARE_PROCESS_ID(ProcessCalleeId);
ACIR_DECLARE_PROCESS_ID(ProcessValueTypeId);
ACIR_DECLARE_PROCESS_ID(ProcessCaptureId);
ACIR_DECLARE_PROCESS_ID(ProcessPcId);
ACIR_DECLARE_PROCESS_ID(ProcessBlockId);
ACIR_DECLARE_PROCESS_ID(ProcessLiveSlotId);
ACIR_DECLARE_PROCESS_ID(ProcessWakeId);
ACIR_DECLARE_PROCESS_ID(ProcessTransitionId);
#undef ACIR_DECLARE_PROCESS_ID

enum class ProcessWakeKind { Condition, Resource, EventQueue, NextDelta };
enum class ProcessSubscriptionSourceKind { Capture, Value, Symbol };
enum class ProcessActionKind {
  Original,
  Constant,
  ForInitialize,
  ForCondition,
  ForIncrement,
  ScalarWrap,
  ScalarUnwrap
};
enum class ProcessEmissionClass {
  CopyScalar,
  Inline,
  Invoke,
  Wrap,
  Unwrap,
  ForwardOnly
};
enum class ProcessOccurrenceKind {
  Original,
  SyntheticLoop,
  SyntheticWrapper,
  SyntheticConstant
};
enum class ProcessLoopPhase { Initialize, Condition, Increment };
enum class ProcessWrapperDirection { Wrap, Unwrap };
enum class ProcessFrameKind { Entry, ScfIf, ScfFor, ScfWhile };
enum class ProcessFramePhase {
  Entry,
  Then,
  Else,
  Merge,
  Header,
  Body,
  Before,
  After,
  Exit
};
enum class ProcessPlannedValueKind {
  Original,
  Capture,
  LiveSlot,
  Synthetic,
  Constant
};
enum class ProcessValueCoordinateKind { Result, BlockArgument };
enum class ProcessControlEdgeKind { Branch, LocalContinue, Suspend, Terminate };
enum class ProcessTerminateStatus { Success, Failure };
enum class ProcessEffectKind { Pure, Stateful };
enum class ProcessValueTypeKind { Value, Packet };
enum class ProcessHelperRole {
  TraceDecode,
  QueueTrySend,
  QueueTryRecv,
  EventSchedule,
  TraceOpen,
  TraceNext,
  TraceEof,
  TracePosition,
  ContractRequire,
  ContractEnsure,
  ContractAssert,
  Probe,
  StatAdd,
  WakeCondition,
  WakeResource,
  WakeEventQueue,
  WakeNextDelta,
  ScalarWrap,
  ScalarUnwrap
};
enum class ProcessValueTypeMemberKind { Field, Element };
enum class ProcessStorageSignedness { Signless, Signed, Unsigned };

#define ACIR_PROCESS_PIMPL(Name)                                               \
  struct Impl;                                                                 \
  explicit Name(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}  \
  std::shared_ptr<const Impl> impl_;                                           \
  friend class detail::PlanSetBuilder

class ProcessCallSitePlan {
public:
  mlir::Operation *operation() const;
  llvm::StringRef operationPath() const;
  llvm::ArrayRef<uint64_t> iterationVector() const;

private:
  ACIR_PROCESS_PIMPL(ProcessCallSitePlan);
};

class ProcessOccurrenceId;
class ProcessOriginalOccurrence {
public:
  mlir::Operation *operation() const;
  llvm::StringRef operationPath() const;
  llvm::ArrayRef<ProcessCallSitePlan> callSites() const;
  llvm::ArrayRef<uint64_t> iterationVector() const;

private:
  ACIR_PROCESS_PIMPL(ProcessOriginalOccurrence);
};
class ProcessSyntheticLoopOccurrence {
public:
  const ProcessOccurrenceId &anchor() const;
  ProcessLoopPhase phase() const;

private:
  ACIR_PROCESS_PIMPL(ProcessSyntheticLoopOccurrence);
};
class ProcessSyntheticWrapperOccurrence {
public:
  const ProcessOccurrenceId &anchor() const;
  ProcessTransitionId transition() const;
  ProcessLiveSlotId slot() const;
  ProcessWrapperDirection direction() const;

private:
  ACIR_PROCESS_PIMPL(ProcessSyntheticWrapperOccurrence);
};
class ProcessSyntheticConstantOccurrence {
public:
  const ProcessOccurrenceId &anchor() const;
  uint32_t constant() const;

private:
  ACIR_PROCESS_PIMPL(ProcessSyntheticConstantOccurrence);
};
class ProcessOccurrenceId {
public:
  ProcessOccurrenceKind kind() const;
  const ProcessOriginalOccurrence &original() const;
  const ProcessSyntheticLoopOccurrence &syntheticLoop() const;
  const ProcessSyntheticWrapperOccurrence &syntheticWrapper() const;
  const ProcessSyntheticConstantOccurrence &syntheticConstant() const;

private:
  ACIR_PROCESS_PIMPL(ProcessOccurrenceId);
};

class ProcessValueCoordinate {
public:
  ProcessValueCoordinateKind kind() const;
  llvm::StringRef ownerPath() const;
  uint32_t index() const;

private:
  ACIR_PROCESS_PIMPL(ProcessValueCoordinate);
};
class ProcessOriginalPlannedValue {
public:
  mlir::Value value() const;
  const ProcessOccurrenceId &occurrence() const;
  const ProcessValueCoordinate &coordinate() const;
  llvm::StringRef path() const;

private:
  ACIR_PROCESS_PIMPL(ProcessOriginalPlannedValue);
};
class ProcessCapturePlannedValue {
public:
  ProcessCaptureId capture() const;

private:
  ACIR_PROCESS_PIMPL(ProcessCapturePlannedValue);
};
class ProcessLiveSlotPlannedValue {
public:
  ProcessLiveSlotId slot() const;

private:
  ACIR_PROCESS_PIMPL(ProcessLiveSlotPlannedValue);
};
class ProcessSyntheticPlannedValue {
public:
  const ProcessOccurrenceId &occurrence() const;
  const ProcessValueCoordinate &coordinate() const;

private:
  ACIR_PROCESS_PIMPL(ProcessSyntheticPlannedValue);
};
class ProcessConstantPlannedValue {
public:
  llvm::StringRef value() const;

private:
  ACIR_PROCESS_PIMPL(ProcessConstantPlannedValue);
};
class ProcessPlannedValue {
public:
  ProcessPlannedValueKind kind() const;
  mlir::Type type() const;
  const ProcessOriginalPlannedValue &original() const;
  const ProcessCapturePlannedValue &capture() const;
  const ProcessLiveSlotPlannedValue &liveSlot() const;
  const ProcessSyntheticPlannedValue &synthetic() const;
  const ProcessConstantPlannedValue &constant() const;

private:
  ACIR_PROCESS_PIMPL(ProcessPlannedValue);
};

class ProcessScalarAttribute {
public:
  llvm::StringRef name() const;
  llvm::StringRef value() const;

private:
  ACIR_PROCESS_PIMPL(ProcessScalarAttribute);
};
class ProcessScalarOperationPlan {
public:
  llvm::StringRef name() const;
  llvm::ArrayRef<ProcessScalarAttribute> attributes() const;
  llvm::StringRef properties() const;

private:
  ACIR_PROCESS_PIMPL(ProcessScalarOperationPlan);
};

class ProcessCapturePlan {
public:
  ProcessCaptureId id() const;
  llvm::StringRef name() const;
  mlir::Value operand() const;
  mlir::Value entryArgument() const;
  mlir::Type type() const;
  llvm::StringRef operandPath() const;
  llvm::StringRef argumentPath() const;

private:
  ACIR_PROCESS_PIMPL(ProcessCapturePlan);
};
class ProcessActionPlan {
public:
  uint32_t id() const;
  ProcessActionKind kind() const;
  ProcessEmissionClass emission() const;
  const ProcessOccurrenceId &occurrence() const;
  mlir::Operation *sourceOperation() const;
  llvm::ArrayRef<uint64_t> iterationVector() const;
  llvm::ArrayRef<ProcessPlannedValue> operands() const;
  llvm::ArrayRef<ProcessPlannedValue> results() const;
  uint32_t cost() const;
  llvm::ArrayRef<mlir::Type> resultTypes() const;
  std::optional<ProcessCalleeId> callee() const;
  const ProcessScalarOperationPlan *scalarOp() const;

private:
  ACIR_PROCESS_PIMPL(ProcessActionPlan);
};
class ProcessLiveSlotPlan {
public:
  ProcessLiveSlotId id() const;
  llvm::StringRef name() const;
  mlir::Type type() const;
  ProcessValueTypeId storageType() const;
  llvm::ArrayRef<ProcessPlannedValue> memberValues() const;
  std::optional<ProcessCalleeId> wrapCallee() const;
  std::optional<ProcessCalleeId> unwrapCallee() const;

private:
  ACIR_PROCESS_PIMPL(ProcessLiveSlotPlan);
};
class ProcessSubscriptionSourcePlan {
public:
  ProcessSubscriptionSourceKind kind() const;
  mlir::Value value() const;
  mlir::Operation *owner() const;
  mlir::Operation *declaration() const;
  std::optional<ProcessCaptureId> capture() const;
  llvm::StringRef symbol() const;
  llvm::StringRef path() const;
  llvm::StringRef ownerPath() const;

private:
  ACIR_PROCESS_PIMPL(ProcessSubscriptionSourcePlan);
};
class ProcessWakePlan {
public:
  ProcessWakeId id() const;
  ProcessWakeKind kind() const;
  mlir::Operation *operation() const;
  mlir::Value triggeringValue() const;
  mlir::Operation *declaration() const;
  ProcessCalleeId callee() const;
  llvm::StringRef typeKey() const;
  llvm::StringRef operationPath() const;
  llvm::StringRef target() const;
  const ProcessOccurrenceId &occurrence() const;
  llvm::ArrayRef<uint64_t> iterationVector() const;
  llvm::ArrayRef<ProcessSubscriptionSourcePlan> sources() const;

private:
  ACIR_PROCESS_PIMPL(ProcessWakePlan);
};
class ProcessTransitionStorePlan {
public:
  ProcessLiveSlotId slot() const;
  const ProcessPlannedValue &source() const;
  mlir::Value sourceValue() const;

private:
  ACIR_PROCESS_PIMPL(ProcessTransitionStorePlan);
};
class ProcessTransitionLoadPlan {
public:
  ProcessLiveSlotId slot() const;
  llvm::ArrayRef<ProcessPlannedValue> replacements() const;

private:
  ACIR_PROCESS_PIMPL(ProcessTransitionLoadPlan);
};
class ProcessTransitionPlan {
public:
  ProcessTransitionId id() const;
  ProcessPcId sourcePc() const;
  ProcessPcId targetPc() const;
  ProcessWakeId wake() const;
  llvm::ArrayRef<uint64_t> iterationVector() const;
  llvm::ArrayRef<ProcessTransitionStorePlan> stores() const;
  llvm::ArrayRef<ProcessTransitionLoadPlan> loads() const;

private:
  ACIR_PROCESS_PIMPL(ProcessTransitionPlan);
};
class ProcessForwardingBindingPlan {
public:
  const ProcessPlannedValue &from() const;
  const ProcessPlannedValue &to() const;

private:
  ACIR_PROCESS_PIMPL(ProcessForwardingBindingPlan);
};
class ProcessControlFramePlan {
public:
  ProcessFrameKind kind() const;
  ProcessFramePhase phase() const;
  mlir::Operation *operation() const;
  llvm::StringRef operationPath() const;
  llvm::ArrayRef<ProcessForwardingBindingPlan> bindings() const;

private:
  ACIR_PROCESS_PIMPL(ProcessControlFramePlan);
};
class ProcessControlEdgePlan {
public:
  ProcessControlEdgeKind kind() const;
  const ProcessPlannedValue &condition() const;
  ProcessBlockId trueBlock() const;
  ProcessBlockId falseBlock() const;
  llvm::ArrayRef<ProcessForwardingBindingPlan> trueBindings() const;
  llvm::ArrayRef<ProcessForwardingBindingPlan> falseBindings() const;
  ProcessBlockId targetBlock() const;
  llvm::ArrayRef<ProcessForwardingBindingPlan> bindings() const;
  ProcessTransitionId transition() const;
  ProcessTerminateStatus status() const;

private:
  ACIR_PROCESS_PIMPL(ProcessControlEdgePlan);
};
class ProcessBlockPlan {
public:
  ProcessBlockId id() const;
  ProcessPcId pc() const;
  mlir::Region *originRegion() const;
  mlir::Block *originBlock() const;
  llvm::StringRef path() const;
  llvm::ArrayRef<ProcessControlFramePlan> frames() const;
  llvm::ArrayRef<ProcessTransitionLoadPlan> loads() const;
  llvm::ArrayRef<ProcessActionPlan> actions() const;
  const ProcessControlEdgePlan &edge() const;
  uint64_t cost() const;

private:
  ACIR_PROCESS_PIMPL(ProcessBlockPlan);
};
class ProcessPcPlan {
public:
  ProcessPcId id() const;
  llvm::StringRef name() const;
  llvm::StringRef entryPath() const;
  llvm::ArrayRef<ProcessBlockId> blocks() const;

private:
  ACIR_PROCESS_PIMPL(ProcessPcPlan);
};
class ProcessStatePlan {
public:
  llvm::StringRef definitionKey() const;
  ac::ProcessOp process() const;
  llvm::ArrayRef<ProcessCapturePlan> captures() const;
  ProcessPcId entryPc() const;
  llvm::ArrayRef<ProcessPcPlan> pcs() const;
  llvm::ArrayRef<ProcessBlockPlan> blocks() const;
  llvm::ArrayRef<ProcessLiveSlotPlan> liveSlots() const;
  llvm::ArrayRef<ProcessWakePlan> wakes() const;
  llvm::ArrayRef<ProcessTransitionPlan> transitions() const;
  uint32_t pcBitWidth() const;
  uint64_t fairnessWork() const;

private:
  ACIR_PROCESS_PIMPL(ProcessStatePlan);
};

#define ACIR_DECLARE_STRING_PAYLOAD(Name, ...)                                 \
  class Name {                                                                 \
  public:                                                                      \
  __VA_ARGS__ private : ACIR_PROCESS_PIMPL(Name);                              \
  }
ACIR_DECLARE_STRING_PAYLOAD(ProcessTraceDecodePayload,
                            llvm::StringRef entry() const;
                            llvm::StringRef result() const;
                            llvm::StringRef source() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessQueueTrySendPayload,
                            llvm::StringRef element() const;
                            llvm::StringRef queue() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessQueueTryRecvPayload,
                            llvm::StringRef element() const;
                            llvm::StringRef queue() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessEventSchedulePayload,
                            llvm::StringRef delay() const;
                            llvm::StringRef target() const;
                            llvm::StringRef value() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessTraceOpenPayload,
                            llvm::StringRef source() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessTraceNextPayload,
                            llvm::StringRef entry() const;
                            llvm::StringRef source() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessTraceEofPayload,
                            llvm::StringRef source() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessTracePositionPayload,
                            llvm::StringRef source() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessContractRequirePayload,
                            llvm::StringRef message() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessContractEnsurePayload,
                            llvm::StringRef message() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessContractAssertPayload,
                            llvm::StringRef message() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessProbePayload, llvm::StringRef kind() const;
                            llvm::StringRef result() const;
                            llvm::StringRef target() const;);
ACIR_DECLARE_STRING_PAYLOAD(ProcessStatAddPayload, llvm::StringRef stat() const;
                            llvm::StringRef valueType() const;);
#undef ACIR_DECLARE_STRING_PAYLOAD
#define ACIR_DECLARE_WAKE_PAYLOAD(Name)                                        \
  class Name {                                                                 \
  public:                                                                      \
    ProcessWakeKind wakeKind() const;                                          \
    llvm::StringRef wakeType() const;                                          \
                                                                               \
  private:                                                                     \
    ACIR_PROCESS_PIMPL(Name);                                                  \
  }
ACIR_DECLARE_WAKE_PAYLOAD(ProcessWakeConditionPayload);
ACIR_DECLARE_WAKE_PAYLOAD(ProcessWakeResourcePayload);
ACIR_DECLARE_WAKE_PAYLOAD(ProcessWakeEventQueuePayload);
ACIR_DECLARE_WAKE_PAYLOAD(ProcessWakeNextDeltaPayload);
#undef ACIR_DECLARE_WAKE_PAYLOAD
class ProcessScalarWrapPayload {
public:
  ProcessWrapperDirection direction() const;
  llvm::StringRef scalar() const;
  llvm::StringRef valueType() const;

private:
  ACIR_PROCESS_PIMPL(ProcessScalarWrapPayload);
};
class ProcessScalarUnwrapPayload {
public:
  ProcessWrapperDirection direction() const;
  llvm::StringRef scalar() const;
  llvm::StringRef valueType() const;

private:
  ACIR_PROCESS_PIMPL(ProcessScalarUnwrapPayload);
};

class ProcessGeneratedCalleePayload {
public:
  ProcessHelperRole role() const;
  const ProcessTraceDecodePayload &traceDecode() const;
  const ProcessQueueTrySendPayload &queueTrySend() const;
  const ProcessQueueTryRecvPayload &queueTryRecv() const;
  const ProcessEventSchedulePayload &eventSchedule() const;
  const ProcessTraceOpenPayload &traceOpen() const;
  const ProcessTraceNextPayload &traceNext() const;
  const ProcessTraceEofPayload &traceEof() const;
  const ProcessTracePositionPayload &tracePosition() const;
  const ProcessContractRequirePayload &contractRequire() const;
  const ProcessContractEnsurePayload &contractEnsure() const;
  const ProcessContractAssertPayload &contractAssert() const;
  const ProcessProbePayload &probe() const;
  const ProcessStatAddPayload &statAdd() const;
  const ProcessWakeConditionPayload &wakeCondition() const;
  const ProcessWakeResourcePayload &wakeResource() const;
  const ProcessWakeEventQueuePayload &wakeEventQueue() const;
  const ProcessWakeNextDeltaPayload &wakeNextDelta() const;
  const ProcessScalarWrapPayload &scalarWrap() const;
  const ProcessScalarUnwrapPayload &scalarUnwrap() const;

private:
  ACIR_PROCESS_PIMPL(ProcessGeneratedCalleePayload);
};

class ProcessValueTypeMemberPlan {
public:
  ProcessValueTypeMemberKind kind() const;
  llvm::StringRef name() const;
  std::optional<uint32_t> index() const;
  uint64_t offsetBits() const;
  uint64_t widthBits() const;
  std::optional<ProcessStorageSignedness> signedness() const;
  llvm::StringRef encoding() const;
  llvm::StringRef typeKey() const;

private:
  ACIR_PROCESS_PIMPL(ProcessValueTypeMemberPlan);
};
class ProcessStorageValuePayload {
public:
  llvm::ArrayRef<ProcessValueTypeMemberPlan> members() const;
  uint64_t widthBits() const;
  llvm::StringRef encoding() const;

private:
  ACIR_PROCESS_PIMPL(ProcessStorageValuePayload);
};
class ProcessStoragePacketPayload {
public:
  llvm::ArrayRef<ProcessValueTypeMemberPlan> members() const;
  uint64_t widthBits() const;
  uint64_t bytes() const;
  llvm::StringRef encoding() const;

private:
  ACIR_PROCESS_PIMPL(ProcessStoragePacketPayload);
};
class ProcessValueTypePayload {
public:
  ProcessValueTypeKind kind() const;
  const ProcessStorageValuePayload &value() const;
  const ProcessStoragePacketPayload &packet() const;

private:
  ACIR_PROCESS_PIMPL(ProcessValueTypePayload);
};
class ProcessGeneratedCalleePlan {
public:
  ProcessCalleeId id() const;
  llvm::StringRef symbol() const;
  llvm::StringRef cpp() const;
  llvm::StringRef kind() const;
  llvm::StringRef fingerprint() const;
  ProcessEffectKind effect() const;
  llvm::ArrayRef<llvm::StringRef> inputTypeKeys() const;
  llvm::ArrayRef<llvm::StringRef> resultTypeKeys() const;
  ProcessHelperRole role() const;
  const ProcessGeneratedCalleePayload &payload() const;
  llvm::ArrayRef<mlir::Operation *> sourceOperations() const;
  llvm::ArrayRef<mlir::Operation *> declarations() const;
  llvm::ArrayRef<llvm::StringRef> sourcePaths() const;

private:
  ACIR_PROCESS_PIMPL(ProcessGeneratedCalleePlan);
};
class ProcessValueTypePlan {
public:
  ProcessValueTypeId id() const;
  llvm::StringRef symbol() const;
  llvm::StringRef cpp() const;
  ProcessValueTypeKind kind() const;
  llvm::StringRef fingerprint() const;
  mlir::Type acirType() const;
  const ProcessValueTypePayload &payload() const;

private:
  ACIR_PROCESS_PIMPL(ProcessValueTypePlan);
};
class ProcessStatePlanSet {
public:
  llvm::ArrayRef<ProcessStatePlan> processes() const;
  llvm::ArrayRef<ProcessGeneratedCalleePlan> callees() const;
  llvm::ArrayRef<ProcessValueTypePlan> valueTypes() const;
  const ProcessStatePlan *
  lookupByDefinitionKey(llvm::StringRef definitionKey) const;

private:
  ACIR_PROCESS_PIMPL(ProcessStatePlanSet);
};

#undef ACIR_PROCESS_PIMPL

mlir::LogicalResult
verifyProcessStatePlan(const ProcessStatePlanSet &plans,
                       const ProcessStateLimits &limits = ProcessStateLimits());
mlir::FailureOr<ProcessStatePlanSet>
planProcessState(mlir::ModuleOp module,
                 const ProcessStateLimits &limits = ProcessStateLimits());
llvm::Expected<std::string> serializeProcessStatePlan(
    const ProcessStatePlanSet &plans,
    const ProcessStateLimits &limits = ProcessStateLimits());

} // namespace acir

#endif
