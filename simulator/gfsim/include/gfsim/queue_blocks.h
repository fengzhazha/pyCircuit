#ifndef GFSIM_QUEUE_BLOCKS_H
#define GFSIM_QUEUE_BLOCKS_H

#include "gfsim/bits.h"
#include "gfsim/object.h"
#include "gfsim/queue.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace gfsim {

template <typename Input, typename Output, typename Policy, size_t Rate = 1>
  requires std::invocable<const Policy &, const Input &> && (Rate > 0) &&
           std::convertible_to<
               std::invoke_result_t<const Policy &, const Input &>, Output>
class QueueTransform : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.transform";
  static constexpr ObjectKind componentKind = ObjectKind::Compute;

  QueueTransform(std::string name, ObjectId id, SimObject *parent,
                 SimQueue<Input> &input, SimQueue<Output> &output,
                 Policy policy = {}, ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), output_(output), policy_(std::move(policy)) {}

  void doWork(Epoch) override {
    while (fired_ < Rate && input_.canProposePop() &&
           output_.canProposePush()) {
      const Input *head = input_.peekProposable();
      if (head == nullptr)
        return;
      Output result = std::invoke(std::as_const(policy_), *head);
      if (!output_.proposePush(std::move(result)) || !input_.proposePop())
        return;
      ++fired_;
    }
  }

  void doXfer(Epoch) override { fired_ = 0; }
  bool hasPendingCommit() const override { return fired_ != 0; }
  bool isRunnable(Epoch) const override {
    return fired_ < Rate && input_.canProposePop() && output_.canProposePush();
  }
  void reset() override {
    fired_ = 0;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<Input> &input_;
  SimQueue<Output> &output_;
  [[no_unique_address]] Policy policy_;
  size_t fired_ = 0;
};

template <typename Input, typename Output, size_t Rate, typename Policy>
  requires std::invocable<const Policy &, const Input &> && (Rate > 0) &&
           std::convertible_to<
               std::invoke_result_t<const Policy &, const Input &>, Output>
class Compute final : public QueueTransform<Input, Output, Policy, Rate> {
public:
  static constexpr std::string_view contractName = "ac.compute";
  static constexpr ObjectKind componentKind = ObjectKind::Compute;

  Compute(std::string name, ObjectId id, SimObject *parent,
          SimQueue<Input> &input, SimQueue<Output> &output, Policy policy = {},
          ObservationSink *observations = nullptr)
      : QueueTransform<Input, Output, Policy, Rate>(
            std::move(name), id, parent, input, output, std::move(policy),
            observations) {}
};

template <typename T> struct Identity {
  T operator()(const T &value) const { return value; }
};

template <typename T, size_t Stages, size_t Rate>
  requires(Stages > 0) && (Rate > 0)
class Pipeline final : public QueueTransform<T, T, Identity<T>, Rate> {
public:
  static constexpr std::string_view contractName = "ac.pipeline";
  static constexpr ObjectKind componentKind = ObjectKind::Compute;

  Pipeline(std::string name, ObjectId id, SimObject *parent, SimQueue<T> &input,
           SimQueue<T> &output, ObservationSink *observations = nullptr)
      : QueueTransform<T, T, Identity<T>, Rate>(
            std::move(name), id, parent, input, output, {}, observations) {
    if (output.latency() != Stages)
      throw std::invalid_argument(
          "Pipeline stages must match output SimQueue latency");
  }
};

template <typename Policy, typename InputTypes, typename OutputTypes>
class QueueAtomicTransform;

template <typename Policy, typename... Inputs, typename... Outputs>
  requires std::invocable<const Policy &, const Inputs &...> &&
           std::same_as<std::invoke_result_t<const Policy &, const Inputs &...>,
                        std::tuple<Outputs...>>
class QueueAtomicTransform<Policy, std::tuple<Inputs...>,
                           std::tuple<Outputs...>>
    final : public SimObject {
public:
  static_assert(sizeof...(Inputs) > 0 && sizeof...(Outputs) > 0,
                "atomic transform requires input and output Queues");
  static constexpr std::string_view contractName = "ac.transform.atomic";
  static constexpr ObjectKind componentKind = ObjectKind::Compute;

  QueueAtomicTransform(std::string name, ObjectId id, SimObject *parent,
                       std::tuple<SimQueue<Inputs> *...> inputs,
                       std::tuple<SimQueue<Outputs> *...> outputs,
                       Policy policy = {},
                       ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        inputs_(inputs), outputs_(outputs), policy_(std::move(policy)) {
    if (id == kInvalidObjectId)
      throw std::invalid_argument(
          "atomic transform requires a stable object ID");
    if (std::apply(
            [](const auto *...queues) { return ((queues == nullptr) || ...); },
            inputs_) ||
        std::apply(
            [](const auto *...queues) { return ((queues == nullptr) || ...); },
            outputs_))
      throw std::invalid_argument("atomic transform Queue is null");
  }

  void doWork(Epoch) override {
    if (fired_ || !allInputsReady() || !allOutputsReady())
      return;
    const CommitGroupId group = id();
    if (!prepareOutputs(group, std::index_sequence_for<Outputs...>{}) ||
        !prepareInputs(group, std::index_sequence_for<Inputs...>{})) {
      cancelPrepared(group);
      return;
    }
    auto values = inputValues(group, std::index_sequence_for<Inputs...>{});
    auto results = std::apply(std::as_const(policy_), values);
    if (!publishOutputs(group, results,
                        std::index_sequence_for<Outputs...>{}) ||
        !publishInputs(group, std::index_sequence_for<Inputs...>{})) {
      setRuntimeFailureCode("commit_group_publish_failed");
      cancelPrepared(group);
      return;
    }
    fired_ = true;
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    return !fired_ && allInputsReady() && allOutputsReady();
  }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  bool allInputsReady() const {
    return std::apply(
        [](const auto *...queues) {
          return ((queues != nullptr && queues->canProposePop()) && ...);
        },
        inputs_);
  }
  bool allOutputsReady() const {
    return std::apply(
        [](const auto *...queues) {
          return ((queues != nullptr && queues->canProposePush()) && ...);
        },
        outputs_);
  }
  template <size_t... Indices>
  bool prepareOutputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(outputs_)->preparePush(group) && ...);
  }
  template <size_t... Indices>
  bool prepareInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->preparePop(group) && ...);
  }
  template <size_t... Indices>
  std::tuple<Inputs...> inputValues(CommitGroupId group,
                                    std::index_sequence<Indices...>) const {
    return std::tuple<Inputs...>{
        *std::get<Indices>(inputs_)->preparedPopValue(group)...};
  }
  template <size_t... Indices>
  bool publishOutputs(CommitGroupId group, const std::tuple<Outputs...> &values,
                      std::index_sequence<Indices...>) {
    return (std::get<Indices>(outputs_)->publishPush(
                group, std::get<Indices>(values)) &&
            ...);
  }
  template <size_t... Indices>
  bool publishInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->publishPop(group).has_value() && ...);
  }
  void cancelPrepared(CommitGroupId group) {
    std::apply([&](auto *...queues) { (queues->cancelPrepared(group), ...); },
               inputs_);
    std::apply([&](auto *...queues) { (queues->cancelPrepared(group), ...); },
               outputs_);
  }

  std::tuple<SimQueue<Inputs> *...> inputs_;
  std::tuple<SimQueue<Outputs> *...> outputs_;
  [[no_unique_address]] Policy policy_;
  bool fired_ = false;
};

template <typename Types> class QueueBarrier;

template <typename... Values>
class QueueBarrier<std::tuple<Values...>> final : public SimObject {
public:
  static_assert(sizeof...(Values) > 0,
                "barrier requires at least one Queue pair");
  static constexpr std::string_view contractName = "ac.barrier";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;

  QueueBarrier(std::string name, ObjectId id, SimObject *parent,
               std::tuple<SimQueue<Values> *...> inputs,
               std::tuple<SimQueue<Values> *...> outputs,
               ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        inputs_(inputs), outputs_(outputs) {
    if (id == kInvalidObjectId)
      throw std::invalid_argument("barrier requires a stable object ID");
    if (std::apply(
            [](const auto *...queues) { return ((queues == nullptr) || ...); },
            inputs_) ||
        std::apply(
            [](const auto *...queues) { return ((queues == nullptr) || ...); },
            outputs_))
      throw std::invalid_argument("barrier Queue is null");
  }

  void doWork(Epoch) override {
    if (fired_ || !allInputsReady() || !allOutputsReady())
      return;
    const CommitGroupId group = id();
    if (!prepareOutputs(group, std::index_sequence_for<Values...>{}) ||
        !prepareInputs(group, std::index_sequence_for<Values...>{})) {
      cancelPrepared(group);
      return;
    }
    auto values = inputValues(group, std::index_sequence_for<Values...>{});
    if (!publishOutputs(group, values, std::index_sequence_for<Values...>{}) ||
        !publishInputs(group, std::index_sequence_for<Values...>{})) {
      setRuntimeFailureCode("commit_group_publish_failed");
      cancelPrepared(group);
      return;
    }
    fired_ = true;
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    return !fired_ && allInputsReady() && allOutputsReady();
  }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  bool allInputsReady() const {
    return std::apply(
        [](const auto *...queues) {
          return ((queues != nullptr && queues->canProposePop()) && ...);
        },
        inputs_);
  }
  bool allOutputsReady() const {
    return std::apply(
        [](const auto *...queues) {
          return ((queues != nullptr && queues->canProposePush()) && ...);
        },
        outputs_);
  }
  template <size_t... Indices>
  bool prepareOutputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(outputs_)->preparePush(group) && ...);
  }
  template <size_t... Indices>
  bool prepareInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->preparePop(group) && ...);
  }
  template <size_t... Indices>
  std::tuple<Values...> inputValues(CommitGroupId group,
                                    std::index_sequence<Indices...>) const {
    return std::tuple<Values...>{
        *std::get<Indices>(inputs_)->preparedPopValue(group)...};
  }
  template <size_t... Indices>
  bool publishOutputs(CommitGroupId group, const std::tuple<Values...> &values,
                      std::index_sequence<Indices...>) {
    return (std::get<Indices>(outputs_)->publishPush(
                group, std::get<Indices>(values)) &&
            ...);
  }
  template <size_t... Indices>
  bool publishInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->publishPop(group).has_value() && ...);
  }
  void cancelPrepared(CommitGroupId group) {
    std::apply([&](auto *...queues) { (queues->cancelPrepared(group), ...); },
               inputs_);
    std::apply([&](auto *...queues) { (queues->cancelPrepared(group), ...); },
               outputs_);
  }

  std::tuple<SimQueue<Values> *...> inputs_;
  std::tuple<SimQueue<Values> *...> outputs_;
  bool fired_ = false;
};

template <typename T, typename Key>
  requires std::invocable<const Key &, const T &> &&
           IntegralLike<std::invoke_result_t<const Key &, const T &>>
class QueueReorder : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.reorder";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;

  QueueReorder(std::string name, ObjectId id, SimObject *parent,
               SimQueue<T> &input, SimQueue<T> &output, size_t capacity,
               uint64_t start = 0, Key key = {},
               ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), output_(output), capacity_(capacity), start_(start),
        nextKey_(start), key_(std::move(key)) {}

  void doWork(Epoch) override {
    if (!pendingInput_ && entries_.size() < capacity_ &&
        input_.canProposePop()) {
      const T *head = input_.peek();
      if (head != nullptr) {
        using KeyResult = std::invoke_result_t<const Key &, const T &>;
        const KeyResult rawKey = std::invoke(std::as_const(key_), *head);
        if constexpr (std::signed_integral<KeyResult>)
          if (rawKey < 0) {
            setRuntimeFailureCode("reorder_negative_key");
            return;
          }
        const uint64_t key = static_cast<uint64_t>(rawKey);
        if (key < nextKey_) {
          setRuntimeFailureCode("reorder_stale_key");
          return;
        }
        if (entries_.contains(key)) {
          setRuntimeFailureCode("reorder_duplicate_key");
          return;
        }
        if (input_.proposePop())
          pendingInput_ = std::pair<uint64_t, T>{key, *head};
      }
    }
    if (!pendingOutputKey_) {
      auto next = entries_.find(nextKey_);
      if (next != entries_.end() && output_.canProposePush() &&
          output_.proposePush(next->second))
        pendingOutputKey_ = nextKey_;
    }
  }

  void doXfer(Epoch) override {
    if (pendingOutputKey_) {
      entries_.erase(*pendingOutputKey_);
      ++nextKey_;
      pendingOutputKey_.reset();
    }
    if (pendingInput_) {
      entries_.emplace(pendingInput_->first, std::move(pendingInput_->second));
      pendingInput_.reset();
    }
  }
  bool hasPendingCommit() const override {
    return pendingInput_.has_value() || pendingOutputKey_.has_value();
  }
  size_t active() const { return entries_.size(); }
  bool isRunnable(Epoch) const override {
    const bool canRetire = !pendingOutputKey_ && entries_.contains(nextKey_) &&
                           output_.canProposePush();
    const bool canAdmit =
        !pendingInput_ && entries_.size() < capacity_ && input_.canProposePop();
    return canRetire || canAdmit;
  }
  size_t buffered() const { return entries_.size(); }
  uint64_t nextKey() const { return nextKey_; }
  void reset() override {
    entries_.clear();
    pendingInput_.reset();
    pendingOutputKey_.reset();
    nextKey_ = start_;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  SimQueue<T> &output_;
  size_t capacity_;
  uint64_t start_;
  uint64_t nextKey_;
  [[no_unique_address]] Key key_;
  std::map<uint64_t, T> entries_;
  std::optional<std::pair<uint64_t, T>> pendingInput_;
  std::optional<uint64_t> pendingOutputKey_;
};

template <typename T, size_t Entries, uint64_t Start, typename Key>
  requires(Entries > 0) && std::invocable<const Key &, const T &> &&
          IntegralLike<std::invoke_result_t<const Key &, const T &>>
class Reorder final : public QueueReorder<T, Key> {
public:
  static constexpr std::string_view contractName = "ac.reorder";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;

  Reorder(std::string name, ObjectId id, SimObject *parent, SimQueue<T> &input,
          SimQueue<T> &output, Key key = {},
          ObservationSink *observations = nullptr)
      : QueueReorder<T, Key>(std::move(name), id, parent, input, output,
                             Entries, Start, std::move(key), observations) {}
};

template <typename T, typename Key, typename Dependency, typename Resource,
          typename Cost>
  requires std::invocable<const Key &, const T &> &&
           IntegralLike<std::invoke_result_t<const Key &, const T &>> &&
           std::invocable<const Dependency &, const T &> &&
           IntegralLike<std::invoke_result_t<const Dependency &, const T &>> &&
           std::invocable<const Resource &, const T &> &&
           IntegralLike<std::invoke_result_t<const Resource &, const T &>> &&
           std::invocable<const Cost &, const T &> &&
           IntegralLike<std::invoke_result_t<const Cost &, const T &>>
class QueueDependency : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.dependency";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;

  QueueDependency(std::string name, ObjectId id, SimObject *parent,
                  SimQueue<T> &input, SimQueue<T> &output, size_t capacity,
                  size_t resources, uint64_t noDependency, Key key = {},
                  Dependency dependency = {}, Resource resource = {},
                  Cost cost = {}, ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), output_(output), capacity_(capacity),
        resources_(resources), noDependency_(noDependency),
        key_(std::move(key)), dependency_(std::move(dependency)),
        resource_(std::move(resource)), cost_(std::move(cost)) {}

  void doWork(Epoch epoch) override {
    if (proposed_)
      return;
    if (entries_.size() < capacity_ && input_.canProposePop()) {
      const T *head = input_.peek();
      if (head != nullptr && !proposeInput(*head))
        return;
    }

    const Entry *completed = nullptr;
    for (const auto &[key, entry] : entries_)
      if (entry.state == State::Done &&
          (completed == nullptr || entry.ready < completed->ready ||
           (entry.ready == completed->ready && key < completed->key)))
        completed = &entry;
    if (completed != nullptr && output_.canProposePush() &&
        output_.proposePush(completed->value))
      pendingOutputKey_ = completed->key;

    for (const auto &[key, entry] : entries_)
      if (entry.state == State::Executing && entry.ready <= epoch)
        pendingCompletions_.push_back(key);
    for (size_t resource = 0; resource < resources_; ++resource) {
      if (!resourceFree(resource, epoch))
        continue;
      for (const auto &[key, entry] : entries_)
        if (entry.state == State::Waiting && entry.resource == resource &&
            dependencyReady(entry)) {
          pendingIssues_.push_back(key);
          break;
        }
    }
    proposed_ = pendingInput_.has_value() || pendingOutputKey_.has_value() ||
                !pendingIssues_.empty() || !pendingCompletions_.empty();
  }

  void doXfer(Epoch epoch) override {
    if (!proposed_)
      return;
    if (pendingOutputKey_)
      entries_.erase(*pendingOutputKey_);
    for (uint64_t key : pendingCompletions_)
      if (auto found = entries_.find(key); found != entries_.end())
        found->second.state = State::Done;
    for (uint64_t key : pendingIssues_)
      if (auto found = entries_.find(key); found != entries_.end()) {
        if (epoch.time >
            std::numeric_limits<uint64_t>::max() - found->second.cost) {
          setRuntimeFailureCode("dependency_time_overflow");
          continue;
        }
        found->second.state = State::Executing;
        found->second.ready = {epoch.time + found->second.cost, 0};
      }
    if (pendingInput_) {
      Entry entry;
      entry.key = pendingInput_->key;
      entry.dependency = pendingInput_->dependency;
      entry.resource = pendingInput_->resource;
      entry.cost = pendingInput_->cost;
      entry.value = std::move(pendingInput_->value);
      entries_.emplace(entry.key, std::move(entry));
    }
    pendingInput_.reset();
    pendingOutputKey_.reset();
    pendingIssues_.clear();
    pendingCompletions_.clear();
    proposed_ = false;
  }
  bool hasPendingCommit() const override { return proposed_; }
  bool isRunnable(Epoch epoch) const override {
    if (proposed_)
      return false;
    if (entries_.size() < capacity_ && input_.canProposePop())
      return true;
    for (const auto &[key, entry] : entries_) {
      (void)key;
      if ((entry.state == State::Done && output_.canProposePush()) ||
          (entry.state == State::Executing && entry.ready <= epoch) ||
          (entry.state == State::Waiting && dependencyReady(entry) &&
           resourceFree(entry.resource, epoch)))
        return true;
    }
    return false;
  }
  size_t active() const { return entries_.size(); }
  size_t resourceActive(size_t resource) const {
    return static_cast<size_t>(
        std::count_if(entries_.begin(), entries_.end(), [&](const auto &entry) {
          return entry.second.resource == resource &&
                 entry.second.state == State::Executing;
        }));
  }
  void reset() override {
    entries_.clear();
    pendingInput_.reset();
    pendingOutputKey_.reset();
    pendingIssues_.clear();
    pendingCompletions_.clear();
    proposed_ = false;
    clearRuntimeFailureCode();
  }

private:
  enum class State : uint8_t { Waiting, Executing, Done };
  struct Entry {
    uint64_t key = 0;
    uint64_t dependency = 0;
    uint64_t resource = 0;
    uint64_t cost = 0;
    T value{};
    State state = State::Waiting;
    Epoch ready{};
  };
  struct PendingInput {
    uint64_t key = 0;
    uint64_t dependency = 0;
    uint64_t resource = 0;
    uint64_t cost = 0;
    T value;
  };

  bool dependencyReady(const Entry &entry) const {
    if (entry.dependency == noDependency_)
      return true;
    auto found = entries_.find(entry.dependency);
    return found != entries_.end() && found->second.state == State::Done;
  }
  bool resourceFree(uint64_t resource, Epoch epoch) const {
    if (resource >= resources_)
      return false;
    for (const auto &[key, entry] : entries_) {
      (void)key;
      if (entry.resource == resource && entry.state == State::Executing &&
          entry.ready > epoch)
        return false;
    }
    return true;
  }
  bool proposeInput(const T &value) {
    using KeyResult = std::invoke_result_t<const Key &, const T &>;
    using DependencyResult =
        std::invoke_result_t<const Dependency &, const T &>;
    using ResourceResult = std::invoke_result_t<const Resource &, const T &>;
    using CostResult = std::invoke_result_t<const Cost &, const T &>;
    const KeyResult rawKey = std::invoke(std::as_const(key_), value);
    const DependencyResult rawDependency =
        std::invoke(std::as_const(dependency_), value);
    const ResourceResult rawResource =
        std::invoke(std::as_const(resource_), value);
    const CostResult rawCost = std::invoke(std::as_const(cost_), value);
    if constexpr (std::signed_integral<KeyResult>)
      if (rawKey < 0) {
        setRuntimeFailureCode("dependency_negative_key");
        return false;
      }
    if constexpr (std::signed_integral<DependencyResult>)
      if (rawDependency < 0) {
        setRuntimeFailureCode("dependency_negative_predecessor");
        return false;
      }
    if constexpr (std::signed_integral<ResourceResult>)
      if (rawResource < 0) {
        setRuntimeFailureCode("dependency_negative_resource");
        return false;
      }
    if constexpr (std::signed_integral<CostResult>)
      if (rawCost <= 0) {
        setRuntimeFailureCode("dependency_nonpositive_cost");
        return false;
      }
    if constexpr (UnsignedIntegralLike<CostResult>)
      if (rawCost == 0) {
        setRuntimeFailureCode("dependency_nonpositive_cost");
        return false;
      }
    const uint64_t key = static_cast<uint64_t>(rawKey);
    const uint64_t predecessor = static_cast<uint64_t>(rawDependency);
    const uint64_t resource = static_cast<uint64_t>(rawResource);
    const uint64_t cost = static_cast<uint64_t>(rawCost);
    if (entries_.contains(key)) {
      setRuntimeFailureCode("dependency_duplicate_key");
      return false;
    }
    if (resource >= resources_) {
      setRuntimeFailureCode("dependency_resource_out_of_range");
      return false;
    }
    if (!input_.proposePop())
      return true;
    pendingInput_ = PendingInput{key, predecessor, resource, cost, value};
    return true;
  }

  SimQueue<T> &input_;
  SimQueue<T> &output_;
  size_t capacity_;
  size_t resources_;
  uint64_t noDependency_;
  [[no_unique_address]] Key key_;
  [[no_unique_address]] Dependency dependency_;
  [[no_unique_address]] Resource resource_;
  [[no_unique_address]] Cost cost_;
  std::map<uint64_t, Entry> entries_;
  std::optional<PendingInput> pendingInput_;
  std::optional<uint64_t> pendingOutputKey_;
  std::vector<uint64_t> pendingIssues_;
  std::vector<uint64_t> pendingCompletions_;
  bool proposed_ = false;
};

template <typename T, size_t Entries, size_t Resources, uint64_t NoDependency,
          typename Key, typename Dependency, typename Resource, typename Cost>
  requires(Entries > 0) && (Resources > 0) &&
          std::invocable<const Key &, const T &> &&
          IntegralLike<std::invoke_result_t<const Key &, const T &>> &&
          std::invocable<const Dependency &, const T &> &&
          IntegralLike<std::invoke_result_t<const Dependency &, const T &>> &&
          std::invocable<const Resource &, const T &> &&
          IntegralLike<std::invoke_result_t<const Resource &, const T &>> &&
          std::invocable<const Cost &, const T &> &&
          IntegralLike<std::invoke_result_t<const Cost &, const T &>>
class Schedule final
    : public QueueDependency<T, Key, Dependency, Resource, Cost> {
public:
  static constexpr std::string_view contractName = "ac.schedule";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;

  Schedule(std::string name, ObjectId id, SimObject *parent, SimQueue<T> &input,
           SimQueue<T> &output, Key key = {}, Dependency dependency = {},
           Resource resource = {}, Cost cost = {},
           ObservationSink *observations = nullptr)
      : QueueDependency<T, Key, Dependency, Resource, Cost>(
            std::move(name), id, parent, input, output, Entries, Resources,
            NoDependency, std::move(key), std::move(dependency),
            std::move(resource), std::move(cost), observations) {}
};

template <typename T, typename Cost>
  requires std::invocable<const Cost &, const T &> &&
           IntegralLike<std::invoke_result_t<const Cost &, const T &>>
class QueueCredit : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.credit";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;

  QueueCredit(std::string name, ObjectId id, SimObject *parent,
              SimQueue<T> &input, SimQueue<T> &output, size_t credits,
              Cost cost = {}, ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), output_(output), slots_(credits),
        cost_(std::move(cost)) {}

  void doWork(Epoch) override {
    if (proposed_)
      return;

    for (size_t index = 0; index < slots_.size(); ++index)
      if (slots_[index] && slots_[index]->remaining == 0 &&
          output_.canProposePush() &&
          output_.proposePush(slots_[index]->value)) {
        pendingOutput_ = index;
        break;
      }

    for (size_t index = 0; index < slots_.size(); ++index)
      if (slots_[index] && slots_[index]->remaining > 0)
        pendingCountdowns_.push_back(index);

    if (input_.canProposePop()) {
      auto free = std::find_if(slots_.begin(), slots_.end(),
                               [](const auto &slot) { return !slot; });
      const T *head = input_.peek();
      if (free != slots_.end() && head != nullptr)
        proposeInput(static_cast<size_t>(free - slots_.begin()), *head);
    }

    proposed_ = pendingInput_.has_value() || pendingOutput_.has_value() ||
                !pendingCountdowns_.empty();
  }

  void doXfer(Epoch) override {
    if (!proposed_)
      return;
    if (pendingOutput_)
      slots_[*pendingOutput_].reset();
    for (size_t index : pendingCountdowns_)
      if (slots_[index] && slots_[index]->remaining > 0)
        --slots_[index]->remaining;
    if (pendingInput_)
      slots_[pendingInput_->slot] =
          Entry{std::move(pendingInput_->value), pendingInput_->cost};
    pendingInput_.reset();
    pendingOutput_.reset();
    pendingCountdowns_.clear();
    proposed_ = false;
  }

  bool hasPendingCommit() const override { return proposed_; }
  bool isRunnable(Epoch) const override {
    if (proposed_)
      return false;
    if (input_.canProposePop() &&
        std::any_of(slots_.begin(), slots_.end(),
                    [](const auto &slot) { return !slot; }))
      return true;
    for (const auto &slot : slots_)
      if (slot && (slot->remaining > 0 || output_.canProposePush()))
        return true;
    return false;
  }

  size_t active() const {
    return static_cast<size_t>(
        std::count_if(slots_.begin(), slots_.end(),
                      [](const auto &slot) { return slot.has_value(); }));
  }

  void reset() override {
    for (auto &slot : slots_)
      slot.reset();
    pendingInput_.reset();
    pendingOutput_.reset();
    pendingCountdowns_.clear();
    proposed_ = false;
    clearRuntimeFailureCode();
  }

private:
  struct Entry {
    T value;
    uint64_t remaining = 0;
  };
  struct PendingInput {
    size_t slot = 0;
    T value;
    uint64_t cost = 0;
  };

  void proposeInput(size_t slot, const T &value) {
    using CostResult = std::invoke_result_t<const Cost &, const T &>;
    const CostResult rawCost = std::invoke(std::as_const(cost_), value);
    if constexpr (std::signed_integral<CostResult>)
      if (rawCost <= 0) {
        setRuntimeFailureCode("credit_nonpositive_cost");
        return;
      }
    if constexpr (UnsignedIntegralLike<CostResult>)
      if (rawCost == 0) {
        setRuntimeFailureCode("credit_nonpositive_cost");
        return;
      }
    if (!input_.proposePop())
      return;
    pendingInput_ = PendingInput{slot, value, static_cast<uint64_t>(rawCost)};
  }

  SimQueue<T> &input_;
  SimQueue<T> &output_;
  std::vector<std::optional<Entry>> slots_;
  [[no_unique_address]] Cost cost_;
  std::optional<PendingInput> pendingInput_;
  std::optional<size_t> pendingOutput_;
  std::vector<size_t> pendingCountdowns_;
  bool proposed_ = false;
};

template <typename T, size_t Lanes, typename Cost>
  requires(Lanes > 0) && std::invocable<const Cost &, const T &> &&
          IntegralLike<std::invoke_result_t<const Cost &, const T &>>
class Engine final : public QueueCredit<T, Cost> {
public:
  static constexpr std::string_view contractName = "ac.engine";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;

  Engine(std::string name, ObjectId id, SimObject *parent, SimQueue<T> &input,
         SimQueue<T> &output, Cost cost = {},
         ObservationSink *observations = nullptr)
      : QueueCredit<T, Cost>(std::move(name), id, parent, input, output, Lanes,
                             std::move(cost), observations) {}
};

template <typename T, typename Data, typename Address, typename Write,
          typename WriteData, typename Response>
  requires std::invocable<const Address &, const T &> &&
           IntegralLike<std::invoke_result_t<const Address &, const T &>> &&
           std::invocable<const Write &, const T &> &&
           std::convertible_to<std::invoke_result_t<const Write &, const T &>,
                               bool> &&
           std::invocable<const WriteData &, const T &> &&
           std::convertible_to<
               std::invoke_result_t<const WriteData &, const T &>, Data> &&
           std::invocable<const Response &, const T &, const Data &> &&
           std::convertible_to<
               std::invoke_result_t<const Response &, const T &, const Data &>,
               T>
class QueueMemory : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.memory";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  QueueMemory(std::string name, ObjectId id, SimObject *parent,
              SimQueue<T> &input, SimQueue<T> &output, size_t entries,
              Data init = {}, Address address = {}, Write write = {},
              WriteData writeData = {}, Response response = {},
              ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), output_(output), init_(init), storage_(entries, init),
        address_(std::move(address)), write_(std::move(write)),
        writeData_(std::move(writeData)), response_(std::move(response)) {}

  void doWork(Epoch) override {
    if (fired_ || !input_.canProposePop() || !output_.canProposePush())
      return;
    const T *head = input_.peek();
    if (head == nullptr)
      return;
    using AddressResult = std::invoke_result_t<const Address &, const T &>;
    const AddressResult rawAddress =
        std::invoke(std::as_const(address_), *head);
    if constexpr (std::signed_integral<AddressResult>)
      if (rawAddress < 0) {
        setRuntimeFailureCode("memory_address_out_of_range");
        return;
      }
    const uint64_t address = static_cast<uint64_t>(rawAddress);
    if (address >= storage_.size()) {
      setRuntimeFailureCode("memory_address_out_of_range");
      return;
    }
    const Data oldData = storage_[address];
    T response = std::invoke(std::as_const(response_), *head, oldData);
    if (!output_.proposePush(std::move(response)) || !input_.proposePop())
      return;
    if (static_cast<bool>(std::invoke(std::as_const(write_), *head)))
      pendingWrite_ = std::pair<size_t, Data>{
          static_cast<size_t>(address),
          static_cast<Data>(std::invoke(std::as_const(writeData_), *head))};
    fired_ = true;
  }
  void doXfer(Epoch) override {
    if (pendingWrite_) {
      storage_[pendingWrite_->first] = std::move(pendingWrite_->second);
      pendingWrite_.reset();
    }
    fired_ = false;
  }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    return !fired_ && input_.canProposePop() && output_.canProposePush();
  }
  const Data &at(size_t address) const { return storage_.at(address); }
  void reset() override {
    std::fill(storage_.begin(), storage_.end(), init_);
    pendingWrite_.reset();
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  SimQueue<T> &output_;
  Data init_;
  std::vector<Data> storage_;
  [[no_unique_address]] Address address_;
  [[no_unique_address]] Write write_;
  [[no_unique_address]] WriteData writeData_;
  [[no_unique_address]] Response response_;
  std::optional<std::pair<size_t, Data>> pendingWrite_;
  bool fired_ = false;
};

template <typename Entry> struct TableFullEntryMerge {
  static constexpr std::array<size_t, 1> fields{0};
  void operator()(Entry &target, const Entry &value) const { target = value; }
};

enum class TableWriteMode : uint8_t { FieldMerge, Replace };

struct StateReservation {
  uint64_t wholeEntries = 0;
  uint64_t fieldRelation = 0;
  uint8_t fieldCount = 0;

  constexpr StateReservation() = default;
  constexpr StateReservation(uint64_t entries) : wholeEntries(entries) {}

  static constexpr StateReservation
  forFields(uint64_t entries, uint64_t fieldMask, uint8_t fieldsPerEntry) {
    StateReservation result;
    result.fieldCount = fieldsPerEntry;
    if (fieldsPerEntry == 0 || fieldsPerEntry > 64)
      return result;
    for (size_t entry = 0; entry < 64; ++entry) {
      if ((entries & (uint64_t{1} << entry)) == 0)
        continue;
      for (size_t field = 0; field < fieldsPerEntry; ++field) {
        const size_t bit = entry * fieldsPerEntry + field;
        if (bit >= 64)
          return StateReservation{};
        if ((fieldMask & (uint64_t{1} << field)) != 0)
          result.fieldRelation |= uint64_t{1} << bit;
      }
    }
    return result;
  }

  constexpr uint64_t fieldEntryMask() const {
    if (fieldCount == 0)
      return 0;
    uint64_t result = 0;
    for (size_t entry = 0; entry < 64; ++entry) {
      const size_t offset = entry * fieldCount;
      if (offset >= 64)
        break;
      const uint64_t mask = fieldCount == 64
                                ? ~uint64_t{0}
                                : ((uint64_t{1} << fieldCount) - 1) << offset;
      if ((fieldRelation & mask) != 0)
        result |= uint64_t{1} << entry;
    }
    return result;
  }
  constexpr uint64_t entryMask() const {
    return wholeEntries | fieldEntryMask();
  }
  constexpr bool empty() const { return entryMask() == 0; }

  friend constexpr StateReservation operator|(StateReservation left,
                                              StateReservation right) {
    left.wholeEntries |= right.wholeEntries;
    if (left.fieldCount == 0) {
      left.fieldCount = right.fieldCount;
      left.fieldRelation = right.fieldRelation;
    } else if (right.fieldCount == 0 || left.fieldCount == right.fieldCount) {
      left.fieldRelation |= right.fieldRelation;
    } else {
      left.wholeEntries |= left.fieldEntryMask() | right.fieldEntryMask();
      left.fieldRelation = 0;
      left.fieldCount = 0;
    }
    return left;
  }
};

template <typename Policy, typename... Args>
concept TablePolicyInvocable = std::invocable<const Policy &, Epoch, Args...> ||
                               std::invocable<const Policy &, Args...>;

template <typename Policy, typename... Args>
  requires TablePolicyInvocable<Policy, Args...>
decltype(auto) invokeTablePolicy(const Policy &policy, Epoch epoch,
                                 Args &&...args) {
  if constexpr (std::invocable<const Policy &, Epoch, Args...>)
    return std::invoke(policy, epoch, std::forward<Args>(args)...);
  else
    return std::invoke(policy, std::forward<Args>(args)...);
}

template <typename Policy, typename... Args>
using TablePolicyResult =
    decltype(invokeTablePolicy(std::declval<const Policy &>(),
                               std::declval<Epoch>(), std::declval<Args>()...));

struct TableSelectionResult {
  size_t index = 0;
  bool valid = false;
};

template <typename Entry> class SimTable;

template <typename Entry, typename Predicate> class TableMatchCache {
public:
  TableMatchCache(SimTable<Entry> &table, Predicate predicate = {})
      : table_(table), predicate_(std::move(predicate)) {}

  uint64_t get(Epoch epoch) const {
    if (epoch_ && *epoch_ == epoch)
      return mask_;
    mask_ = 0;
    for (size_t index = 0; index < table_.size(); ++index)
      if (static_cast<bool>(
              std::invoke(std::as_const(predicate_), table_.at(index))))
        mask_ |= uint64_t{1} << index;
    epoch_ = epoch;
    return mask_;
  }

  void reset() {
    epoch_.reset();
    mask_ = 0;
  }

private:
  SimTable<Entry> &table_;
  [[no_unique_address]] Predicate predicate_;
  mutable std::optional<Epoch> epoch_;
  mutable uint64_t mask_ = 0;
};

enum class TableChoosePolicy : uint8_t { First, Min, Max };

template <typename Entry, typename Mask, typename Key>
class TableSelectionCache {
public:
  TableSelectionCache(SimTable<Entry> &table, Mask mask, Key key = {},
                      TableChoosePolicy policy = TableChoosePolicy::First)
      : table_(table), mask_(std::move(mask)), key_(std::move(key)),
        policy_(policy) {}

  TableSelectionResult get(Epoch epoch) const {
    if (epoch_ && *epoch_ == epoch)
      return result_;
    result_ = {};
    const uint64_t mask = static_cast<uint64_t>(std::invoke(mask_, epoch));
    uint64_t best = 0;
    for (size_t index = 0; index < table_.size(); ++index) {
      if ((mask & (uint64_t{1} << index)) == 0)
        continue;
      if (policy_ == TableChoosePolicy::First) {
        result_ = {index, true};
        break;
      }
      const uint64_t key = static_cast<uint64_t>(
          std::invoke(std::as_const(key_), table_.at(index)));
      if (!result_.valid ||
          (policy_ == TableChoosePolicy::Min ? key < best : key > best)) {
        result_ = {index, true};
        best = key;
      }
    }
    epoch_ = epoch;
    return result_;
  }

  void reset() {
    epoch_.reset();
    result_ = {};
  }

private:
  SimTable<Entry> &table_;
  [[no_unique_address]] Mask mask_;
  [[no_unique_address]] Key key_;
  TableChoosePolicy policy_;
  mutable std::optional<Epoch> epoch_;
  mutable TableSelectionResult result_;
};

/// A one-dimensional committed-state table. Reads observe only committed
/// state. Writer-specific field proposals are merged from the same committed
/// snapshot and published together during the tick transfer phase.
template <typename Entry> class SimTable final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.table";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  SimTable(std::string name, ObjectId id, SimObject *parent, size_t entries,
           ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        committed_(entries) {
    if (entries == 0)
      throw std::invalid_argument("table entries must be positive");
  }

  size_t size() const { return committed_.size(); }
  const Entry &at(size_t index) const { return committed_.at(index); }
  const Entry &checkedAt(size_t index) {
    if (index >= committed_.size()) {
      setRuntimeFailureCode("table_index_out_of_range");
      return zeroEntry_;
    }
    return committed_[index];
  }
  template <typename Merge>
  bool proposeWrite(ObjectId writerId, size_t index, Entry value,
                    std::span<const size_t> fields, Merge merge,
                    TableWriteMode mode = TableWriteMode::FieldMerge) {
    return proposeMaskedWrite(writerId, {{index, std::move(value)}}, fields,
                              std::move(merge), mode);
  }
  template <typename Merge>
  bool proposeMaskedWrite(ObjectId writerId,
                          std::vector<std::pair<size_t, Entry>> values,
                          std::span<const size_t> fields, Merge merge,
                          TableWriteMode mode = TableWriteMode::FieldMerge) {
    auto footprint = makeFootprint(values, fields, mode);
    if (!footprint || writerHasProposal(writerId) ||
        conflictsWithPendingEndpoint(*footprint) ||
        conflictsWithPrepared(*footprint))
      return false;
    PendingProposal proposal;
    proposal.values = std::move(values);
    proposal.footprint = std::move(*footprint);
    proposal.merge = [merge = std::move(merge)](Entry &target,
                                                const Entry &value) {
      std::invoke(merge, target, value);
    };
    pending_.emplace(writerId, std::move(proposal));
    return true;
  }

  bool prepareWrite(CommitGroupId group, ObjectId writerId, size_t index,
                    std::span<const size_t> fields,
                    TableWriteMode mode = TableWriteMode::FieldMerge) {
    return prepareMaskedWrite(group, writerId,
                              std::span<const size_t>(&index, 1), fields, mode);
  }

  bool prepareMaskedWrite(CommitGroupId group, ObjectId writerId,
                          std::span<const size_t> indices,
                          std::span<const size_t> fields,
                          TableWriteMode mode = TableWriteMode::FieldMerge) {
    return prepareTransaction(group, writerId, 0, indices, fields, mode);
  }

  bool prepareTransaction(CommitGroupId group, ObjectId writerId,
                          StateReservation snapshot,
                          std::span<const size_t> writeIndices,
                          std::span<const size_t> writeFields,
                          TableWriteMode mode = TableWriteMode::FieldMerge) {
    if (group == kInvalidCommitGroupId || prepared_.contains(group) ||
        writerHasProposal(writerId))
      return false;
    const uint64_t validSnapshotMask =
        size() == 64 ? ~uint64_t{0} : ((uint64_t{1} << size()) - 1);
    if ((snapshot.entryMask() & ~validSnapshotMask) != 0 ||
        (snapshot.fieldRelation != 0 &&
         (snapshot.fieldCount == 0 ||
          size() * static_cast<size_t>(snapshot.fieldCount) > 64)))
      return false;
    std::optional<WriteFootprint> write;
    if (!writeIndices.empty()) {
      write = makeFootprint(writeIndices, writeFields, mode);
      if (!write)
        return false;
    }
    if (snapshotConflictsWithPending(snapshot) ||
        snapshotConflictsWithPrepared(snapshot) ||
        (write &&
         (conflictsWithPending(*write) || conflictsWithPrepared(*write))))
      return false;
    PreparedProposal proposal;
    proposal.writerId = writerId;
    proposal.snapshot = snapshot;
    if (write) {
      proposal.footprint = std::move(*write);
      proposal.hasWrite = true;
    }
    prepared_.emplace(group, std::move(proposal));
    return true;
  }

  template <typename Merge>
  bool publishPreparedWrite(CommitGroupId group,
                            std::vector<std::pair<size_t, Entry>> values,
                            Merge merge) {
    auto prepared = prepared_.find(group);
    if (prepared == prepared_.end() || !prepared->second.hasWrite ||
        !valuesMatchFootprint(values, prepared->second.footprint))
      return false;
    PendingProposal proposal;
    proposal.values = std::move(values);
    proposal.footprint = std::move(prepared->second.footprint);
    proposal.group = group;
    proposal.merge = [merge = std::move(merge)](Entry &target,
                                                const Entry &value) {
      std::invoke(merge, target, value);
    };
    const ObjectId writerId = prepared->second.writerId;
    prepared_.erase(prepared);
    pending_.emplace(writerId, std::move(proposal));
    return true;
  }

  template <typename Merge>
  bool publishPreparedSingleWrite(CommitGroupId group,
                                  std::optional<std::pair<size_t, Entry>> value,
                                  Merge merge) {
    auto prepared = prepared_.find(group);
    if (prepared == prepared_.end() || !prepared->second.hasWrite ||
        !valueMatchesFootprint(value, prepared->second.footprint))
      return false;
    PendingProposal proposal;
    proposal.singleValue = std::move(value);
    proposal.footprint = std::move(prepared->second.footprint);
    proposal.group = group;
    proposal.merge = [merge = std::move(merge)](Entry &target,
                                                const Entry &incoming) {
      std::invoke(merge, target, incoming);
    };
    const ObjectId writerId = prepared->second.writerId;
    prepared_.erase(prepared);
    pending_.emplace(writerId, std::move(proposal));
    return true;
  }

  void cancelPreparedWrite(CommitGroupId group) { prepared_.erase(group); }
  bool hasPreparedWrite(CommitGroupId group) const {
    return prepared_.contains(group);
  }

  bool initializeEntry(size_t index, Entry value) {
    if (!pending_.empty() || !prepared_.empty() || index >= committed_.size())
      return false;
    committed_[index] = std::move(value);
    return true;
  }
  void commitWrite() {
    lastCommitChanged_ = false;
    if (pending_.empty())
      return;
    std::vector<std::pair<size_t, Entry>> originalValues;
    if constexpr (std::equality_comparable<Entry>) {
      std::vector<size_t> touchedIndices;
      for (const auto &[writerId, proposal] : pending_) {
        (void)writerId;
        touchedIndices.insert(touchedIndices.end(),
                              proposal.footprint.indices.begin(),
                              proposal.footprint.indices.end());
      }
      std::sort(touchedIndices.begin(), touchedIndices.end());
      touchedIndices.erase(
          std::unique(touchedIndices.begin(), touchedIndices.end()),
          touchedIndices.end());
      originalValues.reserve(touchedIndices.size());
      for (size_t index : touchedIndices)
        originalValues.emplace_back(index, committed_[index]);
    } else {
      // A Table without semantic equality remains correct and conservative.
      // Any published value is treated as a change for activation purposes.
      lastCommitChanged_ = std::ranges::any_of(pending_, [](const auto &item) {
        return !item.second.footprint.indices.empty();
      });
    }
    // Every proposal was computed from the same committed snapshot during
    // Work.  Field footprints are conflict-checked before publication, so the
    // Xfer phase can update only the touched entries without copying the full
    // Table.  Replacements still run after all field merges.
    for (TableWriteMode mode :
         {TableWriteMode::FieldMerge, TableWriteMode::Replace})
      for (const auto &[writerId, proposal] : pending_) {
        (void)writerId;
        if (proposal.footprint.mode != mode)
          continue;
        auto commitValue = [&](size_t index, const Entry &value) {
          if (mode == TableWriteMode::Replace)
            committed_[index] = value;
          else
            proposal.merge(committed_[index], value);
        };
        if (proposal.singleValue)
          commitValue(proposal.singleValue->first,
                      proposal.singleValue->second);
        else
          for (const auto &[index, value] : proposal.values)
            commitValue(index, value);
      }
    if constexpr (std::equality_comparable<Entry>)
      lastCommitChanged_ =
          std::ranges::any_of(originalValues, [&](const auto &original) {
            return committed_[original.first] != original.second;
          });
    pending_.clear();
  }
  void cancelWrite(ObjectId writerId) {
    auto proposal = pending_.find(writerId);
    if (proposal != pending_.end() && !proposal->second.group)
      pending_.erase(proposal);
  }
  void doXfer(Epoch) override {
    if (!prepared_.empty()) {
      setRuntimeFailureCode("table_unpublished_commit_group");
      prepared_.clear();
    }
    commitWrite();
  }
  bool hasPendingCommit() const override {
    return !pending_.empty() || !prepared_.empty();
  }
  bool lastCommitChanged() const override { return lastCommitChanged_; }
  void reset() override {
    std::fill(committed_.begin(), committed_.end(), Entry{});
    pending_.clear();
    prepared_.clear();
    lastCommitChanged_ = false;
    clearRuntimeFailureCode();
  }

private:
  struct WriteFootprint {
    std::vector<size_t> indices;
    std::vector<size_t> fields;
    TableWriteMode mode = TableWriteMode::FieldMerge;
  };

  std::vector<Entry> committed_;
  Entry zeroEntry_{};
  struct PendingProposal {
    std::vector<std::pair<size_t, Entry>> values;
    std::optional<std::pair<size_t, Entry>> singleValue;
    WriteFootprint footprint;
    std::optional<CommitGroupId> group;
    std::function<void(Entry &, const Entry &)> merge;
  };
  struct PreparedProposal {
    ObjectId writerId = kInvalidObjectId;
    StateReservation snapshot;
    WriteFootprint footprint;
    bool hasWrite = false;
  };

  template <typename Values>
  std::optional<WriteFootprint> makeFootprint(const Values &values,
                                              std::span<const size_t> fields,
                                              TableWriteMode mode) const {
    std::vector<size_t> indices;
    indices.reserve(values.size());
    for (const auto &value : values) {
      size_t index;
      if constexpr (requires { value.first; })
        index = value.first;
      else
        index = value;
      if (index >= committed_.size())
        return std::nullopt;
      indices.push_back(index);
    }
    std::sort(indices.begin(), indices.end());
    if (std::adjacent_find(indices.begin(), indices.end()) != indices.end())
      return std::nullopt;

    std::vector<size_t> normalizedFields(fields.begin(), fields.end());
    if (normalizedFields.empty())
      return std::nullopt;
    std::sort(normalizedFields.begin(), normalizedFields.end());
    if (std::adjacent_find(normalizedFields.begin(), normalizedFields.end()) !=
        normalizedFields.end())
      return std::nullopt;
    return WriteFootprint{std::move(indices), std::move(normalizedFields),
                          mode};
  }

  static bool intersects(const std::vector<size_t> &left,
                         const std::vector<size_t> &right) {
    auto leftIt = left.begin();
    auto rightIt = right.begin();
    while (leftIt != left.end() && rightIt != right.end()) {
      if (*leftIt == *rightIt)
        return true;
      if (*leftIt < *rightIt)
        ++leftIt;
      else
        ++rightIt;
    }
    return false;
  }

  static bool footprintsConflict(const WriteFootprint &left,
                                 const WriteFootprint &right) {
    if (!intersects(left.indices, right.indices))
      return false;
    if (left.mode == TableWriteMode::Replace &&
        right.mode == TableWriteMode::Replace)
      return true;
    if (left.mode == TableWriteMode::FieldMerge &&
        right.mode == TableWriteMode::FieldMerge)
      return intersects(left.fields, right.fields);
    return false;
  }

  static bool endpointContractsConflict(const WriteFootprint &left,
                                        const WriteFootprint &right) {
    if (left.mode == TableWriteMode::Replace &&
        right.mode == TableWriteMode::Replace)
      return true;
    if (left.mode == TableWriteMode::FieldMerge &&
        right.mode == TableWriteMode::FieldMerge)
      return intersects(left.fields, right.fields);
    return false;
  }

  bool conflictsWithPending(const WriteFootprint &footprint) const {
    return std::ranges::any_of(pending_, [&](const auto &item) {
      return footprintsConflict(footprint, item.second.footprint);
    });
  }

  bool conflictsWithPendingEndpoint(const WriteFootprint &footprint) const {
    return std::ranges::any_of(pending_, [&](const auto &item) {
      return endpointContractsConflict(footprint, item.second.footprint);
    });
  }

  bool conflictsWithPrepared(const WriteFootprint &footprint) const {
    return std::ranges::any_of(prepared_, [&](const auto &item) {
      return (item.second.hasWrite &&
              footprintsConflict(footprint, item.second.footprint)) ||
             snapshotConflicts(item.second.snapshot, footprint);
    });
  }

  static bool maskIntersects(uint64_t mask,
                             const std::vector<size_t> &indices) {
    return std::ranges::any_of(indices, [&](size_t index) {
      return index < 64 && (mask & (uint64_t{1} << index)) != 0;
    });
  }

  static bool snapshotReadsField(const StateReservation &snapshot, size_t entry,
                                 size_t field) {
    if (snapshot.fieldCount == 0 || field >= snapshot.fieldCount)
      return false;
    const size_t bit = entry * snapshot.fieldCount + field;
    return bit < 64 && (snapshot.fieldRelation & (uint64_t{1} << bit)) != 0;
  }

  static bool snapshotConflicts(const StateReservation &snapshot,
                                const WriteFootprint &footprint) {
    if (maskIntersects(snapshot.wholeEntries, footprint.indices))
      return true;
    if (footprint.mode == TableWriteMode::Replace)
      return maskIntersects(snapshot.fieldEntryMask(), footprint.indices);
    return std::ranges::any_of(footprint.indices, [&](size_t entry) {
      return std::ranges::any_of(footprint.fields, [&](size_t field) {
        return snapshotReadsField(snapshot, entry, field);
      });
    });
  }

  bool snapshotConflictsWithPending(const StateReservation &snapshot) const {
    return std::ranges::any_of(pending_, [&](const auto &item) {
      return snapshotConflicts(snapshot, item.second.footprint);
    });
  }

  bool snapshotConflictsWithPrepared(const StateReservation &snapshot) const {
    return std::ranges::any_of(prepared_, [&](const auto &item) {
      return item.second.hasWrite &&
             snapshotConflicts(snapshot, item.second.footprint);
    });
  }

  bool writerHasProposal(ObjectId writerId) const {
    if (writerId == kInvalidObjectId || pending_.contains(writerId))
      return true;
    return std::ranges::any_of(prepared_, [&](const auto &item) {
      return item.second.writerId == writerId;
    });
  }

  static bool
  valuesMatchFootprint(const std::vector<std::pair<size_t, Entry>> &values,
                       const WriteFootprint &footprint) {
    std::vector<size_t> indices;
    indices.reserve(values.size());
    for (const auto &[index, value] : values) {
      (void)value;
      indices.push_back(index);
    }
    std::sort(indices.begin(), indices.end());
    return indices == footprint.indices;
  }

  static bool
  valueMatchesFootprint(const std::optional<std::pair<size_t, Entry>> &value,
                        const WriteFootprint &footprint) {
    if (!value)
      return footprint.indices.empty();
    return footprint.indices.size() == 1 &&
           footprint.indices.front() == value->first;
  }

  std::map<ObjectId, PendingProposal> pending_;
  std::map<CommitGroupId, PreparedProposal> prepared_;
  bool lastCommitChanged_ = false;
};

template <typename AddressResult>
bool tableAddressInRange(AddressResult address, size_t entries) {
  static_assert(IntegralLike<AddressResult>);
  if constexpr (std::signed_integral<AddressResult>)
    if (address < 0)
      return false;
  return static_cast<uint64_t>(address) < entries;
}

template <typename Entry, typename... Outputs> struct TableTransitionPlan {
  std::vector<std::pair<size_t, Entry>> writes;
  std::tuple<std::optional<Outputs>...> outputs;
  StateReservation reservations;
};

template <typename TableTypes, typename OutputTypes> struct StateTransitionPlan;

template <typename> using TypedStateReservation = StateReservation;

template <typename... Entries, typename... Outputs>
struct StateTransitionPlan<std::tuple<Entries...>, std::tuple<Outputs...>> {
  std::tuple<std::optional<std::pair<size_t, Entries>>...> writes;
  std::tuple<std::optional<Outputs>...> outputs;
  std::tuple<TypedStateReservation<Entries>...> reservations;
};

/// One inferred transaction spanning a Table and zero or more Queue endpoints.
/// The policy observes only committed state and returns the already-selected
/// functional branch. Resource readiness never causes a different branch to be
/// chosen: the selected plan either reserves and publishes in full or stalls.
template <typename Policy, typename Entry, typename InputTypes,
          typename OutputTypes, typename Merge = TableFullEntryMerge<Entry>>
class QueueTableTransition;

template <typename Policy, typename Entry, typename... Inputs,
          typename... Outputs, typename Merge>
  requires TablePolicyInvocable<Policy, const SimTable<Entry> &,
                                const Inputs &...> &&
           std::same_as<
               TablePolicyResult<Policy, const SimTable<Entry> &,
                                 const Inputs &...>,
               std::optional<TableTransitionPlan<Entry, Outputs...>>> &&
           std::invocable<const Merge &, Entry &, const Entry &>
class QueueTableTransition<Policy, Entry, std::tuple<Inputs...>,
                           std::tuple<Outputs...>, Merge>
    final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.firing.table";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;
  using Plan = TableTransitionPlan<Entry, Outputs...>;

  QueueTableTransition(std::string name, ObjectId id, SimObject *parent,
                       SimTable<Entry> &table,
                       std::tuple<SimQueue<Inputs> *...> inputs,
                       std::tuple<SimQueue<Outputs> *...> outputs,
                       TableWriteMode mode, Policy policy = {},
                       Merge merge = {},
                       ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        table_(table), inputs_(inputs), outputs_(outputs),
        policy_(std::move(policy)), merge_(std::move(merge)), mode_(mode) {
    if (id == kInvalidObjectId)
      throw std::invalid_argument("transition requires a stable object ID");
    if (std::apply(
            [](const auto *...queues) { return ((queues == nullptr) || ...); },
            inputs_) ||
        std::apply(
            [](const auto *...queues) { return ((queues == nullptr) || ...); },
            outputs_))
      throw std::invalid_argument("transition Queue is null");
  }

  void doWork(Epoch epoch) override {
    if (fired_ || candidate_ || !allInputsReady())
      return;
    const auto inputValues =
        peekInputValues(std::index_sequence_for<Inputs...>{});
    auto plan = std::apply(
        [&](const auto &...values) {
          return invokeTablePolicy(std::as_const(policy_), epoch,
                                   std::as_const(table_), values...);
        },
        inputValues);
    if (!plan ||
        !selectedOutputsReady(*plan, std::index_sequence_for<Outputs...>{}))
      return;

    candidate_ = std::move(*plan);
  }

  void doArbitrate(Epoch) override {
    if (fired_ || !candidate_)
      return;
    Plan plan = std::move(*candidate_);
    candidate_.reset();

    const CommitGroupId group = id();
    std::vector<size_t> writeIndices;
    writeIndices.reserve(plan.writes.size());
    for (const auto &[index, value] : plan.writes) {
      (void)value;
      writeIndices.push_back(index);
    }
    const bool hasTableReservation =
        !plan.reservations.empty() || !writeIndices.empty();
    if (!selectedOutputsReady(plan, std::index_sequence_for<Outputs...>{}) ||
        !prepareOutputs(group, plan, std::index_sequence_for<Outputs...>{}) ||
        !prepareInputs(group, std::index_sequence_for<Inputs...>{}) ||
        (hasTableReservation &&
         !table_.prepareTransaction(group, id(), plan.reservations,
                                    writeIndices, Merge::fields, mode_))) {
      cancelPrepared(group);
      return;
    }

    if (!allPrepared(group, plan, std::index_sequence_for<Inputs...>{},
                     std::index_sequence_for<Outputs...>{}) ||
        !publishOutputs(group, plan, std::index_sequence_for<Outputs...>{}) ||
        !publishInputs(group, std::index_sequence_for<Inputs...>{}) ||
        !publishTable(group, std::move(plan))) {
      setRuntimeFailureCode("commit_group_publish_failed");
      cancelPrepared(group);
      return;
    }
    proposed_ = true;
    fired_ = true;
  }

  void doXfer(Epoch) override {
    candidate_.reset();
    proposed_ = false;
    fired_ = false;
  }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    if constexpr (sizeof...(Inputs) == 0)
      return false;
    return !fired_ && !candidate_ && allInputsReady();
  }
  void reset() override {
    cancelPrepared(id());
    candidate_.reset();
    proposed_ = false;
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  bool allInputsReady() const {
    return std::apply(
        [](const auto *...queues) {
          return ((queues != nullptr && queues->canProposePop()) && ...);
        },
        inputs_);
  }

  template <size_t... Indices>
  std::tuple<Inputs...> peekInputValues(std::index_sequence<Indices...>) const {
    return std::tuple<Inputs...>{
        *std::get<Indices>(inputs_)->peekProposable()...};
  }

  template <size_t... Indices>
  bool selectedOutputsReady(const Plan &plan,
                            std::index_sequence<Indices...>) const {
    return ((!std::get<Indices>(plan.outputs)
                 ? true
                 : (std::get<Indices>(outputs_) != nullptr &&
                    std::get<Indices>(outputs_)->canProposePush())) &&
            ...);
  }

  template <size_t... Indices>
  bool prepareOutputs(CommitGroupId group, const Plan &plan,
                      std::index_sequence<Indices...>) {
    return ((!std::get<Indices>(plan.outputs)
                 ? true
                 : std::get<Indices>(outputs_)->preparePush(group)) &&
            ...);
  }

  template <size_t... Indices>
  bool prepareInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->preparePop(group) && ...);
  }

  template <size_t... Indices>
  bool publishOutputs(CommitGroupId group, const Plan &plan,
                      std::index_sequence<Indices...>) {
    return ((!std::get<Indices>(plan.outputs)
                 ? true
                 : std::get<Indices>(outputs_)->publishPush(
                       group, *std::get<Indices>(plan.outputs))) &&
            ...);
  }

  template <size_t... Indices>
  bool publishInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->publishPop(group).has_value() && ...);
  }

  template <size_t... InputIndices, size_t... OutputIndices>
  bool allPrepared(CommitGroupId group, const Plan &plan,
                   std::index_sequence<InputIndices...>,
                   std::index_sequence<OutputIndices...>) const {
    return (std::get<InputIndices>(inputs_)->hasPrepared(group) && ...) &&
           ((!std::get<OutputIndices>(plan.outputs)
                 ? true
                 : (std::get<OutputIndices>(outputs_) != nullptr &&
                    std::get<OutputIndices>(outputs_)->hasPrepared(group))) &&
            ...) &&
           ((plan.reservations.empty() && plan.writes.empty()) ||
            table_.hasPreparedWrite(group));
  }

  bool publishTable(CommitGroupId group, Plan plan) {
    if (plan.writes.empty()) {
      if (!plan.reservations.empty())
        table_.cancelPreparedWrite(group);
      return true;
    }
    return table_.publishPreparedWrite(group, std::move(plan.writes), merge_);
  }

  void cancelPrepared(CommitGroupId group) {
    std::apply([&](auto *...queues) { (queues->cancelPrepared(group), ...); },
               inputs_);
    std::apply(
        [&](auto *...queues) {
          (
              [&] {
                if (queues != nullptr)
                  queues->cancelPrepared(group);
              }(),
              ...);
        },
        outputs_);
    table_.cancelPreparedWrite(group);
  }

  SimTable<Entry> &table_;
  std::tuple<SimQueue<Inputs> *...> inputs_;
  std::tuple<SimQueue<Outputs> *...> outputs_;
  [[no_unique_address]] Policy policy_;
  [[no_unique_address]] Merge merge_;
  TableWriteMode mode_;
  std::optional<Plan> candidate_;
  bool proposed_ = false;
  bool fired_ = false;
};

/// One inferred transaction spanning multiple heterogeneous state owners and
/// zero or more Queue endpoints. Work computes only an immutable candidate;
/// stable Arbitrate order reserves and publishes every selected resource.
template <typename Policy, typename TableTypes, typename InputTypes,
          typename OutputTypes, typename MergeTypes>
class QueueStateTransition;

template <typename Policy, typename... Entries, typename... Inputs,
          typename... Outputs, typename... Merges>
class QueueStateTransition<Policy, std::tuple<Entries...>,
                           std::tuple<Inputs...>, std::tuple<Outputs...>,
                           std::tuple<Merges...>>
    final : public SimObject {
public:
  static_assert(sizeof...(Entries) == sizeof...(Merges));
  static constexpr std::string_view contractName = "ac.firing.state";
  static constexpr ObjectKind componentKind = ObjectKind::Scheduler;
  using Tables = std::tuple<SimTable<Entries> *...>;
  using InputsTuple = std::tuple<SimQueue<Inputs> *...>;
  using OutputsTuple = std::tuple<SimQueue<Outputs> *...>;
  using Plan =
      StateTransitionPlan<std::tuple<Entries...>, std::tuple<Outputs...>>;

  QueueStateTransition(std::string name, ObjectId id, SimObject *parent,
                       Tables tables, InputsTuple inputs, OutputsTuple outputs,
                       std::array<TableWriteMode, sizeof...(Entries)> modes,
                       Policy policy = {}, std::tuple<Merges...> merges = {},
                       ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        tables_(tables), inputs_(inputs), outputs_(outputs), modes_(modes),
        policy_(std::move(policy)), merges_(std::move(merges)) {
    if (id == kInvalidObjectId)
      throw std::invalid_argument(
          "state transition requires a stable object ID");
    if (std::apply(
            [](const auto *...values) { return ((values == nullptr) || ...); },
            tables_) ||
        std::apply(
            [](const auto *...values) { return ((values == nullptr) || ...); },
            inputs_) ||
        std::apply(
            [](const auto *...values) { return ((values == nullptr) || ...); },
            outputs_))
      throw std::invalid_argument("state transition endpoint is null");
  }

  void doWork(Epoch epoch) override {
    if (fired_ || candidate_ || !allInputsReady())
      return;
    const auto inputValues =
        peekInputValues(std::index_sequence_for<Inputs...>{});
    const auto tableViews =
        constTableViews(std::index_sequence_for<Entries...>{});
    auto plan = std::apply(
        [&](const auto &...values) {
          return std::invoke(std::as_const(policy_), epoch, tableViews,
                             values...);
        },
        inputValues);
    static_assert(std::same_as<decltype(plan), std::optional<Plan>>);
    if (!plan ||
        !selectedOutputsReady(*plan, std::index_sequence_for<Outputs...>{}))
      return;
    candidate_ = std::move(*plan);
  }

  void doArbitrate(Epoch) override {
    if (fired_ || !candidate_)
      return;
    Plan plan = std::move(*candidate_);
    candidate_.reset();
    const CommitGroupId group = id();
    if (!selectedOutputsReady(plan, std::index_sequence_for<Outputs...>{}) ||
        !prepareOutputs(group, plan, std::index_sequence_for<Outputs...>{}) ||
        !prepareInputs(group, std::index_sequence_for<Inputs...>{}) ||
        !prepareTables(group, plan, std::index_sequence_for<Entries...>{})) {
      cancelPrepared(group);
      return;
    }
    if (!allPrepared(group, plan, std::index_sequence_for<Entries...>{},
                     std::index_sequence_for<Inputs...>{},
                     std::index_sequence_for<Outputs...>{}) ||
        !publishOutputs(group, plan, std::index_sequence_for<Outputs...>{}) ||
        !publishInputs(group, std::index_sequence_for<Inputs...>{}) ||
        !publishTables(group, std::move(plan),
                       std::index_sequence_for<Entries...>{})) {
      setRuntimeFailureCode("state_commit_group_publish_failed");
      cancelPrepared(group);
      return;
    }
    fired_ = true;
  }

  void doXfer(Epoch) override {
    candidate_.reset();
    fired_ = false;
  }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    if constexpr (sizeof...(Inputs) == 0)
      return false;
    return !fired_ && !candidate_ && allInputsReady();
  }
  void reset() override {
    cancelPrepared(id());
    candidate_.reset();
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  bool allInputsReady() const {
    return std::apply(
        [](const auto *...queues) {
          return ((queues != nullptr && queues->canProposePop()) && ...);
        },
        inputs_);
  }

  template <size_t... Indices>
  std::tuple<Inputs...> peekInputValues(std::index_sequence<Indices...>) const {
    return std::tuple<Inputs...>{
        *std::get<Indices>(inputs_)->peekProposable()...};
  }

  template <size_t... Indices>
  auto constTableViews(std::index_sequence<Indices...>) const {
    return std::tuple<const SimTable<Entries> *...>{
        std::get<Indices>(tables_)...};
  }

  template <size_t... Indices>
  bool selectedOutputsReady(const Plan &plan,
                            std::index_sequence<Indices...>) const {
    return ((!std::get<Indices>(plan.outputs)
                 ? true
                 : std::get<Indices>(outputs_)->canProposePush()) &&
            ...);
  }

  template <size_t... Indices>
  bool prepareOutputs(CommitGroupId group, const Plan &plan,
                      std::index_sequence<Indices...>) {
    return ((!std::get<Indices>(plan.outputs)
                 ? true
                 : std::get<Indices>(outputs_)->preparePush(group)) &&
            ...);
  }

  template <size_t... Indices>
  bool prepareInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->preparePop(group) && ...);
  }

  template <size_t Index>
  bool prepareTable(CommitGroupId group, const Plan &plan) {
    const auto &write = std::get<Index>(plan.writes);
    const StateReservation &reservation = std::get<Index>(plan.reservations);
    std::array<size_t, 1> writeIndices{};
    const std::span<const size_t> selectedWrites =
        write ? std::span<const size_t>(writeIndices.data(), size_t{1})
              : std::span<const size_t>{};
    if (write)
      writeIndices[0] = write->first;
    using Merge = std::tuple_element_t<Index, std::tuple<Merges...>>;
    return (reservation.empty() && !write) ||
           std::get<Index>(tables_)->prepareTransaction(
               group, id(), reservation, selectedWrites, Merge::fields,
               modes_[Index]);
  }

  template <size_t... Indices>
  bool prepareTables(CommitGroupId group, const Plan &plan,
                     std::index_sequence<Indices...>) {
    return (prepareTable<Indices>(group, plan) && ...);
  }

  template <size_t... Indices>
  bool publishOutputs(CommitGroupId group, const Plan &plan,
                      std::index_sequence<Indices...>) {
    return ((!std::get<Indices>(plan.outputs)
                 ? true
                 : std::get<Indices>(outputs_)->publishPush(
                       group, *std::get<Indices>(plan.outputs))) &&
            ...);
  }

  template <size_t... Indices>
  bool publishInputs(CommitGroupId group, std::index_sequence<Indices...>) {
    return (std::get<Indices>(inputs_)->publishPop(group).has_value() && ...);
  }

  template <size_t Index> bool publishTable(CommitGroupId group, Plan &plan) {
    if (!std::get<Index>(plan.writes)) {
      if (!std::get<Index>(plan.reservations).empty())
        std::get<Index>(tables_)->cancelPreparedWrite(group);
      return true;
    }
    return std::get<Index>(tables_)->publishPreparedSingleWrite(
        group, std::move(std::get<Index>(plan.writes)),
        std::get<Index>(merges_));
  }

  template <size_t... Indices>
  bool publishTables(CommitGroupId group, Plan plan,
                     std::index_sequence<Indices...>) {
    return (publishTable<Indices>(group, plan) && ...);
  }

  template <size_t... TableIndices, size_t... InputIndices,
            size_t... OutputIndices>
  bool allPrepared(CommitGroupId group, const Plan &plan,
                   std::index_sequence<TableIndices...>,
                   std::index_sequence<InputIndices...>,
                   std::index_sequence<OutputIndices...>) const {
    return (((std::get<TableIndices>(plan.reservations).empty() &&
              !std::get<TableIndices>(plan.writes)) ||
             std::get<TableIndices>(tables_)->hasPreparedWrite(group)) &&
            ...) &&
           (std::get<InputIndices>(inputs_)->hasPrepared(group) && ...) &&
           ((!std::get<OutputIndices>(plan.outputs)
                 ? true
                 : std::get<OutputIndices>(outputs_)->hasPrepared(group)) &&
            ...);
  }

  void cancelPrepared(CommitGroupId group) {
    std::apply([&](auto *...queues) { (queues->cancelPrepared(group), ...); },
               inputs_);
    std::apply([&](auto *...queues) { (queues->cancelPrepared(group), ...); },
               outputs_);
    std::apply(
        [&](auto *...tables) { (tables->cancelPreparedWrite(group), ...); },
        tables_);
  }

  Tables tables_;
  InputsTuple inputs_;
  OutputsTuple outputs_;
  std::array<TableWriteMode, sizeof...(Entries)> modes_;
  [[no_unique_address]] Policy policy_;
  std::tuple<Merges...> merges_;
  std::optional<Plan> candidate_;
  bool fired_ = false;
};

template <typename Input, typename Entry, typename Address, typename When>
  requires TablePolicyInvocable<Address, const Input &> &&
           IntegralLike<TablePolicyResult<Address, const Input &>> &&
           TablePolicyInvocable<When, const Input &> &&
           std::convertible_to<TablePolicyResult<When, const Input &>, bool>
class QueueTableRead final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.table.read";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  QueueTableRead(std::string name, ObjectId id, SimObject *parent,
                 SimTable<Entry> &table, SimQueue<Input> &input,
                 SimQueue<Entry> &output, Address address = {}, When when = {},
                 ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        table_(table), input_(input), output_(output),
        address_(std::move(address)), when_(std::move(when)) {}

  void doWork(Epoch epoch) override {
    if (fired_ || !input_.canProposePop())
      return;
    const Input *head = input_.peekProposable();
    if (!head || !static_cast<bool>(invokeTablePolicy(when_, epoch, *head)))
      return;
    if (!output_.canProposePush())
      return;
    const auto address = invokeTablePolicy(address_, epoch, *head);
    if (!tableAddressInRange(address, table_.size())) {
      setRuntimeFailureCode("table_index_out_of_range");
      return;
    }
    if (!output_.proposePush(table_.at(static_cast<size_t>(address))) ||
        !input_.proposePop())
      return;
    fired_ = true;
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch epoch) const override {
    if (fired_ || !input_.canProposePop())
      return false;
    const Input *head = input_.peekProposable();
    return head && static_cast<bool>(invokeTablePolicy(when_, epoch, *head)) &&
           output_.canProposePush();
  }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimTable<Entry> &table_;
  SimQueue<Input> &input_;
  SimQueue<Entry> &output_;
  [[no_unique_address]] Address address_;
  [[no_unique_address]] When when_;
  bool fired_ = false;
};

template <typename Entry, typename Address, typename When>
  requires TablePolicyInvocable<Address> &&
           IntegralLike<TablePolicyResult<Address>> &&
           TablePolicyInvocable<When> &&
           std::convertible_to<TablePolicyResult<When>, bool>
class TableReadSource final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.table.read";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  TableReadSource(std::string name, ObjectId id, SimObject *parent,
                  SimTable<Entry> &table, SimQueue<Entry> &output,
                  Address address = {}, When when = {},
                  ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        table_(table), output_(output), address_(std::move(address)),
        when_(std::move(when)) {}

  void doWork(Epoch epoch) override {
    if (fired_ || !static_cast<bool>(invokeTablePolicy(when_, epoch)) ||
        !output_.canProposePush())
      return;
    const auto address = invokeTablePolicy(address_, epoch);
    if (!tableAddressInRange(address, table_.size())) {
      setRuntimeFailureCode("table_index_out_of_range");
      return;
    }
    fired_ = output_.proposePush(table_.at(static_cast<size_t>(address)));
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch epoch) const override {
    return !fired_ && static_cast<bool>(invokeTablePolicy(when_, epoch)) &&
           output_.canProposePush();
  }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimTable<Entry> &table_;
  SimQueue<Entry> &output_;
  [[no_unique_address]] Address address_;
  [[no_unique_address]] When when_;
  bool fired_ = false;
};

template <typename Input, typename Entry, typename Address, typename Enable,
          typename Value, typename Merge = TableFullEntryMerge<Entry>>
  requires TablePolicyInvocable<Address, const Input &> &&
           IntegralLike<TablePolicyResult<Address, const Input &>> &&
           TablePolicyInvocable<Enable, const Input &> &&
           std::convertible_to<TablePolicyResult<Enable, const Input &>,
                               bool> &&
           TablePolicyInvocable<Value, const Input &> &&
           std::convertible_to<TablePolicyResult<Value, const Input &>,
                               Entry> &&
           std::invocable<const Merge &, Entry &, const Entry &>
class QueueTableWrite final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.table.write";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  QueueTableWrite(std::string name, ObjectId id, SimObject *parent,
                  SimTable<Entry> &table, SimQueue<Input> &input,
                  Address address = {}, Enable enable = {}, Value value = {},
                  Merge merge = {},
                  TableWriteMode mode = TableWriteMode::FieldMerge,
                  ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        table_(table), input_(input), address_(std::move(address)),
        enable_(std::move(enable)), value_(std::move(value)),
        merge_(std::move(merge)), writerId_(id), mode_(mode) {}

  void doWork(Epoch epoch) override {
    if (fired_ || !input_.canProposePop())
      return;
    const Input *head = input_.peekProposable();
    if (!head)
      return;
    proposed_ = false;
    if (static_cast<bool>(invokeTablePolicy(enable_, epoch, *head))) {
      const auto address = invokeTablePolicy(address_, epoch, *head);
      if (!tableAddressInRange(address, table_.size())) {
        setRuntimeFailureCode("table_index_out_of_range");
        return;
      }
      if (!table_.proposeWrite(
              writerId_, static_cast<size_t>(address),
              static_cast<Entry>(invokeTablePolicy(value_, epoch, *head)),
              Merge::fields, merge_, mode_)) {
        setRuntimeFailureCode("table_write_conflict");
        return;
      }
      proposed_ = true;
    }
    if (!input_.proposePop()) {
      if (proposed_)
        table_.cancelWrite(writerId_);
      proposed_ = false;
      return;
    }
    fired_ = true;
  }
  void doXfer(Epoch) override {
    proposed_ = false;
    fired_ = false;
  }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    return !fired_ && input_.canProposePop();
  }
  void reset() override {
    if (proposed_)
      table_.cancelWrite(writerId_);
    proposed_ = false;
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimTable<Entry> &table_;
  SimQueue<Input> &input_;
  [[no_unique_address]] Address address_;
  [[no_unique_address]] Enable enable_;
  [[no_unique_address]] Value value_;
  [[no_unique_address]] Merge merge_;
  ObjectId writerId_;
  TableWriteMode mode_;
  bool proposed_ = false;
  bool fired_ = false;
};

template <typename Entry, typename Address, typename Enable, typename Value,
          typename Merge = TableFullEntryMerge<Entry>>
  requires TablePolicyInvocable<Address> &&
           IntegralLike<TablePolicyResult<Address>> &&
           TablePolicyInvocable<Enable> &&
           std::convertible_to<TablePolicyResult<Enable>, bool> &&
           TablePolicyInvocable<Value> &&
           std::convertible_to<TablePolicyResult<Value>, Entry> &&
           std::invocable<const Merge &, Entry &, const Entry &>
class TableWriteSource final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.table.write";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  TableWriteSource(std::string name, ObjectId id, SimObject *parent,
                   SimTable<Entry> &table, Address address = {},
                   Enable enable = {}, Value value = {}, Merge merge = {},
                   TableWriteMode mode = TableWriteMode::FieldMerge,
                   ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        table_(table), address_(std::move(address)), enable_(std::move(enable)),
        value_(std::move(value)), merge_(std::move(merge)), writerId_(id),
        mode_(mode) {}

  void doWork(Epoch epoch) override {
    if (fired_ || !static_cast<bool>(invokeTablePolicy(enable_, epoch)))
      return;
    const auto address = invokeTablePolicy(address_, epoch);
    if (!tableAddressInRange(address, table_.size())) {
      setRuntimeFailureCode("table_index_out_of_range");
      return;
    }
    if (!table_.proposeWrite(
            writerId_, static_cast<size_t>(address),
            static_cast<Entry>(invokeTablePolicy(value_, epoch)), Merge::fields,
            merge_, mode_)) {
      setRuntimeFailureCode("table_write_conflict");
      return;
    }
    proposed_ = true;
    fired_ = true;
  }
  void doXfer(Epoch) override {
    proposed_ = false;
    fired_ = false;
  }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch epoch) const override {
    return !fired_ && static_cast<bool>(invokeTablePolicy(enable_, epoch));
  }
  void reset() override {
    if (proposed_)
      table_.cancelWrite(writerId_);
    proposed_ = false;
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimTable<Entry> &table_;
  [[no_unique_address]] Address address_;
  [[no_unique_address]] Enable enable_;
  [[no_unique_address]] Value value_;
  [[no_unique_address]] Merge merge_;
  ObjectId writerId_;
  TableWriteMode mode_;
  bool proposed_ = false;
  bool fired_ = false;
};

template <typename Entry, typename Mask, typename Enable, typename Value,
          typename Merge = TableFullEntryMerge<Entry>>
  requires TablePolicyInvocable<Mask> &&
           IntegralLike<TablePolicyResult<Mask>> &&
           TablePolicyInvocable<Enable> &&
           std::convertible_to<TablePolicyResult<Enable>, bool> &&
           TablePolicyInvocable<Value, const Entry &> &&
           std::convertible_to<TablePolicyResult<Value, const Entry &>,
                               Entry> &&
           std::invocable<const Merge &, Entry &, const Entry &>
class TableMaskedWriteSource final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.table.masked_write";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  TableMaskedWriteSource(std::string name, ObjectId id, SimObject *parent,
                         SimTable<Entry> &table, Mask mask = {},
                         Enable enable = {}, Value value = {}, Merge merge = {},
                         ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        table_(table), mask_(std::move(mask)), enable_(std::move(enable)),
        value_(std::move(value)), merge_(std::move(merge)), writerId_(id) {}

  void doWork(Epoch epoch) override {
    if (fired_ || !static_cast<bool>(invokeTablePolicy(enable_, epoch)))
      return;
    const auto rawMask = invokeTablePolicy(mask_, epoch);
    const uint64_t mask = static_cast<uint64_t>(rawMask);
    std::vector<std::pair<size_t, Entry>> values;
    values.reserve(table_.size());
    for (size_t index = 0; index < table_.size(); ++index) {
      if ((mask & (uint64_t{1} << index)) == 0)
        continue;
      values.emplace_back(index, static_cast<Entry>(invokeTablePolicy(
                                     value_, epoch, table_.at(index))));
    }
    if (!table_.proposeMaskedWrite(writerId_, std::move(values), Merge::fields,
                                   merge_)) {
      setRuntimeFailureCode("table_write_conflict");
      return;
    }
    proposed_ = true;
    fired_ = true;
  }
  void doXfer(Epoch) override {
    proposed_ = false;
    fired_ = false;
  }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch epoch) const override {
    return !fired_ && static_cast<bool>(invokeTablePolicy(enable_, epoch));
  }
  void reset() override {
    if (proposed_)
      table_.cancelWrite(writerId_);
    proposed_ = false;
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimTable<Entry> &table_;
  [[no_unique_address]] Mask mask_;
  [[no_unique_address]] Enable enable_;
  [[no_unique_address]] Value value_;
  [[no_unique_address]] Merge merge_;
  ObjectId writerId_;
  bool proposed_ = false;
  bool fired_ = false;
};

/// A committed one-entry Queue capture.  Empty slots capture one input token;
/// full slots apply only the release decision and deliberately do not refill
/// until a later tick.  Releasing retains the old payload for observability.
template <typename T> struct SlotState {
  bool valid = false;
  T value{};
};

template <typename T, typename Release>
  requires(
      requires(const Release &release) {
        { std::invoke(release) } -> std::convertible_to<bool>;
      } ||
      requires(const Release &release, Epoch epoch) {
        { std::invoke(release, epoch) } -> std::convertible_to<bool>;
      })
class QueueSlot final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.slot";
  static constexpr ObjectKind componentKind = ObjectKind::Queue;

  QueueSlot(std::string name, ObjectId id, SimObject *parent,
            SimQueue<T> &input, SlotState<T> &state, Release release = {},
            ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), state_(state), release_(std::move(release)) {}

  bool valid() const { return state_.valid; }
  const T &value() const { return state_.value; }

  void doWork(Epoch epoch) override {
    if (fired_)
      return;
    if (state_.valid) {
      if (releaseEnabled(epoch)) {
        pendingRelease_ = true;
        fired_ = true;
      }
      return;
    }
    if (!input_.canProposePop())
      return;
    const T *head = input_.peekProposable();
    if (!head || !input_.proposePop())
      return;
    pendingPayload_ = *head;
    pendingCapture_ = true;
    fired_ = true;
  }

  void doXfer(Epoch) override {
    if (pendingRelease_)
      state_.valid = false;
    else if (pendingCapture_) {
      state_.value = std::move(pendingPayload_);
      state_.valid = true;
    }
    pendingRelease_ = false;
    pendingCapture_ = false;
    fired_ = false;
  }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch epoch) const override {
    if (fired_)
      return false;
    return state_.valid ? releaseEnabled(epoch) : input_.canProposePop();
  }
  void reset() override {
    state_.valid = false;
    state_.value = T{};
    pendingPayload_ = T{};
    pendingRelease_ = false;
    pendingCapture_ = false;
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  bool releaseEnabled(Epoch epoch) const {
    if constexpr (std::invocable<const Release &, Epoch>)
      return static_cast<bool>(std::invoke(std::as_const(release_), epoch));
    else
      return static_cast<bool>(std::invoke(std::as_const(release_)));
  }

  SimQueue<T> &input_;
  SlotState<T> &state_;
  [[no_unique_address]] Release release_;
  T pendingPayload_{};
  bool pendingRelease_ = false;
  bool pendingCapture_ = false;
  bool fired_ = false;
};

/// One physical, single-outstanding memory shared by a statically ordered set
/// of logical request/response endpoints.  Requests are accepted in endpoint
/// index order only while idle.  The selected response is retained until its
/// response Queue accepts it; no other request is admitted while busy.
template <typename T, typename Data, size_t N, typename Address, typename Write,
          typename WriteData, typename Response>
  requires std::invocable<const Address &, size_t, const T &> &&
           IntegralLike<
               std::invoke_result_t<const Address &, size_t, const T &>> &&
           std::invocable<const Write &, size_t, const T &> &&
           std::convertible_to<
               std::invoke_result_t<const Write &, size_t, const T &>, bool> &&
           std::invocable<const WriteData &, size_t, const T &> &&
           std::convertible_to<
               std::invoke_result_t<const WriteData &, size_t, const T &>,
               Data> &&
           std::invocable<const Response &, size_t, const T &, const Data &> &&
           std::convertible_to<std::invoke_result_t<const Response &, size_t,
                                                    const T &, const Data &>,
                               T>
class QueueMemoryArbiter final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.memory.instance";
  static constexpr ObjectKind componentKind = ObjectKind::Memory;

  QueueMemoryArbiter(std::string name, ObjectId id, SimObject *parent,
                     std::array<SimQueue<T> *, N> inputs,
                     std::array<SimQueue<T> *, N> outputs, size_t entries,
                     Data init = {}, size_t latency = 1, Address address = {},
                     Write write = {}, WriteData writeData = {},
                     Response response = {},
                     ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        inputs_(inputs), outputs_(outputs), init_(init),
        storage_(entries, init), latency_(latency),
        address_(std::move(address)), write_(std::move(write)),
        writeData_(std::move(writeData)), response_(std::move(response)) {
    if (latency_ == 0)
      throw std::invalid_argument("memory latency must be positive");
  }

  void doWork(Epoch epoch) override {
    if (fired_)
      return;
    if (busy_) {
      if (!responseReady_ || epoch < *responseReady_) {
        ticking_ = true;
        fired_ = true;
        workEpoch_ = epoch;
        return;
      }
      if (!outputs_[selected_]->canProposePush() || !pendingResponse_)
        return;
      if (!outputs_[selected_]->proposePush(*pendingResponse_))
        return;
      completing_ = true;
      fired_ = true;
      workEpoch_ = epoch;
      return;
    }
    if (completedEpoch_ && *completedEpoch_ == epoch)
      return;
    for (size_t endpoint = 0; endpoint < N; ++endpoint) {
      if (!inputs_[endpoint]->canProposePop())
        continue;
      const T *head = inputs_[endpoint]->peek();
      if (!head)
        continue;
      using AddressResult =
          std::invoke_result_t<const Address &, size_t, const T &>;
      const AddressResult rawAddress =
          std::invoke(std::as_const(address_), endpoint, *head);
      if constexpr (std::signed_integral<AddressResult>)
        if (rawAddress < 0) {
          setRuntimeFailureCode("memory_address_out_of_range");
          return;
        }
      const uint64_t address = static_cast<uint64_t>(rawAddress);
      if (address >= storage_.size()) {
        setRuntimeFailureCode("memory_address_out_of_range");
        return;
      }
      const Data oldData = storage_[address];
      if (epoch.time > std::numeric_limits<uint64_t>::max() - latency_) {
        setRuntimeFailureCode("memory_latency_overflow");
        return;
      }
      pendingResponse_ =
          std::invoke(std::as_const(response_), endpoint, *head, oldData);
      responseReady_ = Epoch{epoch.time + latency_, 0};
      if (!inputs_[endpoint]->proposePop()) {
        pendingResponse_.reset();
        responseReady_.reset();
        return;
      }
      if (static_cast<bool>(
              std::invoke(std::as_const(write_), endpoint, *head)))
        pendingWrite_ = std::pair<size_t, Data>{
            static_cast<size_t>(address),
            static_cast<Data>(
                std::invoke(std::as_const(writeData_), endpoint, *head))};
      selected_ = endpoint;
      accepting_ = true;
      fired_ = true;
      workEpoch_ = epoch;
      return;
    }
  }

  void doXfer(Epoch) override {
    if (accepting_) {
      if (pendingWrite_) {
        storage_[pendingWrite_->first] = std::move(pendingWrite_->second);
        pendingWrite_.reset();
      }
      busy_ = true;
    }
    if (completing_) {
      busy_ = false;
      pendingResponse_.reset();
      responseReady_.reset();
      completedEpoch_ = workEpoch_;
    }
    accepting_ = false;
    completing_ = false;
    ticking_ = false;
    fired_ = false;
  }

  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch epoch) const override {
    if (fired_)
      return false;
    if (busy_) {
      if (!responseReady_ || epoch < *responseReady_)
        return true;
      return pendingResponse_.has_value() &&
             outputs_[selected_]->canProposePush();
    }
    if (completedEpoch_ && *completedEpoch_ == epoch)
      return false;
    return std::any_of(
        inputs_.begin(), inputs_.end(),
        [](const SimQueue<T> *queue) { return queue->canProposePop(); });
  }
  bool busy() const { return busy_; }
  size_t latency() const { return latency_; }
  size_t selectedEndpoint() const { return selected_; }
  const Data &at(size_t address) const { return storage_.at(address); }
  void reset() override {
    std::fill(storage_.begin(), storage_.end(), init_);
    pendingResponse_.reset();
    responseReady_.reset();
    pendingWrite_.reset();
    completedEpoch_.reset();
    busy_ = accepting_ = completing_ = ticking_ = fired_ = false;
    selected_ = 0;
    clearRuntimeFailureCode();
  }

private:
  std::array<SimQueue<T> *, N> inputs_;
  std::array<SimQueue<T> *, N> outputs_;
  Data init_;
  std::vector<Data> storage_;
  size_t latency_ = 1;
  [[no_unique_address]] Address address_;
  [[no_unique_address]] Write write_;
  [[no_unique_address]] WriteData writeData_;
  [[no_unique_address]] Response response_;
  std::optional<T> pendingResponse_;
  std::optional<Epoch> responseReady_;
  std::optional<std::pair<size_t, Data>> pendingWrite_;
  std::optional<Epoch> completedEpoch_;
  Epoch workEpoch_{};
  size_t selected_ = 0;
  bool busy_ = false;
  bool accepting_ = false;
  bool completing_ = false;
  bool ticking_ = false;
  bool fired_ = false;
};

template <typename T> class QueueSink final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.sink";
  static constexpr ObjectKind componentKind = ObjectKind::Sink;

  QueueSink(std::string name, ObjectId id, SimObject *parent,
            SimQueue<T> &input, ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input) {}

  void doWork(Epoch) override {
    if (pending_ || !input_.canProposePop())
      return;
    pending_ = input_.proposePop();
  }
  void doXfer(Epoch) override {
    if (pending_)
      received_.push_back(std::move(*pending_));
    pending_.reset();
  }
  bool hasPendingCommit() const override { return pending_.has_value(); }
  bool isRunnable(Epoch) const override {
    return !pending_ && input_.canProposePop();
  }
  const std::vector<T> &received() const { return received_; }
  void reset() override {
    pending_.reset();
    received_.clear();
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  std::optional<T> pending_;
  std::vector<T> received_;
};

template <typename T>
  requires std::equality_comparable<T>
class QueueObserve final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.observe";
  static constexpr ObjectKind componentKind = ObjectKind::Probe;

  QueueObserve(std::string name, ObjectId id, SimObject *parent,
               SimQueue<T> &input, ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input) {}

  void doWork(Epoch) override {
    const T *head = input_.peek();
    if (pending_ || head == nullptr)
      return;
    if (last_ && input_.totalPops() == lastPopCount_ && *last_ == *head)
      return;
    pending_ = *head;
    pendingPopCount_ = input_.totalPops();
  }
  void doXfer(Epoch) override {
    if (!pending_)
      return;
    observed_.push_back(*pending_);
    last_ = std::move(pending_);
    pending_.reset();
    lastPopCount_ = pendingPopCount_;
  }
  bool hasPendingCommit() const override { return pending_.has_value(); }
  bool isRunnable(Epoch) const override {
    const T *head = input_.peek();
    return !pending_ && head != nullptr &&
           (!last_ || input_.totalPops() != lastPopCount_ || *last_ != *head);
  }
  const std::vector<T> &observed() const { return observed_; }
  void reset() override {
    pending_.reset();
    last_.reset();
    observed_.clear();
    lastPopCount_ = 0;
    pendingPopCount_ = 0;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  std::optional<T> pending_;
  std::optional<T> last_;
  std::vector<T> observed_;
  uint64_t lastPopCount_ = 0;
  uint64_t pendingPopCount_ = 0;
};

template <typename T, typename Predicate>
  requires std::equality_comparable<T> &&
           std::predicate<const Predicate &, const T &>
class QueueExpect final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.expect";
  static constexpr ObjectKind componentKind = ObjectKind::Probe;

  QueueExpect(std::string name, ObjectId id, SimObject *parent,
              SimQueue<T> &input, std::string message, Predicate predicate = {},
              ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), message_(std::move(message)),
        predicate_(std::move(predicate)) {}

  void doWork(Epoch) override {
    const T *head = input_.peek();
    if (pending_ || head == nullptr)
      return;
    if (last_ && input_.totalPops() == lastPopCount_ && *last_ == *head)
      return;
    if (!std::invoke(std::as_const(predicate_), *head)) {
      setRuntimeFailureCode("expectation_failed");
      return;
    }
    pending_ = *head;
    pendingPopCount_ = input_.totalPops();
  }
  void doXfer(Epoch) override {
    if (!pending_)
      return;
    last_ = std::move(pending_);
    pending_.reset();
    lastPopCount_ = pendingPopCount_;
  }
  bool hasPendingCommit() const override { return pending_.has_value(); }
  bool isRunnable(Epoch) const override {
    const T *head = input_.peek();
    return !pending_ && head != nullptr &&
           (!last_ || input_.totalPops() != lastPopCount_ || *last_ != *head);
  }
  std::string_view message() const { return message_; }
  void reset() override {
    pending_.reset();
    last_.reset();
    lastPopCount_ = 0;
    pendingPopCount_ = 0;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  std::string message_;
  [[no_unique_address]] Predicate predicate_;
  std::optional<T> pending_;
  std::optional<T> last_;
  uint64_t lastPopCount_ = 0;
  uint64_t pendingPopCount_ = 0;
};

template <typename T, size_t Outputs>
class QueueBroadcast final : public SimObject {
public:
  static_assert(Outputs >= 2);
  static constexpr std::string_view contractName = "ac.broadcast";
  static constexpr ObjectKind componentKind = ObjectKind::Link;

  QueueBroadcast(std::string name, ObjectId id, SimObject *parent,
                 SimQueue<T> &input, std::array<SimQueue<T> *, Outputs> outputs,
                 ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), outputs_(outputs) {}

  void doWork(Epoch) override {
    if (fired_ || !input_.canProposePop() ||
        std::any_of(outputs_.begin(), outputs_.end(), [](const auto *output) {
          return output == nullptr || !output->canProposePush();
        }))
      return;
    const T *head = input_.peek();
    if (head == nullptr)
      return;
    for (SimQueue<T> *output : outputs_)
      if (!output->proposePush(*head))
        return;
    if (!input_.proposePop())
      return;
    fired_ = true;
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    return !fired_ && input_.canProposePop() &&
           std::all_of(outputs_.begin(), outputs_.end(),
                       [](const auto *output) {
                         return output != nullptr && output->canProposePush();
                       });
  }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  std::array<SimQueue<T> *, Outputs> outputs_;
  bool fired_ = false;
};

template <typename T, size_t Outputs> class QueueFork final : public SimObject {
public:
  static_assert(Outputs >= 2);
  static constexpr std::string_view contractName = "ac.fork";
  static constexpr ObjectKind componentKind = ObjectKind::Link;

  QueueFork(std::string name, ObjectId id, SimObject *parent,
            SimQueue<T> &input, std::array<SimQueue<T> *, Outputs> outputs,
            ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), outputs_(outputs) {}

  void doWork(Epoch) override {
    if (proposal_)
      return;
    const T *token = pending_ ? &*pending_ : input_.peek();
    if (token == nullptr)
      return;
    std::array<bool, Outputs> next = delivered_;
    bool changed = !pending_.has_value();
    for (size_t index = 0; index < Outputs; ++index) {
      SimQueue<T> *output = outputs_[index];
      if (next[index] || output == nullptr || !output->canProposePush())
        continue;
      if (!output->proposePush(*token))
        continue;
      next[index] = true;
      changed = true;
    }
    const bool deliveredAll =
        std::all_of(next.begin(), next.end(), [](bool value) { return value; });
    bool complete = false;
    if (deliveredAll && input_.canProposePop()) {
      complete = input_.proposePop().has_value();
      changed = changed || complete;
    }
    if (!changed)
      return;
    proposedToken_ = *token;
    proposedDelivered_ = next;
    proposedComplete_ = complete;
    proposal_ = true;
  }
  void doXfer(Epoch) override {
    if (!proposal_)
      return;
    if (proposedComplete_) {
      pending_.reset();
      delivered_.fill(false);
    } else {
      pending_ = std::move(proposedToken_);
      delivered_ = proposedDelivered_;
    }
    proposedToken_.reset();
    proposedDelivered_.fill(false);
    proposedComplete_ = false;
    proposal_ = false;
  }
  bool hasPendingCommit() const override { return proposal_; }
  bool isRunnable(Epoch) const override {
    if (proposal_ || (!pending_ && input_.peek() == nullptr))
      return false;
    for (size_t index = 0; index < Outputs; ++index)
      if (!delivered_[index] && outputs_[index] != nullptr &&
          outputs_[index]->canProposePush())
        return true;
    return false;
  }
  void reset() override {
    pending_.reset();
    proposedToken_.reset();
    delivered_.fill(false);
    proposedDelivered_.fill(false);
    proposedComplete_ = false;
    proposal_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  std::array<SimQueue<T> *, Outputs> outputs_;
  std::optional<T> pending_;
  std::optional<T> proposedToken_;
  std::array<bool, Outputs> delivered_{};
  std::array<bool, Outputs> proposedDelivered_{};
  bool proposedComplete_ = false;
  bool proposal_ = false;
};

template <typename T, size_t Outputs, typename Selector>
  requires std::invocable<const Selector &, const T &> &&
           IntegralLike<std::invoke_result_t<const Selector &, const T &>>
class QueueRoute final : public SimObject {
public:
  static_assert(Outputs >= 2);
  static constexpr std::string_view contractName = "ac.route";
  static constexpr ObjectKind componentKind = ObjectKind::Link;

  QueueRoute(std::string name, ObjectId id, SimObject *parent,
             SimQueue<T> &input, std::array<SimQueue<T> *, Outputs> outputs,
             Selector selector = {}, ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), outputs_(outputs), selector_(std::move(selector)) {}

  void doWork(Epoch) override {
    if (fired_ || !input_.canProposePop())
      return;
    const T *head = input_.peek();
    if (head == nullptr)
      return;
    auto selected = std::invoke(std::as_const(selector_), *head);
    if constexpr (std::signed_integral<decltype(selected)>)
      if (selected < 0) {
        setRuntimeFailureCode("route_selector_out_of_range");
        return;
      }
    const size_t index = static_cast<size_t>(selected);
    if (index >= Outputs || outputs_[index] == nullptr) {
      setRuntimeFailureCode("route_selector_out_of_range");
      return;
    }
    if (!outputs_[index]->canProposePush())
      return;
    if (!outputs_[index]->proposePush(*head) || !input_.proposePop())
      return;
    fired_ = true;
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<T> &input_;
  std::array<SimQueue<T> *, Outputs> outputs_;
  [[no_unique_address]] Selector selector_;
  bool fired_ = false;
};

template <typename Control, typename T, size_t Inputs, typename Selector>
  requires std::invocable<const Selector &, const Control &> &&
           IntegralLike<std::invoke_result_t<const Selector &, const Control &>>
class QueueSelect final : public SimObject {
public:
  static_assert(Inputs >= 2);
  static constexpr std::string_view contractName = "ac.select";
  static constexpr ObjectKind componentKind = ObjectKind::Link;

  QueueSelect(std::string name, ObjectId id, SimObject *parent,
              SimQueue<Control> &control,
              std::array<SimQueue<T> *, Inputs> inputs, SimQueue<T> &output,
              Selector selector = {}, ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        control_(control), inputs_(inputs), output_(output),
        selector_(std::move(selector)) {}

  void doWork(Epoch) override {
    if (fired_ || !control_.canProposePop() || !output_.canProposePush())
      return;
    const Control *control = control_.peek();
    if (control == nullptr)
      return;
    auto selected = std::invoke(std::as_const(selector_), *control);
    if constexpr (std::signed_integral<decltype(selected)>)
      if (selected < 0) {
        setRuntimeFailureCode("select_selector_out_of_range");
        return;
      }
    const size_t index = static_cast<size_t>(selected);
    if (index >= Inputs || inputs_[index] == nullptr) {
      setRuntimeFailureCode("select_selector_out_of_range");
      return;
    }
    SimQueue<T> &input = *inputs_[index];
    const T *head = input.peek();
    if (!input.canProposePop() || head == nullptr)
      return;
    if (!output_.proposePush(*head) || !input.proposePop() ||
        !control_.proposePop())
      return;
    fired_ = true;
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  bool isRunnable(Epoch) const override {
    return !fired_ && control_.canProposePop() && output_.canProposePush();
  }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  SimQueue<Control> &control_;
  std::array<SimQueue<T> *, Inputs> inputs_;
  SimQueue<T> &output_;
  [[no_unique_address]] Selector selector_;
  bool fired_ = false;
};

enum class QueueMergePolicy { RoundRobin, Priority };

template <typename T, size_t Inputs> class QueueMerge final : public SimObject {
public:
  static_assert(Inputs >= 2);
  static constexpr std::string_view contractName = "ac.merge";
  static constexpr ObjectKind componentKind = ObjectKind::Link;

  QueueMerge(std::string name, ObjectId id, SimObject *parent,
             std::array<SimQueue<T> *, Inputs> inputs, SimQueue<T> &output,
             QueueMergePolicy policy = QueueMergePolicy::RoundRobin,
             ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        inputs_(inputs), output_(output), policy_(policy) {}

  void doWork(Epoch) override {
    if (selected_ || !output_.canProposePush())
      return;
    const size_t start =
        policy_ == QueueMergePolicy::RoundRobin ? cursor_ : size_t{0};
    for (size_t offset = 0; offset < Inputs; ++offset) {
      const size_t index = (start + offset) % Inputs;
      SimQueue<T> *input = inputs_[index];
      if (input == nullptr || !input->canProposePop())
        continue;
      const T *head = input->peek();
      if (head == nullptr || !output_.proposePush(*head) ||
          !input->proposePop())
        return;
      selected_ = index;
      return;
    }
  }
  void doXfer(Epoch) override {
    if (selected_ && policy_ == QueueMergePolicy::RoundRobin)
      cursor_ = (*selected_ + 1) % Inputs;
    selected_.reset();
  }
  bool hasPendingCommit() const override { return selected_.has_value(); }
  void reset() override {
    cursor_ = 0;
    selected_.reset();
    clearRuntimeFailureCode();
  }

private:
  std::array<SimQueue<T> *, Inputs> inputs_;
  SimQueue<T> &output_;
  QueueMergePolicy policy_;
  size_t cursor_ = 0;
  std::optional<size_t> selected_;
};

template <typename T> struct FeedbackToken {
  T value;
  size_t iteration = 0;
};

template <typename T, typename Update, typename Condition>
  requires std::invocable<const Update &, const T &> &&
           std::convertible_to<std::invoke_result_t<const Update &, const T &>,
                               T> &&
           std::predicate<const Condition &, const T &>
class QueueFeedback final : public SimObject {
public:
  static constexpr std::string_view contractName = "ac.feedback";
  static constexpr ObjectKind componentKind = ObjectKind::Compute;

  QueueFeedback(std::string name, ObjectId id, SimObject *parent,
                SimQueue<T> &input, SimQueue<FeedbackToken<T>> &feedback,
                SimQueue<T> &output, size_t maxIterations, Update update = {},
                Condition condition = {},
                ObservationSink *observations = nullptr)
      : SimObject(componentKind, std::move(name), id, parent, observations),
        input_(input), feedback_(feedback), output_(output),
        maxIterations_(maxIterations), update_(std::move(update)),
        condition_(std::move(condition)) {}

  void doWork(Epoch) override {
    if (fired_)
      return;
    if (feedback_.canProposePop()) {
      const FeedbackToken<T> *head = feedback_.peek();
      if (head != nullptr)
        fire(*head, feedback_);
      return;
    }
    if (!input_.canProposePop())
      return;
    const T *head = input_.peek();
    if (head != nullptr)
      fire(FeedbackToken<T>{*head, 0}, input_);
  }
  void doXfer(Epoch) override { fired_ = false; }
  bool hasPendingCommit() const override { return fired_; }
  void reset() override {
    fired_ = false;
    clearRuntimeFailureCode();
  }

private:
  template <typename InputQueue>
  void fire(const FeedbackToken<T> &token, InputQueue &source) {
    if (!std::invoke(std::as_const(condition_), token.value)) {
      if (!output_.canProposePush() || !output_.proposePush(token.value) ||
          !source.proposePop())
        return;
      fired_ = true;
      return;
    }
    if (token.iteration >= maxIterations_) {
      setRuntimeFailureCode("feedback_iteration_limit");
      return;
    }
    const bool replacesFeedback =
        std::same_as<InputQueue, SimQueue<FeedbackToken<T>>>;
    const bool canPush = replacesFeedback ? feedback_.canProposePushAfterPop()
                                          : feedback_.canProposePush();
    if (!canPush)
      return;
    FeedbackToken<T> next{std::invoke(std::as_const(update_), token.value),
                          token.iteration + 1};
    if (!source.proposePop() || !feedback_.proposePush(std::move(next)))
      return;
    fired_ = true;
  }

  SimQueue<T> &input_;
  SimQueue<FeedbackToken<T>> &feedback_;
  SimQueue<T> &output_;
  size_t maxIterations_;
  [[no_unique_address]] Update update_;
  [[no_unique_address]] Condition condition_;
  bool fired_ = false;
};

} // namespace gfsim

#endif // GFSIM_QUEUE_BLOCKS_H
