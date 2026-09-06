#include "pyc/Transforms/Passes.h"

#include "pyc/Dialect/PYC/PYCOps.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>

using namespace mlir;

namespace pyc {
namespace {

/// Unwrap VectorType down to the leaf IntegerType.
static IntegerType leafIntType(Type ty) {
  while (auto vt = dyn_cast<VectorType>(ty))
    ty = vt.getElementType();
  return dyn_cast<IntegerType>(ty);
}

// Walk a vector shape, calling fn for each leaf index tuple.
static void walkShape(ArrayRef<int64_t> shape, unsigned depth,
                      llvm::SmallVectorImpl<int64_t> &indices,
                      std::function<void(const llvm::SmallVectorImpl<int64_t> &)> fn) {
  if (depth == shape.size()) { fn(indices); return; }
  for (int64_t i = 0; i < shape[depth]; ++i) {
    indices.push_back(i);
    walkShape(shape, depth + 1, indices, fn);
    indices.pop_back();
  }
}

// Create v_get chain to extract a scalar from a vector.
static Value extractLane(OpBuilder &builder, Location loc, Value vec, ArrayRef<int64_t> indices) {
  Value cur = vec;
  for (int64_t idx : indices) {
    auto vt = dyn_cast<VectorType>(cur.getType());
    if (!vt) return cur;
    IntegerAttr idxAttr = builder.getI64IntegerAttr(idx);
    Type resultTy = (vt.getRank() > 1)
                        ? VectorType::get(vt.getShape().drop_front(), vt.getElementType())
                        : vt.getElementType();
    cur = builder.create<pyc::VGetOp>(loc, resultTy, cur, idxAttr);
  }
  return cur;
}

// Build nested v_create to reconstruct a vector from scalar lanes.
static Value createVector(OpBuilder &builder, Location loc, Type vecTy,
                          llvm::SmallVectorImpl<Value> &lanes,
                          ArrayRef<int64_t> shape, unsigned depth) {
  if (depth == shape.size() - 1)
    return builder.create<pyc::VCreateOp>(loc, vecTy, lanes);
  int64_t groupSize = 1;
  for (unsigned d = depth + 1; d < shape.size(); ++d) groupSize *= shape[d];
  auto rowTy = VectorType::get(shape.drop_front(), cast<VectorType>(vecTy).getElementType());
  llvm::SmallVector<Value> rows;
  for (int64_t i = 0; i < shape[depth]; ++i) {
    llvm::SmallVector<Value> group(lanes.begin() + i * groupSize,
                                   lanes.begin() + (i + 1) * groupSize);
    rows.push_back(createVector(builder, loc, rowTy, group, shape, depth + 1));
  }
  return builder.create<pyc::VCreateOp>(loc, vecTy, rows);
}

// Unroll an element-wise vector op into per-lane scalar ops.
static Value unrollElementwiseOp(Operation &op, OpBuilder &builder) {
  Location loc = op.getLoc();
  auto vt = cast<VectorType>(op.getResult(0).getType());
  ArrayRef<int64_t> shape = vt.getShape();

  llvm::SmallVector<Value> lanes;
  llvm::SmallVector<int64_t> indices;
  walkShape(shape, 0, indices, [&](const llvm::SmallVectorImpl<int64_t> &idx) {
    llvm::SmallVector<Value> scalarOperands;
    for (Value operand : op.getOperands()) {
      if (isa<VectorType>(operand.getType()))
        scalarOperands.push_back(extractLane(builder, loc, operand, idx));
      else
        scalarOperands.push_back(operand);
    }
    OperationState state(loc, op.getName());
    state.addOperands(scalarOperands);
    Type scalarResultTy = vt.getElementType();
    if (isa<pyc::CmpOp>(op))
      scalarResultTy = builder.getI1Type();
    if (isa<pyc::TruncOp, pyc::ZextOp, pyc::SextOp>(op))
      scalarResultTy = leafIntType(op.getResult(0).getType());
    state.addTypes(scalarResultTy);
    for (auto attr : op.getAttrs())
      state.addAttribute(attr.getName(), attr.getValue());
    lanes.push_back(builder.create(state)->getResult(0));
  });

  if (lanes.empty()) return Value();
  if (shape.size() == 1)
    return builder.create<pyc::VCreateOp>(loc, vt, lanes);
  return createVector(builder, loc, vt, lanes, shape, 0);
}

static bool isTreeReduceMode(Operation &op) {
  if (auto mode = op.getAttrOfType<StringAttr>("mode"))
    return mode.getValue() == "tree";
  return false;
}

// Unroll vector reduce according to its mode attr.
static Value unrollReduceOp(Operation &op, OpBuilder &builder) {
  Location loc = op.getLoc();
  Value vec = op.getOperand(0);
  auto vt = cast<VectorType>(vec.getType());
  auto dimAttr = op.getAttrOfType<IntegerAttr>("dim");
  Type leafTy = vt.getElementType();

  std::function<Value(Value, Value)> reducePair;
  if (isa<pyc::VOrReduceOp>(op))
    reducePair = [&](Value a, Value b) { return builder.create<pyc::OrOp>(loc, leafTy, a, b); };
  else if (isa<pyc::VAndReduceOp>(op))
    reducePair = [&](Value a, Value b) { return builder.create<pyc::AndOp>(loc, leafTy, a, b); };
  else if (isa<pyc::VAddReduceOp>(op))
    reducePair = [&](Value a, Value b) { return builder.create<pyc::AddOp>(loc, leafTy, a, b); };
  else
    return Value();

  auto chainReduce = [&](llvm::SmallVectorImpl<Value> &values) -> Value {
    Value out = values[0];
    for (size_t i = 1; i < values.size(); ++i)
      out = reducePair(out, values[i]);
    return out;
  };
  auto treeReduce = [&](llvm::SmallVectorImpl<Value> &values) -> Value {
    while (values.size() > 1) {
      llvm::SmallVector<Value> next;
      for (size_t i = 0; i < values.size(); i += 2) {
        if (i + 1 < values.size())
          next.push_back(reducePair(values[i], values[i + 1]));
        else
          next.push_back(values[i]);
      }
      values = std::move(next);
    }
    return values[0];
  };
  auto reduceValues = [&](llvm::SmallVectorImpl<Value> &values) -> Value {
    return isTreeReduceMode(op) ? treeReduce(values) : chainReduce(values);
  };

  if (vt.getRank() == 1) {
    int64_t lanes = vt.getShape()[0];
    llvm::SmallVector<Value> values;
    for (int64_t i = 0; i < lanes; ++i)
      values.push_back(extractLane(builder, loc, vec, {i}));
    return reduceValues(values);
  }

  int64_t rows = vt.getShape()[0], cols = vt.getShape()[1];
  if (!dimAttr) {
    llvm::SmallVector<Value> values;
    values.reserve(rows * cols);
    for (int64_t i = 0; i < rows; ++i)
      for (int64_t j = 0; j < cols; ++j)
        values.push_back(extractLane(builder, loc, vec, {i, j}));
    return reduceValues(values);
  }

  int64_t dim = dimAttr.getInt();
  if (dim == 0) {
    llvm::SmallVector<Value> resultLanes;
    for (int64_t j = 0; j < cols; ++j) {
      llvm::SmallVector<Value> colVals;
      for (int64_t i = 0; i < rows; ++i)
        colVals.push_back(extractLane(builder, loc, vec, {i, j}));
      resultLanes.push_back(reduceValues(colVals));
    }
    return builder.create<pyc::VCreateOp>(loc, VectorType::get({cols}, leafTy), resultLanes);
  } else {
    llvm::SmallVector<Value> resultLanes;
    for (int64_t i = 0; i < rows; ++i) {
      llvm::SmallVector<Value> rowVals;
      for (int64_t j = 0; j < cols; ++j)
        rowVals.push_back(extractLane(builder, loc, vec, {i, j}));
      resultLanes.push_back(reduceValues(rowVals));
    }
    return builder.create<pyc::VCreateOp>(loc, VectorType::get({rows}, leafTy), resultLanes);
  }
}

// Unroll v_broadcast into v_create of N copies.
static Value unrollBroadcast(pyc::VBroadcastOp op, OpBuilder &builder) {
  auto vt = cast<VectorType>(op.getResult().getType());
  llvm::SmallVector<Value> elements(vt.getShape()[0], op.getScalar());
  return builder.create<pyc::VCreateOp>(op.getLoc(), vt, elements);
}

// Unroll v_broadcast_dim: walk result lanes, mapping each to source lane.
static Value unrollBroadcastDim(pyc::VBroadcastDimOp op, OpBuilder &builder) {
  auto srcVT = cast<VectorType>(op.getVec().getType());
  auto dstVT = cast<VectorType>(op.getResult().getType());
  int64_t dim = op.getDimAttr().getInt();
  ArrayRef<int64_t> dstShape = dstVT.getShape();

  llvm::SmallVector<Value> lanes;
  llvm::SmallVector<int64_t> indices;
  walkShape(dstShape, 0, indices, [&](const llvm::SmallVectorImpl<int64_t> &dstIdx) {
    // Build source index by dropping the broadcast dimension.
    llvm::SmallVector<int64_t> srcIdx;
    for (unsigned d = 0; d < dstShape.size(); ++d)
      if (static_cast<int64_t>(d) != dim)
        srcIdx.push_back(dstIdx[d]);
    lanes.push_back(extractLane(builder, op.getLoc(), op.getVec(), srcIdx));
  });

  if (dstShape.size() == 1)
    return builder.create<pyc::VCreateOp>(op.getLoc(), dstVT, lanes);
  return createVector(builder, op.getLoc(), dstVT, lanes, dstShape, 0);
}

// Map from a vector wire's post-unroll replacement (v_create) to the scalar
// WireOp results created for each leaf lane (walkShape order). AssignOp::verify
// requires each assign dst to be defined directly by pyc.wire; looking those
// lanes up here avoids extractLane(v_create) -> illegal pyc.v_get destinations.
using VectorWireLaneMap = llvm::DenseMap<Value, llvm::SmallVector<Value, 8>>;

// Unroll WireOp: split into N scalar wires, rebuild with v_create.
static void unrollWire(pyc::WireOp op, OpBuilder &builder, VectorWireLaneMap &laneMap) {
  auto vt = cast<VectorType>(op.getResult().getType());
  ArrayRef<int64_t> shape = vt.getShape();
  llvm::SmallVector<Value> lanes;
  llvm::SmallVector<int64_t> indices;
  walkShape(shape, 0, indices, [&](const llvm::SmallVectorImpl<int64_t> &idx) {
    (void)idx;
    auto w = builder.create<pyc::WireOp>(op.getLoc(), vt.getElementType());
    lanes.push_back(w.getResult());
  });
  Value replacement = (shape.size() == 1)
      ? builder.create<pyc::VCreateOp>(op.getLoc(), vt, lanes)
      : createVector(builder, op.getLoc(), vt, lanes, shape, 0);
  laneMap[replacement] = std::move(lanes);
  op.getResult().replaceAllUsesWith(replacement);
  op.erase();
}

// Unroll AssignOp: per-lane scalar assigns onto the original scalar wires.
// Returns failure if the vector dst was not produced by an unrolled WireOp.
static LogicalResult unrollAssign(pyc::AssignOp op, OpBuilder &builder,
                                  const VectorWireLaneMap &laneMap) {
  auto vt = cast<VectorType>(op.getDst().getType());
  ArrayRef<int64_t> shape = vt.getShape();
  auto mapped = laneMap.find(op.getDst());
  if (mapped == laneMap.end()) {
    return op.emitOpError(
        "vector assign dst has no scalar wire lanes; expected a pyc.wire "
        "unrolled earlier in pyc-unroll-vector");
  }
  size_t laneIdx = 0;
  llvm::SmallVector<int64_t> indices;
  walkShape(shape, 0, indices, [&](const llvm::SmallVectorImpl<int64_t> &idx) {
    assert(laneIdx < mapped->second.size() && "vector wire lane map size mismatch");
    Value dstL = mapped->second[laneIdx++];
    Value srcL = extractLane(builder, op.getLoc(), op.getSrc(), idx);
    builder.create<pyc::AssignOp>(op.getLoc(), dstL, srcL);
  });
  op.erase();
  return success();
}

// Unroll RegOp: N scalar regs sharing clk/rst/en.
static void unrollReg(pyc::RegOp op, OpBuilder &builder) {
  auto vt = cast<VectorType>(op.getQ().getType());
  ArrayRef<int64_t> shape = vt.getShape();
  Value clk = op.getClk(), rst = op.getRst(), en = op.getEn();
  llvm::SmallVector<Value> qLanes;
  llvm::SmallVector<int64_t> indices;
  walkShape(shape, 0, indices, [&](const llvm::SmallVectorImpl<int64_t> &idx) {
    Value n = extractLane(builder, op.getLoc(), op.getNext(), idx);
    Value i = extractLane(builder, op.getLoc(), op.getInit(), idx);
    qLanes.push_back(builder.create<pyc::RegOp>(op.getLoc(), vt.getElementType(),
                                                  clk, rst, en, n, i).getQ());
  });
  Value replacement = (shape.size() == 1)
      ? builder.create<pyc::VCreateOp>(op.getLoc(), vt, qLanes)
      : createVector(builder, op.getLoc(), vt, qLanes, shape, 0);
  op.getQ().replaceAllUsesWith(replacement);
  op.erase();
}

// --- Classification helpers ---

static bool isElementWiseVectorOp(Operation &op) {
  if (op.getNumResults() != 1) return false;
  if (!isa<VectorType>(op.getResult(0).getType())) return false;
  if (isa<pyc::VGetOp, pyc::VCreateOp, pyc::VBroadcastOp, pyc::VBroadcastDimOp,
          pyc::VOrReduceOp, pyc::VAndReduceOp, pyc::VAddReduceOp,
          pyc::WireOp, pyc::AssignOp, pyc::RegOp>(op))
    return false;
  return isa<pyc::AddOp, pyc::SubOp, pyc::MulOp,
             pyc::UdivOp, pyc::UremOp, pyc::SdivOp, pyc::SremOp,
             pyc::AndOp, pyc::OrOp, pyc::XorOp, pyc::NotOp,
             pyc::CmpOp,
             pyc::TruncOp, pyc::ZextOp, pyc::SextOp, pyc::ExtractOp,
             pyc::ShlOp, pyc::LshrOp, pyc::AshrOp,
             pyc::SelectOp>(op);
}

static bool isVectorReduceOp(Operation &op) {
  return isa<pyc::VOrReduceOp, pyc::VAndReduceOp, pyc::VAddReduceOp>(op);
}

struct VectorUnrollPass : public PassWrapper<VectorUnrollPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(VectorUnrollPass)

  StringRef getArgument() const override { return "pyc-unroll-vector"; }
  StringRef getDescription() const override {
    return "Unroll vector operations into per-lane scalar operations";
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    OpBuilder builder(f.getContext());

    // Collect all vector ops in the function body and CombOp regions.
    llvm::SmallVector<Operation *> vgetOps, reduceOps, broadcastOps, broadcastDimOps;
    llvm::SmallVector<Operation *> wireOps, assignOps, regOps, elemOps;

    std::function<void(Operation &)> collect;
    collect = [&](Operation &op) {
      if (isVectorReduceOp(op))
        reduceOps.push_back(&op);
      else if (isa<pyc::VGetOp>(op) && isa<VectorType>(op.getOperand(0).getType()))
        vgetOps.push_back(&op);
      else if (isa<pyc::VBroadcastOp>(op))
        broadcastOps.push_back(&op);
      else if (isa<pyc::VBroadcastDimOp>(op))
        broadcastDimOps.push_back(&op);
      else if (isa<pyc::WireOp>(op) && isa<VectorType>(op.getResult(0).getType()))
        wireOps.push_back(&op);
      else if (isa<pyc::AssignOp>(op) && isa<VectorType>(op.getOperand(0).getType()))
        assignOps.push_back(&op);
      else if (isa<pyc::RegOp>(op) && isa<VectorType>(op.getResult(0).getType()))
        regOps.push_back(&op);
      else if (isElementWiseVectorOp(op))
        elemOps.push_back(&op);
      else if (auto comb = dyn_cast<pyc::CombOp>(&op))
        for (Operation &inner : comb.getBody().front())
          collect(inner);
    };

    for (Block &b : f.getBody())
      for (Operation &op : b)
        collect(op);

    // Pass 1: consumers that extract lanes from vectors.
    for (auto *op : vgetOps) {
      auto vg = cast<pyc::VGetOp>(op);
      if (!isa<VectorType>(vg.getVec().getType())) continue;
      builder.setInsertionPoint(op);
      int64_t idx = vg.getIndexAttr().getInt();
      Value e = extractLane(builder, op->getLoc(), vg.getVec(), {idx});
      op->getResult(0).replaceAllUsesWith(e);
      op->erase();
    }
    for (auto *op : reduceOps) {
      if (!isa<VectorType>(op->getOperand(0).getType())) continue;
      builder.setInsertionPoint(op);
      Value r = unrollReduceOp(*op, builder);
      if (r) { op->getResult(0).replaceAllUsesWith(r); op->erase(); }
    }
    for (auto *op : broadcastOps) {
      builder.setInsertionPoint(op);
      auto vb = cast<pyc::VBroadcastOp>(op);
      Value r = unrollBroadcast(vb, builder);
      op->getResult(0).replaceAllUsesWith(r); op->erase();
    }
    for (auto *op : broadcastDimOps) {
      builder.setInsertionPoint(op);
      auto vbd = cast<pyc::VBroadcastDimOp>(op);
      Value r = unrollBroadcastDim(vbd, builder);
      op->getResult(0).replaceAllUsesWith(r); op->erase();
    }
    VectorWireLaneMap wireLaneMap;
    for (auto *op : wireOps) {
      builder.setInsertionPoint(op);
      unrollWire(cast<pyc::WireOp>(op), builder, wireLaneMap);
    }
    for (auto *op : assignOps) {
      builder.setInsertionPoint(op);
      if (failed(unrollAssign(cast<pyc::AssignOp>(op), builder, wireLaneMap)))
        return signalPassFailure();
    }
    for (auto *op : regOps) {
      builder.setInsertionPoint(op);
      unrollReg(cast<pyc::RegOp>(op), builder);
    }

    // Pass 2: element-wise producers.
    for (auto *op : elemOps) {
      if (!isa<VectorType>(op->getResult(0).getType())) continue;
      builder.setInsertionPoint(op);
      Value r = unrollElementwiseOp(*op, builder);
      if (r) { op->getResult(0).replaceAllUsesWith(r); op->erase(); }
    }
  }
};

} // namespace

std::unique_ptr<::mlir::Pass> createVectorUnrollPass() {
  return std::make_unique<VectorUnrollPass>();
}

static PassRegistration<VectorUnrollPass> pass;

} // namespace pyc
