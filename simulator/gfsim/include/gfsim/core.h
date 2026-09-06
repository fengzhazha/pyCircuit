#ifndef GFSIM_CORE_H
#define GFSIM_CORE_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gfsim {

// ── Exact time and epoch ──────────────────────────────────────────────

/// Simulation time is a non-negative, unbounded-in-semantics integer tick.
using Tick = uint64_t;

/// Causal-delta index within a single tick.
using CausalDelta = uint32_t;

/// The global epoch is the exact pair (time, delta). Equality, ordering,
/// scheduling, tracing, and wake decisions use this exact pair.
struct Epoch {
  Tick time = 0;
  CausalDelta delta = 0;

  auto operator<=>(const Epoch &) const = default;

  Epoch nextDelta() const { return {time, delta + 1}; }
  bool sameTime(const Epoch &other) const { return time == other.time; }
};

/// Maximum causal deltas per tick (must be finite).
inline constexpr CausalDelta kMaxDeltasPerTick = 1024;

// ── Object identity ───────────────────────────────────────────────────

/// Compile-time-assigned stable object ID.
using ObjectId = uint32_t;

/// Stable owner for one cross-resource prepare/publish transaction.
using CommitGroupId = uint64_t;

inline constexpr CommitGroupId kInvalidCommitGroupId =
    std::numeric_limits<CommitGroupId>::max();

inline constexpr ObjectId kInvalidObjectId =
    std::numeric_limits<ObjectId>::max();
inline constexpr ObjectId kSystemObjectId = kInvalidObjectId - 1;
inline constexpr ObjectId kRootObjectId = kInvalidObjectId - 2;

/// Statically known runtime kind for every SimObject.
enum class ObjectKind : uint8_t {
  System,
  Module,
  Queue,
  EventQueue,
  Resource,
  Process,
  TraceSource,
  Compute,
  Link,
  Memory,
  Sink,
  Probe,
  Statistic,
  Scheduler,
};

// ── Termination ───────────────────────────────────────────────────────

enum class TerminationClass : uint8_t {
  Completed,  // All work finished, no pending events, trace exhausted
  Incomplete, // Reached a declared cap without contract violation
  Failed,     // Contract violation, assertion, or runtime error
};

struct StatSnapshot;

struct TimeDomainRuntime {
  std::string name;
  uint64_t period = 1;
  uint64_t phase = 0;
  uint64_t tickScale = 1;
};

struct RuntimeLimits {
  std::optional<uint64_t> deadlockWindow;
  std::optional<uint64_t> maxTicks;
  std::map<std::string, uint64_t> maxDomainCycles;
};

/// One swimlane sample emitted by generated architecture models.
struct TimelineEvent {
  Epoch epoch;
  std::string lane;
  std::string phase;
  uint64_t handle = 0;
  uint64_t sequence = 0;
  uint64_t opcode = 0;
  uint8_t dependencyValid = 0;
  uint8_t dependencies[3] = {};
  /// When true, `handle` is an occupancy count rather than a trace record.
  bool counter = false;
};

/// One validated-profile record of an object participating in Commit.
struct CommitEvent {
  Epoch epoch;
  ObjectId objectId = kInvalidObjectId;
  bool semanticChanged = false;

  bool operator==(const CommitEvent &) const = default;
};

struct TerminationResult {
  TerminationClass classification = TerminationClass::Incomplete;
  Epoch finalEpoch;
  uint64_t committedEventCount = 0;
  uint64_t tracePosition = 0;
  std::optional<uint64_t> traceLastCommittedSequenceId;
  std::optional<uint64_t> terminationCap;
  std::map<std::string, uint64_t> domainCycles;
  std::string diagnosticCode;
  std::optional<std::string> message;
  std::vector<StatSnapshot> stats;
};

/// Result of an indexed, non-consuming PTO trace lookup.
struct TraceNextResult {
  uint64_t cursor = 0;
  uint64_t handle = 0;
  bool advanced = false;
};

// ── Build profiles ────────────────────────────────────────────────────

enum class BuildProfile : uint8_t {
  Fast,      // Required representation, safety, capacity, and contract checks
  Validated, // Fast + post-pass verification, transaction-lifetime checks,
             //   arbitration audits, event provenance, quiescence checks
  Custom,    // Explicit pass pipeline retaining all mandatory Fast checks
};

// ── Static preflight ──────────────────────────────────────────────────

struct PreflightResult {
  bool passed = false;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

// ── Work and proposal ─────────────────────────────────────────────────

/// A proposal is a private buffer associated with a stable object ID.
/// Each Work execution writes only to its own proposal.
struct Proposal {
  ObjectId ownerId = kInvalidObjectId;
  bool hasCommit = false;
  // Component-specific proposal data is stored in derived types
};

// ── Event ─────────────────────────────────────────────────────────────

/// An event scheduled for a future epoch.
struct Event {
  Epoch readyTime;
  ObjectId targetId = kInvalidObjectId;
  uint32_t eventKind = 0;
  uint64_t payload = 0;

  // Stable ordering: epoch first, then target, event kind, and payload.
  auto operator<=>(const Event &other) const {
    if (auto cmp = readyTime <=> other.readyTime; cmp != 0)
      return cmp;
    if (auto cmp = targetId <=> other.targetId; cmp != 0)
      return cmp;
    if (auto cmp = eventKind <=> other.eventKind; cmp != 0)
      return cmp;
    return payload <=> other.payload;
  }
};

// ── Statistics ────────────────────────────────────────────────────────

enum class StatisticKind : uint8_t { Counter, Gauge, Histogram };

struct HistogramBucket {
  uint64_t upperBound = 0;
  uint64_t count = 0;
  bool operator==(const HistogramBucket &) const = default;
};

struct StatSnapshot {
  std::string name;
  std::string objectPath;
  StatisticKind kind = StatisticKind::Counter;
  uint64_t value = 0;
  uint64_t count = 0;
  uint64_t sum = 0;
  uint64_t minimum = 0;
  uint64_t maximum = 0;
  std::vector<HistogramBucket> buckets;
  Epoch lastUpdate;
};

struct RuntimeObjectState {
  bool quiescent = true;
  bool runnable = false;
  bool pendingCommit = false;
  std::string reason;
  std::vector<std::string> subscriptions;
  std::vector<uint64_t> dependencyChain;
  std::vector<uint64_t> correlationChain;
  size_t queueOccupancy = 0;
  size_t pendingOffers = 0;
  size_t activeReservations = 0;
  std::string protocolState;
  bool traceOwner = false;
  uint64_t tracePosition = 0;
  std::optional<uint64_t> traceLastCommittedSequenceId;
  bool traceEof = false;
};

struct BlockedObject {
  ObjectId id = kInvalidObjectId;
  std::string path;
  std::string reason;
  std::vector<std::string> subscriptions;
  std::vector<uint64_t> dependencyChain;
  std::vector<uint64_t> correlationChain;
  size_t queueOccupancy = 0;
  size_t pendingOffers = 0;
  size_t activeReservations = 0;
  std::string protocolState;
};

struct NoProgressReport {
  std::vector<BlockedObject> blockedObjects;
  size_t queueOccupancy = 0;
  size_t pendingOffers = 0;
  size_t activeReservations = 0;
  std::optional<Event> nextEvent;
  uint64_t tracePosition = 0;
  std::optional<uint64_t> lastCommittedSequenceId;
  std::string summary;
  bool empty() const { return blockedObjects.empty() && !nextEvent; }
};

} // namespace gfsim

#endif // GFSIM_CORE_H
