#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Operation.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

using namespace mlir;

namespace pyc {
namespace {

static FailureOr<int64_t> getConstantIndex(Value v) {
  if (auto c = v.getDefiningOp<arith::ConstantIndexOp>())
    return c.value();
  if (auto c = v.getDefiningOp<arith::ConstantOp>()) {
    if (c.getType().isIndex()) {
      if (auto i = dyn_cast<IntegerAttr>(c.getValue()))
        return i.getInt();
    }
  }
  return failure();
}

static std::optional<bool> getConstantI1(Value v) {
  if (auto c = v.getDefiningOp<pyc::ConstantOp>()) {
    auto intTy = dyn_cast<IntegerType>(c.getType());
    if (!intTy || intTy.getWidth() != 1)
      return std::nullopt;
    return c.getValueAttr().getValue().getZExtValue() != 0;
  }
  if (auto c = v.getDefiningOp<arith::ConstantOp>()) {
    auto intTy = dyn_cast<IntegerType>(c.getType());
    if (!intTy || intTy.getWidth() != 1)
      return std::nullopt;
    if (auto i = dyn_cast<IntegerAttr>(c.getValue()))
      return i.getValue().getZExtValue() != 0;
  }
  return std::nullopt;
}

static bool isPredicatable(Operation &op) {
  if (op.hasTrait<OpTrait::IsTerminator>())
    return true;
  // Allow scf.if to appear inside scf.for bodies; it will be lowered to muxes
  // later in this pass, and validated there.
  if (isa<scf::IfOp>(op))
    return true;
  return isMemoryEffectFree(&op);
}

static bool isAllowedInForBody(Operation &op) {
  if (op.hasTrait<OpTrait::IsTerminator>())
    return true;
  if (isa<scf::IfOp>(op))
    return true;
  if (isa<arith::ConstantOp, arith::ConstantIndexOp>(op))
    return true;
  return op.getName().getDialectNamespace() == "pyc";
}

static LogicalResult unrollFor(IRRewriter &rewriter, scf::ForOp op) {
  // We currently only support elaboration-style loops: the induction variable
  // must not be used in hardware computations.
  if (!op.getInductionVar().use_empty())
    return op.emitError("pyc-lower-scf-static requires scf.for induction variable to be unused");

  auto lb = getConstantIndex(op.getLowerBound());
  auto ub = getConstantIndex(op.getUpperBound());
  auto step = getConstantIndex(op.getStep());
  if (failed(lb) || failed(ub) || failed(step))
    return op.emitError("pyc-lower-scf-static requires constant scf.for bounds/step");
  if (*step <= 0)
    return op.emitError("pyc-lower-scf-static requires positive scf.for step");
  if (*ub < *lb)
    return op.emitError("pyc-lower-scf-static requires upperBound >= lowerBound");

  int64_t span = *ub - *lb;
  int64_t tripCount = llvm::divideCeil(span, *step);
  if (tripCount < 0)
    tripCount = 0;

  Block *body = op.getBody();
  auto yield = dyn_cast_or_null<scf::YieldOp>(body->getTerminator());
  if (!yield)
    return op.emitError("scf.for must terminate with scf.yield");
  if (yield.getNumOperands() != op.getNumRegionIterArgs())
    return op.emitError("scf.yield operand count must match scf.for iter_args");

  for (Type t : op.getResultTypes()) {
    if (!isa<IntegerType>(t))
      return op.emitError("pyc-lower-scf-static only supports scf.for iter_args of integer type");
  }

  for (Operation &inner : body->without_terminator()) {
    if (!isAllowedInForBody(inner))
      return inner.emitError("pyc-lower-scf-static only supports pyc + scf.if + arith.constant ops in scf.for");
  }

  SmallVector<Value> iterVals;
  iterVals.reserve(op.getNumRegionIterArgs());
  for (Value v : op.getInitArgs())
    iterVals.push_back(v);

  rewriter.setInsertionPoint(op);

  for (int64_t it = 0; it < tripCount; ++it) {
    IRMapping mapping;
    // Induction var is unused by construction.
    for (unsigned i = 0; i < iterVals.size(); ++i)
      mapping.map(body->getArgument(1 + i), iterVals[i]);

    for (Operation &inner : body->without_terminator())
      rewriter.clone(inner, mapping);

    SmallVector<Value> nextIterVals;
    nextIterVals.reserve(iterVals.size());
    for (Value v : yield.getOperands())
      nextIterVals.push_back(mapping.lookup(v));
    iterVals.swap(nextIterVals);
  }

  for (auto [res, v] : llvm::zip(op.getResults(), iterVals))
    res.replaceAllUsesWith(v);

  rewriter.eraseOp(op);
  return success();
}

static LogicalResult lowerIf(IRRewriter &rewriter, scf::IfOp op) {
  if (!op.getCondition().getType().isInteger(1))
    return op.emitError("pyc-lower-scf-static requires scf.if condition to be i1");

  if (auto c = getConstantI1(op.getCondition())) {
    Region &chosen = *c ? op.getThenRegion() : op.getElseRegion();
    if (chosen.empty())
      return op.emitError("scf.if region must not be empty");
    Block &b = chosen.front();
    auto yield = dyn_cast_or_null<scf::YieldOp>(b.getTerminator());
    if (!yield)
      return op.emitError("scf.if must terminate with scf.yield");
    if (yield.getNumOperands() != op.getNumResults())
      return op.emitError("scf.yield operand count must match scf.if results");

    for (Operation &inner : b.without_terminator()) {
      if (!isPredicatable(inner))
        return inner.emitError("pyc-lower-scf-static only supports side-effect-free ops in scf.if");
    }

    rewriter.setInsertionPoint(op);
    IRMapping mapping;
    for (Operation &inner : b.without_terminator())
      rewriter.clone(inner, mapping);

    for (auto [res, v] : llvm::zip(op.getResults(), yield.getOperands()))
      res.replaceAllUsesWith(mapping.lookupOrDefault(v));

    rewriter.eraseOp(op);
    return success();
  }

  // Lower dynamic scf.if by speculating both branches and muxing yields.
  if (op.getThenRegion().empty() || op.getElseRegion().empty())
    return op.emitError("pyc-lower-scf-static requires both then/else regions");
  Block &thenB = op.getThenRegion().front();
  Block &elseB = op.getElseRegion().front();
  auto thenYield = dyn_cast_or_null<scf::YieldOp>(thenB.getTerminator());
  auto elseYield = dyn_cast_or_null<scf::YieldOp>(elseB.getTerminator());
  if (!thenYield || !elseYield)
    return op.emitError("scf.if must terminate with scf.yield");
  if (thenYield.getNumOperands() != op.getNumResults() || elseYield.getNumOperands() != op.getNumResults())
    return op.emitError("scf.if regions must yield one value per result");

  for (Type t : op.getResultTypes()) {
    if (!isa<IntegerType>(t))
      return op.emitError("pyc-lower-scf-static only supports scf.if results of integer type");
  }

  for (Operation &inner : thenB.without_terminator()) {
    if (!isPredicatable(inner))
      return inner.emitError("pyc-lower-scf-static only supports side-effect-free ops in scf.if");
  }
  for (Operation &inner : elseB.without_terminator()) {
    if (!isPredicatable(inner))
      return inner.emitError("pyc-lower-scf-static only supports side-effect-free ops in scf.if");
  }

  rewriter.setInsertionPoint(op);
  IRMapping thenMap;
  for (Operation &inner : thenB.without_terminator())
    rewriter.clone(inner, thenMap);
  SmallVector<Value> thenVals;
  thenVals.reserve(op.getNumResults());
  for (Value v : thenYield.getOperands())
    thenVals.push_back(thenMap.lookupOrDefault(v));

  IRMapping elseMap;
  for (Operation &inner : elseB.without_terminator())
    rewriter.clone(inner, elseMap);
  SmallVector<Value> elseVals;
  elseVals.reserve(op.getNumResults());
  for (Value v : elseYield.getOperands())
    elseVals.push_back(elseMap.lookupOrDefault(v));

  SmallVector<Value> muxed;
  muxed.reserve(op.getNumResults());
  for (auto [ty, tv, ev] : llvm::zip(op.getResultTypes(), thenVals, elseVals)) {
    muxed.push_back(rewriter.create<pyc::SelectOp>(op.getLoc(), ty, op.getCondition(), tv, ev));
  }

  for (auto [res, v] : llvm::zip(op.getResults(), muxed))
    res.replaceAllUsesWith(v);

  rewriter.eraseOp(op);
  return success();
}

struct LowerSCFToPYCStaticPass
    : public PassWrapper<LowerSCFToPYCStaticPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerSCFToPYCStaticPass)

  StringRef getArgument() const override { return "pyc-lower-scf-static"; }
  StringRef getDescription() const override {
    return "Lower scf.if/scf.for into static PYC hardware ops (mux + unrolled loops)";
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    IRRewriter rewriter(f.getContext());

    // 1) Unroll all loops (post-order so nested loops are handled first).
    bool changed = true;
    while (changed) {
      changed = false;
      SmallVector<scf::ForOp> fors;
      f.walk<WalkOrder::PostOrder>([&](scf::ForOp op) { fors.push_back(op); });
      for (scf::ForOp op : fors) {
        if (failed(unrollFor(rewriter, op))) {
          signalPassFailure();
          return;
        }
        changed = true;
      }
    }

    // 2) Lower if-then-else to mux (post-order so nested ifs are handled first).
    changed = true;
    while (changed) {
      changed = false;
      SmallVector<scf::IfOp> ifs;
      f.walk<WalkOrder::PostOrder>([&](scf::IfOp op) { ifs.push_back(op); });
      for (scf::IfOp op : ifs) {
        if (failed(lowerIf(rewriter, op))) {
          signalPassFailure();
          return;
        }
        changed = true;
      }
    }

    // After lowering, no SCF ops should remain.
    bool hasSCF = false;
    f.walk([&](Operation *op) {
      if (isa<scf::ForOp, scf::IfOp>(op))
        hasSCF = true;
    });
    if (hasSCF) {
      f.emitError("pyc-lower-scf-static failed to remove all scf ops");
      signalPassFailure();
      return;
    }
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createLowerSCFToPYCStaticPass() {
  return std::make_unique<LowerSCFToPYCStaticPass>();
}

static PassRegistration<LowerSCFToPYCStaticPass> pass;

} // namespace pyc
