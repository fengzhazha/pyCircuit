#include "acir/Analysis/VariableAnalysis.h"

#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACIR/ACIRTypes.h"
#include "mlir/Analysis/DataFlow/SparseAnalysis.h"
#include "mlir/Analysis/DataFlow/Utils.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

using namespace mlir;

namespace acir {
namespace {

using VariablePropertiesLattice = dataflow::Lattice<VariableProperties>;

struct ConstraintLatticeValue {
  std::optional<ValueConstraint> constraint;

  static ConstraintLatticeValue join(const ConstraintLatticeValue &left,
                                     const ConstraintLatticeValue &right) {
    if (!left.constraint)
      return right;
    if (!right.constraint)
      return left;
    return {ValueConstraint::join(*left.constraint, *right.constraint)};
  }
  bool operator==(const ConstraintLatticeValue &) const = default;
  void print(llvm::raw_ostream &os) const {
    if (constraint)
      constraint->print(os);
    else
      os << "uninitialized";
  }
};

using ValueConstraintLattice = dataflow::Lattice<ConstraintLatticeValue>;

std::optional<unsigned> integerWidth(Type type) {
  auto variable = dyn_cast<ac::VarType>(type);
  auto integer = variable
                     ? dyn_cast<IntegerType>(variable.getElementType())
                     : dyn_cast<IntegerType>(type);
  if (!integer || !integer.isSignless() || integer.getWidth() == 0 ||
      integer.getWidth() > 64)
    return std::nullopt;
  return integer.getWidth();
}

uint64_t widthMask(unsigned width) {
  return width == 64 ? std::numeric_limits<uint64_t>::max()
                     : (uint64_t{1} << width) - 1;
}

ValueConstraint defaultConstraint(Type type) {
  auto width = integerWidth(type);
  return width ? ValueConstraint::closedInterval(0, widthMask(*width))
               : ValueConstraint::unknown();
}

std::optional<SmallVector<uint64_t, 8>> exactValues(
    const ValueConstraint &constraint) {
  if (constraint.kind == ValueConstraintKind::Constant)
    return SmallVector<uint64_t, 8>{constraint.values.front()};
  if (constraint.kind == ValueConstraintKind::FiniteSet)
    return constraint.values;
  if (constraint.kind == ValueConstraintKind::ClosedInterval &&
      constraint.upper - constraint.lower < 64) {
    SmallVector<uint64_t, 8> values;
    for (uint64_t value = constraint.lower;; ++value) {
      values.push_back(value);
      if (value == constraint.upper)
        break;
    }
    return values;
  }
  return std::nullopt;
}

template <typename Fn>
ValueConstraint evaluateUnary(const ValueConstraint &input, Fn &&evaluate) {
  auto values = exactValues(input);
  if (!values)
    return ValueConstraint::unknown();
  SmallVector<uint64_t, 8> result;
  for (uint64_t value : *values)
    result.push_back(evaluate(value));
  return ValueConstraint::finiteSet(result);
}

template <typename Fn>
ValueConstraint evaluateBinary(const ValueConstraint &left,
                               const ValueConstraint &right, Fn &&evaluate) {
  auto leftValues = exactValues(left);
  auto rightValues = exactValues(right);
  if (!leftValues || !rightValues ||
      leftValues->size() * rightValues->size() > 64)
    return ValueConstraint::unknown();
  SmallVector<uint64_t, 8> result;
  llvm::DenseSet<uint64_t> unique;
  for (uint64_t lhs : *leftValues)
    for (uint64_t rhs : *rightValues) {
      uint64_t value = evaluate(lhs, rhs);
      if (unique.insert(value).second) {
        result.push_back(value);
      }
    }
  return ValueConstraint::finiteSet(result);
}

ValueConstraint operandConstraint(
    ArrayRef<const ValueConstraintLattice *> operands, unsigned index) {
  if (index >= operands.size() || !operands[index]->getValue().constraint)
    return ValueConstraint::unknown();
  return *operands[index]->getValue().constraint;
}

ValueConstraint inferConstraint(
    Operation *operation, unsigned resultIndex,
    ArrayRef<const ValueConstraintLattice *> operands) {
  Type resultType = operation->getResult(resultIndex).getType();
  if (auto constant = dyn_cast<ac::VarConstantOp>(operation)) {
    if (auto integer = dyn_cast<IntegerAttr>(constant.getValueAttr()))
      return ValueConstraint::constant(integer.getValue().getZExtValue());
  }
  if (auto enumeration = dyn_cast<ac::VarEnumOp>(operation)) {
    auto declaration = SymbolTable::lookupNearestSymbolFrom<ac::EnumOp>(
        enumeration, enumeration.getDeclaration());
    if (!declaration)
      return ValueConstraint::unknown();
    for (auto [ordinal, enumerant] :
         llvm::enumerate(declaration.getEnumerants()))
      if (cast<StringAttr>(enumerant).getValue() ==
          enumeration.getEnumerant())
        return ValueConstraint::constant(ordinal);
    return ValueConstraint::unknown();
  }
  auto width = integerWidth(resultType);
  if (!width)
    return ValueConstraint::unknown();
  const uint64_t mask = widthMask(*width);
  if (auto matches = dyn_cast<ac::VarMatchesOp>(operation)) {
    if (matches.getMask() == 0)
      return ValueConstraint::constant(1);
    ValueConstraint input = operandConstraint(operands, 0);
    if (input.kind == ValueConstraintKind::Constant)
      return ValueConstraint::constant(
          (input.values.front() & matches.getMask()) == matches.getValue());
    return ValueConstraint::closedInterval(0, 1);
  }
  if (auto select = dyn_cast<ac::VarSelectOp>(operation)) {
    ValueConstraint condition = operandConstraint(operands, 0);
    if (condition.kind == ValueConstraintKind::Constant)
      return operandConstraint(operands,
                               condition.values.front() == 0 ? 2 : 1);
    return ValueConstraint::join(operandConstraint(operands, 1),
                                 operandConstraint(operands, 2));
  }
  if (isa<ac::VarCmpOp>(operation)) {
    auto compare = cast<ac::VarCmpOp>(operation);
    auto operandWidth = integerWidth(compare.getLhs().getType());
    auto predicate = compare.getPredicate();
    ValueConstraint result = evaluateBinary(
        operandConstraint(operands, 0), operandConstraint(operands, 1),
        [&](uint64_t lhs, uint64_t rhs) -> uint64_t {
          if (predicate == "eq")
            return lhs == rhs;
          if (predicate == "ne")
            return lhs != rhs;
          if (!operandWidth)
            return false;
          llvm::APInt left(*operandWidth, lhs), right(*operandWidth, rhs);
          return llvm::StringSwitch<bool>(predicate)
              .Case("ult", left.ult(right))
              .Case("ule", left.ule(right))
              .Case("ugt", left.ugt(right))
              .Case("uge", left.uge(right))
              .Case("slt", left.slt(right))
              .Case("sle", left.sle(right))
              .Case("sgt", left.sgt(right))
              .Case("sge", left.sge(right))
              .Default(false);
        });
    return result.kind == ValueConstraintKind::Unknown
               ? ValueConstraint::closedInterval(0, 1)
               : result;
  }
  auto binary = [&](auto &&fn) {
    return evaluateBinary(operandConstraint(operands, 0),
                          operandConstraint(operands, 1), fn);
  };
  auto boundedBinary = [&](auto &&fn) {
    ValueConstraint result = binary(std::forward<decltype(fn)>(fn));
    return result.kind == ValueConstraintKind::Unknown
               ? defaultConstraint(resultType)
               : result;
  };
  if (isa<ac::VarAddOp>(operation))
    return boundedBinary([&](uint64_t lhs, uint64_t rhs) {
      return (lhs + rhs) & mask;
    });
  if (isa<ac::VarSubOp>(operation))
    return boundedBinary([&](uint64_t lhs, uint64_t rhs) {
      return (lhs - rhs) & mask;
    });
  if (isa<ac::VarMulOp>(operation))
    return boundedBinary([&](uint64_t lhs, uint64_t rhs) {
      return (lhs * rhs) & mask;
    });
  if (isa<ac::VarAndOp>(operation)) {
    ValueConstraint left = operandConstraint(operands, 0);
    ValueConstraint right = operandConstraint(operands, 1);
    ValueConstraint exact = evaluateBinary(left, right,
                                           [](uint64_t lhs, uint64_t rhs) {
                                             return lhs & rhs;
                                           });
    if (exact.kind != ValueConstraintKind::Unknown)
      return exact;
    if (right.kind == ValueConstraintKind::Constant)
      return ValueConstraint::closedInterval(0, right.values.front() & mask);
    if (left.kind == ValueConstraintKind::Constant)
      return ValueConstraint::closedInterval(0, left.values.front() & mask);
    return defaultConstraint(resultType);
  }
  if (isa<ac::VarOrOp>(operation))
    return boundedBinary(
        [](uint64_t lhs, uint64_t rhs) { return lhs | rhs; });
  if (isa<ac::VarXorOp>(operation))
    return boundedBinary(
        [](uint64_t lhs, uint64_t rhs) { return lhs ^ rhs; });
  if (isa<ac::VarShlOp>(operation))
    return boundedBinary([&](uint64_t lhs, uint64_t rhs) {
      return rhs >= *width ? uint64_t{0} : (lhs << rhs) & mask;
    });
  if (isa<ac::VarShrOp>(operation))
    return boundedBinary([&](uint64_t lhs, uint64_t rhs) {
      return rhs >= *width ? uint64_t{0} : lhs >> rhs;
    });
  if (isa<ac::VarNotOp>(operation)) {
    ValueConstraint result = evaluateUnary(
        operandConstraint(operands, 0),
        [&](uint64_t value) { return (~value) & mask; });
    return result.kind == ValueConstraintKind::Unknown
               ? defaultConstraint(resultType)
               : result;
  }
  if (auto extract = dyn_cast<ac::VarExtractOp>(operation)) {
    ValueConstraint result = evaluateUnary(
        operandConstraint(operands, 0), [&](uint64_t value) {
      return (value >> extract.getLsb()) & mask;
    });
    return result.kind == ValueConstraintKind::Unknown
               ? defaultConstraint(resultType)
               : result;
  }
  if (auto concat = dyn_cast<ac::VarConcatOp>(operation)) {
    ValueConstraint accumulated = ValueConstraint::constant(0);
    for (auto [index, input] : llvm::enumerate(concat.getInputs())) {
      unsigned inputWidth = *integerWidth(input.getType());
      ValueConstraint next = operandConstraint(operands, index);
      accumulated = evaluateBinary(
          accumulated, next, [&](uint64_t lhs, uint64_t rhs) {
            return inputWidth == 64 ? rhs : (lhs << inputWidth) | rhs;
          });
    }
    return accumulated.kind == ValueConstraintKind::Unknown
               ? defaultConstraint(resultType)
               : accumulated;
  }
  if (auto insert = dyn_cast<ac::VarInsertOp>(operation)) {
    unsigned insertedWidth = *integerWidth(insert.getValue().getType());
    uint64_t insertedMask = widthMask(insertedWidth) << insert.getLsb();
    return boundedBinary([&](uint64_t base, uint64_t value) {
      return (base & ~insertedMask) |
             ((value << insert.getLsb()) & insertedMask);
    });
  }
  if (auto popcount = dyn_cast<ac::VarPopcountOp>(operation)) {
    ValueConstraint result = evaluateUnary(
        operandConstraint(operands, 0),
        [](uint64_t value) { return llvm::popcount(value); });
    return result.kind == ValueConstraintKind::Unknown
               ? ValueConstraint::closedInterval(
                     0, *integerWidth(popcount.getIn().getType()))
               : result;
  }
  if (auto zeros = dyn_cast<ac::VarCountZerosOp>(operation)) {
    unsigned inputWidth = *integerWidth(zeros.getIn().getType());
    ValueConstraint result = evaluateUnary(
        operandConstraint(operands, 0), [&](uint64_t value) {
      if (value == 0)
        return static_cast<uint64_t>(inputWidth);
      return zeros.getDirection() == "leading"
                 ? static_cast<uint64_t>(llvm::countl_zero(value) -
                                         (64 - inputWidth))
                 : static_cast<uint64_t>(llvm::countr_zero(value));
        });
    return result.kind == ValueConstraintKind::Unknown
               ? ValueConstraint::closedInterval(0, inputWidth)
               : result;
  }
  if (auto priority = dyn_cast<ac::VarPriorityEncodeOp>(operation)) {
    if (resultIndex == 1)
      return ValueConstraint::closedInterval(0, 1);
    unsigned inputWidth = *integerWidth(priority.getIn().getType());
    return ValueConstraint::closedInterval(0, inputWidth - 1);
  }
  if (auto choose = dyn_cast<ac::VarChooseOp>(operation)) {
    if (resultIndex == 1)
      return ValueConstraint::closedInterval(0, 1);
    if (auto maskWidth = integerWidth(choose.getMask().getType()))
      return ValueConstraint::closedInterval(0, *maskWidth - 1);
  }
  if (auto choose = dyn_cast<ac::TableChooseOp>(operation)) {
    if (resultIndex == 1)
      return ValueConstraint::closedInterval(0, 1);
    if (auto maskWidth = integerWidth(choose.getMask().getType()))
      return ValueConstraint::closedInterval(0, *maskWidth - 1);
  }
  return defaultConstraint(resultType);
}

class ValueConstraintPropagation final
    : public dataflow::SparseForwardDataFlowAnalysis<ValueConstraintLattice> {
public:
  using SparseForwardDataFlowAnalysis::SparseForwardDataFlowAnalysis;

  LogicalResult
  visitOperation(Operation *operation,
                 ArrayRef<const ValueConstraintLattice *> operands,
                 ArrayRef<ValueConstraintLattice *> results) override {
    for (auto [index, lattice] : llvm::enumerate(results))
      propagateIfChanged(
          lattice,
          lattice->join(ConstraintLatticeValue{
              inferConstraint(operation, index, operands)}));
    return success();
  }

  void setToEntryState(ValueConstraintLattice *lattice) override {
    propagateIfChanged(lattice,
                       lattice->join(ConstraintLatticeValue{
                           defaultConstraint(lattice->getAnchor().getType())}));
  }
};

template <typename Enum> Enum joinEnum(Enum left, Enum right) {
  if (left == Enum::Unknown)
    return right;
  if (right == Enum::Unknown || left == right)
    return left;
  return static_cast<Enum>(
      std::max(static_cast<unsigned>(left), static_cast<unsigned>(right)));
}

std::string lexicalOwner(Operation *operation) {
  SmallVector<StringRef> names;
  for (Operation *owner = operation; owner; owner = owner->getParentOp())
    if (auto scope = dyn_cast<ac::ScopeOp>(owner))
      names.push_back(scope.getSymName());
  std::reverse(names.begin(), names.end());
  std::string path = "/";
  for (auto [index, name] : llvm::enumerate(names)) {
    if (index)
      path.push_back('/');
    path.append(name);
  }
  return path;
}

VariableProperties temporaryValue(Value value) {
  Operation *definition = value.getDefiningOp();
  Operation *owner =
      definition ? definition : value.getParentBlock()->getParentOp();
  return {VariableLifetime::Temporary, VariableUpdate::Immutable,
          lexicalOwner(owner)};
}

llvm::SmallVector<std::string> completeStateFields(Operation *anchor,
                                                   Type entryType) {
  auto structure = dyn_cast<ac::StructType>(entryType);
  if (!structure)
    return {"$entry"};
  Operation *declaration =
      SymbolTable::lookupNearestSymbolFrom(anchor, structure.getName());
  auto fields = declaration ? declaration->getAttrOfType<ArrayAttr>("fields")
                            : ArrayAttr();
  llvm::SmallVector<std::string> result;
  if (!fields)
    return result;
  for (Attribute rawField : fields) {
    auto field = dyn_cast<DictionaryAttr>(rawField);
    auto name = field ? field.getAs<StringAttr>("name") : StringAttr();
    if (!name)
      return {};
    result.push_back(name.getValue().str());
  }
  return result;
}

class VariablePropertyPropagation final
    : public dataflow::SparseForwardDataFlowAnalysis<
          VariablePropertiesLattice> {
public:
  using SparseForwardDataFlowAnalysis::SparseForwardDataFlowAnalysis;

  LogicalResult
  visitOperation(Operation *operation,
                 ArrayRef<const VariablePropertiesLattice *> operands,
                 ArrayRef<VariablePropertiesLattice *> results) override {
    (void)operands;
    for (auto [value, lattice] :
         llvm::zip_equal(operation->getResults(), results)) {
      VariableProperties properties = temporaryValue(value);
      if (isa<ac::VarConstantOp>(operation))
        properties.lifetime = VariableLifetime::Static;
      propagateIfChanged(lattice, lattice->join(properties));
    }
    return success();
  }

  void setToEntryState(VariablePropertiesLattice *lattice) override {
    propagateIfChanged(lattice,
                       lattice->join(temporaryValue(lattice->getAnchor())));
  }
};

} // namespace

ValueConstraint ValueConstraint::unknown() { return {}; }

ValueConstraint ValueConstraint::constant(uint64_t value) {
  ValueConstraint result;
  result.kind = ValueConstraintKind::Constant;
  result.values.push_back(value);
  result.lower = value;
  result.upper = value;
  return result;
}

ValueConstraint ValueConstraint::finiteSet(ArrayRef<uint64_t> rawValues) {
  if (rawValues.empty())
    return unknown();
  SmallVector<uint64_t, 8> normalized(rawValues);
  llvm::sort(normalized);
  normalized.erase(std::unique(normalized.begin(), normalized.end()),
                   normalized.end());
  if (normalized.size() == 1)
    return constant(normalized.front());
  if (normalized.size() > 64)
    return closedInterval(normalized.front(), normalized.back());
  ValueConstraint result;
  result.kind = ValueConstraintKind::FiniteSet;
  result.values = std::move(normalized);
  result.lower = result.values.front();
  result.upper = result.values.back();
  return result;
}

ValueConstraint ValueConstraint::closedInterval(uint64_t requestedLower,
                                                uint64_t requestedUpper) {
  if (requestedLower > requestedUpper)
    return unknown();
  if (requestedLower == requestedUpper)
    return constant(requestedLower);
  ValueConstraint result;
  result.kind = ValueConstraintKind::ClosedInterval;
  result.lower = requestedLower;
  result.upper = requestedUpper;
  return result;
}

ValueConstraint ValueConstraint::join(const ValueConstraint &left,
                                      const ValueConstraint &right) {
  if (left.kind == ValueConstraintKind::Unknown ||
      right.kind == ValueConstraintKind::Unknown)
    return unknown();
  // ClosedInterval is less precise than every Constant/FiniteSet it contains.
  // Once a join reaches the interval representation it must never refine back
  // to a finite set, even when the interval happens to contain <= 64 values.
  // Otherwise join(interval, interval) changes representation and violates the
  // idempotence/absorption contract required by MLIR's sparse lattice.
  if (left.kind == ValueConstraintKind::ClosedInterval ||
      right.kind == ValueConstraintKind::ClosedInterval)
    return closedInterval(std::min(left.lower, right.lower),
                          std::max(left.upper, right.upper));

  SmallVector<uint64_t, 8> values(left.values);
  values.append(right.values);
  llvm::sort(values);
  values.erase(std::unique(values.begin(), values.end()), values.end());
  if (values.size() <= 64)
    return finiteSet(values);
  return closedInterval(std::min(left.lower, right.lower),
                        std::max(left.upper, right.upper));
}

bool ValueConstraint::provesWithin(uint64_t requestedLower,
                                   uint64_t requestedUpper) const {
  if (kind == ValueConstraintKind::Unknown || requestedLower > requestedUpper)
    return false;
  return lower >= requestedLower && upper <= requestedUpper;
}

void ValueConstraint::print(llvm::raw_ostream &os) const {
  switch (kind) {
  case ValueConstraintKind::Unknown:
    os << "unknown";
    return;
  case ValueConstraintKind::Constant:
    os << "constant(" << values.front() << ')';
    return;
  case ValueConstraintKind::FiniteSet:
    os << "set{";
    llvm::interleaveComma(values, os);
    os << '}';
    return;
  case ValueConstraintKind::ClosedInterval:
    os << "interval[" << lower << ',' << upper << ']';
    return;
  }
}

VariableProperties VariableProperties::join(const VariableProperties &left,
                                            const VariableProperties &right) {
  return {
      joinEnum(left.lifetime, right.lifetime),
      joinEnum(left.update, right.update),
      left.ownerPath == right.ownerPath
          ? left.ownerPath
          : (left.ownerPath.empty()
                 ? right.ownerPath
                 : (right.ownerPath.empty() ? left.ownerPath : std::string()))};
}

void VariableProperties::print(llvm::raw_ostream &os) const {
  os << "lifetime=" << static_cast<unsigned>(lifetime)
     << ",update=" << static_cast<unsigned>(update) << ",owner=" << ownerPath;
}

struct ACDataFlowAnalyzer::Impl {
  explicit Impl(Operation *root)
      : root(root), frameworkSolver(DataFlowConfig().setInterprocedural(true)) {
    dataflow::loadBaselineAnalyses(frameworkSolver);
    frameworkSolver.load<VariablePropertyPropagation>();
    frameworkSolver.load<ValueConstraintPropagation>();
  }

  Operation *root;
  // Keep the MLIR solver behind the AC-specific analyzer API. Passes consume
  // ACDataFlowAnalyzer rather than depending on the framework solver directly.
  DataFlowSolver frameworkSolver;
};

ACDataFlowAnalyzer::ACDataFlowAnalyzer(Operation *root)
    : impl(std::make_unique<Impl>(root)) {}

ACDataFlowAnalyzer::~ACDataFlowAnalyzer() = default;

LogicalResult ACDataFlowAnalyzer::run() {
  return impl->frameworkSolver.initializeAndRun(impl->root);
}

VariableProperties ACDataFlowAnalyzer::lookup(Value value) const {
  const auto *lattice =
      impl->frameworkSolver.lookupState<VariablePropertiesLattice>(value);
  return lattice ? lattice->getValue() : VariableProperties{};
}

ValueConstraint ACDataFlowAnalyzer::lookupConstraint(Value value) const {
  const auto *lattice =
      impl->frameworkSolver.lookupState<ValueConstraintLattice>(value);
  if (!lattice || !lattice->getValue().constraint)
    return ValueConstraint::unknown();
  return *lattice->getValue().constraint;
}

bool ACDataFlowAnalyzer::provesWithin(Value value, uint64_t lower,
                                     uint64_t upper) const {
  return lookupConstraint(value).provesWithin(lower, upper);
}

VariableProperties
ACDataFlowAnalyzer::lookupOwnedState(Operation *operation) const {
  if (auto variable = dyn_cast<ac::VarDeclOp>(operation))
    return {VariableLifetime::Persistent, VariableUpdate::Assignable,
            variable.getOwner().str()};
  if (auto table = dyn_cast<ac::TableOp>(operation))
    return {VariableLifetime::Persistent, VariableUpdate::Assignable,
            table.getOwner().str()};
  if (isa<ac::MemoryInstanceOp, ac::QueueOp, ac::EventQueueOp, ac::ResourceOp,
          ac::SlotOp>(operation))
    return {VariableLifetime::Persistent, VariableUpdate::Assignable,
            lexicalOwner(operation)};
  return {};
}

llvm::SmallVector<StateAccessFootprint>
ACDataFlowAnalyzer::stateFootprints(Operation *scope) const {
  llvm::SmallVector<StateAccessFootprint> footprints;
  scope->walk([&](Operation *operation) {
    Value index;
    StateAccessFootprint footprint;
    if (auto read = dyn_cast<ac::TableGetOp>(operation)) {
      index = read.getIndex();
      footprint.resource = read.getTable().str();
      footprint.access = "read";
    } else if (auto match = dyn_cast<ac::TableMatchOp>(operation)) {
      footprint.resource = match.getTable().str();
      footprint.access = "read";
      footprint.indexKind = "all";
    } else if (auto choose = dyn_cast<ac::TableChooseOp>(operation)) {
      footprint.resource = choose.getTable().str();
      footprint.access = "read";
      footprint.indexKind = "all";
    } else if (auto write = dyn_cast<ac::TableProposeOp>(operation)) {
      index = write.getIndex();
      footprint.resource = write.getTable().str();
      footprint.access = write.getMode().str();
      footprint.present = write.getWhen();
      for (Attribute rawField : write.getWriteFields())
        footprint.fields.push_back(cast<StringAttr>(rawField).getValue().str());
    } else {
      return;
    }
    if (!index) {
      footprints.push_back(std::move(footprint));
      return;
    }
    VariableProperties indexProperties = lookup(index);
    if ((indexProperties.lifetime != VariableLifetime::Static &&
         indexProperties.lifetime != VariableLifetime::Temporary) ||
        indexProperties.update != VariableUpdate::Immutable)
      footprint.indexKind = "unknown";
    else
      footprint.indexKind =
          index.getDefiningOp<ac::VarConstantOp>() ? "static" : "dynamic";
    footprints.push_back(std::move(footprint));
  });
  return footprints;
}

llvm::SmallVector<StateSnapshotFootprint>
ACDataFlowAnalyzer::stateSnapshots(Operation *scope) const {
  Value candidate;
  scope->walk([&](Operation *operation) {
    if (auto condition = dyn_cast<ac::RuleConditionOp>(operation))
      candidate = condition.getCondition();
    else if (auto condition = dyn_cast<ac::FiringConditionOp>(operation))
      candidate = condition.getCondition();
  });
  llvm::SmallVector<Value> predicates;
  llvm::DenseSet<Value> seenPredicates;
  if (candidate) {
    predicates.push_back(candidate);
    seenPredicates.insert(candidate);
  }
  scope->walk([&](Operation *operation) {
    Value predicate;
    if (auto proposal = dyn_cast<ac::TableProposeOp>(operation))
      predicate = proposal.getWhen();
    else if (auto output = dyn_cast<ac::RuleOutputOp>(operation))
      predicate = output.getWhen();
    else if (auto output = dyn_cast<ac::FiringOutputOp>(operation))
      predicate = output.getWhen();
    if (predicate && seenPredicates.insert(predicate).second)
      predicates.push_back(predicate);
  });

  llvm::SmallVector<StateSnapshotFootprint> snapshots;
  auto insideScope = [&](Operation *operation) {
    for (Operation *owner = operation; owner; owner = owner->getParentOp())
      if (owner == scope)
        return true;
    return false;
  };
  auto addSnapshot = [&](StringRef resource, Value index, Value source,
                         StringRef indexKind, Value predicate,
                         llvm::SmallVector<std::string> fields) {
    if (indexKind == "all" && !fields.empty()) {
      llvm::erase_if(snapshots, [&](const StateSnapshotFootprint &candidate) {
        return candidate.resource == resource &&
               candidate.predicate == predicate &&
               llvm::ArrayRef<std::string>(candidate.fields) ==
                   llvm::ArrayRef<std::string>(fields);
      });
    } else if (llvm::any_of(snapshots, [&](const auto &candidate) {
                 return candidate.resource == resource &&
                        candidate.predicate == predicate &&
                        candidate.indexKind == "all" &&
                        llvm::ArrayRef<std::string>(candidate.fields) ==
                            llvm::ArrayRef<std::string>(fields);
               })) {
      return;
    }
    auto existing = llvm::find_if(snapshots, [&](const auto &candidate) {
      return candidate.resource == resource && candidate.index == index &&
             candidate.source == source && candidate.indexKind == indexKind &&
             candidate.predicate == predicate;
    });
    if (existing != snapshots.end()) {
      for (const std::string &field : fields)
        if (!llvm::is_contained(existing->fields, field))
          existing->fields.push_back(field);
      return;
    }
    snapshots.push_back(
        {resource.str(), index, source, indexKind.str(), predicate,
         std::vector<std::string>(fields.begin(), fields.end())});
  };
  auto classifyIndex = [&](Value index) -> StringRef {
    VariableProperties properties = lookup(index);
    if ((properties.lifetime != VariableLifetime::Static &&
         properties.lifetime != VariableLifetime::Temporary) ||
        properties.update != VariableUpdate::Immutable)
      return "unknown";
    return index.getDefiningOp<ac::VarConstantOp>() ? "static" : "dynamic";
  };

  for (Value predicate : predicates) {
    std::function<void(Value, Value, llvm::SmallVector<std::string>)> collect =
        [&](Value value, Value setSource,
            llvm::SmallVector<std::string> requestedFields) {
          if (!value)
            return;
          Operation *definition = value.getDefiningOp();
          if (!definition || !insideScope(definition) ||
              isa<ac::StateSnapshotOp, ac::StateSnapshotSetOp>(definition))
            return;
          if (auto read = dyn_cast<ac::TableGetOp>(definition)) {
            auto fields =
                requestedFields.empty()
                    ? completeStateFields(
                          read, cast<ac::VarType>(read.getResult().getType())
                                    .getElementType())
                    : std::move(requestedFields);
            if (read->getBlock() != &scope->getRegion(0).front()) {
              if (setSource)
                addSnapshot(read.getTable(), {}, setSource, "set", predicate,
                            std::move(fields));
              else
                addSnapshot(read.getTable(), {}, {}, "all", predicate,
                            std::move(fields));
            } else {
              addSnapshot(read.getTable(), read.getIndex(), {},
                          classifyIndex(read.getIndex()), predicate,
                          std::move(fields));
            }
            collect(read.getIndex(), setSource, {});
            return;
          }
          if (auto get = dyn_cast<ac::VarGetOp>(definition)) {
            collect(get.getRecord(), setSource, {get.getField().str()});
            return;
          }
          if (auto match = dyn_cast<ac::TableMatchOp>(definition)) {
            Type entryType =
                cast<ac::VarType>(
                    match.getPredicate().front().getArgument(0).getType())
                    .getElementType();
            addSnapshot(match.getTable(), {}, {}, "all", predicate,
                        completeStateFields(match, entryType));
            for (Value operand : match->getOperands())
              collect(operand, {}, {});
            for (Block &block : match.getPredicate())
              for (Value operand : block.getTerminator()->getOperands())
                collect(operand, match.getMask(), {});
            return;
          }
          if (auto choose = dyn_cast<ac::TableChooseOp>(definition)) {
            if (!choose.getKey().empty()) {
              Type entryType =
                  cast<ac::VarType>(
                      choose.getKey().front().getArgument(0).getType())
                      .getElementType();
              addSnapshot(choose.getTable(), {}, {}, "all", predicate,
                          completeStateFields(choose, entryType));
            }
            collect(choose.getMask(), {}, {});
            for (Block &block : choose.getKey())
              for (Value operand : block.getTerminator()->getOperands())
                collect(operand, choose.getIndex(), {});
            return;
          }
          for (Value operand : definition->getOperands())
            collect(operand, setSource, {});
          for (Region &region : definition->getRegions())
            for (Block &block : region)
              for (Value operand : block.getTerminator()->getOperands())
                collect(operand, setSource, {});
        };
    collect(predicate, {}, {});
  }
  return snapshots;
}

} // namespace acir
