#include "pyc/Dialect/PYC/PYCOps.h"

#include "pyc/Dialect/PYC/PYCDialect.h"
#include "pyc/Dialect/PYC/PYCTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Types.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

using namespace mlir;
using namespace pyc;

ParseResult ConstantOp::parse(OpAsmParser &parser, OperationState &result) {
  // Parse: `pyc.constant <integer> : <type>`
  SMLoc loc = parser.getCurrentLocation();

  // Parse the literal as an APInt (avoid consuming `: <type>` as part of the
  // attribute).
  APInt v;
  Type type;
  if (parser.parseInteger(v) || parser.parseColonType(type))
    return failure();

  auto intTy = dyn_cast<IntegerType>(type);
  if (!intTy)
    return parser.emitError(loc,
                            "pyc.constant requires an integer result type");

  // Re-type the value to match the result type width.
  if (v.getBitWidth() != (unsigned)intTy.getWidth())
    v = v.zextOrTrunc(intTy.getWidth());

  result.addAttribute("value", IntegerAttr::get(intTy, v));
  result.addTypes(type);
  return success();
}

void ConstantOp::print(OpAsmPrinter &p) {
  p << " " << getValueAttr().getValue().getZExtValue() << " : " << getType();
}

OpFoldResult ConstantOp::fold(FoldAdaptor) { return getValueAttr(); }

static std::optional<llvm::APInt> asIntAttr(Attribute a) {
  if (!a)
    return std::nullopt;
  if (auto ia = dyn_cast<IntegerAttr>(a))
    return ia.getValue();
  return std::nullopt;
}

template <typename Pred> static bool integerConstMatch(Value v, Pred pred) {
  if (!v)
    return false;
  if (auto c = v.getDefiningOp<ConstantOp>())
    return pred(c.getValueAttr().getValue());
  if (auto c = v.getDefiningOp<arith::ConstantOp>()) {
    auto attr = c.getValue();
    if (auto ia = dyn_cast<IntegerAttr>(attr))
      return pred(ia.getValue());
  }
  return false;
}

static bool isConstZero(Value v) {
  return integerConstMatch(v, [](const APInt &x) { return x.isZero(); });
}
static bool isConstOne(Value v) {
  return integerConstMatch(v, [](const APInt &x) { return x.isOne(); });
}
static bool isConstAllOnes(Value v) {
  return integerConstMatch(v, [](const APInt &x) { return x.isAllOnes(); });
}

static IntegerAttr intAttrFor(Type ty, const llvm::APInt &v) {
  auto intTy = dyn_cast<IntegerType>(ty);
  if (!intTy)
    return {};
  llvm::APInt vv = v;
  if (vv.getBitWidth() != intTy.getWidth())
    vv = vv.zextOrTrunc(intTy.getWidth());
  return IntegerAttr::get(intTy, vv);
}

static OpFoldResult foldValueIfResultTypeMatches(Value v, Type resultTy) {
  if (v && v.getType() == resultTy)
    return v;
  return {};
}

OpFoldResult AddOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (a && b)
    return intAttrFor(outTy, (*a + *b).trunc(outTy.getWidth()));
  if (a && a->isZero())
    return getRhs();
  if (b && b->isZero())
    return getLhs();
  return {};
}

OpFoldResult SubOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (a && b)
    return intAttrFor(outTy, (*a - *b).trunc(outTy.getWidth()));
  if (b && b->isZero())
    return getLhs();
  if (getLhs() == getRhs())
    return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  return {};
}

OpFoldResult MulOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    if (isConstOne(getLhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstOne(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (a && b)
    return intAttrFor(outTy, (*a * *b).trunc(outTy.getWidth()));
  if (a) {
    if (a->isZero())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
    if (a->isOne())
      return getRhs();
  }
  if (b) {
    if (b->isZero())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
    if (b->isOne())
      return getLhs();
  }
  return {};
}

OpFoldResult UdivOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstOne(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (b) {
    if (b->isZero())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
    if (b->isOne())
      return getLhs();
  }
  if (a && a->isZero())
    return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  if (a && b)
    return intAttrFor(outTy, a->udiv(*b).trunc(outTy.getWidth()));
  return {};
}

OpFoldResult UremOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (b) {
    if (b->isZero())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
    if (b->isOne())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  }
  if (a && a->isZero())
    return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  if (a && b)
    return intAttrFor(outTy, a->urem(*b).trunc(outTy.getWidth()));
  return {};
}

OpFoldResult SdivOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstOne(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (b) {
    if (b->isZero())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
    if (b->isOne())
      return getLhs();
  }
  if (a && a->isZero())
    return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  if (a && b)
    return intAttrFor(outTy, a->sdiv(*b).trunc(outTy.getWidth()));
  return {};
}

OpFoldResult SremOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (b) {
    if (b->isZero())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
    if (b->isOne())
      return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  }
  if (a && a->isZero())
    return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  if (a && b)
    return intAttrFor(outTy, a->srem(*b).trunc(outTy.getWidth()));
  return {};
}

OpFoldResult AndOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    if (isConstAllOnes(getLhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstAllOnes(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (a && b)
    return intAttrFor(outTy, (*a & *b).trunc(outTy.getWidth()));
  if (a) {
    if (a->isZero())
      return intAttrFor(outTy, *a);
    if (a->isAllOnes())
      return getRhs();
  }
  if (b) {
    if (b->isZero())
      return intAttrFor(outTy, *b);
    if (b->isAllOnes())
      return getLhs();
  }
  return {};
}

OpFoldResult OrOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstAllOnes(getLhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    if (isConstAllOnes(getRhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (a && b)
    return intAttrFor(outTy, (*a | *b).trunc(outTy.getWidth()));
  if (a) {
    if (a->isZero())
      return getRhs();
    if (a->isAllOnes())
      return intAttrFor(outTy, *a);
  }
  if (b) {
    if (b->isZero())
      return getLhs();
    if (b->isAllOnes())
      return intAttrFor(outTy, *b);
  }
  return {};
}

OpFoldResult XorOp::fold(FoldAdaptor adaptor) {
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy) {
    if (isConstZero(getLhs()))
      return foldValueIfResultTypeMatches(getRhs(), getResult().getType());
    if (isConstZero(getRhs()))
      return foldValueIfResultTypeMatches(getLhs(), getResult().getType());
    return {};
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (a && b)
    return intAttrFor(outTy, (*a ^ *b).trunc(outTy.getWidth()));
  if (a && a->isZero())
    return getRhs();
  if (b && b->isZero())
    return getLhs();
  if (getLhs() == getRhs())
    return intAttrFor(outTy, llvm::APInt(outTy.getWidth(), 0));
  return {};
}

OpFoldResult NotOp::fold(FoldAdaptor adaptor) {
  if (auto inner = getIn().getDefiningOp<NotOp>())
    return inner.getIn();
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy)
    return {};
  auto a = asIntAttr(adaptor.getIn());
  if (a)
    return intAttrFor(outTy, (~(*a)).trunc(outTy.getWidth()));
  return {};
}

OpFoldResult SelectOp::fold(FoldAdaptor adaptor) {
  auto sel = asIntAttr(adaptor.getSel());
  if (sel) {
    if (sel->isZero())
      return getB();
    return getA();
  }
  if (getA() == getB())
    return getA();
  return {};
}

OpFoldResult CmpOp::fold(FoldAdaptor adaptor) {
  if (!isa<IntegerType>(getResult().getType()))
    return {};
  StringRef predicate = getPredicate();
  if (getLhs() == getRhs()) {
    bool value = predicate == "eq";
    return IntegerAttr::get(IntegerType::get(getContext(), 1), value ? 1 : 0);
  }
  auto a = asIntAttr(adaptor.getLhs());
  auto b = asIntAttr(adaptor.getRhs());
  if (a && b) {
    bool value = predicate == "eq"    ? (*a == *b)
                 : predicate == "ult" ? a->ult(*b)
                                      : a->slt(*b);
    return IntegerAttr::get(IntegerType::get(getContext(), 1), value ? 1 : 0);
  }
  return {};
}

OpFoldResult TruncOp::fold(FoldAdaptor adaptor) {
  if (getIn().getType() == getResult().getType())
    return getIn();
  if (auto z = getIn().getDefiningOp<ZextOp>()) {
    if (z.getIn().getType() == getResult().getType())
      return z.getIn();
  }
  if (auto s = getIn().getDefiningOp<SextOp>()) {
    if (s.getIn().getType() == getResult().getType())
      return s.getIn();
  }
  auto a = asIntAttr(adaptor.getIn());
  if (a) {
    auto outTy = dyn_cast<IntegerType>(getResult().getType());
    if (!outTy)
      return {};
    return intAttrFor(getResult().getType(), a->trunc(outTy.getWidth()));
  }
  return {};
}

OpFoldResult ZextOp::fold(FoldAdaptor adaptor) {
  if (getIn().getType() == getResult().getType())
    return getIn();
  auto a = asIntAttr(adaptor.getIn());
  if (a) {
    auto outTy = dyn_cast<IntegerType>(getResult().getType());
    if (!outTy)
      return {};
    return intAttrFor(getResult().getType(), a->zext(outTy.getWidth()));
  }
  return {};
}

OpFoldResult SextOp::fold(FoldAdaptor adaptor) {
  if (getIn().getType() == getResult().getType())
    return getIn();
  auto a = asIntAttr(adaptor.getIn());
  if (a) {
    auto outTy = dyn_cast<IntegerType>(getResult().getType());
    if (!outTy)
      return {};
    return intAttrFor(getResult().getType(), a->sext(outTy.getWidth()));
  }
  return {};
}

OpFoldResult ExtractOp::fold(FoldAdaptor adaptor) {
  auto inTy = dyn_cast<IntegerType>(getIn().getType());
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!inTy || !outTy)
    return {};
  std::int64_t lsb = getLsbAttr().getInt();
  if (lsb == 0 && outTy.getWidth() == inTy.getWidth())
    return getIn();
  if (auto c = getIn().getDefiningOp<ConcatOp>()) {
    auto cTy = cast<IntegerType>(c.getResult().getType());
    std::int64_t pos = static_cast<std::int64_t>(cTy.getWidth());
    for (Value v : c.getInputs()) {
      auto vTy = cast<IntegerType>(v.getType());
      pos -= static_cast<std::int64_t>(vTy.getWidth());
      if (pos == lsb && vTy.getWidth() == outTy.getWidth())
        return v;
    }
  }
  auto a = asIntAttr(adaptor.getIn());
  if (a) {
    llvm::APInt shifted = a->lshr(static_cast<unsigned>(lsb));
    llvm::APInt sliced = shifted.trunc(outTy.getWidth());
    return intAttrFor(getResult().getType(), sliced);
  }
  return {};
}

static OpFoldResult foldShift(Value input, Attribute inputAttr,
                              Attribute amountAttr, Type resultType,
                              StringRef kind) {
  auto amount = asIntAttr(amountAttr);
  if (!amount)
    return {};
  uint64_t shift = amount->getLimitedValue();
  if (shift == 0)
    return input;
  auto outTy = dyn_cast<IntegerType>(resultType);
  if (!outTy)
    return {};
  auto value = asIntAttr(inputAttr);
  if (shift >= outTy.getWidth()) {
    if (kind != "ashr")
      return intAttrFor(resultType, llvm::APInt(outTy.getWidth(), 0));
    if (!value)
      return {};
    return intAttrFor(resultType,
                      value->isNegative()
                          ? llvm::APInt::getAllOnes(outTy.getWidth())
                          : llvm::APInt(outTy.getWidth(), 0));
  }
  if (!value)
    return {};
  llvm::APInt result = kind == "shl"    ? (*value << shift)
                       : kind == "lshr" ? value->lshr(shift)
                                        : value->ashr(shift);
  return intAttrFor(resultType, result.trunc(outTy.getWidth()));
}

OpFoldResult ShlOp::fold(FoldAdaptor adaptor) {
  return foldShift(getIn(), adaptor.getIn(), adaptor.getAmount(),
                   getResult().getType(), "shl");
}

OpFoldResult LshrOp::fold(FoldAdaptor adaptor) {
  return foldShift(getIn(), adaptor.getIn(), adaptor.getAmount(),
                   getResult().getType(), "lshr");
}

OpFoldResult AshrOp::fold(FoldAdaptor adaptor) {
  return foldShift(getIn(), adaptor.getIn(), adaptor.getAmount(),
                   getResult().getType(), "ashr");
}

OpFoldResult ConcatOp::fold(FoldAdaptor adaptor) {
  if (getInputs().size() == 1)
    return getInputs().front();

  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy)
    return {};
  llvm::APInt acc(outTy.getWidth(), 0);

  bool allConst = true;
  unsigned offset = outTy.getWidth();
  for (auto [v, a] : llvm::zip(getInputs(), adaptor.getInputs())) {
    auto inTy = cast<IntegerType>(v.getType());
    offset -= inTy.getWidth();
    auto av = asIntAttr(a);
    if (!av) {
      allConst = false;
      break;
    }
    llvm::APInt piece = av->zextOrTrunc(inTy.getWidth());
    acc.insertBits(piece, offset);
  }
  if (allConst)
    return intAttrFor(getResult().getType(), acc);

  return {};
}

OpFoldResult AliasOp::fold(FoldAdaptor) {
  // Preserve alias ops that carry a debug name (used for codegen name
  // mangling).
  if (auto nAttr = (*this)->getAttrOfType<StringAttr>("pyc.name"))
    return {};
  return getIn();
}

LogicalResult SelectOp::verify() {
  if (getA().getType() != getB().getType())
    return emitOpError("selected values must have the same integer type");
  if (getResult().getType() != getA().getType())
    return emitOpError("result type must match the selected value type");
  return success();
}

LogicalResult NotOp::verify() {
  if (getIn().getType() != getResult().getType())
    return emitOpError("result type must match input type");
  return success();
}

static LogicalResult verifyIntCast(Operation *op, Type inTyRaw, Type outTyRaw,
                                   bool requireWiden, bool signExtend) {
  (void)signExtend;
  auto inTy = dyn_cast<IntegerType>(inTyRaw);
  auto outTy = dyn_cast<IntegerType>(outTyRaw);
  if (!inTy || !outTy)
    return op->emitOpError("only supports scalar integer types");
  if (requireWiden) {
    if (outTy.getWidth() < inTy.getWidth())
      return op->emitOpError("result width must be >= input width");
  } else {
    if (outTy.getWidth() > inTy.getWidth())
      return op->emitOpError("result width must be <= input width");
  }
  return success();
}

LogicalResult TruncOp::verify() {
  return verifyIntCast(*this, getIn().getType(), getResult().getType(),
                       /*requireWiden=*/false, /*signExtend=*/false);
}

LogicalResult ZextOp::verify() {
  return verifyIntCast(*this, getIn().getType(), getResult().getType(),
                       /*requireWiden=*/true, /*signExtend=*/false);
}

LogicalResult SextOp::verify() {
  return verifyIntCast(*this, getIn().getType(), getResult().getType(),
                       /*requireWiden=*/true, /*signExtend=*/true);
}

LogicalResult ExtractOp::verify() {
  auto inTy = dyn_cast<IntegerType>(getIn().getType());
  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!inTy || !outTy)
    return emitOpError("only supports scalar integer types");
  if (outTy.getWidth() == 0)
    return emitOpError("result width must be > 0");
  std::int64_t lsb = getLsbAttr().getInt();
  if (lsb < 0)
    return emitOpError("lsb must be >= 0");
  if (static_cast<std::uint64_t>(lsb) +
          static_cast<std::uint64_t>(outTy.getWidth()) >
      static_cast<std::uint64_t>(inTy.getWidth()))
    return emitOpError("slice out of range for input type");
  if (auto msbAttr = getMsbAttr()) {
    std::int64_t msb = msbAttr.getInt();
    std::int64_t expected =
        lsb + static_cast<std::int64_t>(outTy.getWidth()) - 1;
    if (msb != expected)
      return emitOpError("msb must equal lsb + result_width - 1 (expected ")
             << expected << ", got " << msb << ")";
  }
  return success();
}

static LogicalResult verifyDynShift(Operation *op, Type inTyRaw, Type amtTyRaw,
                                    Type outTyRaw) {
  if (!isa<IntegerType>(inTyRaw) || !isa<IntegerType>(amtTyRaw) ||
      !isa<IntegerType>(outTyRaw))
    return op->emitOpError("only supports scalar integer types");
  if (outTyRaw != inTyRaw)
    return op->emitOpError("result type must match input type");
  return success();
}

LogicalResult ShlOp::verify() {
  return verifyDynShift(*this, getIn().getType(), getAmount().getType(),
                        getResult().getType());
}

LogicalResult LshrOp::verify() {
  return verifyDynShift(*this, getIn().getType(), getAmount().getType(),
                        getResult().getType());
}

LogicalResult AshrOp::verify() {
  return verifyDynShift(*this, getIn().getType(), getAmount().getType(),
                        getResult().getType());
}

LogicalResult ConcatOp::verify() {
  if (getInputs().empty())
    return emitOpError("requires at least one input");

  auto outTy = dyn_cast<IntegerType>(getResult().getType());
  if (!outTy)
    return emitOpError("only supports integer result types");

  std::uint64_t sum = 0;
  for (Value v : getInputs()) {
    auto ty = dyn_cast<IntegerType>(v.getType());
    if (!ty)
      return emitOpError("only supports integer input types");
    sum += static_cast<std::uint64_t>(ty.getWidth());
  }

  if (sum != static_cast<std::uint64_t>(outTy.getWidth()))
    return emitOpError("result width must equal sum of input widths");

  return success();
}

LogicalResult PriorityEncodeOp::verify() {
  auto inputType = dyn_cast<IntegerType>(getIn().getType());
  auto indexType = dyn_cast<IntegerType>(getIndex().getType());
  if (!inputType || !indexType)
    return emitOpError("input and index result must be integer types");
  const unsigned inputWidth = inputType.getWidth();
  unsigned indexWidth = 1;
  for (unsigned extent = 2; extent < inputWidth; extent <<= 1)
    ++indexWidth;
  if (indexType.getWidth() != indexWidth)
    return emitOpError() << "index result width must be max(1, ceil(log2("
                         << inputWidth << "))) = " << indexWidth;
  if (getOrder() != "low" && getOrder() != "high")
    return emitOpError("order must be \"low\" or \"high\"");
  return success();
}

LogicalResult PopcountOp::verify() {
  auto inputType = dyn_cast<IntegerType>(getIn().getType());
  auto countType = dyn_cast<IntegerType>(getCount().getType());
  if (!inputType || !countType)
    return emitOpError("input and count result must be integer types");
  unsigned expectedWidth = 1;
  uint64_t representable = 1;
  while (representable < inputType.getWidth()) {
    ++expectedWidth;
    representable = (representable << 1) | 1;
  }
  if (countType.getWidth() != expectedWidth)
    return emitOpError()
           << "count result width must be max(1, ceil(log2(N+1))) = "
           << expectedWidth;
  return success();
}

LogicalResult CountZerosOp::verify() {
  auto inputType = dyn_cast<IntegerType>(getIn().getType());
  auto countType = dyn_cast<IntegerType>(getCount().getType());
  if (!inputType || !countType)
    return emitOpError("input and count result must be integer types");
  unsigned expectedWidth = 1;
  uint64_t representable = 1;
  while (representable < inputType.getWidth()) {
    ++expectedWidth;
    representable = (representable << 1) | 1;
  }
  if (countType.getWidth() != expectedWidth)
    return emitOpError()
           << "count result width must be max(1, ceil(log2(N+1))) = "
           << expectedWidth;
  if (getDirection() != "leading" && getDirection() != "trailing")
    return emitOpError("direction must be \"leading\" or \"trailing\"");
  return success();
}

static bool isRtlIdentifier(llvm::StringRef value) {
  if (value.empty() || !(llvm::isAlpha(value.front()) || value.front() == '_'))
    return false;
  return llvm::all_of(value.drop_front(), [](char c) {
    return llvm::isAlnum(c) || c == '_' || c == '$';
  });
}

static bool isSha256Fingerprint(llvm::StringRef value) {
  if (!value.consume_front("sha256:") || value.size() != 64)
    return false;
  return llvm::all_of(
      value, [](char c) { return llvm::isDigit(c) || (c >= 'a' && c <= 'f'); });
}

LogicalResult RtlCombOp::verify() {
  if (getInputs().empty() || getOutputs().empty())
    return emitOpError(
        "selected combinational RTL requires inputs and outputs");
  auto semantic = (*this)->getAttrOfType<StringAttr>("semantic_id");
  auto implementation = (*this)->getAttrOfType<StringAttr>("implementation_id");
  auto module = (*this)->getAttrOfType<StringAttr>("module");
  auto parameters = (*this)->getAttrOfType<DictionaryAttr>("parameters");
  auto inputPorts = (*this)->getAttrOfType<ArrayAttr>("input_ports");
  auto outputPorts = (*this)->getAttrOfType<ArrayAttr>("output_ports");
  auto sources = (*this)->getAttrOfType<ArrayAttr>("sources");
  auto catalog = (*this)->getAttrOfType<StringAttr>("catalog_sha256");
  if (!semantic || !semantic.getValue().starts_with("pyc.") ||
      semantic.getValue().size() <= 4)
    return emitOpError("semantic_id must be a non-empty pyc.* identifier");
  if (!implementation || implementation.getValue().empty())
    return emitOpError("implementation_id must be non-empty");
  if (!module || !isRtlIdentifier(module.getValue()))
    return emitOpError("module must be a Verilog identifier");
  if (!catalog || !isSha256Fingerprint(catalog.getValue()))
    return emitOpError(
        "catalog_sha256 must be sha256: followed by 64 lowercase hex digits");
  if (!parameters)
    return emitOpError("parameters must be present");
  for (NamedAttribute parameter : parameters) {
    if (!isRtlIdentifier(parameter.getName().strref()))
      return emitOpError() << "parameter '" << parameter.getName()
                           << "' is not a Verilog identifier";
    auto value = dyn_cast<IntegerAttr>(parameter.getValue());
    if (!value)
      return emitOpError() << "parameter '" << parameter.getName()
                           << "' must be an integer attribute";
    if (value.getInt() < 0)
      return emitOpError() << "parameter '" << parameter.getName()
                           << "' must be non-negative";
  }

  llvm::StringSet<> allPorts;
  auto verifyPorts = [&](ArrayAttr ports, size_t arity,
                         llvm::StringRef kind) -> LogicalResult {
    if (!ports || ports.size() != arity)
      return emitOpError() << kind << "_ports arity must match " << kind
                           << " value arity";
    llvm::StringSet<> seen;
    for (Attribute raw : ports) {
      auto port = dyn_cast<StringAttr>(raw);
      if (!port || !isRtlIdentifier(port.getValue()))
        return emitOpError()
               << kind << "_ports must contain Verilog identifiers";
      if (!seen.insert(port.getValue()).second)
        return emitOpError() << kind << "_ports must be unique";
      if (!allPorts.insert(port.getValue()).second)
        return emitOpError("input and output port names must be disjoint");
    }
    return success();
  };
  if (failed(verifyPorts(inputPorts, getInputs().size(), "input")) ||
      failed(verifyPorts(outputPorts, getOutputs().size(), "output")))
    return failure();

  if (!sources || sources.empty())
    return emitOpError("sources must contain a non-empty dependency closure");
  llvm::StringSet<> sourcePaths;
  for (Attribute raw : sources) {
    auto source = dyn_cast<DictionaryAttr>(raw);
    auto path = source ? source.getAs<StringAttr>("path") : StringAttr();
    auto digest = source ? source.getAs<StringAttr>("sha256") : StringAttr();
    auto license = source ? source.getAs<StringAttr>("license") : StringAttr();
    if (!path || !digest || !license || license.getValue().empty())
      return emitOpError(
          "each source requires path, sha256, and license strings");
    llvm::StringRef value = path.getValue();
    bool escapes = false;
    for (auto part = llvm::sys::path::begin(value),
              end = llvm::sys::path::end(value);
         part != end; ++part)
      escapes |= *part == "..";
    if (value.empty() || llvm::sys::path::is_absolute(value) ||
        value.contains("\\") || escapes)
      return emitOpError("source paths must be normalized relative paths");
    if (!sourcePaths.insert(value).second)
      return emitOpError("source paths must be unique");
    if (!isSha256Fingerprint(digest.getValue()))
      return emitOpError(
          "source sha256 must use lowercase sha256:<64-hex> format");
  }
  return success();
}

LogicalResult AssignOp::verify() {
  if (!getDst().getDefiningOp<WireOp>())
    return emitOpError("dst must be defined by pyc.wire");
  return success();
}

LogicalResult RegOp::verify() {
  auto nextTy = getNext().getType();
  if (getInit().getType() != nextTy)
    return emitOpError("init type must match next type");
  if (getQ().getType() != nextTy)
    return emitOpError("result type must match next type");
  return success();
}

LogicalResult FifoOp::verify() {
  auto inTy = getInData().getType();
  auto outTy = getOutData().getType();
  if (inTy != outTy)
    return emitOpError("out_data type must match in_data type");
  auto depthAttr = (*this)->getAttrOfType<IntegerAttr>("depth");
  if (!depthAttr)
    return emitOpError("requires integer attribute `depth`");
  if (depthAttr.getValue().getSExtValue() <= 0)
    return emitOpError("depth must be > 0");
  return success();
}

LogicalResult ByteMemOp::verify() {
  auto addrTy = dyn_cast<IntegerType>(getRaddr().getType());
  auto waddrTy = dyn_cast<IntegerType>(getWaddr().getType());
  if (!addrTy || !waddrTy)
    return emitOpError("only supports integer address types");
  if (addrTy != waddrTy)
    return emitOpError("waddr type must match raddr type");

  auto dataTy = dyn_cast<IntegerType>(getWdata().getType());
  auto rdataTy = dyn_cast<IntegerType>(getRdata().getType());
  if (!dataTy || !rdataTy)
    return emitOpError("only supports integer data types");
  if (dataTy != rdataTy)
    return emitOpError("rdata type must match wdata type");

  unsigned dataW = dataTy.getWidth();
  if (dataW == 0)
    return emitOpError("data width must be >= 1");

  auto strbTy = dyn_cast<IntegerType>(getWstrb().getType());
  if (!strbTy)
    return emitOpError("only supports integer wstrb types");
  if (strbTy.getWidth() != ((dataW + 7) / 8))
    return emitOpError("wstrb width must be ceil(data_width / 8)");

  auto depthAttr = (*this)->getAttrOfType<IntegerAttr>("depth");
  if (!depthAttr)
    return emitOpError("requires integer attribute `depth` (bytes)");
  if (depthAttr.getValue().getSExtValue() <= 0)
    return emitOpError("depth must be > 0");

  if (auto nameAttr = (*this)->getAttrOfType<StringAttr>("name")) {
    if (nameAttr.getValue().empty())
      return emitOpError("name must be non-empty when provided");
  }

  return success();
}

LogicalResult SyncMemOp::verify() {
  auto addrTy = dyn_cast<IntegerType>(getRaddr().getType());
  auto waddrTy = dyn_cast<IntegerType>(getWaddr().getType());
  if (!addrTy || !waddrTy)
    return emitOpError("only supports integer address types");
  if (addrTy != waddrTy)
    return emitOpError("waddr type must match raddr type");

  auto dataTy = dyn_cast<IntegerType>(getWdata().getType());
  auto rdataTy = dyn_cast<IntegerType>(getRdata().getType());
  if (!dataTy || !rdataTy)
    return emitOpError("only supports integer data types");
  if (dataTy != rdataTy)
    return emitOpError("rdata type must match wdata type");

  unsigned dataW = dataTy.getWidth();
  if (dataW == 0)
    return emitOpError("data width must be >= 1");

  auto strbTy = dyn_cast<IntegerType>(getWstrb().getType());
  if (!strbTy)
    return emitOpError("only supports integer wstrb types");
  if (strbTy.getWidth() != ((dataW + 7) / 8))
    return emitOpError("wstrb width must be ceil(data_width / 8)");

  auto depthAttr = (*this)->getAttrOfType<IntegerAttr>("depth");
  if (!depthAttr)
    return emitOpError("requires integer attribute `depth` (entries)");
  if (depthAttr.getValue().getSExtValue() <= 0)
    return emitOpError("depth must be > 0");

  if (auto nameAttr = (*this)->getAttrOfType<StringAttr>("name")) {
    if (nameAttr.getValue().empty())
      return emitOpError("name must be non-empty when provided");
  }

  return success();
}

LogicalResult SyncMemDPOp::verify() {
  auto addrTy0 = dyn_cast<IntegerType>(getRaddr0().getType());
  auto addrTy1 = dyn_cast<IntegerType>(getRaddr1().getType());
  auto waddrTy = dyn_cast<IntegerType>(getWaddr().getType());
  if (!addrTy0 || !addrTy1 || !waddrTy)
    return emitOpError("only supports integer address types");
  if (addrTy0 != addrTy1 || addrTy0 != waddrTy)
    return emitOpError("raddr0/raddr1/waddr types must match");

  auto dataTy = dyn_cast<IntegerType>(getWdata().getType());
  auto rdataTy0 = dyn_cast<IntegerType>(getRdata0().getType());
  auto rdataTy1 = dyn_cast<IntegerType>(getRdata1().getType());
  if (!dataTy || !rdataTy0 || !rdataTy1)
    return emitOpError("only supports integer data types");
  if (dataTy != rdataTy0 || dataTy != rdataTy1)
    return emitOpError("rdata types must match wdata type");

  unsigned dataW = dataTy.getWidth();
  if (dataW == 0)
    return emitOpError("data width must be >= 1");

  auto strbTy = dyn_cast<IntegerType>(getWstrb().getType());
  if (!strbTy)
    return emitOpError("only supports integer wstrb types");
  if (strbTy.getWidth() != ((dataW + 7) / 8))
    return emitOpError("wstrb width must be ceil(data_width / 8)");

  auto depthAttr = (*this)->getAttrOfType<IntegerAttr>("depth");
  if (!depthAttr)
    return emitOpError("requires integer attribute `depth` (entries)");
  if (depthAttr.getValue().getSExtValue() <= 0)
    return emitOpError("depth must be > 0");

  if (auto nameAttr = (*this)->getAttrOfType<StringAttr>("name")) {
    if (nameAttr.getValue().empty())
      return emitOpError("name must be non-empty when provided");
  }

  return success();
}

LogicalResult AsyncFifoOp::verify() {
  auto inTy = getInData().getType();
  auto outTy = getOutData().getType();
  if (inTy != outTy)
    return emitOpError("out_data type must match in_data type");
  auto depthAttr = (*this)->getAttrOfType<IntegerAttr>("depth");
  if (!depthAttr)
    return emitOpError("requires integer attribute `depth`");
  std::int64_t depth = depthAttr.getValue().getSExtValue();
  if (depth < 2)
    return emitOpError("depth must be >= 2");
  // Prototype async FIFO assumes a power-of-two depth for gray-code pointers.
  std::uint64_t d = static_cast<std::uint64_t>(depth);
  if ((d & (d - 1)) != 0)
    return emitOpError("depth must be a power of two in the prototype");
  return success();
}

LogicalResult CdcSyncOp::verify() {
  auto ty = dyn_cast<IntegerType>(getIn().getType());
  if (!ty)
    return emitOpError("only supports integer types");
  if (ty.getWidth() == 0 || ty.getWidth() > 64)
    return emitOpError("prototype supports widths 1..64");
  auto stagesAttr = (*this)->getAttrOfType<IntegerAttr>("stages");
  if (stagesAttr) {
    if (stagesAttr.getValue().getSExtValue() < 1)
      return emitOpError("stages must be >= 1");
  }
  return success();
}

LogicalResult InstanceOp::verify() {
  auto calleeAttr = getCalleeAttr();
  if (!calleeAttr)
    return emitOpError("requires FlatSymbolRefAttr attribute `callee`");

  auto module = (*this)->getParentOfType<ModuleOp>();
  if (!module)
    return emitOpError("must be contained in an MLIR module");

  Operation *sym = SymbolTable::lookupSymbolIn(module, calleeAttr);
  auto callee = dyn_cast_or_null<func::FuncOp>(sym);
  if (!callee)
    return emitOpError("callee must reference a func.func");

  FunctionType ft = callee.getFunctionType();
  if (ft.getNumInputs() != getNumOperands())
    return emitOpError("operand count does not match callee signature");
  if (ft.getNumResults() != getNumResults())
    return emitOpError("result count does not match callee signature");

  auto isBoundaryType = [](Type type) {
    return isa<IntegerType, pyc::ClockType, pyc::ResetType>(type);
  };

  for (auto [i, ty] : llvm::enumerate(ft.getInputs())) {
    if (!isBoundaryType(ty))
      return emitOpError() << "callee input #" << i
                           << " must use a scalar PYC boundary type";
    if (getOperand(i).getType() != ty)
      return emitOpError() << "operand type mismatch at #" << i << ": got "
                           << getOperand(i).getType() << " expected " << ty;
  }
  for (auto [i, ty] : llvm::enumerate(ft.getResults())) {
    if (!isBoundaryType(ty))
      return emitOpError() << "callee result #" << i
                           << " must use a scalar PYC boundary type";
    if (getResult(i).getType() != ty)
      return emitOpError() << "result type mismatch at #" << i << ": got "
                           << getResult(i).getType() << " expected " << ty;
  }

  if (auto n = getNameAttr()) {
    if (n.getValue().empty())
      return emitOpError("name must be non-empty when provided");
  }

  return success();
}

LogicalResult AssertOp::verify() {
  if (auto m = getMsgAttr()) {
    if (m.getValue().empty())
      return emitOpError("msg must be non-empty when provided");
  }
  return success();
}

LogicalResult CombOp::verify() {
  if (getBody().empty())
    return emitOpError("requires a non-empty region");
  if (!llvm::hasSingleElement(getBody()))
    return emitOpError("requires a single block region");

  Block &b = getBody().front();
  if (b.getNumArguments() != getNumOperands())
    return emitOpError("body block argument count must match comb inputs");

  for (auto [arg, in] : llvm::zip(b.getArguments(), getInputs())) {
    if (!isa<IntegerType>(in.getType()))
      return emitOpError("comb inputs must be scalar integers");
    if (arg.getType() != in.getType())
      return emitOpError(
          "body block argument types must match comb input types");
  }

  auto yield = dyn_cast<YieldOp>(b.getTerminator());
  if (!yield)
    return emitOpError("body must terminate with pyc.yield");

  if (yield.getNumOperands() != getNumResults())
    return emitOpError("pyc.yield operand count must match comb results");

  for (auto [v, r] : llvm::zip(yield.getOperands(), getResults())) {
    if (!isa<IntegerType>(r.getType()))
      return emitOpError("comb results must be scalar integers");
    if (v.getType() != r.getType())
      return emitOpError(
          "pyc.yield operand types must match comb result types");
  }

  return success();
}

//===----------------------------------------------------------------------===//
// Scalar binary op verifiers
//===----------------------------------------------------------------------===//

static LogicalResult verifyScalarBinary(Operation *op, Type lhsTy, Type rhsTy,
                                        Type resultTy, bool compareResult) {
  if (!isa<IntegerType>(lhsTy) || !isa<IntegerType>(rhsTy) ||
      !isa<IntegerType>(resultTy))
    return op->emitOpError("operands and result must be scalar integers");
  if (lhsTy != rhsTy)
    return op->emitOpError("operand integer types must match");
  Type expected =
      compareResult ? Type(IntegerType::get(op->getContext(), 1)) : lhsTy;
  if (resultTy != expected)
    return op->emitOpError("result type must be ") << expected;
  return success();
}

#define DEFINE_VALUE_BINARY_VERIFY(OP)                                         \
  LogicalResult OP::verify() {                                                 \
    return verifyScalarBinary(getOperation(), getLhs().getType(),              \
                              getRhs().getType(), getResult().getType(),       \
                              /*compareResult=*/false);                        \
  }

DEFINE_VALUE_BINARY_VERIFY(AddOp)
DEFINE_VALUE_BINARY_VERIFY(SubOp)
DEFINE_VALUE_BINARY_VERIFY(MulOp)
DEFINE_VALUE_BINARY_VERIFY(UdivOp)
DEFINE_VALUE_BINARY_VERIFY(UremOp)
DEFINE_VALUE_BINARY_VERIFY(SdivOp)
DEFINE_VALUE_BINARY_VERIFY(SremOp)
DEFINE_VALUE_BINARY_VERIFY(AndOp)
DEFINE_VALUE_BINARY_VERIFY(OrOp)
DEFINE_VALUE_BINARY_VERIFY(XorOp)

#undef DEFINE_VALUE_BINARY_VERIFY

LogicalResult CmpOp::verify() {
  StringRef predicate = getPredicate();
  if (predicate != "eq" && predicate != "ult" && predicate != "slt")
    return emitOpError("predicate must be eq, ult, or slt");
  return verifyScalarBinary(getOperation(), getLhs().getType(),
                            getRhs().getType(), getResult().getType(),
                            /*compareResult=*/true);
}

#define GET_OP_CLASSES
#include "pyc/Dialect/PYC/PYCOps.cpp.inc"
