#include "acir/Dialect/ACIR/GraphRegion.h"

#include "acir/Dialect/ACIR/ACIRDialect.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"

#include <functional>

using namespace mlir;

namespace acir::ac {
namespace {

bool validSegment(StringRef segment) {
  return !segment.empty() && llvm::all_of(segment, [](char c) {
    return llvm::isAlnum(c) || c == '_' || c == '-';
  });
}

bool isModuleDeclaration(Operation *op) {
  return isa_and_nonnull<ModuleOp, ModuleExternOp>(op);
}

Operation *lookupDefinition(SymbolTable &symbols, FlatSymbolRefAttr reference) {
  return symbols.lookup(reference.getValue());
}

SmallVector<Operation *> instantiatedDefinitions(ModuleOp module,
                                                 SymbolTable &symbols) {
  SmallVector<Operation *> definitions;
  for (Operation &child : module.getBody().front()) {
    if (auto instance = dyn_cast<InstanceOp>(child)) {
      definitions.push_back(
          lookupDefinition(symbols, instance.getDefinitionAttr()));
    } else if (auto array = dyn_cast<ArrayOp>(child)) {
      definitions.push_back(
          lookupDefinition(symbols, array.getDefinitionAttr()));
    } else if (auto instances = dyn_cast<InstancesOp>(child)) {
      for (Attribute definition : instances.getDefinitions())
        definitions.push_back(
            lookupDefinition(symbols, cast<FlatSymbolRefAttr>(definition)));
    }
  }
  return definitions;
}

LogicalResult verifyFiniteInstantiationGraph(mlir::ModuleOp file,
                                             SymbolTable &symbols) {
  enum class State : uint8_t { Unvisited, Active, Complete };
  struct Frame {
    ModuleOp module;
    SmallVector<Operation *> definitions;
    size_t nextDefinition = 0;
  };
  llvm::DenseMap<Operation *, State> states;
  SmallVector<Frame> stack;
  for (ModuleOp root : file.getOps<ModuleOp>()) {
    if (states.lookup(root) != State::Unvisited)
      continue;
    states[root] = State::Active;
    stack.push_back({root, instantiatedDefinitions(root, symbols)});
    while (!stack.empty()) {
      Frame &frame = stack.back();
      if (frame.nextDefinition == frame.definitions.size()) {
        states[frame.module] = State::Complete;
        stack.pop_back();
        continue;
      }

      auto child =
          dyn_cast_or_null<ModuleOp>(frame.definitions[frame.nextDefinition++]);
      if (!child)
        continue;
      if (states.lookup(child) == State::Active) {
        auto start = llvm::find_if(stack, [child](const Frame &candidate) {
          return candidate.module == child;
        });
        InFlightDiagnostic diagnostic =
            frame.module.emitOpError("recursive module instantiation cycle: ");
        for (auto current = start; current != stack.end(); ++current)
          diagnostic << '@' << current->module.getSymName() << " -> ";
        diagnostic << '@' << child.getSymName();
        return failure();
      }
      if (states.lookup(child) == State::Unvisited) {
        states[child] = State::Active;
        stack.push_back({child, instantiatedDefinitions(child, symbols)});
      }
    }
  }
  return success();
}

} // namespace

void StructuralProviderRegistry::registerExternal(StringRef name) {
  externalProviders.insert(name);
}

void StructuralProviderRegistry::registerGenerator(StringRef name) {
  generatorProviders.insert(name);
}

bool StructuralProviderRegistry::hasExternal(StringRef name) const {
  return externalProviders.contains(name);
}

bool StructuralProviderRegistry::hasGenerator(StringRef name) const {
  return generatorProviders.contains(name);
}

StructuralProviderRegistry &
getStructuralProviderRegistry(MLIRContext *context) {
  auto *dialect = context->getOrLoadDialect<ACIRDialect>();
  auto *interface =
      dialect->getRegisteredInterface<StructuralProviderDialectInterface>();
  assert(interface && "ACIR structural provider interface must be registered");
  return interface->getRegistry();
}

bool isConcreteStaticValue(Attribute value) {
  if (!value)
    return false;
  if (isa<IntegerAttr, BoolAttr, StringAttr, TypeAttr, SymbolRefAttr>(value))
    return true;
  if (auto dictionary = dyn_cast<DictionaryAttr>(value)) {
    if (dictionary.size() != 2)
      return false;
    auto amount = dictionary.getAs<IntegerAttr>("value");
    auto unit = dictionary.getAs<StringAttr>("unit");
    return amount && amount.getType().isSignlessInteger(64) && unit &&
           symbolizeUnit(unit.getValue()).has_value();
  }
  return false;
}

std::string buildArrayElementPath(StringRef base, ArrayRef<int64_t> indices) {
  std::string path = base.str();
  for (int64_t index : indices) {
    path.push_back('[');
    path.append(std::to_string(index));
    path.push_back(']');
  }
  return path;
}

static LogicalResult verifyGraphStructureImpl(
    Operation *topLevel,
    SmallVectorImpl<ElaboratedStateOwner> *elaboratedStateOwners,
    SmallVectorImpl<ElaboratedTopologyOwner> *elaboratedTopologyOwners) {
  auto file = dyn_cast<mlir::ModuleOp>(topLevel);
  if (!file)
    return success();
  SymbolTable symbols(file);

  unsigned selected = 0;
  SystemOp selectedSystem;
  for (SystemOp system : file.getOps<SystemOp>()) {
    if (system.getSelected()) {
      ++selected;
      selectedSystem = system;
    }
    Operation *root = lookupDefinition(symbols, system.getRootAttr());
    if (!isa_and_nonnull<ModuleOp>(root)) {
      if (system.getSelected())
        return system.emitOpError(
            "selected root must resolve to a materialized ac.module");
      return system.emitOpError() << "root '" << system.getRootAttr()
                                  << "' does not resolve to ac.module";
    }
    if (SymbolRefAttr workload = system.getPrimaryWorkloadAttr()) {
      Operation *target = nullptr;
      if (workload.getRootReference() == system.getRootAttr().getValue() &&
          workload.getNestedReferences().size() == 1) {
        auto module = cast<ModuleOp>(root);
        StringRef processName =
            workload.getNestedReferences().front().getValue();
        for (ProcessOp process : module.getBody().front().getOps<ProcessOp>())
          if (process.getSymName() == processName) {
            target = process;
            break;
          }
      }
      if (!target)
        return system.emitOpError()
               << "primary workload '" << workload << "' is unresolved";
      if (!isa<ProcessOp>(target))
        return system.emitOpError() << "primary workload '" << workload
                                    << "' does not resolve to ac.process";
    }
  }
  if (!file.getOps<SystemOp>().empty() && selected != 1)
    return file.emitError() << "ACIR file requires exactly one selected "
                               "ac.system, found "
                            << selected;
  if (failed(verifyFiniteInstantiationGraph(file, symbols)))
    return failure();
  if (!selectedSystem)
    return success();

  constexpr uint64_t maxHierarchyDepth = 1024;
  constexpr uint64_t maxHierarchyOwners = 1048576;
  struct ExpansionStats {
    uint64_t owners = 0;
    uint64_t depth = 0;
  };
  auto saturatedAdd = [](uint64_t left, uint64_t right, uint64_t cap) {
    return left >= cap || right >= cap || left > cap - right ? cap
                                                             : left + right;
  };
  auto saturatedMultiply = [](uint64_t left, uint64_t right, uint64_t cap) {
    return left && right > cap / left ? cap : std::min(left * right, cap);
  };

  auto selectedRoot =
      cast<ModuleOp>(lookupDefinition(symbols, selectedSystem.getRootAttr()));
  llvm::DenseMap<Operation *, uint64_t> greatestIncomingDepth;
  SmallVector<std::pair<ModuleOp, uint64_t>> depthWorklist;
  greatestIncomingDepth[selectedRoot] = 0;
  depthWorklist.push_back({selectedRoot, 0});
  while (!depthWorklist.empty()) {
    auto [module, incomingDepth] = depthWorklist.pop_back_val();
    if (greatestIncomingDepth.lookup(module) != incomingDepth)
      continue;
    for (Operation &child : module.getBody().front()) {
      auto enqueue = [&](Operation *definition,
                         uint64_t depth) -> LogicalResult {
        if (depth > maxHierarchyDepth)
          return failure();
        auto target = dyn_cast_or_null<ModuleOp>(definition);
        if (target && greatestIncomingDepth.lookup(target) < depth) {
          greatestIncomingDepth[target] = depth;
          depthWorklist.push_back({target, depth});
        }
        return success();
      };
      if (auto instance = dyn_cast<InstanceOp>(child)) {
        if (failed(
                enqueue(lookupDefinition(symbols, instance.getDefinitionAttr()),
                        incomingDepth + 1)))
          return selectedSystem.emitOpError()
                 << "elaborated hierarchy depth exceeds bound "
                 << maxHierarchyDepth;
      } else if (auto array = dyn_cast<ArrayOp>(child)) {
        if (incomingDepth + 1 > maxHierarchyDepth)
          return selectedSystem.emitOpError()
                 << "elaborated hierarchy depth exceeds bound "
                 << maxHierarchyDepth;
        uint64_t count = 1;
        for (int64_t extent : array.getShape())
          count = saturatedMultiply(count, static_cast<uint64_t>(extent), 1);
        if (count != 0 &&
            failed(enqueue(lookupDefinition(symbols, array.getDefinitionAttr()),
                           incomingDepth + 2)))
          return selectedSystem.emitOpError()
                 << "elaborated hierarchy depth exceeds bound "
                 << maxHierarchyDepth;
      } else if (auto instances = dyn_cast<InstancesOp>(child)) {
        if (incomingDepth + 1 > maxHierarchyDepth)
          return selectedSystem.emitOpError()
                 << "elaborated hierarchy depth exceeds bound "
                 << maxHierarchyDepth;
        for (Attribute reference : instances.getDefinitions())
          if (failed(enqueue(
                  lookupDefinition(symbols, cast<FlatSymbolRefAttr>(reference)),
                  incomingDepth + 2)))
            return selectedSystem.emitOpError()
                   << "elaborated hierarchy depth exceeds bound "
                   << maxHierarchyDepth;
      }
    }
  }

  SmallVector<ModuleOp> postorder;
  SmallVector<std::pair<ModuleOp, bool>> traversal;
  llvm::DenseSet<Operation *> scheduled;
  traversal.push_back({selectedRoot, false});
  while (!traversal.empty()) {
    auto [module, expanded] = traversal.pop_back_val();
    if (expanded) {
      postorder.push_back(module);
      continue;
    }
    if (!scheduled.insert(module).second)
      continue;
    traversal.push_back({module, true});
    SmallVector<Operation *> definitions =
        instantiatedDefinitions(module, symbols);
    for (Operation *definition : llvm::reverse(definitions))
      if (auto child = dyn_cast_or_null<ModuleOp>(definition))
        traversal.push_back({child, false});
  }

  llvm::DenseMap<Operation *, ExpansionStats> expansionMemo;
  llvm::DenseMap<Operation *, SmallVector<std::pair<std::string, Operation *>>>
      traceSourceMemo;
  for (ModuleOp module : postorder) {
    ExpansionStats stats;
    SmallVector<std::pair<std::string, Operation *>> traceSources;
    llvm::StringMap<Operation *> traceSourceIndex;
    auto addTraceSource = [&](StringRef source,
                              Operation *owner) -> LogicalResult {
      if (!traceSourceIndex.try_emplace(source, owner).second)
        return owner->emitOpError()
               << "trace source '" << source
               << "' has multiple elaborated cursor owners";
      traceSources.push_back({source.str(), owner});
      return success();
    };
    auto mergeTraceSources = [&](Operation *definition, uint64_t multiplicity,
                                 Operation *owner) -> LogicalResult {
      auto found = traceSourceMemo.find(definition);
      if (found == traceSourceMemo.end() || found->second.empty() ||
          multiplicity == 0)
        return success();
      if (multiplicity > 1)
        return owner->emitOpError()
               << "trace source '" << found->second.front().first
               << "' has multiple elaborated cursor owners";
      for (const auto &[source, declaration] : found->second)
        if (failed(addTraceSource(source, declaration)))
          return failure();
      return success();
    };
    for (Operation &child : module.getBody().front()) {
      ExpansionStats childStats;
      uint64_t localOwners = 0;
      uint64_t localDepth = 0;
      if (auto instance = dyn_cast<InstanceOp>(child)) {
        Operation *definition =
            lookupDefinition(symbols, instance.getDefinitionAttr());
        childStats = expansionMemo.lookup(definition);
        if (failed(mergeTraceSources(definition, 1, &child)))
          return failure();
        localOwners =
            saturatedAdd(1, childStats.owners, maxHierarchyOwners + 1);
        localDepth = saturatedAdd(1, childStats.depth, maxHierarchyDepth + 1);
      } else if (auto array = dyn_cast<ArrayOp>(child)) {
        uint64_t count = 1;
        for (int64_t extent : array.getShape())
          count = saturatedMultiply(count, static_cast<uint64_t>(extent),
                                    maxHierarchyOwners + 1);
        Operation *definition =
            lookupDefinition(symbols, array.getDefinitionAttr());
        childStats = expansionMemo.lookup(definition);
        if (failed(mergeTraceSources(definition, count, &child)))
          return failure();
        uint64_t perElement =
            saturatedAdd(1, childStats.owners, maxHierarchyOwners + 1);
        localOwners = saturatedAdd(
            1, saturatedMultiply(count, perElement, maxHierarchyOwners + 1),
            maxHierarchyOwners + 1);
        localDepth = count == 0 ? 1
                                : saturatedAdd(2, childStats.depth,
                                               maxHierarchyDepth + 1);
      } else if (auto instances = dyn_cast<InstancesOp>(child)) {
        localOwners = 1;
        localDepth = 1;
        for (Attribute reference : instances.getDefinitions()) {
          Operation *definition =
              lookupDefinition(symbols, cast<FlatSymbolRefAttr>(reference));
          childStats = expansionMemo.lookup(definition);
          if (failed(mergeTraceSources(definition, 1, &child)))
            return failure();
          localOwners = saturatedAdd(
              localOwners,
              saturatedAdd(1, childStats.owners, maxHierarchyOwners + 1),
              maxHierarchyOwners + 1);
          localDepth =
              std::max(localDepth, saturatedAdd(2, childStats.depth,
                                                maxHierarchyDepth + 1));
        }
      } else if (isa<QueueOp, EventQueueOp, ResourceOp, AddressSpaceOp,
                     ProcessOp, StatOp>(child)) {
        localOwners = 1;
        localDepth = 1;
      }
      if (auto process = dyn_cast<ProcessOp>(child)) {
        WalkResult result = process.getBody().walk([&](TraceOpenOp trace) {
          if (failed(addTraceSource(trace.getSource(), trace)))
            return WalkResult::interrupt();
          return WalkResult::advance();
        });
        if (result.wasInterrupted())
          return failure();
      }
      stats.owners =
          saturatedAdd(stats.owners, localOwners, maxHierarchyOwners + 1);
      stats.depth = std::max(stats.depth, localDepth);
    }
    expansionMemo[module] = stats;
    traceSourceMemo[module] = std::move(traceSources);
  }
  ExpansionStats selectedStats = expansionMemo.lookup(selectedRoot);
  if (selectedStats.depth > maxHierarchyDepth)
    return selectedSystem.emitOpError()
           << "elaborated hierarchy depth exceeds bound " << maxHierarchyDepth;
  if (saturatedAdd(1, selectedStats.owners, maxHierarchyOwners + 1) >
      maxHierarchyOwners)
    return selectedSystem.emitOpError()
           << "elaborated hierarchy owner count exceeds bound "
           << maxHierarchyOwners;

  llvm::StringSet<> paths;
  llvm::StringSet<> stableIds;
  auto registerOwner = [&](Operation *op, StringRef path,
                           StringRef stableId) -> LogicalResult {
    if (!paths.insert(path).second)
      return op->emitOpError()
             << "duplicate elaborated hierarchy path '" << path << "'";
    if (!stableIds.insert(stableId).second)
      return op->emitOpError()
             << "duplicate elaborated stable ownership id '" << stableId << "'";
    if (elaboratedTopologyOwners)
      elaboratedTopologyOwners->push_back({op, path.str(), stableId.str()});
    return success();
  };

  std::function<LogicalResult(Operation *, StringRef, StringRef)> elaborate =
      [&](Operation *definition, StringRef parentPath,
          StringRef parentId) -> LogicalResult {
    auto module = dyn_cast<ModuleOp>(definition);
    if (!module)
      return success();
    for (Operation &child : module.getBody().front()) {
      if (auto instance = dyn_cast<InstanceOp>(child)) {
        std::string path = (parentPath + "." + instance.getPath()).str();
        std::string id = (parentId + "/" + instance.getStableId()).str();
        if (failed(registerOwner(&child, path, id)) ||
            failed(elaborate(
                lookupDefinition(symbols, instance.getDefinitionAttr()), path,
                id)))
          return failure();
      } else if (auto array = dyn_cast<ArrayOp>(child)) {
        std::string basePath = (parentPath + "." + array.getPath()).str();
        std::string baseId = (parentId + "/" + array.getStableId()).str();
        if (failed(registerOwner(&child, basePath, baseId)))
          return failure();
        uint64_t count = 1;
        for (int64_t extent : array.getShape())
          count *= static_cast<uint64_t>(extent);
        SmallVector<int64_t> indices(array.getShape().size());
        for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
          uint64_t remainder = ordinal;
          for (int64_t dimension = array.getShape().size() - 1; dimension >= 0;
               --dimension) {
            int64_t extent = array.getShape()[dimension];
            indices[dimension] = remainder % static_cast<uint64_t>(extent);
            remainder /= static_cast<uint64_t>(extent);
          }
          std::string path = buildArrayElementPath(basePath, indices);
          std::string id = buildArrayElementPath(baseId, indices);
          if (failed(registerOwner(&child, path, id)) ||
              failed(elaborate(
                  lookupDefinition(symbols, array.getDefinitionAttr()), path,
                  id)))
            return failure();
        }
      } else if (auto instances = dyn_cast<InstancesOp>(child)) {
        std::string collectionPath =
            (parentPath + "." + instances.getPath()).str();
        std::string collectionId =
            (parentId + "/" + instances.getStableId()).str();
        if (failed(registerOwner(&child, collectionPath, collectionId)))
          return failure();
        for (size_t index = 0; index < instances.getDefinitions().size();
             ++index) {
          StringRef segment =
              cast<StringAttr>(instances.getPaths()[index]).getValue();
          StringRef localId =
              cast<StringAttr>(instances.getStableIds()[index]).getValue();
          std::string path = (collectionPath + "." + segment).str();
          std::string id = (collectionId + "/" + localId).str();
          auto target =
              cast<FlatSymbolRefAttr>(instances.getDefinitions()[index]);
          if (failed(registerOwner(&child, path, id)) ||
              failed(elaborate(lookupDefinition(symbols, target), path, id)))
            return failure();
        }
      } else if (auto queue = dyn_cast<QueueOp>(child)) {
        std::string path = (parentPath + "." + queue.getPath()).str();
        std::string id = (parentId + "/" + queue.getStableId()).str();
        if (failed(registerOwner(&child, path, id)))
          return failure();
        if (elaboratedStateOwners)
          elaboratedStateOwners->push_back({&child, path, id});
      } else if (auto eventQueue = dyn_cast<EventQueueOp>(child)) {
        std::string path = (parentPath + "." + eventQueue.getPath()).str();
        std::string id = (parentId + "/" + eventQueue.getStableId()).str();
        if (failed(registerOwner(&child, path, id)))
          return failure();
        if (elaboratedStateOwners)
          elaboratedStateOwners->push_back({&child, path, id});
      } else if (auto resource = dyn_cast<ResourceOp>(child)) {
        std::string path = (parentPath + "." + resource.getPath()).str();
        std::string id = (parentId + "/" + resource.getStableId()).str();
        if (failed(registerOwner(&child, path, id)))
          return failure();
        if (elaboratedStateOwners)
          elaboratedStateOwners->push_back({&child, path, id});
      } else if (auto addressSpace = dyn_cast<AddressSpaceOp>(child)) {
        std::string path = (parentPath + "." + addressSpace.getPath()).str();
        std::string id = (parentId + "/" + addressSpace.getStableId()).str();
        if (failed(registerOwner(&child, path, id)))
          return failure();
        if (elaboratedStateOwners)
          elaboratedStateOwners->push_back({&child, path, id});
      } else if (auto process = dyn_cast<ProcessOp>(child)) {
        std::string path = (parentPath + "." + process.getSymName()).str();
        std::string id = (parentId + "/" + process.getSymName()).str();
        if (failed(registerOwner(&child, path, id)))
          return failure();
        if (elaboratedStateOwners) {
          SmallVector<std::string> sources;
          process.getBody().walk([&](TraceOpenOp trace) {
            sources.push_back(trace.getSource().str());
          });
          elaboratedStateOwners->push_back(
              {&child, path, id, std::move(sources)});
        }
      } else if (auto stat = dyn_cast<StatOp>(child)) {
        std::string path = (parentPath + "." + stat.getSymName()).str();
        std::string id = (parentId + "/" + stat.getSymName()).str();
        if (failed(registerOwner(&child, path, id)))
          return failure();
        if (elaboratedStateOwners)
          elaboratedStateOwners->push_back({&child, path, id});
      }
    }
    return success();
  };

  if (!validSegment(selectedSystem.getRootName()))
    return selectedSystem.emitOpError(
        "root instance name must be one stable hierarchy segment");
  std::string root = selectedSystem.getRootName().str();
  paths.insert(root);
  stableIds.insert(root);
  return elaborate(lookupDefinition(symbols, selectedSystem.getRootAttr()),
                   root, root);
}

LogicalResult verifyGraphStructure(Operation *topLevel) {
  return verifyGraphStructureImpl(topLevel, nullptr, nullptr);
}

LogicalResult
collectElaboratedStateOwners(Operation *topLevel,
                             SmallVectorImpl<ElaboratedStateOwner> &owners) {
  owners.clear();
  if (failed(verifyGraphStructureImpl(topLevel, &owners, nullptr))) {
    owners.clear();
    return failure();
  }
  return success();
}

LogicalResult collectElaboratedTopologyOwners(
    Operation *topLevel, SmallVectorImpl<ElaboratedTopologyOwner> &owners) {
  owners.clear();
  if (failed(verifyGraphStructureImpl(topLevel, nullptr, &owners))) {
    owners.clear();
    return failure();
  }
  return success();
}

} // namespace acir::ac
