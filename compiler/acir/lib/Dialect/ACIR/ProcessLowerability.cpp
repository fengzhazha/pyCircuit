#include "ProcessLowerability.h"

#include "acir/Dialect/ACIR/ACIROps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <limits>

using namespace mlir;

namespace acir::ac {
namespace {

constexpr uint64_t kMaxStaticForTrips = 1U << 20;

bool isSuspension(Operation *operation) {
  return isa<WaitUntilOp, WaitForOp, AwaitEventOp, YieldSimOp>(operation);
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

bool isAllowedProcessOperation(Operation *operation) {
  StringRef name = operation->getName().getStringRef();
  if (name.starts_with("arith.") || name.starts_with("index.") ||
      isa<func::CallOp, func::ReturnOp, scf::IfOp, scf::ForOp, scf::WhileOp,
          scf::ConditionOp, scf::YieldOp>(operation))
    return true;
  return isa<TrySendOp, TryRecvOp, ScheduleOp, WaitUntilOp, WaitForOp,
             AwaitEventOp, YieldSimOp, TraceOpenOp, TraceNextOp,
             TraceDecodeOp, TraceEofOp, TracePositionOp, RequireOp, EnsureOp,
             AssertOp, ProbeOp, StatAddOp, InstrumentationOp>(operation);
}

LogicalResult verifySCFShape(Operation *operation) {
  auto requireSingleBlockTerminator = [&](Region &region, StringRef owner,
                                          StringRef terminator) -> Operation * {
    if (region.empty() || !llvm::hasSingleElement(region) ||
        region.front().empty() ||
        region.front().back().getName().getStringRef() != terminator) {
      operation->emitOpError() << "malformed " << owner
                               << " region must terminate with " << terminator;
      return nullptr;
    }
    return &region.front().back();
  };
  auto sameTypes = [](auto left, auto right) {
    if (left.size() != right.size())
      return false;
    return llvm::all_of(llvm::zip(left, right), [](auto pair) {
      return std::get<0>(pair).getType() == std::get<1>(pair).getType();
    });
  };
  auto malformed = [&](StringRef owner) {
    operation->emitOpError()
        << "malformed " << owner
        << " operand/result/block argument/yield arity or type mismatch";
    return failure();
  };

  if (auto ifOp = dyn_cast<scf::IfOp>(operation)) {
    if (operation->getNumRegions() != 2) {
      operation->emitOpError(
          "malformed scf.if region must terminate with scf.yield");
      return failure();
    }
    Region &thenRegion = ifOp.getThenRegion();
    Region &elseRegion = ifOp.getElseRegion();
    Operation *thenYield =
        requireSingleBlockTerminator(thenRegion, "scf.if", "scf.yield");
    Operation *elseYield =
        elseRegion.empty()
            ? nullptr
            : requireSingleBlockTerminator(elseRegion, "scf.if", "scf.yield");
    if (!thenYield || (!elseRegion.empty() && !elseYield))
      return failure();
    if (!ifOp.getCondition().getType().isInteger(1) ||
        !thenRegion.front().getArguments().empty() ||
        (!elseRegion.empty() && !elseRegion.front().getArguments().empty()) ||
        !sameTypes(thenYield->getOperands(), operation->getResults()) ||
        (elseRegion.empty()
             ? operation->getNumResults() != 0
             : !sameTypes(elseYield->getOperands(), operation->getResults())))
      return malformed("scf.if");
  } else if (auto forOp = dyn_cast<scf::ForOp>(operation)) {
    if (operation->getNumRegions() != 1)
      return malformed("scf.for");
    Region &body = forOp.getRegion();
    Operation *yield =
        requireSingleBlockTerminator(body, "scf.for", "scf.yield");
    if (!yield || body.front().getNumArguments() != forOp.getNumResults() + 1 ||
        !sameTypes(forOp.getInitArgs(), forOp.getResults()) ||
        !sameTypes(forOp.getRegionIterArgs(), forOp.getResults()) ||
        !sameTypes(yield->getOperands(), forOp.getResults()))
      return yield ? malformed("scf.for") : failure();
  } else if (auto whileOp = dyn_cast<scf::WhileOp>(operation)) {
    if (operation->getNumRegions() != 2)
      return malformed("scf.while");
    Region &before = whileOp.getBefore();
    Region &after = whileOp.getAfter();
    Operation *condition = requireSingleBlockTerminator(
        before, "scf.while before", "scf.condition");
    Operation *yield =
        requireSingleBlockTerminator(after, "scf.while after", "scf.yield");
    if (!condition || !yield || condition->getNumOperands() < 1 ||
        !condition->getOperand(0).getType().isInteger(1) ||
        !sameTypes(whileOp.getInits(), whileOp.getBeforeArguments()) ||
        !sameTypes(condition->getOperands().drop_front(),
                   whileOp.getResults()) ||
        !sameTypes(whileOp.getAfterArguments(), whileOp.getResults()) ||
        !sameTypes(yield->getOperands(), whileOp.getBeforeArguments()))
      return (condition && yield) ? malformed("scf.while") : failure();
  }
  return success();
}

std::optional<int64_t> constantInt(Value value) {
  Attribute constant;
  if (!matchPattern(value, m_Constant(&constant)))
    return std::nullopt;
  auto integer = dyn_cast<IntegerAttr>(constant);
  if (!integer || !integer.getValue().isSignedIntN(64))
    return std::nullopt;
  return integer.getValue().getSExtValue();
}

bool isIntegerConstant(Value value) {
  Attribute constant;
  return matchPattern(value, m_Constant(&constant)) &&
         isa<IntegerAttr>(constant);
}

} // namespace

LogicalResult walkStructuredOperationsIterative(
    Operation *root, function_ref<LogicalResult(Operation *)> visitor,
    const RawModelStructureLimits &limits) {
  struct WorkItem {
    Operation *operation;
    uint64_t depth;
  };
  SmallVector<WorkItem> worklist{{root, 0}};
  uint64_t nodes = 0;
  uint64_t edges = 0;
  while (!worklist.empty()) {
    WorkItem item = worklist.pop_back_val();
    if (item.depth > limits.maxNestedRegionDepth)
      return emitError(root->getLoc())
             << "whole-model region nesting exceeds ACIR capability "
                "limit "
             << limits.maxNestedRegionDepth;
    if (nodes == limits.maxNodes ||
        item.operation->getNumOperands() > limits.maxEdges - edges)
      return emitError(root->getLoc())
             << "whole-model indexed analysis exceeds ACIR capability "
                "limits (nodes "
             << limits.maxNodes << ", edges " << limits.maxEdges << ')';
    ++nodes;
    edges += item.operation->getNumOperands();
    if (failed(visitor(item.operation)))
      return failure();

    SmallVector<Operation *> children;
    for (Region &region : item.operation->getRegions())
      for (Block &block : region)
        for (Operation &child : block)
          children.push_back(&child);
    for (Operation *child : llvm::reverse(children))
      worklist.push_back({child, item.depth + 1});
  }
  return success();
}

LogicalResult
preflightRawModelStructure(mlir::ModuleOp module,
                           const RawModelStructureLimits &limits) {
  return walkStructuredOperationsIterative(
      module.getOperation(), [](Operation *) { return success(); }, limits);
}

FailureOr<StaticForTripCount> analyzeStaticFor(scf::ForOp op) {
  std::optional<int64_t> lower = constantInt(op.getLowerBound());
  std::optional<int64_t> upper = constantInt(op.getUpperBound());
  std::optional<int64_t> step = constantInt(op.getStep());
  if (!lower || !upper || !step) {
    if (isIntegerConstant(op.getLowerBound()) &&
        isIntegerConstant(op.getUpperBound()) &&
        isIntegerConstant(op.getStep()))
      op.emitOpError()
          << "static scf.for trip count exceeds ACIR capability limit "
          << kMaxStaticForTrips;
    return failure();
  }
  if (*step <= 0) {
    op.emitOpError("static scf.for step must be positive");
    return failure();
  }
  unsigned __int128 distance =
      *upper <= *lower
          ? 0
          : static_cast<unsigned __int128>(static_cast<__int128>(*upper) -
                                           static_cast<__int128>(*lower));
  unsigned __int128 trips = (distance + static_cast<uint64_t>(*step) - 1) /
                            static_cast<uint64_t>(*step);
  if (trips > kMaxStaticForTrips ||
      trips > std::numeric_limits<uint64_t>::max()) {
    op.emitOpError()
        << "static scf.for trip count exceeds ACIR capability limit "
        << kMaxStaticForTrips;
    return failure();
  }
  return StaticForTripCount{*lower, *upper, *step,
                            static_cast<uint64_t>(trips)};
}

LogicalResult verifyProcessLowerability(Operation *processLikeOp,
                                        const RawModelStructureLimits &limits) {
  SmallVector<scf::ForOp> dynamicLoops;
  if (failed(walkStructuredOperationsIterative(
          processLikeOp,
          [&](Operation *operation) -> LogicalResult {
            if (operation == processLikeOp)
              return success();
            if (!isAllowedProcessOperation(operation))
              return operation->emitOpError()
                     << "ac.process contains unsupported operation "
                     << operation->getName();
            StringRef name = operation->getName().getStringRef();
            if ((name.starts_with("arith.") || name.starts_with("index.")) &&
                (!operation->getRegions().empty() ||
                 !isMemoryEffectFree(operation)))
              return operation->emitOpError(
                  "arith/index operation in ac.process must be regionless and "
                  "memory-effect free");
            if (failed(verifySCFShape(operation)))
              return failure();
            if (auto forOp = dyn_cast<scf::ForOp>(operation)) {
              bool allConstant = isIntegerConstant(forOp.getLowerBound()) &&
                                 isIntegerConstant(forOp.getUpperBound()) &&
                                 isIntegerConstant(forOp.getStep());
              if (allConstant) {
                if (failed(analyzeStaticFor(forOp)))
                  return failure();
              } else
                dynamicLoops.push_back(forOp);
            }
            return success();
          },
          limits)))
    return failure();

  // Compute suspension guarantees bottom-up without consuming the native
  // stack. The preceding bounded walk has already validated every node.
  DenseMap<Region *, bool> guarantees;
  struct SummaryTask {
    Operation *operation;
    bool expanded;
  };
  SmallVector<SummaryTask> pending{{processLikeOp, false}};
  while (!pending.empty()) {
    SummaryTask task = pending.pop_back_val();
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
    for (Region &region : task.operation->getRegions()) {
      bool regionGuarantee = false;
      if (!region.empty()) {
        for (Operation &operation : region.front()) {
          if (isSuspension(&operation)) {
            regionGuarantee = true;
            break;
          }
          auto ifOp = dyn_cast<scf::IfOp>(operation);
          if (!ifOp)
            continue;
          if (std::optional<bool> condition =
                  constantBoolean(ifOp.getCondition())) {
            Region &taken =
                *condition ? ifOp.getThenRegion() : ifOp.getElseRegion();
            regionGuarantee = guarantees.lookup(&taken);
          } else
            regionGuarantee = !ifOp.getElseRegion().empty() &&
                              guarantees.lookup(&ifOp.getThenRegion()) &&
                              guarantees.lookup(&ifOp.getElseRegion());
          if (regionGuarantee)
            break;
        }
      }
      guarantees[&region] = regionGuarantee;
    }
  }
  for (scf::ForOp loop : dynamicLoops)
    if (!guarantees.lookup(&loop.getRegion()))
      return loop.emitOpError(
          "dynamic scf.for requires every reachable backedge to suspend");
  return success();
}

} // namespace acir::ac
