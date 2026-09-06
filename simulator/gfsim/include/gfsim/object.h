#ifndef GFSIM_OBJECT_H
#define GFSIM_OBJECT_H

#include "gfsim/core.h"
#include "gfsim/dispatch.h"
#include "gfsim/observation.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gfsim {

class Module;
class SimSystem;

// ── SimObject ─────────────────────────────────────────────────────────

/// Every runtime object has one owning parent (except the root system),
/// a compile-time-assigned stable object ID, a stable local name,
/// and a canonical hierarchy path.
class SimObject {
public:
  SimObject(ObjectKind kind, std::string name, ObjectId id,
            SimObject *parent = nullptr,
            ObservationSink *observationSink = nullptr)
      : kind_(kind), name_(std::move(name)), id_(id), parent_(parent),
        observationSink_(observationSink) {}

  virtual ~SimObject() = default;

  ObjectKind kind() const { return kind_; }
  ObjectId id() const { return id_; }
  std::string_view name() const { return name_; }
  std::string_view path() const { return path_; }
  SimObject *parent() const { return parent_; }
  std::string_view runtimeFailureCode() const { return runtimeFailureCode_; }

  /// Set the canonical hierarchy path (called during construction).
  virtual void setPath(std::string path) { path_ = std::move(path); }

  /// Set the parent pointer (called during hierarchy construction).
  void setParent(SimObject *p) { parent_ = p; }

  /// Children owned by this object (for modules).
  virtual std::vector<SimObject *> children() const { return {}; }

  // ── Work interface ──────────────────────────────────────────────────
  //
  // Each component implements Work generation (produce proposals for the
  // current epoch) and Xfer commit (accept/reject proposals atomically).

  /// Generate work proposals for the current epoch.
  /// Called only when the object is scheduled to run.
  virtual void doWork(Epoch epoch) {}

  /// Commit accepted proposals at the Xfer barrier.
  virtual void doXfer(Epoch epoch) {}

  /// Arbitration: select the winning proposal among candidates.
  virtual void doArbitrate(Epoch epoch) {}

  /// True when the next Xfer call must finish an accepted commit transaction.
  virtual bool hasPendingCommit() const { return false; }

  /// True when the most recent successful commit changed observable state.
  /// The default is conservative: objects without a semantic equality policy
  /// wake their activation dependents after every commit.
  virtual bool lastCommitChanged() const { return true; }

  // ── Wake conditions ─────────────────────────────────────────────────

  /// Returns true if this object has work to do at the given epoch.
  virtual bool isRunnable(Epoch epoch) const { return false; }

  /// Describe committed liveness state for diagnostics outside the hot path.
  virtual RuntimeObjectState runtimeState(Epoch epoch) const {
    const bool pending = hasPendingCommit();
    const bool runnable = isRunnable(epoch);
    return {.quiescent = !pending && !runnable,
            .runnable = runnable,
            .pendingCommit = pending,
            .reason =
                pending ? "pending_commit" : (runnable ? "runnable" : "")};
  }

  /// Append deterministic snapshots owned by this object.
  virtual void collectStatistics(std::vector<StatSnapshot> &) const {}

  /// Bind generated objects to their owning runtime after construction.
  virtual void bindSystem(SimSystem *) {}
  void setObservationSink(ObservationSink *sink) { observationSink_ = sink; }

  /// Request deterministic shutdown at a voluntary trace-end yield point.
  virtual bool requestTraceEnd() { return false; }

  // ── Reset ───────────────────────────────────────────────────────────

  virtual void reset() {}

  /// Return this object as a hierarchy module without requiring RTTI.
  virtual Module *asModule() { return nullptr; }
  virtual const Module *asModule() const { return nullptr; }

protected:
  bool emitObservation(EventProposal proposal) {
    if (!observationSink_)
      return true;
    proposal.ownerId = id_;
    if (observationSink_->proposeObservation(std::move(proposal)))
      return true;
    setRuntimeFailureCode("observation_proposal_failed");
    return false;
  }
  void setRuntimeFailureCode(std::string_view code) {
    runtimeFailureCode_ = code;
  }
  void clearRuntimeFailureCode() { runtimeFailureCode_ = {}; }

  ObjectKind kind_;
  std::string name_;
  ObjectId id_ = kInvalidObjectId;
  std::string path_;
  SimObject *parent_ = nullptr;
  ObservationSink *observationSink_ = nullptr;
  std::string_view runtimeFailureCode_;
};

// ── Module ────────────────────────────────────────────────────────────

/// A module owns child modules and local runtime objects.
/// Generated C++ creates one class per specialized ACIR module definition.
class Module : public SimObject {
public:
  Module(std::string name, ObjectId id, SimObject *parent = nullptr)
      : SimObject(ObjectKind::Module, std::move(name), id, parent) {}

  /// Attach a non-owned child to the same deterministic hierarchy index used
  /// by owned children. An object can be attached to exactly one module.
  bool attachChild(SimObject &child) {
    if (&child == this ||
        (child.parent() != nullptr && child.parent() != this) ||
        std::find(children_.begin(), children_.end(), &child) !=
            children_.end())
      return false;

    children_.push_back(&child);
    child.setParent(this);
    child.setPath(std::string(path()) + "/" + std::string(child.name()));
    return true;
  }

  /// Add a child object owned by this module.
  void addChild(std::unique_ptr<SimObject> child) {
    if (!child || !attachChild(*child))
      throw std::invalid_argument("child is null or already attached");
    owned_.push_back(std::move(child));
  }

  std::vector<SimObject *> children() const override { return children_; }

  Module *asModule() override { return this; }
  const Module *asModule() const override { return this; }

  void setPath(std::string path) override {
    SimObject::setPath(std::move(path));
    for (SimObject *child : children_)
      child->setPath(std::string(this->path()) + "/" +
                     std::string(child->name()));
  }

  /// Find a child by name (linear scan, acceptable for small fan-out).
  SimObject *findChild(std::string_view name) const {
    for (auto *c : children_)
      if (c->name() == name)
        return c;
    return nullptr;
  }

  /// Walk all descendants recursively.
  template <typename F> void walk(F &&fn) {
    fn(*this);
    for (auto *c : children_) {
      if (Module *module = c->asModule())
        module->walk(fn);
      else
        // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
        fn(*c); // the children_ invariant guarantees non-null entries
    }
  }

  /// Walk all descendants (const version).
  template <typename F> void walk(F &&fn) const {
    fn(*this);
    for (auto *c : children_) {
      if (const Module *module = c->asModule())
        module->walk(std::forward<F>(fn));
      else
        fn(*c);
    }
  }

  void reset() override {
    for (auto *c : children_)
      c->reset();
  }

private:
  std::vector<SimObject *> children_;
  std::vector<std::unique_ptr<SimObject>> owned_;
};

// ── SimSystem ─────────────────────────────────────────────────────────

class SimQueueBase;
class EventQueue;
class Resource;

/// The system owns the root module, exact global epoch, event scheduling,
/// phase barriers, termination state, and deterministic sequencing.
class SimSystem : public SimObject, public ObservationSink {
public:
  explicit SimSystem(std::string name = "system");
  ~SimSystem() override;

  Module &root() { return *root_; }
  const Module &root() const { return *root_; }

  Epoch currentEpoch() const { return epoch_; }

  // ── Scheduling ──────────────────────────────────────────────────────

  /// Schedule an object for Work at the given epoch.
  bool scheduleWork(ObjectId id, Epoch epoch);

  /// Enroll an externally proposed Queue transfer at the current epoch.
  bool scheduleExternalXfer(ObjectId id);

  /// Schedule an event for a future epoch.
  bool scheduleEvent(Event event);

  /// Install the generated dense static dispatch table.
  bool setDispatchTable(std::span<const DispatchRow> rows);

  /// Install canonical compressed activation adjacency.
  bool setActivationPlan(std::span<const uint32_t> offsets,
                         std::span<const ObjectId> targets);

  /// Install same-epoch Xfer resources indexed by scheduled Work object ID.
  bool setWorkClosurePlan(std::span<const uint32_t> offsets,
                          std::span<const ObjectId> targets);

  /// Install the opaque dispatch ABI emitted by the legacy ACIR C++ path.
  bool setLegacyDispatchTable(LegacyDispatchTable table);
  bool setLegacyActivationGraph(LegacyActivationGraph graph);

  /// Get the next pending event (earliest ready time).
  std::optional<Event> nextEvent() const;

  // ── Simulation loop ─────────────────────────────────────────────────

  /// Run the simulation until termination or cap reached.
  TerminationResult run();

  /// Advance one (time, delta) step. Returns false if no more work.
  bool step();

  // ── Termination ─────────────────────────────────────────────────────

  bool isTerminated() const { return terminated_; }
  TerminationResult terminationResult() const { return result_; }
  NoProgressReport noProgressReport() const;
  std::vector<StatSnapshot> statistics() const;
  std::span<const CommittedEvent> observations() const;
  bool proposeObservation(EventProposal proposal) override;

  // ── Object registry ─────────────────────────────────────────────────

  /// Register an object for ID-based lookup.
  void registerObject(SimObject *obj);

  /// Look up an object by its stable ID.
  SimObject *lookup(ObjectId id) const;

  /// Request cooperative termination from generated process code.
  void requestTerminate(TerminationClass classification,
                        std::string diagnosticCode = {});

  /// Publish an optional generated-model statistic.
  void recordStat(std::string name, uint64_t value);

  /// Record a Perfetto/Chrome-trace swimlane sample at the current epoch.
  void recordTraceEvent(std::string lane, std::string phase, uint64_t handle);

  /// Record a Perfetto counter sample (IQ / ROB occupancy) at the current
  /// epoch.
  void recordTraceCounter(std::string lane, uint64_t value);

  std::string chromeTraceJson() const;

  // ── PTO trace provider ───────────────────────────────────────────────

  void loadPtoTrace(std::string source, const std::string &path);
  uint64_t traceOpen(std::string_view source) const;
  TraceNextResult traceNext(std::string_view source, uint64_t cursor) const;
  uint64_t traceDecode(uint64_t handle) const;
  bool traceEof(std::string_view source, uint64_t cursor) const;
  uint64_t tracePosition(std::string_view source, uint64_t cursor) const;
  uint64_t traceRecordCount(std::string_view source) const;

  void reset() override;

  // ── Caps ────────────────────────────────────────────────────────────

  void setMaxTicks(Tick max) { maxTicks_ = max; }
  void setMaxEvents(uint64_t max) { maxEvents_ = max; }
  void setMaxTraceRecords(uint64_t max) { maxTraceRecords_ = max; }
  bool setDeadlockWindow(std::optional<uint64_t> window);
  bool setMaxDomainCycles(const std::map<std::string, uint64_t> &limits);
  bool setRuntimeLimits(const RuntimeLimits &limits);
  bool setTimeDomains(std::span<const TimeDomainRuntime> domains);
  void setBuildProfile(BuildProfile profile) { profile_ = profile; }
  BuildProfile buildProfile() const { return profile_; }
  uint64_t workInvocationCount() const;
  uint64_t activationTraversalCount() const;
  uint64_t workClosureTraversalCount() const;
  const std::vector<TimelineEvent> &timeline() const;
  const std::vector<CommitEvent> &commitTimeline() const;

private:
  std::unique_ptr<Module> root_;
  Epoch epoch_;
  bool terminated_ = false;
  TerminationResult result_;

  Tick maxTicks_ = UINT64_MAX;
  uint64_t maxEvents_ = UINT64_MAX;
  uint64_t maxTraceRecords_ = UINT64_MAX;
  BuildProfile profile_ = BuildProfile::Fast;

  struct Impl;
  std::unique_ptr<Impl> impl_;

  bool fail(std::string code, std::string message);
  TerminationResult runLegacy();
  std::vector<SimObject *> runtimeObjects() const;
  void refreshRuntimeSummary();
  bool stopAtTraceCap();
  bool validateRuntimeIdentities();
};

} // namespace gfsim

#endif // GFSIM_OBJECT_H
