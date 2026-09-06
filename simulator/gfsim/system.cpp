#include "gfsim/object.h"
#include "gfsim/pto_trace.h"
#include "gfsim/queue.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace gfsim {

struct SimSystem::Impl {
  std::map<ObjectId, SimObject *> objects;
  std::map<Epoch, std::set<ObjectId>> scheduledWork;
  std::map<Epoch, std::set<ObjectId>> scheduledExternalXfer;
  EventQueue eventQueue{"events", kInvalidObjectId, nullptr};
  DispatchTable dispatch;
  ActivationPlan activation;
  ActivationPlan workClosure;
  LegacyDispatchTable legacyDispatch;
  LegacyActivationGraph legacyActivation;
  uint64_t committedEventCount = 0;
  uint64_t workInvocations = 0;
  uint64_t activationTraversals = 0;
  uint64_t workClosureTraversals = 0;
  bool executingEpoch = false;
  std::optional<ObjectId> activeProposalOwner;
  NoProgressReport noProgress;
  size_t traceOwnerCount = 0;
  bool traceEof = true;
  bool preflightValidated = false;
  std::map<ObjectId, Tick> lastCommitTick;
  std::optional<Tick> eventQueueLastCommitTick;
  std::map<std::string, TimeDomainRuntime> timeDomains;
  std::map<std::string, uint64_t> maxDomainCycles;
  std::map<std::string, uint64_t> domainCycles;
  std::map<std::string, StatSnapshot, std::less<>> generatedStats;
  std::vector<TimelineEvent> timeline;
  std::vector<CommitEvent> commitTimeline;
  ObservationRecorder observations;
  PtoTraceProvider ptoTrace;
  std::optional<uint64_t> deadlockWindow;
  Tick lastProgressTick = 0;
};

SimSystem::~SimSystem() = default;

SimSystem::SimSystem(std::string name)
    : SimObject(ObjectKind::System, std::move(name), kSystemObjectId),
      root_(std::make_unique<Module>("root", kRootObjectId, this)),
      impl_(std::make_unique<Impl>()) {
  setPath("/" + std::string(this->name()));
  root_->setPath(std::string(path()));
  impl_->objects[kSystemObjectId] = this;
}

bool SimSystem::fail(std::string code, std::string message) {
  terminated_ = true;
  impl_->executingEpoch = false;
  result_.classification = TerminationClass::Failed;
  result_.finalEpoch = epoch_;
  result_.committedEventCount = impl_->committedEventCount;
  result_.domainCycles = impl_->domainCycles;
  result_.diagnosticCode = std::move(code);
  result_.message = std::move(message);
  return false;
}

std::vector<SimObject *> SimSystem::runtimeObjects() const {
  std::map<ObjectId, SimObject *> objects;
  for (const auto &[id, object] : impl_->objects)
    if (id != kSystemObjectId && object)
      objects[id] = object;
  root_->walk([&](const SimObject &object) {
    if (object.kind() != ObjectKind::Module)
      objects[object.id()] = const_cast<SimObject *>(&object);
  });
  for (ObjectId id = 0; id < impl_->dispatch.size(); ++id)
    if (const DispatchRow *row = impl_->dispatch.lookup(id))
      objects[id] = static_cast<SimObject *>(row->object);

  std::vector<SimObject *> result;
  result.reserve(objects.size());
  for (const auto &[id, object] : objects)
    result.push_back(object);
  return result;
}

bool SimSystem::validateRuntimeIdentities() {
  std::map<ObjectId, const SimObject *> ids;
  std::map<std::string, const SimObject *> paths;
  std::string conflict;
  auto record = [&](const SimObject *object) {
    if (!object || !conflict.empty() || object == this)
      return;
    if (object->id() == kInvalidObjectId && object->asModule() == nullptr) {
      conflict = "runtime object has the invalid object ID";
      return;
    }
    if (object->id() != kInvalidObjectId) {
      if (auto [position, inserted] = ids.emplace(object->id(), object);
          !inserted && position->second != object) {
        conflict = "stable object ID " + std::to_string(object->id()) +
                   " names more than one runtime object";
        return;
      }
    }
    if (object->path().empty())
      return;
    if (auto [position, inserted] =
            paths.emplace(std::string(object->path()), object);
        !inserted && position->second != object)
      conflict = "canonical object path " + std::string(object->path()) +
                 " names more than one runtime object";
  };

  for (const auto &[id, object] : impl_->objects)
    record(object);
  root_->walk([&](const SimObject &object) { record(&object); });
  for (ObjectId id = 0; id < impl_->dispatch.size(); ++id)
    if (const DispatchRow *row = impl_->dispatch.lookup(id))
      record(static_cast<const SimObject *>(row->object));
  if (!conflict.empty())
    return fail("duplicate_object_identity", std::move(conflict));
  impl_->preflightValidated = true;
  return true;
}

void SimSystem::refreshRuntimeSummary() {
  impl_->noProgress = {};
  impl_->noProgress.nextEvent = impl_->eventQueue.nextEvent();
  impl_->traceOwnerCount = 0;
  impl_->traceEof = true;

  for (SimObject *object : runtimeObjects()) {
    RuntimeObjectState state = object->runtimeState(epoch_);
    if (state.traceOwner) {
      ++impl_->traceOwnerCount;
      impl_->traceEof = impl_->traceEof && state.traceEof;
      impl_->noProgress.tracePosition = state.tracePosition;
      impl_->noProgress.lastCommittedSequenceId =
          state.traceLastCommittedSequenceId;
    }
    impl_->noProgress.queueOccupancy += state.queueOccupancy;
    impl_->noProgress.pendingOffers += state.pendingOffers;
    impl_->noProgress.activeReservations += state.activeReservations;
    if (state.quiescent)
      continue;
    impl_->noProgress.blockedObjects.push_back(
        {.id = object->id(),
         .path = std::string(object->path()),
         .reason = std::move(state.reason),
         .subscriptions = std::move(state.subscriptions),
         .dependencyChain = std::move(state.dependencyChain),
         .correlationChain = std::move(state.correlationChain),
         .queueOccupancy = state.queueOccupancy,
         .pendingOffers = state.pendingOffers,
         .activeReservations = state.activeReservations,
         .protocolState = std::move(state.protocolState)});
  }
  result_.tracePosition = impl_->noProgress.tracePosition;
  result_.traceLastCommittedSequenceId =
      impl_->noProgress.lastCommittedSequenceId;
  if (!impl_->noProgress.blockedObjects.empty())
    impl_->noProgress.summary =
        "unfinished runtime state has no scheduled wake or future event";
}

bool SimSystem::stopAtTraceCap() {
  refreshRuntimeSummary();
  if (impl_->traceOwnerCount > 1) {
    fail("multiple_trace_owners",
         "the runtime must have exactly one committed trace cursor owner");
    return true;
  }
  if (impl_->traceOwnerCount == 0 || impl_->traceEof ||
      result_.tracePosition < maxTraceRecords_)
    return false;
  terminated_ = true;
  impl_->executingEpoch = false;
  result_.classification = TerminationClass::Incomplete;
  result_.finalEpoch = epoch_;
  result_.committedEventCount = impl_->committedEventCount;
  result_.domainCycles = impl_->domainCycles;
  result_.terminationCap = maxTraceRecords_;
  result_.diagnosticCode = "max_trace_records_reached";
  return true;
}

NoProgressReport SimSystem::noProgressReport() const {
  return impl_->noProgress;
}

std::vector<StatSnapshot> SimSystem::statistics() const {
  std::vector<StatSnapshot> snapshots;
  for (const SimObject *object : runtimeObjects())
    object->collectStatistics(snapshots);
  for (const auto &[name, snapshot] : impl_->generatedStats)
    snapshots.push_back(snapshot);
  std::stable_sort(snapshots.begin(), snapshots.end(),
                   [](const StatSnapshot &left, const StatSnapshot &right) {
                     return std::tie(left.objectPath, left.name) <
                            std::tie(right.objectPath, right.name);
                   });
  return snapshots;
}

std::span<const CommittedEvent> SimSystem::observations() const {
  return impl_->observations.events();
}

bool SimSystem::proposeObservation(EventProposal proposal) {
  if (terminated_ || !impl_->executingEpoch || !impl_->activeProposalOwner)
    return false;
  if (proposal.ownerId != *impl_->activeProposalOwner ||
      !lookup(proposal.ownerId))
    return fail("invalid_observation_owner",
                "observation owner must be the active runtime object");
  if (!impl_->observations.propose(std::move(proposal)))
    return fail("invalid_observation_proposal",
                std::string(impl_->observations.lastError()));
  return true;
}

void SimSystem::registerObject(SimObject *obj) {
  if (!obj || obj->id() == kInvalidObjectId ||
      impl_->objects.contains(obj->id())) {
    fail("invalid_object_registration",
         "runtime object registration is null, invalid, or duplicate");
    return;
  }
  impl_->objects[obj->id()] = obj;
  impl_->preflightValidated = false;
}

bool SimSystem::setDispatchTable(std::span<const DispatchRow> rows) {
  DispatchTable candidate(rows);
  if (!candidate.validate())
    return fail("invalid_dispatch_table",
                "dispatch rows must be complete and densely indexed");
  impl_->dispatch = candidate;
  impl_->activation = ActivationPlan{};
  impl_->workClosure = ActivationPlan{};
  impl_->preflightValidated = false;
  return true;
}

bool SimSystem::setActivationPlan(std::span<const uint32_t> offsets,
                                  std::span<const ObjectId> targets) {
  ActivationPlan candidate(offsets, targets);
  if (!candidate.validate(impl_->dispatch.size()))
    return fail("invalid_activation_plan",
                "activation offsets and targets must be canonical and dense");
  impl_->activation = candidate;
  return true;
}

bool SimSystem::setWorkClosurePlan(std::span<const uint32_t> offsets,
                                   std::span<const ObjectId> targets) {
  ActivationPlan candidate(offsets, targets);
  if (!candidate.validate(impl_->dispatch.size()))
    return fail("invalid_work_closure_plan",
                "Work closure offsets and targets must be canonical and dense");
  impl_->workClosure = candidate;
  return true;
}

bool SimSystem::setLegacyDispatchTable(LegacyDispatchTable table) {
  if ((!table.rows && table.objectCount != 0) ||
      (table.rows && table.objectCount == 0))
    return fail("invalid_dispatch_table",
                "legacy dispatch storage and object count disagree");
  for (uint32_t id = 0; id < table.objectCount; ++id) {
    const LegacyDispatchThunk &row = table.rows[id];
    if (!row.object || !row.work || !row.xfer || !row.reset || !row.validate ||
        !row.validate(row.object))
      return fail("invalid_dispatch_table",
                  "legacy dispatch rows must be complete and valid");
  }
  impl_->legacyDispatch = table;
  impl_->legacyActivation = {};
  impl_->preflightValidated = false;
  return true;
}

bool SimSystem::setLegacyActivationGraph(LegacyActivationGraph graph) {
  if (!graph.offsets || graph.sourceCount != impl_->legacyDispatch.objectCount)
    return fail("invalid_activation_plan",
                "legacy activation graph must cover every dispatch row");
  const uint32_t targetCount = graph.offsets[graph.sourceCount];
  if (targetCount != 0 && !graph.targets)
    return fail("invalid_activation_plan",
                "legacy activation targets are missing");
  for (uint32_t source = 0; source < graph.sourceCount; ++source) {
    if (graph.offsets[source] > graph.offsets[source + 1])
      return fail("invalid_activation_plan",
                  "legacy activation offsets must be monotonic");
    for (uint32_t index = graph.offsets[source];
         index < graph.offsets[source + 1]; ++index)
      if (graph.targets[index] >= graph.sourceCount)
        return fail("invalid_activation_plan",
                    "legacy activation target is out of range");
  }
  impl_->legacyActivation = graph;
  return true;
}

bool SimSystem::setTimeDomains(std::span<const TimeDomainRuntime> domains) {
  if (terminated_)
    return false;
  std::map<std::string, TimeDomainRuntime> candidate;
  std::string previous;
  for (const TimeDomainRuntime &domain : domains) {
    if (domain.name.empty() || domain.period == 0 || domain.tickScale == 0 ||
        (!previous.empty() && previous >= domain.name) ||
        !candidate.emplace(domain.name, domain).second)
      return fail("invalid_time_domains",
                  "time domains must be sorted, unique, and positive");
    previous = domain.name;
  }
  impl_->timeDomains = std::move(candidate);
  impl_->domainCycles.clear();
  for (const auto &[name, domain] : impl_->timeDomains)
    impl_->domainCycles.emplace(name, 0);
  return true;
}

bool SimSystem::setDeadlockWindow(std::optional<uint64_t> window) {
  if (terminated_)
    return false;
  if (window && *window == 0)
    return fail("invalid_runtime_limits",
                "the deadlock window must be positive");
  impl_->deadlockWindow = window;
  impl_->lastProgressTick = epoch_.time;
  return true;
}

bool SimSystem::setMaxDomainCycles(
    const std::map<std::string, uint64_t> &limits) {
  if (terminated_)
    return false;
  for (const auto &[name, maximum] : limits)
    if (maximum == 0 || !impl_->timeDomains.contains(name))
      return fail("invalid_runtime_limits",
                  "domain limits must name configured time domains");
  impl_->maxDomainCycles = limits;
  return true;
}

bool SimSystem::setRuntimeLimits(const RuntimeLimits &limits) {
  if (terminated_)
    return false;
  if (limits.maxTicks && *limits.maxTicks == 0)
    return fail("invalid_runtime_limits", "runtime limits must be positive");
  if (!setDeadlockWindow(limits.deadlockWindow) ||
      !setMaxDomainCycles(limits.maxDomainCycles))
    return false;
  maxTicks_ = limits.maxTicks.value_or(UINT64_MAX);
  return true;
}

SimObject *SimSystem::lookup(ObjectId id) const {
  if (const DispatchRow *row = impl_->dispatch.lookup(id))
    return static_cast<SimObject *>(row->object);
  auto it = impl_->objects.find(id);
  return it != impl_->objects.end() ? it->second : nullptr;
}

void SimSystem::requestTerminate(TerminationClass classification,
                                 std::string diagnosticCode) {
  terminated_ = true;
  impl_->executingEpoch = false;
  result_.classification = classification;
  result_.finalEpoch = epoch_;
  result_.committedEventCount = impl_->committedEventCount;
  result_.domainCycles = impl_->domainCycles;
  result_.diagnosticCode = std::move(diagnosticCode);
}

void SimSystem::recordStat(std::string name, uint64_t value) {
  StatSnapshot snapshot;
  snapshot.name = name;
  snapshot.objectPath = std::string(path());
  snapshot.kind = StatisticKind::Counter;
  snapshot.value = value;
  snapshot.lastUpdate = epoch_;
  impl_->generatedStats.insert_or_assign(std::move(name), std::move(snapshot));
}

void SimSystem::recordTraceEvent(std::string lane, std::string phase,
                                 uint64_t handle) {
  TimelineEvent event;
  event.epoch = epoch_;
  event.lane = std::move(lane);
  event.phase = std::move(phase);
  event.handle = handle;
  try {
    uint64_t descriptor = impl_->ptoTrace.decode(handle);
    using D = PtoScheduleDescriptor;
    event.sequence = (descriptor >> D::kSequenceShift) & 0xffu;
    event.opcode = (descriptor >> D::kOpcodeShift) & D::kOpcodeMask;
    event.dependencyValid = (descriptor >> D::kDependencyValidShift) & 7u;
    event.dependencies[0] = (descriptor >> D::kDependency0Shift) & 0xffu;
    event.dependencies[1] = (descriptor >> D::kDependency1Shift) & 0xffu;
    event.dependencies[2] = (descriptor >> D::kDependency2Shift) & 0xffu;
  } catch (const std::runtime_error &) {
  }
  impl_->timeline.push_back(std::move(event));
}

void SimSystem::recordTraceCounter(std::string lane, uint64_t value) {
  TimelineEvent event;
  event.epoch = epoch_;
  event.lane = std::move(lane);
  event.phase = "occupancy";
  event.handle = value;
  event.counter = true;
  impl_->timeline.push_back(std::move(event));
}

namespace {
std::string jsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char ch : text) {
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      out.push_back(static_cast<char>(ch));
      break;
    }
  }
  return out;
}

int timelineTid(std::string_view lane) {
  if (lane == "Vector")
    return 1;
  if (lane == "Cube")
    return 2;
  if (lane == "Tlsu")
    return 3;
  return 13;
}

int occupancyTid(std::string_view lane) {
  if (lane == "IQVector")
    return 4;
  if (lane == "IQCube")
    return 5;
  if (lane == "IQTlsu")
    return 6;
  if (lane == "ROB")
    return 7;
  return 0;
}

std::string_view laneDisplay(std::string_view lane) {
  if (lane == "IQVector")
    return "IQ Vector";
  if (lane == "IQCube")
    return "IQ Cube";
  if (lane == "IQTlsu")
    return "IQ Tlsu";
  return lane;
}

std::string occupancyTrackName(std::string_view lane) {
  if (lane == "ROB")
    return "ROB occupancy";
  return std::string(laneDisplay(lane)) + " occupancy";
}

bool keepSliceEvent(const TimelineEvent &event) {
  if (event.counter)
    return false;
  return event.lane == "Vector" || event.lane == "Cube" || event.lane == "Tlsu";
}

bool keepOccupancyEvent(const TimelineEvent &event) {
  return event.counter && occupancyTid(event.lane) != 0;
}

bool isEngineLane(std::string_view lane) {
  return lane == "Scalar" || lane == "Vector" || lane == "Cube" ||
         lane == "Tlsu";
}

const char *chromePhase(std::string_view phase) {
  if (phase == "begin")
    return "B";
  if (phase == "end")
    return "E";
  return "i";
}

struct FlowAnchor {
  uint64_t ts = 0;
  int tid = 0;
  int rank = 0;
};

// Perfetto binds ph=s/f to the currently open slice on that track. Rank
// Engine-begin highest so the arrow attaches while the duration slice is open.
int flowRank(std::string_view lane, std::string_view phase) {
  if (phase == "begin" && isEngineLane(lane))
    return 3;
  if (phase == "issue")
    return 2;
  if (phase == "complete")
    return 1;
  return 0;
}

void considerAnchor(FlowAnchor &anchor, int rank, uint64_t ts, int tid) {
  if (rank >= anchor.rank) {
    anchor.rank = rank;
    anchor.ts = ts;
    anchor.tid = tid;
  }
}

void emitArgs(std::ostringstream &os, const TimelineEvent &event) {
  os << "\"args\":{\"seq\":" << event.sequence << ",\"opcode\":" << event.opcode
     << ",\"handle\":" << event.handle;
  bool first = true;
  for (unsigned index = 0; index < 3; ++index) {
    if ((event.dependencyValid & (1u << index)) == 0)
      continue;
    if (first) {
      os << ",\"deps\":[";
      first = false;
    } else {
      os << ',';
    }
    os << static_cast<unsigned>(event.dependencies[index]);
  }
  if (!first)
    os << ']';
  os << "}";
}

struct JsonEvt {
  uint64_t ts = 0;
  int order = 0;
  size_t index = 0;
  std::string body;
};

constexpr int kOrderC = 0;
constexpr int kOrderB = 1;
constexpr int kOrderS = 2;
constexpr int kOrderF = 3;
constexpr int kOrderI = 4;
constexpr int kOrderE = 5;
} // namespace

std::string SimSystem::chromeTraceJson() const {
  static constexpr std::array<std::pair<int, const char *>, 3> kSliceLanes = {{
      {1, "Vector"},
      {2, "Cube"},
      {3, "Tlsu"},
  }};
  static constexpr std::array<std::pair<int, const char *>, 4> kOccupancyLanes =
      {{{4, "IQ Vector occupancy"},
        {5, "IQ Cube occupancy"},
        {6, "IQ Tlsu occupancy"},
        {7, "ROB occupancy"}}};
  std::array<FlowAnchor, 256> anchor{};
  std::array<uint8_t, 256> dependencyValid{};
  std::array<std::array<uint8_t, 3>, 256> dependencies{};
  std::vector<JsonEvt> events;
  events.reserve(impl_->timeline.size() * 2);

  std::ostringstream os;
  os << "{\"displayTimeUnit\":\"ns\",\"traceEvents\":[";
  os << "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":"
        "{\"name\":\"DavinciOO\"}}";
  for (const auto &[tid, name] : kSliceLanes) {
    os << ",{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << tid
       << ",\"args\":{\"name\":\"" << name << "\"}}";
  }
  for (const auto &[tid, name] : kOccupancyLanes) {
    os << ",{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << tid
       << ",\"args\":{\"name\":\"" << name << "\"}}";
  }
  size_t nextIndex = 0;
  auto push = [&](uint64_t ts, int order, std::string body) {
    events.push_back(JsonEvt{ts, order, nextIndex++, std::move(body)});
  };

  for (const TimelineEvent &event : impl_->timeline) {
    uint64_t ts = event.epoch.time * 1000ull + event.epoch.delta;
    if (event.counter) {
      if (!keepOccupancyEvent(event))
        continue;
      int tid = occupancyTid(event.lane);
      std::string track = occupancyTrackName(event.lane);
      std::ostringstream body;
      body << "{\"name\":\"" << jsonEscape(track) << "\",\"cat\":\""
           << jsonEscape(event.lane) << "\",\"ph\":\"C\",\"ts\":" << ts
           << ",\"pid\":1,\"tid\":" << tid
           << ",\"args\":{\"occupancy\":" << event.handle << "}}";
      push(ts, kOrderC, body.str());
      continue;
    }
    if (!keepSliceEvent(event))
      continue;
    const char *phase = chromePhase(event.phase);
    const std::string name = (phase[0] == 'B' || phase[0] == 'E')
                                 ? std::string(laneDisplay(event.lane))
                                 : event.phase;
    int tid = timelineTid(event.lane);
    std::ostringstream body;
    body << "{\"name\":\"" << jsonEscape(name) << "\",\"cat\":\""
         << jsonEscape(event.lane) << "\",\"ph\":\"" << phase
         << "\",\"ts\":" << ts << ",\"pid\":1,\"tid\":" << tid << ",";
    emitArgs(body, event);
    body << "}";
    int order = kOrderI;
    if (phase[0] == 'B')
      order = kOrderB;
    else if (phase[0] == 'E')
      order = kOrderE;
    push(ts, order, body.str());
    if (event.sequence < anchor.size()) {
      considerAnchor(anchor[event.sequence], flowRank(event.lane, event.phase),
                     ts, tid);
      dependencyValid[event.sequence] = event.dependencyValid;
      dependencies[event.sequence] = {
          event.dependencies[0], event.dependencies[1], event.dependencies[2]};
    }
  }

  uint64_t flowId = 1;
  for (size_t seq = 0; seq < anchor.size(); ++seq) {
    if (anchor[seq].rank == 0 || dependencyValid[seq] == 0)
      continue;
    for (unsigned index = 0; index < 3; ++index) {
      if ((dependencyValid[seq] & (1u << index)) == 0)
        continue;
      unsigned producer = dependencies[seq][index];
      if (producer >= anchor.size() || anchor[producer].rank == 0)
        continue;
      std::ostringstream start;
      start << "{\"name\":\"dep\",\"cat\":\"dep\",\"ph\":\"s\",\"id\":"
            << flowId << ",\"pid\":1,\"tid\":" << anchor[producer].tid
            << ",\"ts\":" << anchor[producer].ts
            << ",\"args\":{\"from\":" << producer << ",\"to\":" << seq << "}}";
      std::ostringstream finish;
      finish << "{\"name\":\"dep\",\"cat\":\"dep\",\"ph\":\"f\",\"id\":"
             << flowId << ",\"pid\":1,\"tid\":" << anchor[seq].tid
             << ",\"ts\":" << anchor[seq].ts
             << ",\"bp\":\"e\",\"args\":{\"from\":" << producer
             << ",\"to\":" << seq << "}}";
      push(anchor[producer].ts, kOrderS, start.str());
      push(anchor[seq].ts, kOrderF, finish.str());
      ++flowId;
    }
  }

  std::sort(events.begin(), events.end(),
            [](const JsonEvt &lhs, const JsonEvt &rhs) {
              if (lhs.ts != rhs.ts)
                return lhs.ts < rhs.ts;
              if (lhs.order != rhs.order)
                return lhs.order < rhs.order;
              return lhs.index < rhs.index;
            });
  for (const JsonEvt &event : events)
    os << ',' << event.body;
  os << "]}";
  return os.str();
}

void SimSystem::loadPtoTrace(std::string source, const std::string &path) {
  impl_->ptoTrace.load(std::move(source), path);
}

uint64_t SimSystem::traceOpen(std::string_view source) const {
  return impl_->ptoTrace.open(source);
}

TraceNextResult SimSystem::traceNext(std::string_view source,
                                     uint64_t cursor) const {
  return impl_->ptoTrace.next(source, static_cast<size_t>(cursor));
}

uint64_t SimSystem::traceDecode(uint64_t handle) const {
  return impl_->ptoTrace.decode(handle);
}

bool SimSystem::traceEof(std::string_view source, uint64_t cursor) const {
  return impl_->ptoTrace.eof(source, static_cast<size_t>(cursor));
}

uint64_t SimSystem::tracePosition(std::string_view source,
                                  uint64_t cursor) const {
  return impl_->ptoTrace.position(source, static_cast<size_t>(cursor));
}

uint64_t SimSystem::traceRecordCount(std::string_view source) const {
  return impl_->ptoTrace.recordCount(source);
}

uint64_t SimSystem::workInvocationCount() const {
  return impl_->workInvocations;
}

uint64_t SimSystem::activationTraversalCount() const {
  return impl_->activationTraversals;
}

uint64_t SimSystem::workClosureTraversalCount() const {
  return impl_->workClosureTraversals;
}

const std::vector<TimelineEvent> &SimSystem::timeline() const {
  return impl_->timeline;
}

const std::vector<CommitEvent> &SimSystem::commitTimeline() const {
  return impl_->commitTimeline;
}

bool SimSystem::scheduleWork(ObjectId id, Epoch epoch) {
  if (terminated_)
    return false;
  if (epoch.delta >= kMaxDeltasPerTick)
    return fail("max_deltas_exceeded",
                "scheduled work exceeds the causal delta limit");
  if (epoch < epoch_)
    return fail("work_before_current_epoch",
                "work cannot be scheduled before the committed epoch");
  const bool hasLegacy = impl_->legacyDispatch.rows &&
                         id < impl_->legacyDispatch.objectCount &&
                         impl_->legacyDispatch.rows[id].object;
  if (!lookup(id) && !hasLegacy)
    return fail("unknown_work_target",
                "work target is absent from the static dispatch table");
  if (impl_->executingEpoch && epoch == epoch_) {
    if (epoch_.delta + 1 >= kMaxDeltasPerTick)
      return fail("max_deltas_exceeded",
                  "causal continuation exceeds the delta limit");
    epoch = epoch_.nextDelta();
  }
  impl_->scheduledWork[epoch].insert(id);
  return true;
}

bool SimSystem::scheduleExternalXfer(ObjectId id) {
  if (terminated_)
    return false;
  if (impl_->executingEpoch)
    return fail("external_xfer_during_work",
                "external Queue transfer cannot enter a frozen Work epoch");
  SimObject *object = lookup(id);
  if (!object || (object->kind() != ObjectKind::Queue &&
                  object->kind() != ObjectKind::EventQueue))
    return fail("invalid_external_xfer_target",
                "external transfer target must be a dispatched Queue");
  impl_->scheduledExternalXfer[epoch_].insert(id);
  return true;
}

bool SimSystem::scheduleEvent(Event event) {
  if (terminated_)
    return false;
  if (event.readyTime.delta >= kMaxDeltasPerTick)
    return fail("max_deltas_exceeded",
                "scheduled event exceeds the causal delta limit");
  if (event.readyTime < epoch_)
    return fail("event_before_current_epoch",
                "events cannot be scheduled before the committed epoch");
  if (!lookup(event.targetId))
    return fail("unknown_event_target",
                "event target is absent from the static dispatch table");
  if (!impl_->eventQueue.proposeSchedule(event))
    return fail("event_queue_capacity_exceeded",
                "the global event queue capacity was exceeded");
  return true;
}

std::optional<Event> SimSystem::nextEvent() const {
  return impl_->eventQueue.nextEvent();
}

bool SimSystem::step() {
  if (terminated_)
    return false;
  if (!impl_->preflightValidated && !validateRuntimeIdentities())
    return false;
  if (stopAtTraceCap())
    return false;

  if (epoch_.time >= maxTicks_) {
    terminated_ = true;
    result_.classification = TerminationClass::Incomplete;
    result_.finalEpoch = epoch_;
    result_.committedEventCount = impl_->committedEventCount;
    result_.domainCycles = impl_->domainCycles;
    result_.terminationCap = maxTicks_;
    result_.diagnosticCode = "max_ticks_reached";
    return false;
  }

  if (epoch_.delta == 0) {
    for (const auto &[name, domain] : impl_->timeDomains) {
      if (epoch_.time < domain.phase ||
          (epoch_.time - domain.phase) % domain.period != 0)
        continue;
      uint64_t &cycles = impl_->domainCycles[name];
      if (auto maximum = impl_->maxDomainCycles.find(name);
          maximum != impl_->maxDomainCycles.end() &&
          cycles >= maximum->second) {
        terminated_ = true;
        impl_->executingEpoch = false;
        result_.classification = TerminationClass::Incomplete;
        result_.finalEpoch = epoch_;
        result_.committedEventCount = impl_->committedEventCount;
        result_.terminationCap = maximum->second;
        result_.domainCycles = impl_->domainCycles;
        result_.diagnosticCode = "max_domain_cycles_reached";
        return false;
      }
      ++cycles;
    }
  }

  auto stopAtEventCap = [this] {
    if (impl_->committedEventCount < maxEvents_)
      return false;
    terminated_ = true;
    impl_->executingEpoch = false;
    result_.classification = TerminationClass::Incomplete;
    result_.finalEpoch = epoch_;
    result_.committedEventCount = impl_->committedEventCount;
    result_.domainCycles = impl_->domainCycles;
    result_.terminationCap = maxEvents_;
    result_.diagnosticCode = "max_events_reached";
    return true;
  };

  if (stopAtEventCap())
    return false;

  // Events committed by a previous epoch activate their target at their exact
  // ready epoch before the immutable Work snapshot is observed.
  while (auto event = impl_->eventQueue.nextEvent()) {
    if (event->readyTime < epoch_)
      return fail("event_before_current_epoch",
                  "the event queue contains a stale event");
    if (event->readyTime != epoch_)
      break;
    if (stopAtEventCap())
      return false;
    impl_->eventQueue.popNext();
    if (!scheduleWork(event->targetId, epoch_))
      return false;
    ++impl_->committedEventCount;
    impl_->lastProgressTick = epoch_.time;
  }

  std::set<ObjectId> currentWork;
  if (auto current = impl_->scheduledWork.find(epoch_);
      current != impl_->scheduledWork.end()) {
    currentWork = std::move(current->second);
    impl_->scheduledWork.erase(current);
  }

  std::set<ObjectId> xferClosure = currentWork;
  if (auto current = impl_->scheduledExternalXfer.find(epoch_);
      current != impl_->scheduledExternalXfer.end()) {
    xferClosure.insert(current->second.begin(), current->second.end());
    impl_->scheduledExternalXfer.erase(current);
  }
  if (!impl_->workClosure.empty())
    for (ObjectId worker : currentWork)
      for (ObjectId resource : impl_->workClosure.targetsFor(worker)) {
        ++impl_->workClosureTraversals;
        xferClosure.insert(resource);
      }

  impl_->executingEpoch = true;
  for (ObjectId id : currentWork) {
    ++impl_->workInvocations;
    impl_->activeProposalOwner = id;
    const LegacyDispatchThunk *legacy =
        impl_->legacyDispatch.rows && id < impl_->legacyDispatch.objectCount
            ? &impl_->legacyDispatch.rows[id]
            : nullptr;
    if (legacy)
      legacy->work(legacy->object, epoch_);
    else if (const DispatchRow *row = impl_->dispatch.lookup(id))
      row->work(row->object, epoch_);
    else if (SimObject *object = lookup(id))
      object->doWork(epoch_);
    impl_->activeProposalOwner.reset();
    if (terminated_)
      return false;
  }

  auto arbitrate = [&](ObjectId id) {
    impl_->activeProposalOwner = id;
    const bool hasLegacy =
        impl_->legacyDispatch.rows && id < impl_->legacyDispatch.objectCount;
    if (!hasLegacy) {
      if (const DispatchRow *row = impl_->dispatch.lookup(id))
        row->xfer(row->object, epoch_, XferPhase::Arbitrate);
      else if (SimObject *object = lookup(id))
        object->doArbitrate(epoch_);
    }
    impl_->activeProposalOwner.reset();
  };
  for (ObjectId id : currentWork) {
    arbitrate(id);
    if (terminated_)
      return false;
  }
  for (ObjectId id : xferClosure) {
    if (currentWork.contains(id))
      continue;
    arbitrate(id);
    if (terminated_)
      return false;
  }

  std::vector<ObjectId> committedSources;
  std::map<ObjectId, bool> pendingCommits;
  for (ObjectId id : xferClosure) {
    SimObject *object = lookup(id);
    const DispatchRow *row = impl_->dispatch.lookup(id);
    const LegacyDispatchThunk *legacy =
        impl_->legacyDispatch.rows && id < impl_->legacyDispatch.objectCount
            ? &impl_->legacyDispatch.rows[id]
            : nullptr;
    bool willCommit =
        legacy ? true
               : (row ? row->xfer(row->object, epoch_, XferPhase::Probe)
                      : object && object->hasPendingCommit());
    pendingCommits[id] = willCommit;
    if (willCommit) {
      auto previousCommit = impl_->lastCommitTick.find(id);
      if (!legacy && previousCommit != impl_->lastCommitTick.end() &&
          previousCommit->second == epoch_.time)
        return fail("multiple_stateful_commits",
                    "a stateful object cannot commit twice in one tick");
    }
  }

  for (ObjectId id : xferClosure) {
    SimObject *object = lookup(id);
    const DispatchRow *row = impl_->dispatch.lookup(id);
    const LegacyDispatchThunk *legacy =
        impl_->legacyDispatch.rows && id < impl_->legacyDispatch.objectCount
            ? &impl_->legacyDispatch.rows[id]
            : nullptr;
    const bool willCommit = pendingCommits.at(id);
    bool committed = false;
    if (legacy) {
      legacy->xfer(legacy->object, epoch_);
      committed = true;
    } else if (row)
      committed = row->xfer(row->object, epoch_, XferPhase::Commit);
    else if (object) {
      object->doXfer(epoch_);
      committed = willCommit;
    }
    if (committed != willCommit)
      return fail("xfer_probe_mismatch",
                  "Xfer pending state changed between probe and commit");
    if (committed) {
      const bool semanticChanged = !object || object->lastCommitChanged();
      if (profile_ == BuildProfile::Validated)
        impl_->commitTimeline.push_back({epoch_, id, semanticChanged});
      if (semanticChanged)
        committedSources.push_back(id);
      impl_->lastCommitTick[id] = epoch_.time;
      impl_->lastProgressTick = epoch_.time;
    }
    if (terminated_)
      return false;
    if (object && !object->runtimeFailureCode().empty())
      return fail(std::string(object->runtimeFailureCode()),
                  "runtime object reported a committed failure");
    if (committed) {
      if (!impl_->observations.commitOwner(id, epoch_))
        return fail("invalid_observation_commit",
                    std::string(impl_->observations.lastError()));
    } else {
      impl_->observations.rejectOwner(id);
    }
  }
  if (impl_->eventQueue.hasPendingCommit()) {
    if (impl_->eventQueueLastCommitTick == epoch_.time)
      return fail("multiple_stateful_commits",
                  "the event queue cannot commit twice in one tick");
    impl_->eventQueueLastCommitTick = epoch_.time;
    impl_->lastProgressTick = epoch_.time;
  }
  impl_->eventQueue.doXfer(epoch_);

  if (!committedSources.empty() && !impl_->activation.empty()) {
    if (epoch_.time == std::numeric_limits<Tick>::max())
      return fail("tick_overflow", "activation would overflow simulation time");
    Epoch activationEpoch{epoch_.time + 1, 0};
    for (ObjectId source : committedSources)
      for (ObjectId target : impl_->activation.targetsFor(source)) {
        ++impl_->activationTraversals;
        if (!scheduleWork(target, activationEpoch))
          return false;
      }
  }
  if (!committedSources.empty() && impl_->legacyActivation.offsets) {
    if (epoch_.time == std::numeric_limits<Tick>::max())
      return fail("tick_overflow", "activation would overflow simulation time");
    Epoch activationEpoch{epoch_.time + 1, 0};
    for (ObjectId source : committedSources) {
      if (source >= impl_->legacyActivation.sourceCount)
        continue;
      for (uint32_t index = impl_->legacyActivation.offsets[source];
           index < impl_->legacyActivation.offsets[source + 1]; ++index) {
        ++impl_->activationTraversals;
        if (!scheduleWork(impl_->legacyActivation.targets[index],
                          activationEpoch))
          return false;
      }
    }
  }

  // An event committed for the active epoch is a causal continuation. Its
  // target runs at the next delta, never inside the closed Work snapshot.
  while (auto event = impl_->eventQueue.nextEvent()) {
    if (event->readyTime < epoch_)
      return fail("event_before_current_epoch",
                  "the event queue contains a stale event");
    if (event->readyTime != epoch_)
      break;
    if (stopAtEventCap())
      return false;
    impl_->eventQueue.popNext();
    if (!scheduleWork(event->targetId, epoch_))
      return false;
    ++impl_->committedEventCount;
    impl_->lastProgressTick = epoch_.time;
  }
  impl_->executingEpoch = false;
  impl_->activeProposalOwner.reset();

  std::optional<Epoch> nextEpoch;
  bool nextEpochIsEvent = false;
  if (!impl_->scheduledWork.empty())
    nextEpoch = impl_->scheduledWork.begin()->first;
  if (!impl_->scheduledExternalXfer.empty() &&
      (!nextEpoch || impl_->scheduledExternalXfer.begin()->first < *nextEpoch))
    nextEpoch = impl_->scheduledExternalXfer.begin()->first;
  if (auto event = impl_->eventQueue.nextEvent();
      event && (!nextEpoch || event->readyTime <= *nextEpoch)) {
    nextEpoch = event->readyTime;
    nextEpochIsEvent = true;
  }

  if (!nextEpoch) {
    if (stopAtTraceCap())
      return false;
    refreshRuntimeSummary();
    if (impl_->traceOwnerCount == 1 && impl_->traceEof) {
      std::vector<ObjectId> traceEndProcesses;
      for (SimObject *object : runtimeObjects())
        if (object->requestTraceEnd())
          traceEndProcesses.push_back(object->id());
      if (!traceEndProcesses.empty()) {
        if (epoch_.time == std::numeric_limits<Tick>::max())
          return fail("tick_overflow",
                      "trace-end process termination exceeds tick range");
        Epoch shutdownEpoch{epoch_.time + 1, 0};
        if (shutdownEpoch.time >= maxTicks_) {
          epoch_ = {maxTicks_, 0};
          terminated_ = true;
          result_.classification = TerminationClass::Incomplete;
          result_.finalEpoch = epoch_;
          result_.committedEventCount = impl_->committedEventCount;
          result_.terminationCap = maxTicks_;
          result_.domainCycles = impl_->domainCycles;
          result_.diagnosticCode = "max_ticks_reached";
          return false;
        }
        for (ObjectId id : traceEndProcesses)
          if (!scheduleWork(id, shutdownEpoch))
            return false;
        epoch_ = shutdownEpoch;
        return true;
      }
    }
    if (!impl_->noProgress.blockedObjects.empty() && impl_->deadlockWindow) {
      const Tick window = *impl_->deadlockWindow;
      if (impl_->lastProgressTick > std::numeric_limits<Tick>::max() - window)
        return fail("tick_overflow", "deadlock window exceeds tick range");
      Tick deadline = impl_->lastProgressTick + window;
      if (deadline >= maxTicks_) {
        epoch_ = {maxTicks_, 0};
        terminated_ = true;
        result_.classification = TerminationClass::Incomplete;
        result_.finalEpoch = epoch_;
        result_.committedEventCount = impl_->committedEventCount;
        result_.terminationCap = maxTicks_;
        result_.domainCycles = impl_->domainCycles;
        result_.diagnosticCode = "max_ticks_reached";
        return false;
      }
      epoch_ = {deadline, 0};
      return fail("deadlock_window_reached", impl_->noProgress.summary);
    }
    if (!impl_->noProgress.blockedObjects.empty())
      return fail("no_progress", impl_->noProgress.summary);
    terminated_ = true;
    result_.classification = TerminationClass::Completed;
    result_.finalEpoch = epoch_;
    result_.committedEventCount = impl_->committedEventCount;
    result_.domainCycles = impl_->domainCycles;
    return false;
  }
  if (*nextEpoch <= epoch_)
    return fail("non_monotonic_epoch",
                "scheduler failed to advance beyond the committed epoch");
  if (!nextEpochIsEvent && impl_->deadlockWindow) {
    const Tick window = *impl_->deadlockWindow;
    if (impl_->lastProgressTick > std::numeric_limits<Tick>::max() - window)
      return fail("tick_overflow", "deadlock window exceeds tick range");
    const Tick deadline = impl_->lastProgressTick + window;
    if (nextEpoch->time >= deadline) {
      epoch_ = {deadline, 0};
      return fail("deadlock_window_reached",
                  "scheduled work made no declared progress within the "
                  "deadlock window");
    }
  }
  if (nextEpoch->time >= maxTicks_) {
    epoch_ = {maxTicks_, 0};
    terminated_ = true;
    result_.classification = TerminationClass::Incomplete;
    result_.finalEpoch = epoch_;
    result_.committedEventCount = impl_->committedEventCount;
    result_.domainCycles = impl_->domainCycles;
    result_.terminationCap = maxTicks_;
    result_.diagnosticCode = "max_ticks_reached";
    return false;
  }
  epoch_ = *nextEpoch;
  return true;
}

TerminationResult SimSystem::runLegacy() {
  epoch_ = {0, 0};
  terminated_ = false;
  result_ = {};
  impl_->scheduledWork.clear();
  impl_->scheduledExternalXfer.clear();
  impl_->committedEventCount = 0;
  impl_->workInvocations = 0;
  impl_->activationTraversals = 0;
  impl_->workClosureTraversals = 0;
  impl_->commitTimeline.clear();
  impl_->generatedStats.clear();

  for (ObjectId id = 0; id < impl_->legacyDispatch.objectCount; ++id)
    if (impl_->legacyDispatch.rows[id].work)
      impl_->scheduledWork[epoch_].insert(id);

  while (!terminated_) {
    if (epoch_.delta >= kMaxDeltasPerTick) {
      result_.classification = TerminationClass::Failed;
      result_.diagnosticCode = "max_deltas_exceeded";
      break;
    }
    if (epoch_.time >= maxTicks_) {
      result_.classification = TerminationClass::Incomplete;
      result_.terminationCap = maxTicks_;
      result_.diagnosticCode = "max_ticks_reached";
      break;
    }
    if (impl_->committedEventCount >= maxEvents_) {
      result_.classification = TerminationClass::Incomplete;
      result_.terminationCap = maxEvents_;
      result_.diagnosticCode = "max_events_reached";
      break;
    }

    std::set<ObjectId> executed;
    if (auto current = impl_->scheduledWork.find(epoch_);
        current != impl_->scheduledWork.end()) {
      executed = std::move(current->second);
      impl_->scheduledWork.erase(current);
    }

    impl_->executingEpoch = true;
    for (ObjectId id : executed) {
      ++impl_->workInvocations;
      LegacyDispatchThunk const &row = impl_->legacyDispatch.rows[id];
      row.work(row.object, epoch_);
      if (terminated_)
        break;
    }
    for (ObjectId id : executed) {
      if (terminated_)
        break;
      LegacyDispatchThunk const &row = impl_->legacyDispatch.rows[id];
      row.xfer(row.object, epoch_);
      if (profile_ == BuildProfile::Validated && !row.validate(row.object)) {
        terminated_ = true;
        result_.classification = TerminationClass::Failed;
        result_.diagnosticCode = "validate_failed";
      }
    }
    impl_->executingEpoch = false;
    if (terminated_)
      break;

    if (impl_->legacyActivation.offsets &&
        epoch_.time != std::numeric_limits<Tick>::max()) {
      Epoch activationEpoch{epoch_.time + 1, 0};
      for (ObjectId source : executed) {
        if (source >= impl_->legacyActivation.sourceCount)
          continue;
        for (uint32_t index = impl_->legacyActivation.offsets[source];
             index < impl_->legacyActivation.offsets[source + 1]; ++index) {
          ++impl_->activationTraversals;
          impl_->scheduledWork[activationEpoch].insert(
              impl_->legacyActivation.targets[index]);
        }
      }
    }

    impl_->eventQueue.doXfer(epoch_);
    while (auto event = impl_->eventQueue.nextEvent()) {
      if (event->readyTime > epoch_)
        break;
      impl_->eventQueue.popNext();
      impl_->scheduledWork[event->readyTime].insert(event->targetId);
      ++impl_->committedEventCount;
    }

    std::optional<Epoch> nextEpoch;
    if (!impl_->scheduledWork.empty())
      nextEpoch = impl_->scheduledWork.begin()->first;
    if (auto event = impl_->eventQueue.nextEvent();
        event && (!nextEpoch || event->readyTime < *nextEpoch))
      nextEpoch = event->readyTime;
    if (!nextEpoch) {
      terminated_ = true;
      result_.classification = TerminationClass::Completed;
      break;
    }
    epoch_ = *nextEpoch;
  }

  result_.finalEpoch = epoch_;
  result_.committedEventCount = impl_->committedEventCount;
  result_.stats = statistics();
  return result_;
}

TerminationResult SimSystem::run() {
  if (impl_->legacyDispatch.rows)
    return runLegacy();

  epoch_ = {0, 0};

  for (SimObject *object : runtimeObjects())
    if (object->kind() == ObjectKind::Process ||
        object->kind() == ObjectKind::TraceSource)
      scheduleWork(object->id(), epoch_);
  if (impl_->legacyDispatch.rows)
    for (ObjectId id = 0; id < impl_->legacyDispatch.objectCount; ++id)
      scheduleWork(id, epoch_);

  while (!terminated_)
    if (!step())
      break;

  result_.finalEpoch = epoch_;
  result_.committedEventCount = impl_->committedEventCount;
  result_.domainCycles = impl_->domainCycles;
  refreshRuntimeSummary();
  result_.stats = statistics();
  return result_;
}

void SimSystem::reset() {
  epoch_ = {0, 0};
  terminated_ = false;
  result_ = TerminationResult{};
  impl_->scheduledWork.clear();
  impl_->scheduledExternalXfer.clear();
  impl_->eventQueue.reset();
  impl_->committedEventCount = 0;
  impl_->workInvocations = 0;
  impl_->activationTraversals = 0;
  impl_->workClosureTraversals = 0;
  impl_->commitTimeline.clear();
  impl_->executingEpoch = false;
  impl_->activeProposalOwner.reset();
  impl_->noProgress = {};
  impl_->generatedStats.clear();
  impl_->observations.reset();
  impl_->traceOwnerCount = 0;
  impl_->traceEof = true;
  impl_->preflightValidated = false;
  impl_->lastCommitTick.clear();
  impl_->eventQueueLastCommitTick.reset();
  impl_->lastProgressTick = 0;
  impl_->domainCycles.clear();
  for (const auto &[name, domain] : impl_->timeDomains)
    impl_->domainCycles.emplace(name, 0);
  if (impl_->legacyDispatch.rows) {
    for (ObjectId id = 0; id < impl_->legacyDispatch.objectCount; ++id) {
      const LegacyDispatchThunk &row = impl_->legacyDispatch.rows[id];
      row.reset(row.object);
    }
  } else if (!impl_->dispatch.empty()) {
    for (ObjectId id = 0; id < impl_->dispatch.size(); ++id) {
      const DispatchRow *row = impl_->dispatch.lookup(id);
      row->reset(row->object);
    }
  } else {
    for (SimObject *object : runtimeObjects())
      if (object->kind() != ObjectKind::Module)
        object->reset();
  }
}

} // namespace gfsim
