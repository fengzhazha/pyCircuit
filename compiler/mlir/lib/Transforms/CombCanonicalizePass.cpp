#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include <type_traits>

using namespace mlir;

namespace pyc {
namespace {

static std::optional<uint64_t> getConstUInt(Value v) {
  if (auto c = v.getDefiningOp<pyc::ConstantOp>())
    return c.getValueAttr().getValue().getZExtValue();
  if (auto c = v.getDefiningOp<arith::ConstantOp>()) {
    if (auto i = dyn_cast<IntegerAttr>(c.getValue()))
      return i.getValue().getZExtValue();
  }
  // Vector constant: v_broadcast of a constant scalar.
  if (auto vb = v.getDefiningOp<pyc::VBroadcastOp>()) {
    if (auto c = vb.getScalar().getDefiningOp<pyc::ConstantOp>())
      return c.getValueAttr().getValue().getZExtValue();
    if (auto c = vb.getScalar().getDefiningOp<arith::ConstantOp>()) {
      if (auto i = dyn_cast<IntegerAttr>(c.getValue()))
        return i.getValue().getZExtValue();
    }
  }
  return std::nullopt;
}

static Value stripAlias(Value v) {
  while (auto a = v.getDefiningOp<pyc::AliasOp>())
    v = a.getIn();
  return v;
}

static std::pair<Value, bool> stripNot(Value v) {
  v = stripAlias(v);
  if (auto n = v.getDefiningOp<pyc::NotOp>())
    return {stripAlias(n.getIn()), true};
  return {v, false};
}

static bool andMatches(pyc::AndOp op, Value a, bool aNot, Value b, bool bNot) {
  auto [lBase, lNot] = stripNot(op.getLhs());
  auto [rBase, rNot] = stripNot(op.getRhs());
  return (lBase == a && lNot == aNot && rBase == b && rNot == bNot) ||
         (lBase == b && lNot == bNot && rBase == a && rNot == aNot);
}

// Create a constant value, broadcasting to a vector when needed.
static Value constInt(PatternRewriter &rewriter, Location loc, Type ty, const llvm::APInt &v) {
  // Scalar path.
  if (auto intTy = dyn_cast<IntegerType>(ty)) {
    llvm::APInt vv = v;
    if (vv.getBitWidth() != intTy.getWidth())
      vv = vv.zextOrTrunc(intTy.getWidth());
    return rewriter.create<pyc::ConstantOp>(loc, intTy, IntegerAttr::get(intTy, vv));
  }
  // Vector path: constant + broadcast.
  if (auto vt = dyn_cast<VectorType>(ty)) {
    auto elemTy = dyn_cast<IntegerType>(vt.getElementType());
    if (!elemTy)
      return {};
    llvm::APInt vv = v;
    if (vv.getBitWidth() != elemTy.getWidth())
      vv = vv.zextOrTrunc(elemTy.getWidth());
    Value scalar = rewriter.create<pyc::ConstantOp>(loc, elemTy, IntegerAttr::get(elemTy, vv));
    return rewriter.create<pyc::VBroadcastOp>(loc, vt, scalar, vt.getShape()[0]);
  }
  return {};
}

/// Return the element type for i1 checks: scalar i1 or vector<…xi1>.
static bool isI1OrI1Vector(Type ty) {
  if (auto intTy = dyn_cast<IntegerType>(ty))
    return intTy.getWidth() == 1;
  if (auto vt = dyn_cast<VectorType>(ty))
    if (auto et = dyn_cast<IntegerType>(vt.getElementType()))
      return et.getWidth() == 1;
  return false;
}

static Type vectorLaneType(VectorType vt) {
  Type elementTy = vt.getElementType();
  if (isa<VectorType>(elementTy))
    return elementTy;
  if (vt.getRank() == 1)
    return elementTy;
  return VectorType::get(vt.getShape().drop_front(), elementTy);
}

static std::optional<SmallVector<Value>> knownVectorLanes(Value value) {
  if (auto create = value.getDefiningOp<pyc::VCreateOp>())
    return SmallVector<Value>(create.getElements().begin(), create.getElements().end());
  if (auto broadcast = value.getDefiningOp<pyc::VBroadcastOp>()) {
    auto vt = dyn_cast<VectorType>(value.getType());
    if (!vt || vt.getRank() != 1)
      return std::nullopt;
    return SmallVector<Value>(static_cast<size_t>(vt.getDimSize(0)),
                              broadcast.getScalar());
  }
  return std::nullopt;
}

static bool hasFoldableVectorLane(Value lhs, Value rhs) {
  const auto lhsVT = dyn_cast<VectorType>(lhs.getType());
  const auto rhsVT = dyn_cast<VectorType>(rhs.getType());
  if (!lhsVT && !rhsVT)
    return getConstUInt(lhs).has_value() && getConstUInt(rhs).has_value();

  auto lhsLanes = lhsVT ? knownVectorLanes(lhs) : std::nullopt;
  auto rhsLanes = rhsVT ? knownVectorLanes(rhs) : std::nullopt;
  if ((lhsVT && !lhsLanes) || (rhsVT && !rhsLanes))
    return false;

  const size_t count = lhsLanes ? lhsLanes->size() : rhsLanes->size();
  for (size_t i = 0; i < count; ++i) {
    Value lhsLane = lhsLanes ? (*lhsLanes)[i] : lhs;
    Value rhsLane = rhsLanes ? (*rhsLanes)[i] : rhs;
    if (hasFoldableVectorLane(lhsLane, rhsLane))
      return true;
  }
  return false;
}

template <typename OpT>
struct FoldVectorConstantLanes : public OpRewritePattern<OpT> {
  using OpRewritePattern<OpT>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpT op, PatternRewriter &rewriter) const override {
    auto resultVT = dyn_cast<VectorType>(op.getResult().getType());
    if (!resultVT || !hasFoldableVectorLane(op.getLhs(), op.getRhs()))
      return failure();

    auto lhsLanes = dyn_cast<VectorType>(op.getLhs().getType())
                        ? knownVectorLanes(op.getLhs())
                        : std::nullopt;
    auto rhsLanes = dyn_cast<VectorType>(op.getRhs().getType())
                        ? knownVectorLanes(op.getRhs())
                        : std::nullopt;
    if ((isa<VectorType>(op.getLhs().getType()) && !lhsLanes) ||
        (isa<VectorType>(op.getRhs().getType()) && !rhsLanes))
      return failure();

    const size_t count = static_cast<size_t>(resultVT.getDimSize(0));
    if ((lhsLanes && lhsLanes->size() != count) ||
        (rhsLanes && rhsLanes->size() != count))
      return failure();

    Type laneTy = vectorLaneType(resultVT);
    SmallVector<Value> lanes;
    lanes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      Value lhs = lhsLanes ? (*lhsLanes)[i] : op.getLhs();
      Value rhs = rhsLanes ? (*rhsLanes)[i] : op.getRhs();
      if constexpr (std::is_same_v<OpT, pyc::CmpOp>)
        lanes.push_back(rewriter.create<OpT>(op.getLoc(), laneTy, lhs, rhs,
                                             op.getPredicateAttr()));
      else
        lanes.push_back(rewriter.create<OpT>(op.getLoc(), laneTy, lhs, rhs));
    }
    rewriter.replaceOpWithNewOp<pyc::VCreateOp>(op, op.getResult().getType(), lanes);
    return success();
  }
};

template <typename ReduceOp>
struct FoldRank2ReduceLanes : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  Value combine(ReduceOp op, Type laneTy, Value lhs, Value rhs,
                PatternRewriter &rewriter) const {
    if constexpr (std::is_same_v<ReduceOp, pyc::VOrReduceOp>)
      return rewriter.create<pyc::OrOp>(op.getLoc(), laneTy, lhs, rhs);
    if constexpr (std::is_same_v<ReduceOp, pyc::VAndReduceOp>)
      return rewriter.create<pyc::AndOp>(op.getLoc(), laneTy, lhs, rhs);
    return rewriter.create<pyc::AddOp>(op.getLoc(), laneTy, lhs, rhs);
  }

  LogicalResult matchAndRewrite(ReduceOp op, PatternRewriter &rewriter) const override {
    if (!op.getDim())
      return failure();
    auto inputTy = dyn_cast<VectorType>(op.getVec().getType());
    auto resultTy = dyn_cast<VectorType>(op.getResult().getType());
    auto rows = knownVectorLanes(op.getVec());
    if (!inputTy || inputTy.getRank() != 2 || !resultTy || resultTy.getRank() != 1 ||
        !rows)
      return failure();

    const int64_t dim = *op.getDim();
    const size_t rowCount = static_cast<size_t>(inputTy.getShape()[0]);
    const size_t colCount = static_cast<size_t>(inputTy.getShape()[1]);
    if (rows->size() != rowCount || (dim != 0 && dim != 1))
      return failure();

    SmallVector<SmallVector<Value>> matrix;
    matrix.reserve(rowCount);
    for (Value row : *rows) {
      auto columns = knownVectorLanes(row);
      if (!columns || columns->size() != colCount)
        return failure();
      matrix.push_back(std::move(*columns));
    }

    const size_t outputCount = dim == 0 ? colCount : rowCount;
    if (resultTy.getDimSize(0) != static_cast<int64_t>(outputCount))
      return failure();
    bool hasConstantGroup = false;
    SmallVector<SmallVector<Value>> groups(outputCount);
    for (size_t output = 0; output < outputCount; ++output) {
      for (size_t input = 0; input < (dim == 0 ? rowCount : colCount); ++input)
        groups[output].push_back(dim == 0 ? matrix[input][output] : matrix[output][input]);
      hasConstantGroup |= llvm::all_of(groups[output],
                                       [](Value value) { return getConstUInt(value).has_value(); });
    }
    if (!hasConstantGroup)
      return failure();

    Type laneTy = vectorLaneType(resultTy);
    SmallVector<Value> lanes;
    lanes.reserve(outputCount);
    for (ArrayRef<Value> group : groups) {
      Value accumulator = group.front();
      for (Value value : group.drop_front())
        accumulator = combine(op, laneTy, accumulator, value, rewriter);
      lanes.push_back(accumulator);
    }
    rewriter.replaceOpWithNewOp<pyc::VCreateOp>(op, op.getResult().getType(), lanes);
    return success();
  }
};

struct MuxSameSelSimplify : public OpRewritePattern<pyc::SelectOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::SelectOp op, PatternRewriter &rewriter) const override {
    Value sel = op.getSel();

    // sel ? (sel ? x : y) : z  ==>  sel ? x : z
    if (auto aMux = op.getA().getDefiningOp<pyc::SelectOp>()) {
      if (aMux.getSel() == sel) {
        rewriter.replaceOpWithNewOp<pyc::SelectOp>(op, op.getType(), sel, aMux.getA(), op.getB());
        return success();
      }
    }

    // sel ? x : (sel ? y : z)  ==>  sel ? x : z
    if (auto bMux = op.getB().getDefiningOp<pyc::SelectOp>()) {
      if (bMux.getSel() == sel) {
        rewriter.replaceOpWithNewOp<pyc::SelectOp>(op, op.getType(), sel, op.getA(), bMux.getB());
        return success();
      }
    }

    // (~sel) ? a : b  ==>  sel ? b : a
    if (auto n = sel.getDefiningOp<pyc::NotOp>()) {
      rewriter.replaceOpWithNewOp<pyc::SelectOp>(op, op.getType(), n.getIn(), op.getB(), op.getA());
      return success();
    }

    return failure();
  }
};

struct MuxI1ToLogic : public OpRewritePattern<pyc::SelectOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::SelectOp op, PatternRewriter &rewriter) const override {
    if (!isI1OrI1Vector(op.getType()))
      return failure();

    auto aConst = getConstUInt(op.getA());
    auto bConst = getConstUInt(op.getB());

    if (aConst && bConst) {
      uint64_t a = (*aConst) & 1;
      uint64_t b = (*bConst) & 1;
      if (a == b) {
        rewriter.replaceOp(op, a ? op.getA() : op.getB());
        return success();
      }
      if (a == 1 && b == 0) {
        rewriter.replaceOp(op, op.getSel());
        return success();
      }
      if (a == 0 && b == 1) {
        rewriter.replaceOpWithNewOp<pyc::NotOp>(op, op.getSel());
        return success();
      }
      return failure();
    }

    // For i1, convert muxes with constant arms into simple gates.
    if (bConst && (((*bConst) & 1) == 0)) {
      // sel ? a : 0  ==>  sel & a
      rewriter.replaceOpWithNewOp<pyc::AndOp>(op, op.getResult().getType(), op.getSel(), op.getA());
      return success();
    }
    if (aConst && (((*aConst) & 1) == 1)) {
      // sel ? 1 : b  ==>  sel | b
      rewriter.replaceOpWithNewOp<pyc::OrOp>(op, op.getResult().getType(), op.getSel(), op.getB());
      return success();
    }
    if (bConst && (((*bConst) & 1) == 1)) {
      // sel ? a : 1  ==>  (~sel) | a
      Value nsel = rewriter.create<pyc::NotOp>(op.getLoc(), op.getSel());
      rewriter.replaceOpWithNewOp<pyc::OrOp>(op, op.getResult().getType(), nsel, op.getA());
      return success();
    }
    if (aConst && (((*aConst) & 1) == 0)) {
      // sel ? 0 : b  ==>  (~sel) & b
      Value nsel = rewriter.create<pyc::NotOp>(op.getLoc(), op.getSel());
      rewriter.replaceOpWithNewOp<pyc::AndOp>(op, op.getResult().getType(), nsel, op.getB());
      return success();
    }

    return failure();
  }
};

struct AndBasicSimplify : public OpRewritePattern<pyc::AndOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::AndOp op, PatternRewriter &rewriter) const override {
    Value lhs = stripAlias(op.getLhs());
    Value rhs = stripAlias(op.getRhs());

    // a & a ==> a
    if (lhs == rhs) {
      rewriter.replaceOp(op, op.getLhs());
      return success();
    }

    // a & ~a ==> 0
    auto [lb, ln] = stripNot(lhs);
    auto [rb, rn] = stripNot(rhs);
    if ((lb == rb) && (ln != rn)) {
      Value z = constInt(rewriter, op.getLoc(), op.getType(), llvm::APInt(1, 0));
      if (!z)
        return failure();
      rewriter.replaceOp(op, z);
      return success();
    }

    return failure();
  }
};

struct OrBasicSimplify : public OpRewritePattern<pyc::OrOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::OrOp op, PatternRewriter &rewriter) const override {
    Value lhs = stripAlias(op.getLhs());
    Value rhs = stripAlias(op.getRhs());

    // a | a ==> a
    if (lhs == rhs) {
      rewriter.replaceOp(op, op.getLhs());
      return success();
    }

    // a | ~a ==> all-ones
    auto [lb, ln] = stripNot(lhs);
    auto [rb, rn] = stripNot(rhs);
    if ((lb == rb) && (ln != rn)) {
      Value ones = constInt(rewriter, op.getLoc(), op.getType(), llvm::APInt::getAllOnes(
          isa<IntegerType>(op.getType()) ? cast<IntegerType>(op.getType()).getWidth() : 1));
      if (!ones)
        return failure();
      rewriter.replaceOp(op, ones);
      return success();
    }

    return failure();
  }
};

struct OrAndXorFactor : public OpRewritePattern<pyc::OrOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::OrOp op, PatternRewriter &rewriter) const override {
    Value lhs = stripAlias(op.getLhs());
    Value rhs = stripAlias(op.getRhs());
    auto a0 = lhs.getDefiningOp<pyc::AndOp>();
    auto a1 = rhs.getDefiningOp<pyc::AndOp>();
    if (!a0 || !a1)
      return failure();

    // Try candidates based on the first AND's operands.
    auto [x0, _x0n] = stripNot(a0.getLhs());
    auto [x1, _x1n] = stripNot(a0.getRhs());
    llvm::SmallVector<std::pair<Value, Value>, 2> cands;
    cands.push_back({x0, x1});
    if (x0 != x1)
      cands.push_back({x1, x0});

    for (auto [A, B] : cands) {
      // XOR: (A & ~B) | (~A & B) ==> A ^ B
      if (andMatches(a0, A, /*aNot=*/false, B, /*bNot=*/true) && andMatches(a1, A, /*aNot=*/true, B, /*bNot=*/false)) {
        rewriter.replaceOpWithNewOp<pyc::XorOp>(op, op.getType(), A, B);
        return success();
      }
      if (andMatches(a0, A, /*aNot=*/true, B, /*bNot=*/false) && andMatches(a1, A, /*aNot=*/false, B, /*bNot=*/true)) {
        rewriter.replaceOpWithNewOp<pyc::XorOp>(op, op.getType(), A, B);
        return success();
      }

      // XNOR: (A & B) | (~A & ~B) ==> ~(A ^ B)
      if (andMatches(a0, A, /*aNot=*/false, B, /*bNot=*/false) && andMatches(a1, A, /*aNot=*/true, B, /*bNot=*/true)) {
        Value x = rewriter.create<pyc::XorOp>(op.getLoc(), op.getType(), A, B);
        rewriter.replaceOpWithNewOp<pyc::NotOp>(op, op.getType(), x);
        return success();
      }
      if (andMatches(a0, A, /*aNot=*/true, B, /*bNot=*/true) && andMatches(a1, A, /*aNot=*/false, B, /*bNot=*/false)) {
        Value x = rewriter.create<pyc::XorOp>(op.getLoc(), op.getType(), A, B);
        rewriter.replaceOpWithNewOp<pyc::NotOp>(op, op.getType(), x);
        return success();
      }
    }

    return failure();
  }
};

struct VGetOfVCreate : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp op, PatternRewriter &rewriter) const override {
    auto create = op.getVec().getDefiningOp<pyc::VCreateOp>();
    if (!create)
      return failure();
    std::uint64_t idx = op.getIndex();
    if (idx >= create.getElements().size())
      return failure();
    rewriter.replaceOp(op, *std::next(create.getElements().begin(), static_cast<long>(idx)));
    return success();
  }
};

struct VGetOfVBroadcast : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp op, PatternRewriter &rewriter) const override {
    auto broadcast = op.getVec().getDefiningOp<pyc::VBroadcastOp>();
    if (!broadcast)
      return failure();
    rewriter.replaceOp(op, broadcast.getScalar());
    return success();
  }
};

struct VGetOfVectorMux : public OpRewritePattern<pyc::VGetOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VGetOp op, PatternRewriter &rewriter) const override {
    auto mux = op.getVec().getDefiningOp<pyc::SelectOp>();
    if (!mux || !mux.getResult().hasOneUse())
      return failure();
    auto muxVT = dyn_cast<VectorType>(mux.getResult().getType());
    if (!muxVT)
      return failure();
    std::uint64_t idx = op.getIndex();
    if (idx >= static_cast<std::uint64_t>(muxVT.getDimSize(0)))
      return failure();

    Value sel = mux.getSel();
    if (auto selVT = dyn_cast<VectorType>(sel.getType())) {
      Type selLaneTy = isa<VectorType>(selVT.getElementType())
                           ? Type(selVT.getElementType())
                           : (selVT.getRank() == 1
                                  ? Type(selVT.getElementType())
                                  : Type(VectorType::get(
                                        selVT.getShape().drop_front(),
                                        selVT.getElementType())));
      sel = rewriter.create<pyc::VGetOp>(op.getLoc(), selLaneTy, sel, idx);
    }

    Type laneTy = op.getResult().getType();
    Value a = rewriter.create<pyc::VGetOp>(op.getLoc(), laneTy, mux.getA(), idx);
    Value b = rewriter.create<pyc::VGetOp>(op.getLoc(), laneTy, mux.getB(), idx);
    rewriter.replaceOpWithNewOp<pyc::SelectOp>(op, laneTy, sel, a, b);
    return success();
  }
};

struct VCreateOfConsecutiveVGets : public OpRewritePattern<pyc::VCreateOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(pyc::VCreateOp op, PatternRewriter &rewriter) const override {
    if (op.getElements().empty())
      return failure();

    Value src;
    std::uint64_t expected = 0;
    for (Value elem : op.getElements()) {
      auto get = elem.getDefiningOp<pyc::VGetOp>();
      if (!get || get.getIndex() != expected++)
        return failure();
      if (!src)
        src = get.getVec();
      else if (src != get.getVec())
        return failure();
    }

    if (!src || src.getType() != op.getResult().getType())
      return failure();
    rewriter.replaceOp(op, src);
    return success();
  }
};

template <typename ReduceOp>
struct VReduceOfVBroadcast : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReduceOp op, PatternRewriter &rewriter) const override {
    auto broadcast = op.getVec().template getDefiningOp<pyc::VBroadcastOp>();
    if (!broadcast)
      return failure();
    auto vecTy = dyn_cast<VectorType>(op.getVec().getType());
    if (!vecTy || vecTy.getRank() != 1)
      return failure();
    if (op.getResult().getType() != broadcast.getScalar().getType())
      return failure();
    rewriter.replaceOp(op, broadcast.getScalar());
    return success();
  }
};

template <typename ReduceOp>
struct VReduceSingleLaneDim : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReduceOp op, PatternRewriter &rewriter) const override {
    auto vecTy = dyn_cast<VectorType>(op.getVec().getType());
    if (!vecTy)
      return failure();
    if (!op.getDim()) {
      for (int64_t extent : vecTy.getShape())
        if (extent != 1)
          return failure();
      if (vecTy.getRank() == 1) {
        rewriter.replaceOpWithNewOp<pyc::VGetOp>(op, op.getResult().getType(), op.getVec(), 0);
        return success();
      }
      auto rowTy = dyn_cast<VectorType>(vecTy.getElementType());
      if (!rowTy)
        return failure();
      auto row = rewriter.create<pyc::VGetOp>(op.getLoc(), rowTy, op.getVec(), 0);
      rewriter.replaceOpWithNewOp<pyc::VGetOp>(op, op.getResult().getType(), row, 0);
      return success();
    }
    std::int64_t dim = op.getDim().value_or(0);
    if (dim < 0 || dim >= vecTy.getRank() || vecTy.getDimSize(dim) != 1)
      return failure();

    if (dim == 0) {
      rewriter.replaceOpWithNewOp<pyc::VGetOp>(op, op.getResult().getType(), op.getVec(), 0);
      return success();
    }

    // For rank-2 vectors represented as v_create(row0, row1, ...), reducing a
    // single-column inner dimension picks lane 0 from every row.
    auto create = op.getVec().template getDefiningOp<pyc::VCreateOp>();
    if (!create)
      return failure();
    SmallVector<Value> elems;
    elems.reserve(create.getElements().size());
    for (Value row : create.getElements()) {
      auto rowTy = dyn_cast<VectorType>(row.getType());
      if (!rowTy || rowTy.getRank() != 1 || rowTy.getDimSize(0) != 1)
        return failure();
      elems.push_back(rewriter.create<pyc::VGetOp>(op.getLoc(), rowTy.getElementType(), row, 0));
    }
    rewriter.replaceOpWithNewOp<pyc::VCreateOp>(op, op.getResult().getType(), elems);
    return success();
  }
};

struct CombCanonicalizePass : public PassWrapper<CombCanonicalizePass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CombCanonicalizePass)

  StringRef getArgument() const override { return "pyc-comb-canonicalize"; }
  StringRef getDescription() const override {
    return "Simplify combinational PYC logic (mux canonicalization and small boolean rewrites)";
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    RewritePatternSet patterns(f.getContext());
    patterns.add<MuxSameSelSimplify,
                 MuxI1ToLogic,
                 AndBasicSimplify,
                 OrBasicSimplify,
                 OrAndXorFactor,
                 VGetOfVCreate,
                 VGetOfVBroadcast,
                 VGetOfVectorMux,
                 VCreateOfConsecutiveVGets,
                 FoldVectorConstantLanes<pyc::AddOp>,
                 FoldVectorConstantLanes<pyc::SubOp>,
                 FoldVectorConstantLanes<pyc::MulOp>,
                 FoldVectorConstantLanes<pyc::UdivOp>,
                 FoldVectorConstantLanes<pyc::UremOp>,
                 FoldVectorConstantLanes<pyc::SdivOp>,
                 FoldVectorConstantLanes<pyc::SremOp>,
                 FoldVectorConstantLanes<pyc::AndOp>,
                 FoldVectorConstantLanes<pyc::OrOp>,
                 FoldVectorConstantLanes<pyc::XorOp>,
                 FoldVectorConstantLanes<pyc::CmpOp>,
                 FoldRank2ReduceLanes<pyc::VOrReduceOp>,
                 FoldRank2ReduceLanes<pyc::VAndReduceOp>,
                 FoldRank2ReduceLanes<pyc::VAddReduceOp>,
                 VReduceOfVBroadcast<pyc::VOrReduceOp>,
                 VReduceOfVBroadcast<pyc::VAndReduceOp>,
                 VReduceSingleLaneDim<pyc::VOrReduceOp>,
                 VReduceSingleLaneDim<pyc::VAndReduceOp>,
                 VReduceSingleLaneDim<pyc::VAddReduceOp>>(f.getContext());

    GreedyRewriteConfig cfg;
    if (failed(applyPatternsAndFoldGreedily(f, std::move(patterns), cfg)))
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createCombCanonicalizePass() { return std::make_unique<CombCanonicalizePass>(); }

static PassRegistration<CombCanonicalizePass> pass;

} // namespace pyc
