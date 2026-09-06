#include "acir/Analysis/ModelAnalysis.h"

#include "Dialect/ACIR/ProcessLowerability.h"
#include "ModelAnalysisInternal.h"
#include "ModelAnalysisTestHooks.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACIR/GraphRegion.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

using namespace mlir;

namespace acir {
namespace {

bool isTopologyDigestAttribute(StringRef name) {
  // Every other freeze attribute participates in the integrity digest. Only
  // the digest itself must be omitted to avoid self-reference.
  return name == "ac.topology_digest";
}

std::string attributeToken(Attribute attribute) {
  std::string storage;
  llvm::raw_string_ostream stream(storage);
  stream << attribute;
  return storage;
}

StringRef symbolName(Operation *operation) {
  if (auto name = operation->getAttrOfType<StringAttr>(
          SymbolTable::getSymbolAttrName()))
    return name.getValue();
  return {};
}

std::string ownerJoinKey(Operation *declaration, StringRef path,
                         StringRef stableId) {
  std::string key;
  llvm::raw_string_ostream stream(key);
  if (auto definition = declaration->getParentOfType<ac::ModuleOp>())
    stream << definition.getSymName();
  stream << "::" << declaration->getName().getStringRef()
         << "::" << symbolName(declaration) << '\0' << path << '\0' << stableId;
  return key;
}

std::string operationKey(Operation *operation) {
  std::string key = operation->getName().getStringRef().str();
  key.push_back('|');
  key.append(symbolName(operation));
  key.push_back('|');
  SmallVector<NamedAttribute> attributes(operation->getAttrs().begin(),
                                         operation->getAttrs().end());
  llvm::sort(attributes, [](NamedAttribute left, NamedAttribute right) {
    return left.getName().getValue() < right.getName().getValue();
  });
  for (NamedAttribute attribute : attributes) {
    if (isTopologyDigestAttribute(attribute.getName().getValue()))
      continue;
    key.append(attribute.getName().getValue());
    key.push_back('=');
    key.append(attributeToken(attribute.getValue()));
    key.push_back(';');
  }
  return key;
}

class StructuredTopologySerializer {
public:
  explicit StructuredTopologySerializer(llvm::raw_ostream &stream)
      : stream(stream) {}

  void run(ModuleOp model) {
    SmallVector<Operation *> topLevel;
    for (Operation &operation : model.getBody()->getOperations())
      if (operation.getName().getStringRef().starts_with("ac."))
        topLevel.push_back(&operation);

    indexResults(model.getOperation(), "root");
    for (auto [ordinal, operation] : llvm::enumerate(topLevel))
      indexOperation(operation, ("root/r0/b0/o" + llvm::Twine(ordinal)).str());

    serializeHeader(model.getOperation(), "root");
    stream << "region root/r0\nblock root/r0/b0\n";
    for (auto [ordinal, operation] : llvm::enumerate(topLevel))
      serializeOperation(operation,
                         ("root/r0/b0/o" + llvm::Twine(ordinal)).str());
  }

private:
  bool skipsRegions(Operation *operation) const {
    return isa<ac::ProcessOp>(operation);
  }

  void indexResults(Operation *operation, StringRef path) {
    for (auto [ordinal, result] : llvm::enumerate(operation->getResults()))
      valueIds[result] = (path + "/v" + llvm::Twine(ordinal)).str();
  }

  void indexOperation(Operation *operation, StringRef path) {
    indexResults(operation, path);
    if (skipsRegions(operation))
      return;
    for (auto [regionIndex, region] :
         llvm::enumerate(operation->getRegions())) {
      for (auto [blockIndex, block] : llvm::enumerate(region)) {
        std::string blockPath = (path + "/r" + llvm::Twine(regionIndex) + "/b" +
                                 llvm::Twine(blockIndex))
                                    .str();
        for (auto [argumentIndex, argument] :
             llvm::enumerate(block.getArguments()))
          valueIds[argument] =
              (blockPath + "/a" + llvm::Twine(argumentIndex)).str();
        for (auto [operationIndex, child] : llvm::enumerate(block))
          indexOperation(
              &child, (blockPath + "/o" + llvm::Twine(operationIndex)).str());
      }
    }
  }

  void serializeHeader(Operation *operation, StringRef path) {
    stream << "op " << path << ' ' << operation->getName().getStringRef()
           << '{';
    SmallVector<NamedAttribute> attributes(operation->getAttrs().begin(),
                                           operation->getAttrs().end());
    llvm::sort(attributes, [](NamedAttribute left, NamedAttribute right) {
      return left.getName().getValue() < right.getName().getValue();
    });
    for (NamedAttribute attribute : attributes) {
      if (isTopologyDigestAttribute(attribute.getName().getValue()))
        continue;
      stream << attribute.getName().getValue() << '=' << attribute.getValue()
             << ';';
    }
    stream << "}props=" << operation->getPropertiesAsAttribute()
           << " operands=";
    for (Value operand : operation->getOperands()) {
      auto found = valueIds.find(operand);
      if (found == valueIds.end())
        stream << "external";
      else
        stream << found->second;
      stream << ':' << operand.getType() << ',';
    }
    stream << " results=";
    for (auto [ordinal, type] : llvm::enumerate(operation->getResultTypes()))
      stream << valueIds.lookup(operation->getResult(ordinal)) << ':' << type
             << ',';
    stream << '\n';
  }

  void serializeOperation(Operation *operation, StringRef path) {
    serializeHeader(operation, path);
    if (skipsRegions(operation))
      return;
    for (auto [regionIndex, region] :
         llvm::enumerate(operation->getRegions())) {
      std::string regionPath = (path + "/r" + llvm::Twine(regionIndex)).str();
      stream << "region " << regionPath << '\n';
      for (auto [blockIndex, block] : llvm::enumerate(region)) {
        std::string blockPath =
            (regionPath + "/b" + llvm::Twine(blockIndex)).str();
        stream << "block " << blockPath << " args=";
        for (auto [argumentIndex, argument] :
             llvm::enumerate(block.getArguments()))
          stream << valueIds.lookup(argument) << ':' << argument.getType()
                 << ',';
        stream << '\n';
        for (auto [operationIndex, child] : llvm::enumerate(block))
          serializeOperation(
              &child, (blockPath + "/o" + llvm::Twine(operationIndex)).str());
      }
    }
  }

  llvm::raw_ostream &stream;
  DenseMap<Value, std::string> valueIds;
};

bool isProcessSkeletonOperation(Operation *operation) {
  StringRef name = operation->getName().getStringRef();
  return name.starts_with("ac.") || name == "func.call";
}

class ProcessSkeletonSerializer {
public:
  explicit ProcessSkeletonSerializer(ac::ProcessOp process)
      : process(process), builder(process.getContext()) {}

  FailureOr<ArrayAttr> run() {
    if (failed(enqueueSeeds()))
      return failure();

    for (size_t cursor = 0; cursor < worklist.size(); ++cursor) {
      Operation *operation = worklist[cursor];
      for (Value operand : operation->getOperands())
        if (Operation *producer = operand.getDefiningOp();
            producer && failed(enqueue(producer, operation,
                                       /*dependency=*/true)))
          return failure();
      if (failed(enqueue(operation->getParentOp(), operation,
                         /*dependency=*/true)))
        return failure();
      // Region results are defined by terminator operands. Commit those
      // dependencies whenever a region-bearing producer/control ancestor is
      // part of the semantic closure.
      for (Region &region : operation->getRegions())
        for (Block &block : region)
          if (!block.empty() && failed(enqueue(&block.back(), operation,
                                               /*dependency=*/true)))
            return failure();
    }

    indexRegions();
    serializeRegions();
    return builder.getArrayAttr(entries);
  }

private:
  LogicalResult enqueueSeeds() {
    struct TraversalTask {
      Operation *operation;
      bool expanded;
    };

    SmallVector<Operation *> roots;
    for (Block &block : process.getBody())
      for (Operation &operation : block)
        roots.push_back(&operation);
    SmallVector<TraversalTask> pending;
    for (Operation *operation : llvm::reverse(roots))
      pending.push_back({operation, false});

    while (!pending.empty()) {
      TraversalTask task = pending.pop_back_val();
      if (task.expanded) {
        if (isProcessSkeletonOperation(task.operation) &&
            failed(enqueue(task.operation, task.operation,
                           /*dependency=*/false)))
          return failure();
        continue;
      }

      pending.push_back({task.operation, true});
      SmallVector<Operation *> children;
      for (Region &region : task.operation->getRegions())
        for (Block &block : region)
          for (Operation &child : block)
            children.push_back(&child);
      for (Operation *child : llvm::reverse(children))
        pending.push_back({child, false});
    }
    return success();
  }

  bool isInsideProcess(Operation *operation) {
    return operation && operation != process.getOperation() &&
           operation->getParentOfType<ac::ProcessOp>() == process;
  }

  LogicalResult enqueue(Operation *candidate, Operation *origin,
                        bool dependency) {
    if (!isInsideProcess(candidate))
      return success();

    if (dependency) {
      uint64_t limit = detail::processSkeletonEdgeLimit();
      if (dependencyEdges >= limit)
        return origin->emitOpError()
               << "process skeleton dependency edge count exceeds bound "
               << limit;
      ++dependencyEdges;
    }

    if (included.contains(candidate))
      return success();
    uint64_t limit = detail::processSkeletonNodeLimit();
    if (included.size() >= limit)
      return origin->emitOpError()
             << "process skeleton dependency node count exceeds bound "
             << limit;
    included.insert(candidate);
    worklist.push_back(candidate);
    return success();
  }

  void indexRegions() {
    struct RegionTask {
      Region *region;
      std::string path;
    };

    SmallVector<RegionTask> pending{{&process.getBody(), "process/r0"}};
    while (!pending.empty()) {
      RegionTask task = pending.pop_back_val();
      SmallVector<RegionTask> nestedRegions;
      for (auto [blockIndex, block] : llvm::enumerate(*task.region)) {
        std::string blockPath =
            (task.path + "/b" + llvm::Twine(blockIndex)).str();
        for (auto [argumentIndex, argument] :
             llvm::enumerate(block.getArguments()))
          valueIds[argument] =
              (blockPath + "/a" + llvm::Twine(argumentIndex)).str();
        unsigned operationIndex = 0;
        for (Operation &operation : block) {
          if (!included.contains(&operation))
            continue;
          std::string path =
              (blockPath + "/o" + llvm::Twine(operationIndex++)).str();
          operationPaths[&operation] = path;
          for (auto [resultIndex, result] :
               llvm::enumerate(operation.getResults()))
            valueIds[result] = (path + "/v" + llvm::Twine(resultIndex)).str();
          for (auto [regionIndex, nested] :
               llvm::enumerate(operation.getRegions()))
            nestedRegions.push_back(
                {&nested, (path + "/r" + llvm::Twine(regionIndex)).str()});
        }
      }
      for (RegionTask &nested : llvm::reverse(nestedRegions))
        pending.push_back(std::move(nested));
    }
  }

  void serializeRegions() {
    struct SerializationTask {
      Region *region = nullptr;
      Operation *operation = nullptr;
    };

    SmallVector<SerializationTask> pending{{&process.getBody(), nullptr}};
    while (!pending.empty()) {
      SerializationTask task = pending.pop_back_val();
      if (task.operation) {
        serializeOperation(task.operation,
                           operationPaths.lookup(task.operation));
        for (Region &nested : llvm::reverse(task.operation->getRegions()))
          pending.push_back({&nested, nullptr});
        continue;
      }

      SmallVector<Operation *> operations;
      for (Block &block : *task.region)
        for (Operation &operation : block)
          if (included.contains(&operation))
            operations.push_back(&operation);
      for (Operation *operation : llvm::reverse(operations))
        pending.push_back({nullptr, operation});
    }
  }

  void serializeOperation(Operation *operation, StringRef path) {
    std::string storage;
    llvm::raw_string_ostream stream(storage);
    stream << path << ' ' << operation->getName().getStringRef() << '{';
    SmallVector<NamedAttribute> attributes(operation->getAttrs().begin(),
                                           operation->getAttrs().end());
    llvm::sort(attributes, [](NamedAttribute left, NamedAttribute right) {
      return left.getName().getValue() < right.getName().getValue();
    });
    for (NamedAttribute attribute : attributes)
      stream << attribute.getName().getValue() << '=' << attribute.getValue()
             << ';';
    stream << "}props=" << operation->getPropertiesAsAttribute()
           << " operands=";
    for (Value operand : operation->getOperands()) {
      auto found = valueIds.find(operand);
      stream << (found == valueIds.end() ? "external" : found->second) << ':'
             << operand.getType() << ',';
    }
    stream << " results=";
    for (Value result : operation->getResults())
      stream << valueIds.lookup(result) << ':' << result.getType() << ',';
    stream << " regions=";
    for (Region &region : operation->getRegions()) {
      stream << '[';
      for (Block &block : region) {
        stream << '(';
        for (BlockArgument argument : block.getArguments())
          stream << argument.getType() << ',';
        stream << ')';
      }
      stream << ']';
    }
    entries.push_back(builder.getStringAttr(storage));
  }

  ac::ProcessOp process;
  Builder builder;
  DenseSet<Operation *> included;
  SmallVector<Operation *> worklist;
  uint64_t dependencyEdges = 0;
  DenseMap<Operation *, std::string> operationPaths;
  DenseMap<Value, std::string> valueIds;
  SmallVector<Attribute> entries;
};

void serializeTopology(ModuleOp model, llvm::raw_ostream &stream) {
  StructuredTopologySerializer(stream).run(model);
}

Operation *lookupDefinition(ModuleOp model, FlatSymbolRefAttr reference) {
  return reference ? SymbolTable::lookupSymbolIn(model, reference) : nullptr;
}

bool isDirectStateOwner(Operation *operation) {
  return isa<ac::QueueOp, ac::EventQueueOp, ac::ResourceOp, ac::AddressSpaceOp,
             ac::ProcessOp, ac::StatOp>(operation);
}

std::optional<bool> constantBoolean(Value value) {
  Attribute constant;
  if (!matchPattern(value, m_Constant(&constant)))
    return std::nullopt;
  if (auto boolean = dyn_cast<BoolAttr>(constant))
    return boolean.getValue();
  if (auto integer = dyn_cast<IntegerAttr>(constant);
      integer && integer.getType().isInteger(1))
    return integer.getValue().getBoolValue();
  return std::nullopt;
}

FailureOr<std::vector<func::CallOp>>
verifyFunctionEffectsIterative(func::FuncOp function) {
  struct EffectTask {
    Operation *operation;
    bool expanded;
  };
  SmallVector<EffectTask> pending{{function, false}};
  DenseMap<Operation *, bool> subtreeEffectFree;
  std::vector<func::CallOp> calls;
  while (!pending.empty()) {
    EffectTask task = pending.pop_back_val();
    if (!task.expanded) {
      pending.push_back({task.operation, true});
      SmallVector<Operation *> children;
      for (Region &region : task.operation->getRegions())
        for (Block &block : region)
          for (Operation &child : block)
            children.push_back(&child);
      for (Operation *child : children)
        pending.push_back({child, false});
      continue;
    }

    bool localEffectFree = true;
    if (auto call = dyn_cast<func::CallOp>(task.operation)) {
      calls.push_back(call);
    } else if (task.operation != function.getOperation() &&
               !isa<func::ReturnOp>(task.operation)) {
      if (task.operation->hasTrait<OpTrait::HasRecursiveMemoryEffects>()) {
        // Region effects are aggregated explicitly from the postorder summary
        // below. Do not invoke a recursive interface implementation here.
        localEffectFree = true;
      } else if (auto effects =
                     dyn_cast<MemoryEffectOpInterface>(task.operation)) {
        SmallVector<MemoryEffects::EffectInstance> localEffects;
        effects.getEffects(localEffects);
        localEffectFree = localEffects.empty();
      } else {
        localEffectFree = false;
      }
      if (!localEffectFree) {
        task.operation->emitOpError(
            "function reachable from ac.process is not effect-free");
        return failure();
      }
    }

    bool childrenEffectFree = true;
    for (Region &region : task.operation->getRegions())
      for (Block &block : region)
        for (Operation &child : block)
          childrenEffectFree &= subtreeEffectFree.lookup(&child);
    subtreeEffectFree[task.operation] = localEffectFree && childrenEffectFree;
  }
  llvm::sort(calls, [](func::CallOp left, func::CallOp right) {
    return left.getCallee() < right.getCallee();
  });
  return calls;
}

} // namespace

LogicalResult detail::preflightModelStructure(ModuleOp model) {
  return ac::preflightRawModelStructure(model);
}

const detail::ValidatedPureFunction *
detail::ValidatedPureCallGraph::lookup(StringRef name, uint64_t *probes) const {
  size_t begin = 0;
  size_t end = functions.size();
  while (begin < end) {
    if (probes)
      ++*probes;
    size_t middle = begin + (end - begin) / 2;
    func::FuncOp function = functions[middle].function;
    StringRef candidate = function.getSymName();
    if (candidate < name)
      begin = middle + 1;
    else if (name < candidate)
      end = middle;
    else
      return &functions[middle];
  }
  return nullptr;
}

FailureOr<detail::ValidatedPureCallGraph>
detail::validatePureProcessCallGraph(ModuleOp model,
                                     const ac::RawModelStructureLimits &limits,
                                     const PureCallGraphLimits &callLimits) {
  std::map<std::string, func::FuncOp> symbols;
  for (func::FuncOp function : model.getOps<func::FuncOp>()) {
    auto [position, inserted] =
        symbols.emplace(function.getSymName().str(), function);
    if (!inserted) {
      function.emitOpError()
          << "duplicate pure func.call symbol '@" << position->first << "'";
      return failure();
    }
  }
  if (symbols.size() > callLimits.maxFunctions) {
    model.emitError() << "pure func.call analysis exceeds ACIR function "
                         "limit "
                      << callLimits.maxFunctions;
    return failure();
  }

  SmallVector<std::pair<std::string, func::CallOp>> roots;
  if (failed(ac::walkStructuredOperationsIterative(
          model,
          [&](Operation *operation) -> LogicalResult {
            auto call = dyn_cast<func::CallOp>(operation);
            if (!call || !operation->getParentOfType<ac::ProcessOp>())
              return success();
            ac::ProcessOp process = operation->getParentOfType<ac::ProcessOp>();
            ac::ModuleOp owner = process->getParentOfType<ac::ModuleOp>();
            roots.push_back({(owner.getSymName() + "::" + process.getSymName() +
                              "::" + call.getCallee())
                                 .str(),
                             call});
            return success();
          },
          limits)))
    return failure();
  llvm::sort(roots, [](const auto &left, const auto &right) {
    return left.first < right.first;
  });

  enum class State : uint8_t { Unvisited, Active, Pure };
  std::map<std::string, State> states;
  std::map<std::string, std::vector<func::CallOp>> indexedCalls;
  ValidatedPureCallGraph graph;
  uint64_t edges = 0;
  struct Frame {
    func::FuncOp function;
    Operation *origin;
    size_t nextCall = 0;
    bool entered = false;
  };

  for (auto &[key, root] : roots) {
    (void)key;
    auto rootFunction = symbols.find(root.getCallee().str());
    if (rootFunction == symbols.end()) {
      root.emitOpError() << "process func.call callee '@" << root.getCallee()
                         << "' is unresolved";
      return failure();
    }
    if (states[rootFunction->first] == State::Pure)
      continue;
    SmallVector<Frame> stack{{rootFunction->second, root, 0, false}};
    while (!stack.empty()) {
      Frame &frame = stack.back();
      std::string name = frame.function.getSymName().str();
      if (!frame.entered) {
        if (frame.function.isExternal()) {
          frame.origin->emitOpError()
              << "process func.call callee '@" << name
              << "' has no body and cannot be proven effect-free";
          return failure();
        }
        if (stack.size() > callLimits.maxDepth) {
          frame.origin->emitOpError()
              << "pure func.call analysis exceeds ACIR depth limit "
              << callLimits.maxDepth;
          return failure();
        }
        if (failed(ac::verifyProcessLowerability(frame.function, limits)))
          return failure();
        FailureOr<std::vector<func::CallOp>> calls =
            verifyFunctionEffectsIterative(frame.function);
        if (failed(calls))
          return failure();
        indexedCalls[name] = std::move(*calls);
        states[name] = State::Active;
        frame.entered = true;
      }

      auto &calls = indexedCalls[name];
      if (frame.nextCall == calls.size()) {
        states[name] = State::Pure;
        graph.functions.push_back({frame.function, calls});
        stack.pop_back();
        continue;
      }
      func::CallOp call = calls[frame.nextCall++];
      if (edges == callLimits.maxEdges) {
        call.emitOpError() << "pure func.call analysis exceeds ACIR edge limit "
                           << callLimits.maxEdges;
        return failure();
      }
      ++edges;
      auto target = symbols.find(call.getCallee().str());
      if (target == symbols.end()) {
        call.emitOpError() << "process func.call callee '@" << call.getCallee()
                           << "' is unresolved";
        return failure();
      }
      State targetState = states[target->first];
      if (targetState == State::Pure)
        continue;
      if (targetState == State::Active) {
        InFlightDiagnostic diagnostic =
            call.emitOpError("recursive func.call purity cycle: ");
        auto begin = llvm::find_if(stack, [&](const Frame &active) {
          return cast<StringAttr>(
                     active.function->getAttr(SymbolTable::getSymbolAttrName()))
                     .getValue() == target->first;
        });
        for (auto current = begin; current != stack.end(); ++current)
          diagnostic << '@' << current->function.getSymName() << " -> ";
        diagnostic << '@' << target->first;
        return failure();
      }
      stack.push_back({target->second, call, 0, false});
    }
  }
  llvm::sort(graph.functions, [](const ValidatedPureFunction &left,
                                 const ValidatedPureFunction &right) {
    auto name = [](func::FuncOp function) {
      return cast<StringAttr>(
                 function->getAttr(SymbolTable::getSymbolAttrName()))
          .getValue();
    };
    return name(left.function) < name(right.function);
  });
  return graph;
}

FailureOr<ArrayAttr> detail::buildFrozenProcessSkeleton(ac::ProcessOp process) {
  return ProcessSkeletonSerializer(process).run();
}

bool detail::hasTopologyFreezeEvidence(ModuleOp model) {
  bool evidence = false;
  model.walk([&](Operation *operation) {
    for (NamedAttribute attribute : operation->getAttrs()) {
      StringRef name = attribute.getName().getValue();
      if (name == "ac.freeze_epoch" || name == "ac.freeze_proven" ||
          name.starts_with("ac.frozen_") || name.starts_with("ac.topology_")) {
        evidence = true;
        return WalkResult::interrupt();
      }
    }
    return WalkResult::advance();
  });
  return evidence;
}

bool isTopologyFrozen(ModuleOp model) {
  auto marker = model->getAttrOfType<BoolAttr>("ac.topology_frozen");
  return marker && marker.getValue();
}

LogicalResult ModelAnalysis::verifyPureProcessCalls() {
  return succeeded(detail::validatePureProcessCallGraph(model)) ? success()
                                                                : failure();
}

LogicalResult ModelAnalysis::verifyZeroDelayDependencies() {
  std::map<std::string, ac::ModuleOp> modules;
  for (ac::ModuleOp module : model.getOps<ac::ModuleOp>())
    modules.emplace(module.getSymName().str(), module);

  DenseMap<Operation *, bool> moduleStateful;
  std::function<bool(ac::ModuleOp)> hasState = [&](ac::ModuleOp module) {
    auto found = moduleStateful.find(module);
    if (found != moduleStateful.end())
      return found->second;
    // The finite-instantiation verifier has already rejected recursion. Mark
    // first to keep this query total even for malformed IR under
    // --verify-each=false.
    moduleStateful[module] = true;
    bool stateful = false;
    if (!module.getBody().empty())
      for (Operation &child : module.getBody().front()) {
        if (isDirectStateOwner(&child)) {
          stateful = true;
          break;
        }
        auto targetIsStateful = [&](FlatSymbolRefAttr reference) {
          Operation *target = lookupDefinition(model, reference);
          if (auto nested = dyn_cast_or_null<ac::ModuleOp>(target))
            return hasState(nested);
          return target != nullptr;
        };
        if (auto instance = dyn_cast<ac::InstanceOp>(child))
          stateful = targetIsStateful(instance.getDefinitionAttr());
        else if (auto array = dyn_cast<ac::ArrayOp>(child))
          stateful = targetIsStateful(array.getDefinitionAttr());
        else if (auto instances = dyn_cast<ac::InstancesOp>(child))
          for (Attribute reference : instances.getDefinitions())
            stateful |= targetIsStateful(cast<FlatSymbolRefAttr>(reference));
        if (stateful)
          break;
      }
    moduleStateful[module] = stateful;
    return stateful;
  };
  for (const auto &[name, module] : modules) {
    (void)name;
    (void)hasState(module);
  }

  StringRef selectedRoot;
  StringRef selectedRootName;
  for (ac::SystemOp system : model.getOps<ac::SystemOp>())
    if (system.getSelected()) {
      selectedRoot = system.getRoot();
      selectedRootName = system.getRootName();
      break;
    }

  for (auto &[moduleName, module] : modules) {
    if (module.getBody().empty())
      continue;
    struct Node {
      Operation *operation;
      std::string label;
    };
    SmallVector<Node> nodes;
    for (Operation &operation : module.getBody().front()) {
      if (operation.getNumResults() == 0)
        continue;
      bool stateful =
          isDirectStateOwner(&operation) ||
          (!isMemoryEffectFree(&operation) && !isa<func::CallOp>(operation));
      auto targetStateful = [&](FlatSymbolRefAttr reference) {
        if (auto target = dyn_cast_or_null<ac::ModuleOp>(
                lookupDefinition(model, reference)))
          return moduleStateful.lookup(target);
        return lookupDefinition(model, reference) != nullptr;
      };
      if (auto instance = dyn_cast<ac::InstanceOp>(operation))
        stateful |= targetStateful(instance.getDefinitionAttr());
      else if (auto array = dyn_cast<ac::ArrayOp>(operation))
        stateful |= targetStateful(array.getDefinitionAttr());
      else if (auto instances = dyn_cast<ac::InstancesOp>(operation))
        for (Attribute reference : instances.getDefinitions())
          stateful |= targetStateful(cast<FlatSymbolRefAttr>(reference));
      if (stateful)
        continue;
      StringRef local = symbolName(&operation);
      std::string prefix = moduleName == selectedRoot ? selectedRootName.str()
                                                      : "@" + moduleName;
      std::string label = local.empty()
                              ? (prefix + "." + operationKey(&operation))
                              : (prefix + "." + local).str();
      nodes.push_back({&operation, std::move(label)});
    }
    if (nodes.size() > kMaxModelAnalysisNodes)
      return module.emitOpError()
             << "zero-delay analysis exceeds ACIR node limit "
             << kMaxModelAnalysisNodes;
    llvm::sort(nodes, [](const Node &left, const Node &right) {
      return left.label < right.label;
    });
    DenseMap<Operation *, unsigned> index;
    for (auto [ordinal, node] : llvm::enumerate(nodes))
      index[node.operation] = ordinal;
    SmallVector<SmallVector<unsigned>> edges(nodes.size());
    uint64_t edgeCount = 0;
    for (auto [ordinal, node] : llvm::enumerate(nodes)) {
      for (Value operand : node.operation->getOperands()) {
        Operation *producer = operand.getDefiningOp();
        auto found = producer ? index.find(producer) : index.end();
        if (found == index.end())
          continue;
        edges[found->second].push_back(ordinal);
        if (++edgeCount > kMaxModelAnalysisEdges)
          return module.emitOpError()
                 << "zero-delay analysis exceeds ACIR edge limit "
                 << kMaxModelAnalysisEdges;
      }
    }
    for (SmallVector<unsigned> &successors : edges) {
      llvm::sort(successors);
      successors.erase(std::unique(successors.begin(), successors.end()),
                       successors.end());
    }

    SmallVector<uint8_t> color(nodes.size());
    SmallVector<int64_t> activePosition(nodes.size(), -1);
    struct Frame {
      unsigned node;
      size_t next = 0;
    };
    SmallVector<Frame> stack;
    for (unsigned start = 0; start < nodes.size(); ++start) {
      if (color[start] != 0)
        continue;
      color[start] = 1;
      activePosition[start] = 0;
      stack.push_back({start, 0});
      while (!stack.empty()) {
        Frame &frame = stack.back();
        if (frame.next == edges[frame.node].size()) {
          color[frame.node] = 2;
          activePosition[frame.node] = -1;
          stack.pop_back();
          continue;
        }
        unsigned successor = edges[frame.node][frame.next++];
        if (color[successor] == 0) {
          color[successor] = 1;
          activePosition[successor] = stack.size();
          stack.push_back({successor, 0});
          continue;
        }
        if (color[successor] != 1)
          continue;
        InFlightDiagnostic diagnostic = nodes[successor].operation->emitOpError(
            "forbidden zero-delay cycle: ");
        for (size_t position = activePosition[successor];
             position < stack.size(); ++position)
          diagnostic << nodes[stack[position].node].label << " -> ";
        diagnostic << nodes[successor].label;
        return failure();
      }
    }
  }
  return success();
}

LogicalResult ModelAnalysis::verifyFreezeContracts() {
  SmallVector<Operation *> contracts;
  for (ac::ModuleOp module : model.getOps<ac::ModuleOp>()) {
    if (module.getBody().empty())
      continue;
    for (Operation &operation : module.getBody().front())
      if (isa<ac::RequireOp, ac::EnsureOp>(operation))
        contracts.push_back(&operation);
  }
  llvm::sort(contracts, [](Operation *left, Operation *right) {
    auto leftModule = left->getParentOfType<ac::ModuleOp>();
    auto rightModule = right->getParentOfType<ac::ModuleOp>();
    return std::tuple(leftModule.getSymName(), left->getName().getStringRef(),
                      left->getAttrOfType<StringAttr>("message").getValue()) <
           std::tuple(rightModule.getSymName(), right->getName().getStringRef(),
                      right->getAttrOfType<StringAttr>("message").getValue());
  });
  for (Operation *operation : contracts) {
    Value condition = isa<ac::RequireOp>(operation)
                          ? cast<ac::RequireOp>(operation).getCondition()
                          : cast<ac::EnsureOp>(operation).getCondition();
    StringRef message =
        operation->getAttrOfType<StringAttr>("message").getValue();
    std::optional<bool> value = constantBoolean(condition);
    if (!value)
      return operation->emitOpError()
             << "topology-freeze contract is not statically provable: "
             << message;
    if (!*value)
      return operation->emitOpError()
             << "topology-freeze contract failed: " << message;
  }
  return success();
}

FailureOr<ArrayAttr> detail::buildFrozenOwnerManifest(ModuleOp model) {
  SmallVector<ac::ElaboratedTopologyOwner> topologyOwners;
  SmallVector<ac::ElaboratedStateOwner> stateOwners;
  if (failed(ac::collectElaboratedTopologyOwners(model, topologyOwners)) ||
      failed(ac::collectElaboratedStateOwners(model, stateOwners)))
    return failure();

  struct Record {
    std::string path;
    std::string stableId;
    std::string kind;
    SymbolRefAttr owner;
    SmallVector<std::string> traceSources;
  };
  llvm::StringMap<SmallVector<std::string>> stateOwnerIndex;
  for (const ac::ElaboratedStateOwner &owner : stateOwners) {
    if (activeFreezeWork)
      ++activeFreezeWork->stateIndexInsertions;
    std::string key =
        ownerJoinKey(owner.declaration, owner.path, owner.stableId);
    bool inserted = stateOwnerIndex.try_emplace(key, owner.traceSources).second;
    if (!inserted)
      return owner.declaration->emitOpError()
             << "duplicate elaborated state-owner identity for path '"
             << owner.path << "' and stable ID '" << owner.stableId << "'";
  }

  SmallVector<Record> records;
  for (ac::SystemOp system : model.getOps<ac::SystemOp>())
    if (system.getSelected())
      records.push_back({system.getRootName().str(),
                         system.getRootName().str(),
                         "ac.system_root",
                         system.getRootAttr(),
                         {}});

  for (const ac::ElaboratedTopologyOwner &owner : topologyOwners) {
    Operation *declaration = owner.declaration;
    auto definition = declaration->getParentOfType<ac::ModuleOp>();
    StringRef local = symbolName(declaration);
    SymbolRefAttr reference;
    if (definition && !local.empty())
      reference = SymbolRefAttr::get(
          model.getContext(), definition.getSymName(),
          {FlatSymbolRefAttr::get(model.getContext(), local)});
    else if (definition)
      reference =
          FlatSymbolRefAttr::get(model.getContext(), definition.getSymName());
    if (activeFreezeWork)
      ++activeFreezeWork->topologyIndexLookups;
    SmallVector<std::string> traces;
    auto state = stateOwnerIndex.find(
        ownerJoinKey(declaration, owner.path, owner.stableId));
    if (state != stateOwnerIndex.end())
      traces = state->second;
    llvm::sort(traces);
    records.push_back({owner.path, owner.stableId,
                       declaration->getName().getStringRef().str(), reference,
                       std::move(traces)});
  }
  llvm::sort(records, [](const Record &left, const Record &right) {
    return std::tie(left.path, left.stableId, left.kind) <
           std::tie(right.path, right.stableId, right.kind);
  });

  Builder builder(model.getContext());
  SmallVector<Attribute> manifest;
  manifest.reserve(records.size());
  for (const Record &record : records) {
    SmallVector<NamedAttribute> fields = {
        builder.getNamedAttr("kind", builder.getStringAttr(record.kind)),
        builder.getNamedAttr("owner", record.owner),
        builder.getNamedAttr("path", builder.getStringAttr(record.path)),
        builder.getNamedAttr("stable_id",
                             builder.getStringAttr(record.stableId)),
    };
    if (!record.traceSources.empty()) {
      SmallVector<Attribute> sources;
      for (const std::string &source : record.traceSources)
        sources.push_back(builder.getStringAttr(source));
      fields.push_back(
          builder.getNamedAttr("trace_sources", builder.getArrayAttr(sources)));
    }
    manifest.push_back(builder.getDictionaryAttr(fields));
  }
  return builder.getArrayAttr(manifest);
}

std::string detail::computeTopologyDigest(ModuleOp model) {
  std::string serialized;
  llvm::raw_string_ostream stream(serialized);
  serializeTopology(model, stream);
  stream.flush();
  llvm::SHA256 sha;
  sha.update(serialized);
  return llvm::toHex(sha.final(), /*LowerCase=*/true);
}

namespace {

std::string sha256Fingerprint(StringRef value) {
  llvm::SHA256 sha;
  sha.update(value);
  return "sha256:" + llvm::toHex(sha.final(), /*LowerCase=*/true);
}

bool isSha256Fingerprint(StringRef value) {
  if (!value.consume_front("sha256:") || value.size() != 64)
    return false;
  return llvm::all_of(value, [](char character) {
    return std::isxdigit(static_cast<unsigned char>(character)) &&
           !std::isupper(static_cast<unsigned char>(character));
  });
}

std::string printWithoutQueueGraphFingerprints(ac::ModuleOp definition) {
  Operation *copy = definition->clone();
  copy->walk([](Operation *operation) {
    operation->removeAttr("ac.definition_fingerprint");
    operation->removeAttr("ac.specialization");
  });
  std::string serialized;
  llvm::raw_string_ostream stream(serialized);
  copy->print(stream);
  stream.flush();
  copy->destroy();
  return serialized;
}

} // namespace

std::string
detail::computeQueueGraphDefinitionFingerprint(ac::ModuleOp definition) {
  return sha256Fingerprint(printWithoutQueueGraphFingerprints(definition));
}

std::string detail::computeQueueGraphSpecializationFingerprint(
    ac::ModuleOp definition, DictionaryAttr staticArguments) {
  std::string serialized = computeQueueGraphDefinitionFingerprint(definition);
  serialized.append("\nstatic_args=");
  llvm::raw_string_ostream stream(serialized);
  stream << staticArguments;
  stream.flush();
  return sha256Fingerprint(serialized);
}

LogicalResult verifyFrozenFlatQueueGraph(ModuleOp model) {
  auto contractEpoch = model->getAttrOfType<StringAttr>("ac.contract_epoch");
  auto modelKind = model->getAttrOfType<StringAttr>("ac.model_kind");
  auto system = model->getAttrOfType<StringAttr>("ac.system");
  auto domain = model->getAttrOfType<StringAttr>("ac.queue_graph_domain");
  if (!contractEpoch || contractEpoch.getValue() != "0.5" || !modelKind ||
      modelKind.getValue() != "queue_graph" || !system ||
      system.getValue().empty() || !domain || domain.getValue() != "cycle")
    return model.emitError(
        "frozen flat QueueGraph requires epoch 0.5, model kind queue_graph, "
        "a non-empty system identity, and exact cycle domain");
  for (Operation &operation : model.getBody()->getOperations()) {
    if (isa<ac::SystemOp, ac::ModuleOp, ac::ModuleExternOp>(operation))
      return operation.emitOpError(
          "is not legal at the top level of a flat QueueGraph model");
  }
  auto frozen = model->getAttrOfType<BoolAttr>("ac.topology_frozen");
  auto epoch = model->getAttrOfType<StringAttr>("ac.freeze_epoch");
  auto owners = model->getAttrOfType<ArrayAttr>("ac.frozen_owners");
  auto digest = model->getAttrOfType<StringAttr>("ac.topology_digest");
  if (!frozen || !frozen.getValue() || !epoch || epoch.getValue() != "0.5" ||
      !owners || !owners.empty() || !digest || digest.getValue().size() != 64)
    return model.emitError("malformed flat QueueGraph freeze evidence");
  if (digest.getValue() != detail::computeTopologyDigest(model))
    return model.emitError("frozen flat QueueGraph digest mismatch");
  return success();
}

LogicalResult verifyFrozenStructuredQueueGraph(ModuleOp model) {
  auto contractEpoch = model->getAttrOfType<StringAttr>("ac.contract_epoch");
  auto modelKind = model->getAttrOfType<StringAttr>("ac.model_kind");
  auto domain = model->getAttrOfType<StringAttr>("ac.queue_graph_domain");
  if (!contractEpoch || contractEpoch.getValue() != "0.5" || !modelKind ||
      modelKind.getValue() != "queue_graph" || !domain ||
      domain.getValue() != "cycle")
    return model.emitError(
        "frozen structured QueueGraph requires epoch 0.5, model kind "
        "queue_graph, and exact cycle domain");
  if (model.getOps<ac::SystemOp>().empty() ||
      model.getOps<ac::ModuleOp>().empty())
    return model.emitError(
        "structured QueueGraph requires ac.system and ac.module definitions");
  if (!model.getOps<ac::ModuleExternOp>().empty())
    return model.emitError(
        "structured QueueGraph first slice requires materialized modules");
  if (failed(ac::verifyGraphStructure(model)))
    return failure();

  ac::SystemOp selected;
  unsigned selectedCount = 0;
  for (ac::SystemOp system : model.getOps<ac::SystemOp>())
    if (system.getSelected()) {
      selected = system;
      ++selectedCount;
    }
  if (selectedCount != 1)
    return model.emitError(
        "structured QueueGraph requires exactly one selected ac.system");
  auto frozenSystem =
      model->getAttrOfType<FlatSymbolRefAttr>("ac.frozen_system");
  if (!frozenSystem || frozenSystem.getValue() != selected.getSymName())
    return model.emitError(
        "structured QueueGraph frozen system identity is missing or stale");

  SymbolTable symbols(model);
  auto root = dyn_cast_or_null<ac::ModuleOp>(
      symbols.lookup(selected.getRootAttr().getValue()));
  if (!root)
    return selected.emitOpError(
        "structured QueueGraph root must resolve to a materialized module");
  if (!root.getFunctionType().getInputs().empty())
    return root.emitOpError(
        "structured QueueGraph root module cannot borrow input Queues");

  for (ac::ModuleOp definition : model.getOps<ac::ModuleOp>()) {
    for (Type type : definition.getFunctionType().getInputs())
      if (!isa<ac::QueueType>(type))
        return definition.emitOpError(
            "structured QueueGraph module inputs must be ac.queue values");
    for (Type type : definition.getFunctionType().getResults())
      if (!isa<ac::QueueType>(type))
        return definition.emitOpError(
            "structured QueueGraph module results must be ac.queue values");
    auto definitionFingerprint =
        definition->getAttrOfType<StringAttr>("ac.definition_fingerprint");
    auto rootSpecialization =
        definition->getAttrOfType<StringAttr>("ac.specialization");
    if (!definitionFingerprint ||
        !isSha256Fingerprint(definitionFingerprint.getValue()) ||
        definitionFingerprint.getValue() !=
            detail::computeQueueGraphDefinitionFingerprint(definition))
      return definition.emitOpError(
          "QueueGraph definition fingerprint is missing or stale");
    if (definition == root) {
      if (!rootSpecialization ||
          rootSpecialization.getValue() !=
              detail::computeQueueGraphSpecializationFingerprint(
                  definition,
                  Builder(model.getContext()).getDictionaryAttr({})))
        return definition.emitOpError(
            "QueueGraph root specialization fingerprint is missing or stale");
    } else if (rootSpecialization) {
      return definition.emitOpError(
          "non-root QueueGraph definition cannot carry one global "
          "specialization fingerprint");
    }
    for (Operation &child : definition.getBody().front()) {
      if (!isa<ac::ScopeOp, ac::InstanceOp, ac::ReturnOp>(child))
        return child.emitOpError(
            "structured QueueGraph module body permits scopes, instances, "
            "and ac.return only");
      auto instance = dyn_cast<ac::InstanceOp>(child);
      if (!instance)
        continue;
      auto target = dyn_cast_or_null<ac::ModuleOp>(
          symbols.lookup(instance.getDefinitionAttr().getValue()));
      auto specialization =
          instance->getAttrOfType<StringAttr>("ac.specialization");
      if (!target || !specialization ||
          !isSha256Fingerprint(specialization.getValue()) ||
          specialization.getValue() !=
              detail::computeQueueGraphSpecializationFingerprint(
                  target, instance.getStaticArgs()))
        return instance.emitOpError(
            "QueueGraph specialization fingerprint is missing or stale");
    }
  }

  auto frozen = model->getAttrOfType<BoolAttr>("ac.topology_frozen");
  auto epoch = model->getAttrOfType<StringAttr>("ac.freeze_epoch");
  auto owners = model->getAttrOfType<ArrayAttr>("ac.frozen_owners");
  auto digest = model->getAttrOfType<StringAttr>("ac.topology_digest");
  if (!frozen || !frozen.getValue() || !epoch || epoch.getValue() != "0.5" ||
      !owners || !digest || digest.getValue().size() != 64)
    return model.emitError("malformed structured QueueGraph freeze evidence");
  FailureOr<ArrayAttr> expectedOwners = detail::buildFrozenOwnerManifest(model);
  if (failed(expectedOwners) || owners != *expectedOwners)
    return model.emitError(
        "structured QueueGraph frozen owner manifest mismatch");
  if (digest.getValue() != detail::computeTopologyDigest(model))
    return model.emitError("frozen structured QueueGraph digest mismatch");
  return success();
}

LogicalResult ModelAnalysis::verifyFrozenIntegrity() {
  if (!detail::hasTopologyFreezeEvidence(model))
    return success();
  auto marker = model->getAttrOfType<BoolAttr>("ac.topology_frozen");
  auto epoch = model->getAttrOfType<StringAttr>("ac.freeze_epoch");
  auto digest = model->getAttrOfType<StringAttr>("ac.topology_digest");
  auto owners = model->getAttrOfType<ArrayAttr>("ac.frozen_owners");
  if (!marker || !marker.getValue() || !epoch || epoch.getValue() != "0.5" ||
      !digest || digest.getValue().size() != 64 || !owners)
    return model.emitError(
        "malformed topology freeze marker; expected epoch 0.5, owner manifest, "
        "and SHA-256 digest");
  LogicalResult skeletonResult = success();
  model.walk([&](ac::ProcessOp process) {
    if (failed(skeletonResult))
      return;
    ArrayAttr frozen =
        process->getAttrOfType<ArrayAttr>("ac.frozen_process_skeleton");
    FailureOr<ArrayAttr> expected = detail::buildFrozenProcessSkeleton(process);
    if (failed(expected)) {
      skeletonResult = failure();
      return;
    }
    if (!frozen || frozen != *expected) {
      process.emitOpError(
          "frozen process skeleton mismatch; effect semantics were mutated "
          "after ac-freeze-topology");
      skeletonResult = failure();
    }
  });
  if (failed(skeletonResult))
    return failure();
  std::string actual = detail::computeTopologyDigest(model);
  if (digest.getValue() != actual)
    return model.emitError(
        "frozen topology digest mismatch; topology was mutated after "
        "ac-freeze-topology");
  FailureOr<ArrayAttr> expectedOwners = detail::buildFrozenOwnerManifest(model);
  if (failed(expectedOwners))
    return failure();
  if (owners != *expectedOwners)
    return model.emitError(
        "frozen owner manifest mismatch; topology ownership was mutated after "
        "ac-freeze-topology");
  return success();
}

LogicalResult ModelAnalysis::verify() {
  if (failed(detail::preflightModelStructure(model)))
    return failure();

  auto epoch = model->getAttrOfType<StringAttr>("ac.contract_epoch");
  if (!epoch || epoch.getValue() != "0.5")
    return model.emitError(
        "expected top-level 'ac.contract_epoch' string attribute equal to "
        "\"0.5\"");

  if (failed(verifyPureProcessCalls()))
    return failure();
  if (failed(mlir::verify(model)))
    return failure();
  WalkResult types = model.walk([&](Operation *operation) {
    return failed(ac::verifyTopologyTypeUses(operation))
               ? WalkResult::interrupt()
               : WalkResult::advance();
  });
  if (types.wasInterrupted())
    return failure();
  if (failed(verifyZeroDelayDependencies()))
    return failure();
  return verifyFrozenIntegrity();
}

LogicalResult verifyModel(ModuleOp model) {
  return ModelAnalysis(model).verify();
}

} // namespace acir
