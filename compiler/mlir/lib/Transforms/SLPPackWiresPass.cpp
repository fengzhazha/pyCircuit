#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

using namespace mlir;

namespace pyc {
namespace {

static VectorType vectorTypeLike(VectorType resultVT, Type laneTy) {
  if (isa<VectorType>(laneTy))
    return VectorType::get({resultVT.getDimSize(0)}, laneTy);
  return VectorType::get(resultVT.getShape(), laneTy);
}

static Value createVectorFromLanes(Location loc, VectorType resultVT, Type laneTy, ArrayRef<Value> lanes,
                                   PatternRewriter &rewriter) {
  return rewriter.create<pyc::VCreateOp>(loc, vectorTypeLike(resultVT, laneTy), lanes);
}

template <typename OpT>
static bool allDefinedBy(Operation::operand_range elems) {
  for (Value elem : elems)
    if (!elem.getDefiningOp<OpT>())
      return false;
  return true;
}

template <typename BinaryOpT>
static LogicalResult packBinary(pyc::VCreateOp op, PatternRewriter &rewriter) {
  if (!allDefinedBy<BinaryOpT>(op.getElements()))
    return failure();
  auto resultVT = dyn_cast<VectorType>(op.getResult().getType());
  if (!resultVT)
    return failure();

  SmallVector<Value> lhs;
  SmallVector<Value> rhs;
  for (Value elem : op.getElements()) {
    auto lane = elem.getDefiningOp<BinaryOpT>();
    if (!lane->getResult(0).hasOneUse())
      return failure();
    lhs.push_back(lane->getOperand(0));
    rhs.push_back(lane->getOperand(1));
  }
  Value lhsV = createVectorFromLanes(op.getLoc(), resultVT, lhs.front().getType(), lhs, rewriter);
  Value rhsV = createVectorFromLanes(op.getLoc(), resultVT, rhs.front().getType(), rhs, rewriter);
  rewriter.replaceOpWithNewOp<BinaryOpT>(op, resultVT, lhsV, rhsV);
  return success();
}

static LogicalResult packNot(pyc::VCreateOp op, PatternRewriter &rewriter) {
  if (!allDefinedBy<pyc::NotOp>(op.getElements()))
    return failure();
  auto resultVT = dyn_cast<VectorType>(op.getResult().getType());
  if (!resultVT)
    return failure();

  SmallVector<Value> inputs;
  for (Value elem : op.getElements()) {
    auto lane = elem.getDefiningOp<pyc::NotOp>();
    if (!lane.getResult().hasOneUse())
      return failure();
    inputs.push_back(lane.getIn());
  }
  Value inV = createVectorFromLanes(op.getLoc(), resultVT, inputs.front().getType(), inputs, rewriter);
  rewriter.replaceOpWithNewOp<pyc::NotOp>(op, resultVT, inV);
  return success();
}

static LogicalResult packCmp(pyc::VCreateOp op, PatternRewriter &rewriter) {
  if (!allDefinedBy<pyc::CmpOp>(op.getElements()))
    return failure();
  auto resultVT = dyn_cast<VectorType>(op.getResult().getType());
  if (!resultVT)
    return failure();

  SmallVector<Value> lhs;
  SmallVector<Value> rhs;
  StringAttr predicate;
  for (Value elem : op.getElements()) {
    auto lane = elem.getDefiningOp<pyc::CmpOp>();
    if (!lane.getResult().hasOneUse())
      return failure();
    if (!predicate)
      predicate = lane.getPredicateAttr();
    else if (predicate != lane.getPredicateAttr())
      return failure();
    lhs.push_back(lane.getLhs());
    rhs.push_back(lane.getRhs());
  }
  Value lhsV = createVectorFromLanes(op.getLoc(), resultVT,
                                     lhs.front().getType(), lhs, rewriter);
  Value rhsV = createVectorFromLanes(op.getLoc(), resultVT,
                                     rhs.front().getType(), rhs, rewriter);
  rewriter.replaceOpWithNewOp<pyc::CmpOp>(op, resultVT, lhsV, rhsV,
                                           predicate);
  return success();
}

static LogicalResult packMux(pyc::VCreateOp op, PatternRewriter &rewriter) {
  if (!allDefinedBy<pyc::SelectOp>(op.getElements()))
    return failure();
  auto resultVT = dyn_cast<VectorType>(op.getResult().getType());
  if (!resultVT)
    return failure();

  SmallVector<Value> sels;
  SmallVector<Value> as;
  SmallVector<Value> bs;
  for (Value elem : op.getElements()) {
    auto lane = elem.getDefiningOp<pyc::SelectOp>();
    if (!lane.getResult().hasOneUse())
      return failure();
    sels.push_back(lane.getSel());
    as.push_back(lane.getA());
    bs.push_back(lane.getB());
  }
  Value selV = createVectorFromLanes(op.getLoc(), resultVT, sels.front().getType(), sels, rewriter);
  Value aV = createVectorFromLanes(op.getLoc(), resultVT, as.front().getType(), as, rewriter);
  Value bV = createVectorFromLanes(op.getLoc(), resultVT, bs.front().getType(), bs, rewriter);
  rewriter.replaceOpWithNewOp<pyc::SelectOp>(op, resultVT, selV, aV, bV);
  return success();
}

struct PackVCreateElementwise : public OpRewritePattern<pyc::VCreateOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VCreateOp op, PatternRewriter &rewriter) const override {
    if (op.getElements().size() < 2)
      return failure();

    if (succeeded(packBinary<pyc::AndOp>(op, rewriter)) ||
        succeeded(packBinary<pyc::OrOp>(op, rewriter)) ||
        succeeded(packBinary<pyc::XorOp>(op, rewriter)) ||
        succeeded(packCmp(op, rewriter)) ||
        succeeded(packNot(op, rewriter)) ||
        succeeded(packMux(op, rewriter)))
      return success();
    return failure();
  }
};

struct SLPPackWiresPass : public PassWrapper<SLPPackWiresPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SLPPackWiresPass)

  StringRef getArgument() const override { return "pyc-slp-pack-wires"; }
  StringRef getDescription() const override {
    return "Pack isomorphic scalar comb lanes into internal vector wires";
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    RewritePatternSet patterns(f.getContext());
    patterns.add<PackVCreateElementwise>(f.getContext());
    GreedyRewriteConfig cfg;
    if (failed(applyPatternsAndFoldGreedily(f, std::move(patterns), cfg)))
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createSLPPackWiresPass() { return std::make_unique<SLPPackWiresPass>(); }

static PassRegistration<SLPPackWiresPass> pass;

} // namespace pyc
