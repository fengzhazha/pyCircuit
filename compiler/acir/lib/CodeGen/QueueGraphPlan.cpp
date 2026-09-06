#include "acir/CodeGen/QueueGraphPlan.h"

#include "acir/Analysis/ModelAnalysis.h"
#include "acir/Analysis/VariableAnalysis.h"
#include "acir/Bindings/Binding.h"
#include "acir/CodeGen/Manifest.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACIR/ACIRTypes.h"

#include "mlir/IR/Operation.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <limits>
#include <optional>
#include <set>
#include <system_error>
#include <tuple>

namespace acir::codegen {
namespace {

llvm::Error planError(const llvm::Twine &message) {
  return llvm::createStringError(
      std::make_error_code(std::errc::invalid_argument),
      "ACLOWER-QUEUE-PLAN: " + message);
}

llvm::Expected<uint64_t> addBitWidths(uint64_t left, uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left)
    return planError("value bit width overflows uint64_t");
  return left + right;
}

llvm::Expected<uint64_t> multiplyBitWidths(uint64_t width, uint64_t count) {
  if (count != 0 && width > std::numeric_limits<uint64_t>::max() / count)
    return planError("value bit width overflows uint64_t");
  return width * count;
}

std::string printType(mlir::Type type) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  stream << type;
  return result;
}

mlir::Operation *lookupTypeDeclaration(mlir::Operation *from,
                                       mlir::SymbolRefAttr name) {
  if (name.getNestedReferences().size() != 1)
    return mlir::SymbolTable::lookupNearestSymbolFrom(from, name);

  mlir::Operation *scope = nullptr;
  if (auto enclosing = from->getParentOfType<ac::TypeScopeOp>();
      enclosing && enclosing.getSymNameAttr() == name.getRootReference())
    scope = enclosing;
  if (!scope) {
    auto root = mlir::FlatSymbolRefAttr::get(name.getRootReference());
    scope = mlir::SymbolTable::lookupNearestSymbolFrom(from, root);
    if (!scope)
      if (auto module = from->getParentOfType<mlir::ModuleOp>())
        scope = mlir::SymbolTable::lookupSymbolIn(module, root);
  }
  if (!mlir::isa_and_nonnull<ac::TypeScopeOp>(scope))
    return nullptr;
  return mlir::SymbolTable::lookupSymbolIn(scope, name.getLeafReference());
}

llvm::Expected<uint64_t>
mlirValueBitWidth(mlir::Operation *from, mlir::Type type,
                  llvm::SmallVectorImpl<mlir::Type> &active) {
  if (auto integer = mlir::dyn_cast<mlir::IntegerType>(type))
    return integer.getWidth();
  if (llvm::is_contained(active, type))
    return planError("recursive value type has no finite bit width");
  active.push_back(type);
  auto finish =
      [&](llvm::Expected<uint64_t> result) -> llvm::Expected<uint64_t> {
    active.pop_back();
    return result;
  };
  if (auto enumeration = mlir::dyn_cast<ac::EnumType>(type)) {
    auto declaration = mlir::dyn_cast_or_null<ac::EnumOp>(
        lookupTypeDeclaration(from, enumeration.getName()));
    if (!declaration)
      return finish(planError("enum type declaration is unresolved"));
    return finish(std::max<uint64_t>(
        1, llvm::Log2_64_Ceil(declaration.getEnumerants().size())));
  }
  if (auto structure = mlir::dyn_cast<ac::StructType>(type)) {
    auto declaration = mlir::dyn_cast_or_null<ac::StructOp>(
        lookupTypeDeclaration(from, structure.getName()));
    if (!declaration)
      return finish(planError("struct type declaration is unresolved"));
    uint64_t total = 0;
    for (mlir::Attribute rawField : declaration.getFields()) {
      auto field = mlir::dyn_cast<mlir::DictionaryAttr>(rawField);
      auto fieldType =
          field ? field.getAs<mlir::TypeAttr>("type") : mlir::TypeAttr();
      if (!fieldType)
        return finish(planError("struct field type is malformed"));
      auto width = mlirValueBitWidth(from, fieldType.getValue(), active);
      if (!width)
        return finish(width.takeError());
      auto next = addBitWidths(total, *width);
      if (!next)
        return finish(next.takeError());
      total = *next;
    }
    return finish(total);
  }
  if (auto tuple = mlir::dyn_cast<mlir::TupleType>(type)) {
    uint64_t total = 0;
    for (mlir::Type element : tuple.getTypes()) {
      auto width = mlirValueBitWidth(from, element, active);
      if (!width)
        return finish(width.takeError());
      auto next = addBitWidths(total, *width);
      if (!next)
        return finish(next.takeError());
      total = *next;
    }
    return finish(total);
  }
  if (auto array = mlir::dyn_cast<ac::ValueArrayType>(type)) {
    auto width = mlirValueBitWidth(from, array.getElementType(), active);
    if (!width)
      return finish(width.takeError());
    return finish(
        multiplyBitWidths(*width, static_cast<uint64_t>(array.getLength())));
  }
  return finish(planError("QueueGraph value type has no bit-width model"));
}

std::optional<unsigned> integerWidth(llvm::StringRef type) {
  if (!type.consume_front("i"))
    return std::nullopt;
  unsigned width = 0;
  if (type.empty() || type.getAsInteger(10, width) || width == 0)
    return std::nullopt;
  return width;
}

std::string exactWidthHex(uint64_t value, unsigned width) {
  std::string result = "0x";
  llvm::raw_string_ostream stream(result);
  stream << llvm::format_hex_no_prefix(value, (width + 3) / 4);
  return result;
}

std::optional<uint64_t> parseExactWidthHex(llvm::StringRef text,
                                           unsigned width) {
  if (!text.starts_with("0x") || text.size() != 2 + (width + 3) / 4)
    return std::nullopt;
  uint64_t value = 0;
  if (text.drop_front(2).getAsInteger(16, value) ||
      exactWidthHex(value, width) != text)
    return std::nullopt;
  return value;
}

ValueConstraint planTypeConstraint(llvm::StringRef type) {
  auto width = integerWidth(type);
  if (!width || *width > 64)
    return ValueConstraint::unknown();
  const uint64_t upper =
      *width == 64 ? std::numeric_limits<uint64_t>::max()
                   : (uint64_t{1} << *width) - 1;
  return ValueConstraint::closedInterval(0, upper);
}

std::optional<uint64_t> planConstantValue(llvm::StringRef literal,
                                          llvm::StringRef type) {
  literal = literal.split(':').first.trim();
  if (literal == "true")
    return 1;
  if (literal == "false")
    return 0;
  auto width = integerWidth(type);
  if (!width || *width > 64)
    return std::nullopt;
  const uint64_t mask =
      *width == 64 ? std::numeric_limits<uint64_t>::max()
                   : (uint64_t{1} << *width) - 1;
  if (literal.starts_with('-')) {
    int64_t value = 0;
    if (literal.getAsInteger(10, value))
      return std::nullopt;
    return static_cast<uint64_t>(value) & mask;
  }
  uint64_t value = 0;
  if (literal.getAsInteger(10, value))
    return std::nullopt;
  return value & mask;
}

ValueConstraint inferPlanConstraint(
    const QueueExpressionPlan &expression,
    const llvm::StringMap<ValueConstraint> &constraints,
    const llvm::StringMap<std::string> &types,
    const llvm::StringMap<const TablePlan *> &tables) {
  ValueConstraint fallback = planTypeConstraint(expression.type);
  auto operand = [&](size_t index) {
    auto found = index < expression.operands.size()
                     ? constraints.find(expression.operands[index])
                     : constraints.end();
    return found == constraints.end() ? ValueConstraint::unknown()
                                      : found->getValue();
  };
  if (expression.kind == "constant") {
    if (auto value = planConstantValue(expression.literal, expression.type))
      return ValueConstraint::constant(*value);
    return ValueConstraint::unknown();
  }
  if (expression.kind == "value_select") {
    ValueConstraint condition = operand(0);
    if (condition.kind == ValueConstraintKind::Constant)
      return operand(condition.values.front() == 0 ? 2 : 1);
    return ValueConstraint::join(operand(1), operand(2));
  }
  if (expression.kind == "masked_match") {
    auto type = expression.operands.empty()
                    ? types.end()
                    : types.find(expression.operands.front());
    auto width = type == types.end() ? std::optional<unsigned>()
                                     : integerWidth(type->getValue());
    auto mask = width ? parseExactWidthHex(expression.mask, *width)
                      : std::optional<uint64_t>();
    auto expected = width ? parseExactWidthHex(expression.value, *width)
                          : std::optional<uint64_t>();
    if (!mask || !expected)
      return ValueConstraint::unknown();
    if (*mask == 0)
      return ValueConstraint::constant(1);
    ValueConstraint input = operand(0);
    if (input.kind == ValueConstraintKind::Constant)
      return ValueConstraint::constant(
          (input.values.front() & *mask) == *expected);
    return ValueConstraint::closedInterval(0, 1);
  }
  if (expression.kind == "cmp" || expression.kind == "priority_valid" ||
      expression.kind == "table_choose_valid" ||
      expression.kind == "table_selection_valid_ref")
    return ValueConstraint::closedInterval(0, 1);
  if (expression.kind == "priority_index") {
    auto type = expression.operands.empty()
                    ? types.end()
                    : types.find(expression.operands.front());
    auto width = type == types.end() ? std::optional<unsigned>()
                                     : integerWidth(type->getValue());
    return width ? ValueConstraint::closedInterval(0, *width - 1)
                 : ValueConstraint::unknown();
  }
  if (expression.kind == "table_choose_index" ||
      expression.kind == "table_selection_index_ref") {
    auto table = tables.find(expression.table);
    return table != tables.end() && table->getValue()->entries != 0
               ? ValueConstraint::closedInterval(
                     0, table->getValue()->entries - 1)
               : ValueConstraint::unknown();
  }

  ValueConstraint left = operand(0);
  ValueConstraint right = operand(1);
  const bool constantOperands =
      left.kind == ValueConstraintKind::Constant &&
      right.kind == ValueConstraintKind::Constant;
  auto resultWidth = integerWidth(expression.type);
  const uint64_t mask =
      !resultWidth || *resultWidth == 64
          ? std::numeric_limits<uint64_t>::max()
          : (uint64_t{1} << *resultWidth) - 1;
  if (constantOperands) {
    uint64_t lhs = left.values.front();
    uint64_t rhs = right.values.front();
    if (expression.kind == "add")
      return ValueConstraint::constant((lhs + rhs) & mask);
    if (expression.kind == "sub")
      return ValueConstraint::constant((lhs - rhs) & mask);
    if (expression.kind == "mul")
      return ValueConstraint::constant((lhs * rhs) & mask);
    if (expression.kind == "and")
      return ValueConstraint::constant(lhs & rhs);
    if (expression.kind == "or")
      return ValueConstraint::constant(lhs | rhs);
    if (expression.kind == "xor")
      return ValueConstraint::constant(lhs ^ rhs);
    if (expression.kind == "shl")
      return ValueConstraint::constant(
          !resultWidth || rhs >= *resultWidth ? 0 : (lhs << rhs) & mask);
    if (expression.kind == "shr")
      return ValueConstraint::constant(
          !resultWidth || rhs >= *resultWidth ? 0 : lhs >> rhs);
  }
  if (expression.kind == "and") {
    if (left.kind == ValueConstraintKind::Constant)
      return ValueConstraint::closedInterval(0, left.values.front() & mask);
    if (right.kind == ValueConstraintKind::Constant)
      return ValueConstraint::closedInterval(0, right.values.front() & mask);
  }
  if (expression.kind == "not" &&
      left.kind == ValueConstraintKind::Constant)
    return ValueConstraint::constant((~left.values.front()) & mask);
  return fallback;
}

std::string printAttribute(mlir::Attribute attribute) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  stream << attribute;
  return result;
}

std::string printRegion(mlir::Region &region) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  region.getParentOp()->print(stream);
  return result;
}

std::string scopePath(llvm::ArrayRef<std::string> scope) {
  std::string result;
  for (llvm::StringRef part : scope) {
    result.push_back('/');
    result.append(part);
  }
  return result.empty() ? "/" : result;
}

llvm::Expected<std::string>
queueName(mlir::Value value,
          const llvm::DenseMap<mlir::Value, std::string> &names) {
  auto found = names.find(value);
  if (found == names.end())
    return planError("Queue operand has no frozen logical identity");
  return found->second;
}

llvm::Expected<std::vector<std::string>>
queueNames(mlir::ValueRange values,
           const llvm::DenseMap<mlir::Value, std::string> &names) {
  std::vector<std::string> result;
  for (mlir::Value value : values) {
    auto name = queueName(value, names);
    if (!name)
      return name.takeError();
    result.push_back(std::move(*name));
  }
  return result;
}

llvm::Expected<std::vector<std::string>> outputNames(mlir::Operation *op,
                                                     size_t count) {
  std::vector<std::string> result;
  if (count == 1)
    if (auto name = op->getAttrOfType<mlir::StringAttr>("ac.name"))
      result.push_back(name.getValue().str());
  if (result.empty())
    if (auto names = op->getAttrOfType<mlir::ArrayAttr>("ac.output_names"))
      for (mlir::Attribute value : names) {
        auto name = mlir::dyn_cast<mlir::StringAttr>(value);
        if (!name)
          return planError("ac.output_names must contain only strings");
        result.push_back(name.getValue().str());
      }
  if (result.size() != count)
    return planError("Queue-producing op requires exact frozen output names");
  return result;
}

using SharedExpression = std::pair<mlir::Value, QueueExpressionPlan>;

llvm::Error
extractExpressions(mlir::Region &region, QueueBlockPlan &plan,
                   llvm::ArrayRef<SharedExpression> sharedExpressions = {}) {
  mlir::Block &block = region.front();
  llvm::DenseMap<mlir::Value, std::string> values;
  for (const auto &[value, expression] : sharedExpressions) {
    values[value] = expression.result;
    if (llvm::none_of(plan.expressions, [&](const QueueExpressionPlan &item) {
          return item.result == expression.result;
        }))
      plan.expressions.push_back(expression);
  }
  for (auto [index, argument] : llvm::enumerate(block.getArguments()))
    values[argument] = index == 0 ? "item" : "item" + std::to_string(index);
  auto operandNames = [&](mlir::ValueRange operands)
      -> llvm::Expected<std::vector<std::string>> {
    std::vector<std::string> result;
    for (mlir::Value operand : operands) {
      auto found = values.find(operand);
      if (found == values.end())
        return planError("Var expression operand has no local identity");
      result.push_back(found->second);
    }
    return result;
  };
  auto append = [&](mlir::Operation &operation, llvm::StringRef kind,
                    llvm::StringRef field = {}, llvm::StringRef predicate = {},
                    llvm::StringRef literal = {}) -> llvm::Error {
    if (operation.getNumResults() != 1)
      return planError("Var expression must produce exactly one result");
    auto resultType =
        mlir::dyn_cast<ac::VarType>(operation.getResult(0).getType());
    if (!resultType)
      return planError("Var expression result must be ac.var");
    auto operands = operandNames(operation.getOperands());
    if (!operands)
      return operands.takeError();
    std::string result = "v" + std::to_string(plan.expressions.size());
    values[operation.getResult(0)] = result;
    plan.expressions.push_back(
        {std::move(result), kind.str(), printType(resultType.getElementType()),
         std::move(*operands), field.str(), predicate.str(), literal.str()});
    return llvm::Error::success();
  };

  bool sawStructuredYield = false;

  for (mlir::Operation &operation : block) {
    if (auto constant = mlir::dyn_cast<ac::VarConstantOp>(operation)) {
      if (auto error = append(operation, "constant", {}, {},
                              printAttribute(constant.getValueAttr())))
        return error;
      continue;
    }
    if (auto value = mlir::dyn_cast<ac::VarEnumOp>(operation)) {
      auto declaration = mlir::dyn_cast_or_null<ac::EnumOp>(
          mlir::SymbolTable::lookupNearestSymbolFrom(value,
                                                     value.getDeclaration()));
      if (!declaration)
        return planError("enum value declaration is unresolved");
      auto enumerant = llvm::find_if(
          declaration.getEnumerants(), [&](mlir::Attribute candidate) {
            return mlir::cast<mlir::StringAttr>(candidate).getValue() ==
                   value.getEnumerant();
          });
      if (enumerant == declaration.getEnumerants().end())
        return planError("enum value is absent from QueueGraph declaration");
      if (auto error =
              append(operation, "enum_constant", value.getEnumerant(), {},
                     std::to_string(std::distance(
                         declaration.getEnumerants().begin(), enumerant))))
        return error;
      continue;
    }
    if (auto tuple = mlir::dyn_cast<ac::VarTupleOp>(operation)) {
      if (auto error = append(operation, "tuple_create"))
        return error;
      llvm::SmallVector<mlir::Type> active;
      auto width = mlirValueBitWidth(
          tuple,
          mlir::cast<ac::VarType>(tuple.getResult().getType()).getElementType(),
          active);
      if (!width)
        return width.takeError();
      plan.expressions.back().width = *width;
      continue;
    }
    if (auto array = mlir::dyn_cast<ac::VarArrayOp>(operation)) {
      if (auto error = append(operation, "array_create"))
        return error;
      llvm::SmallVector<mlir::Type> active;
      auto width = mlirValueBitWidth(
          array,
          mlir::cast<ac::VarType>(array.getResult().getType()).getElementType(),
          active);
      if (!width)
        return width.takeError();
      plan.expressions.back().width = *width;
      continue;
    }
    if (auto element = mlir::dyn_cast<ac::VarElementOp>(operation)) {
      if (auto error = append(operation, "aggregate_get"))
        return error;
      mlir::Type aggregate =
          mlir::cast<ac::VarType>(element.getAggregate().getType())
              .getElementType();
      uint64_t lsb = 0;
      if (auto tuple = mlir::dyn_cast<mlir::TupleType>(aggregate)) {
        for (mlir::Type trailing :
             tuple.getTypes().drop_front(element.getIndex() + 1)) {
          llvm::SmallVector<mlir::Type> active;
          auto width = mlirValueBitWidth(element, trailing, active);
          if (!width)
            return width.takeError();
          lsb += *width;
        }
      } else if (auto array = mlir::dyn_cast<ac::ValueArrayType>(aggregate)) {
        llvm::SmallVector<mlir::Type> active;
        auto width = mlirValueBitWidth(element, array.getElementType(), active);
        if (!width)
          return width.takeError();
        lsb = *width * (static_cast<uint64_t>(array.getLength()) -
                        static_cast<uint64_t>(element.getIndex()) - 1);
      }
      llvm::SmallVector<mlir::Type> active;
      auto width = mlirValueBitWidth(
          element,
          mlir::cast<ac::VarType>(element.getResult().getType())
              .getElementType(),
          active);
      if (!width)
        return width.takeError();
      plan.expressions.back().lsb = lsb;
      plan.expressions.back().width = *width;
      continue;
    }
    if (mlir::isa<ac::VarAddOp>(operation)) {
      if (auto error = append(operation, "add"))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarSubOp>(operation)) {
      if (auto error = append(operation, "sub"))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarMulOp>(operation)) {
      if (auto error = append(operation, "mul"))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarAndOp>(operation)) {
      if (auto error = append(operation, "and"))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarOrOp>(operation)) {
      if (auto error = append(operation, "or"))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarXorOp>(operation)) {
      if (auto error = append(operation, "xor"))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarShlOp>(operation)) {
      if (auto error = append(operation, "shl"))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarShrOp>(operation)) {
      if (auto error = append(operation, "shr"))
        return error;
      continue;
    }
    if (auto matches = mlir::dyn_cast<ac::VarMatchesOp>(operation)) {
      if (auto error = append(operation, "masked_match"))
        return error;
      unsigned width = mlir::cast<mlir::IntegerType>(
                           mlir::cast<ac::VarType>(matches.getInput().getType())
                               .getElementType())
                           .getWidth();
      plan.expressions.back().mask = exactWidthHex(matches.getMask(), width);
      plan.expressions.back().value = exactWidthHex(matches.getValue(), width);
      continue;
    }
    if (mlir::isa<ac::VarNotOp>(operation)) {
      if (auto error = append(operation, "not"))
        return error;
      continue;
    }
    if (auto priority = mlir::dyn_cast<ac::VarPriorityEncodeOp>(operation)) {
      auto operands = operandNames(priority->getOperands());
      if (!operands)
        return operands.takeError();
      const std::array<std::pair<mlir::Value, llvm::StringRef>, 2> results = {{
          {priority.getIndex(), "priority_index"},
          {priority.getValid(), "priority_valid"},
      }};
      for (auto [resultValue, kind] : results) {
        auto resultType = mlir::dyn_cast<ac::VarType>(resultValue.getType());
        if (!resultType)
          return planError("priority encoder result must be ac.var");
        std::string result = "v" + std::to_string(plan.expressions.size());
        values[resultValue] = result;
        plan.expressions.push_back({std::move(result),
                                    kind.str(),
                                    printType(resultType.getElementType()),
                                    *operands,
                                    {},
                                    priority.getOrder().str(),
                                    {}});
      }
      continue;
    }
    if (mlir::isa<ac::VarPopcountOp>(operation)) {
      if (auto error = append(operation, "popcount"))
        return error;
      continue;
    }
    if (auto count = mlir::dyn_cast<ac::VarCountZerosOp>(operation)) {
      if (auto error =
              append(operation, "count_zeros", {}, count.getDirection()))
        return error;
      continue;
    }
    if (auto compare = mlir::dyn_cast<ac::VarCmpOp>(operation)) {
      if (auto error = append(operation, "cmp", {}, compare.getPredicate()))
        return error;
      continue;
    }
    if (mlir::isa<ac::VarSelectOp>(operation)) {
      if (auto error = append(operation, "value_select"))
        return error;
      continue;
    }
    if (auto extract = mlir::dyn_cast<ac::VarExtractOp>(operation)) {
      if (auto error = append(operation, "bit_extract"))
        return error;
      plan.expressions.back().lsb = static_cast<uint64_t>(extract.getLsb());
      plan.expressions.back().width = static_cast<uint64_t>(extract.getWidth());
      continue;
    }
    if (mlir::isa<ac::VarConcatOp>(operation)) {
      if (auto error = append(operation, "bit_concat"))
        return error;
      continue;
    }
    if (auto insert = mlir::dyn_cast<ac::VarInsertOp>(operation)) {
      if (auto error = append(operation, "bit_insert"))
        return error;
      plan.expressions.back().lsb = static_cast<uint64_t>(insert.getLsb());
      continue;
    }
    if (auto get = mlir::dyn_cast<ac::VarGetOp>(operation)) {
      if (auto error = append(operation, "get", get.getField()))
        return error;
      continue;
    }
    if (auto with = mlir::dyn_cast<ac::VarWithOp>(operation)) {
      if (auto error = append(operation, "with", with.getField()))
        return error;
      continue;
    }
    if (auto get = mlir::dyn_cast<ac::TableGetOp>(operation)) {
      if (auto error = append(operation, "table_get"))
        return error;
      plan.expressions.back().table = get.getTable().str();
      continue;
    }
    if (auto get = mlir::dyn_cast<ac::SlotGetOp>(operation)) {
      const std::string base = "v" + std::to_string(plan.expressions.size());
      const std::array<std::pair<mlir::Value, llvm::StringRef>, 2> results = {{
          {get.getValid(), "slot_get_valid"},
          {get.getValue(), "slot_get_value"},
      }};
      for (auto [resultValue, kind] : results) {
        auto resultType = mlir::cast<ac::VarType>(resultValue.getType());
        std::string result =
            base + (kind == "slot_get_valid" ? "_valid" : "_value");
        values[resultValue] = result;
        QueueExpressionPlan expression{
            result, kind.str(), printType(resultType.getElementType()), {}};
        expression.slot = get.getSlot().str();
        plan.expressions.push_back(std::move(expression));
      }
      continue;
    }
    if (auto match = mlir::dyn_cast<ac::TableMatchOp>(operation)) {
      QueueBlockPlan nested;
      if (auto error = extractExpressions(match.getPredicate(), nested))
        return error;
      auto resultType = mlir::cast<ac::VarType>(match.getMask().getType());
      std::string result = "v" + std::to_string(plan.expressions.size());
      values[match.getMask()] = result;
      QueueExpressionPlan expression{
          result, "table_match", printType(resultType.getElementType()), {}};
      expression.table = match.getTable().str();
      expression.nestedExpressions = std::move(nested.expressions);
      expression.nestedYields = std::move(nested.yields);
      plan.expressions.push_back(std::move(expression));
      continue;
    }
    if (auto choose = mlir::dyn_cast<ac::TableChooseOp>(operation)) {
      auto operands = operandNames(choose->getOperands());
      if (!operands)
        return operands.takeError();
      QueueBlockPlan nested;
      if (!choose.getKey().empty())
        if (auto error = extractExpressions(choose.getKey(), nested))
          return error;
      const std::array<std::pair<mlir::Value, llvm::StringRef>, 2> results = {{
          {choose.getIndex(), "table_choose_index"},
          {choose.getValid(), "table_choose_valid"},
      }};
      for (auto [resultValue, kind] : results) {
        auto resultType = mlir::cast<ac::VarType>(resultValue.getType());
        std::string result = "v" + std::to_string(plan.expressions.size());
        values[resultValue] = result;
        QueueExpressionPlan expression{result, kind.str(),
                                       printType(resultType.getElementType()),
                                       *operands};
        expression.table = choose.getTable().str();
        expression.predicate = choose.getPolicy().str();
        expression.nestedExpressions = nested.expressions;
        expression.nestedYields = nested.yields;
        plan.expressions.push_back(std::move(expression));
      }
      continue;
    }
    if (auto proposal = mlir::dyn_cast<ac::TableProposeOp>(operation)) {
      auto operands = operandNames(proposal->getOperands());
      if (!operands)
        return operands.takeError();
      if (operands->size() != 2 && operands->size() != 3)
        return planError(
            "state proposal must contain index, value, and optional presence");
      StateWritePlan write{proposal.getTable().str(),
                           (*operands)[0],
                           (*operands)[1],
                           operands->size() == 3 ? (*operands)[2] : "",
                           proposal.getMode().str(),
                           {}};
      for (mlir::Attribute field : proposal.getWriteFields())
        write.fields.push_back(
            mlir::cast<mlir::StringAttr>(field).getValue().str());
      plan.stateWrites.push_back(std::move(write));
      if (plan.table.empty()) {
        const StateWritePlan &primary = plan.stateWrites.front();
        plan.table = primary.table;
        plan.tableIndex = primary.index;
        plan.tableValue = primary.value;
        plan.writeMode = primary.mode;
        plan.writeFields = primary.fields;
      }
      continue;
    }
    if (auto snapshot = mlir::dyn_cast<ac::StateSnapshotOp>(operation)) {
      auto operands = operandNames(snapshot->getOperands());
      if (!operands)
        return operands.takeError();
      const bool indexed = static_cast<bool>(snapshot.getIndex());
      if (operands->size() != (indexed ? 2U : 1U))
        return planError("state snapshot operands are malformed");
      llvm::StringRef indexKind =
          snapshot.getIndexKind() == ac::RuleIndexKind::Static
              ? "static"
              : (snapshot.getIndexKind() == ac::RuleIndexKind::Dynamic
                     ? "dynamic"
                     : "all");
      StateReservationPlan reservation{snapshot.getTable().str(),
                                       indexed ? operands->front() : "",
                                       "",
                                       operands->back(),
                                       indexKind.str(),
                                       {}};
      for (mlir::Attribute field : snapshot.getReadFields())
        reservation.fields.push_back(
            mlir::cast<mlir::StringAttr>(field).getValue().str());
      plan.stateReservations.push_back(std::move(reservation));
      continue;
    }
    if (auto snapshotSet = mlir::dyn_cast<ac::StateSnapshotSetOp>(operation)) {
      auto operands = operandNames(snapshotSet->getOperands());
      if (!operands)
        return operands.takeError();
      if (operands->size() != 2)
        return planError("state snapshot-set operands are malformed");
      StateReservationPlan reservation{snapshotSet.getTable().str(),
                                       "",
                                       operands->front(),
                                       operands->back(),
                                       "set",
                                       {}};
      for (mlir::Attribute field : snapshotSet.getReadFields())
        reservation.fields.push_back(
            mlir::cast<mlir::StringAttr>(field).getValue().str());
      plan.stateReservations.push_back(std::move(reservation));
      continue;
    }
    if (auto output = mlir::dyn_cast<ac::FiringOutputOp>(operation)) {
      auto operands = operandNames(output->getOperands());
      if (!operands)
        return operands.takeError();
      if (operands->size() != 2 || output.getOrdinal() < 0)
        return planError("firing output presence is malformed");
      plan.outputPresence.push_back({static_cast<uint64_t>(output.getOrdinal()),
                                     (*operands)[0], (*operands)[1]});
      continue;
    }
    if (auto condition = mlir::dyn_cast<ac::FiringConditionOp>(operation)) {
      auto operands = operandNames(condition->getOperands());
      if (!operands)
        return operands.takeError();
      if (operands->size() != 1 || !plan.guard.empty())
        return planError("firing must contain one closed functional condition");
      plan.guard = operands->front();
      continue;
    }
    llvm::SmallVector<mlir::Value, 2> yielded;
    if (auto yield = mlir::dyn_cast<ac::TransformYieldOp>(operation))
      yielded.append(yield.getValues().begin(), yield.getValues().end());
    else if (auto yield = mlir::dyn_cast<ac::FiringYieldOp>(operation))
      yielded.append(yield.getValues().begin(), yield.getValues().end());
    else if (auto yield = mlir::dyn_cast<ac::RouteYieldOp>(operation))
      yielded.push_back(yield.getSelector());
    else if (auto yield = mlir::dyn_cast<ac::SelectYieldOp>(operation))
      yielded.push_back(yield.getSelector());
    else if (auto yield = mlir::dyn_cast<ac::ReorderYieldOp>(operation))
      yielded.push_back(yield.getKey());
    else if (auto yield = mlir::dyn_cast<ac::DependencyYieldOp>(operation))
      yielded.push_back(yield.getValue());
    else if (auto yield = mlir::dyn_cast<ac::CreditYieldOp>(operation))
      yielded.push_back(yield.getCost());
    else if (auto yield = mlir::dyn_cast<ac::MemoryYieldOp>(operation))
      yielded.push_back(yield.getValue());
    else if (auto yield = mlir::dyn_cast<ac::TableYieldOp>(operation))
      yielded.push_back(yield.getValue());
    else if (auto yield = mlir::dyn_cast<ac::SlotYieldOp>(operation))
      yielded.push_back(yield.getValue());
    else if (auto yield = mlir::dyn_cast<ac::TableMatchYieldOp>(operation))
      yielded.push_back(yield.getValue());
    else if (auto yield = mlir::dyn_cast<ac::TableChooseYieldOp>(operation))
      yielded.push_back(yield.getValue());
    else if (auto yield = mlir::dyn_cast<ac::ExpectYieldOp>(operation))
      yielded.push_back(yield.getCondition());
    else if (auto yield = mlir::dyn_cast<ac::FeedbackYieldOp>(operation)) {
      yielded.push_back(yield.getValue());
      yielded.push_back(yield.getContinueValue());
    } else
      return planError("unsupported operation in Queue Var region: " +
                       operation.getName().getStringRef());
    sawStructuredYield = true;
    auto names = operandNames(yielded);
    if (!names)
      return names.takeError();
    plan.yields = std::move(*names);
  }
  if (!sawStructuredYield)
    return planError("Queue Var region has no structured yield");
  return llvm::Error::success();
}

using ActivationNode = QueueActivationNodePlan;
using ActivationEdge = QueueActivationEdgePlan;

unsigned activationKindOrder(QueueActivationNodeKind kind) {
  return static_cast<unsigned>(kind);
}

llvm::StringRef activationKindName(QueueActivationNodeKind kind) {
  switch (kind) {
  case QueueActivationNodeKind::InterfaceInput:
    return "interface_input";
  case QueueActivationNodeKind::InterfaceOutput:
    return "interface_output";
  case QueueActivationNodeKind::Queue:
    return "queue";
  case QueueActivationNodeKind::Block:
    return "block";
  case QueueActivationNodeKind::Table:
    return "table";
  }
  llvm_unreachable("unknown Queue activation node kind");
}

auto activationNodeKey(const ActivationNode &node) {
  return std::pair{activationKindOrder(node.kind), node.index};
}

auto activationEdgeKey(const ActivationEdge &edge) {
  return std::tuple{activationKindOrder(edge.source.kind), edge.source.index,
                    activationKindOrder(edge.target.kind), edge.target.index};
}

std::optional<ActivationNode> queueActivationNode(const QueueGraphPlan &plan,
                                                  llvm::StringRef name) {
  for (auto [index, input] : llvm::enumerate(plan.interfaceInputs))
    if (input.name == name)
      return ActivationNode{QueueActivationNodeKind::InterfaceInput, index};
  for (auto [index, output] : llvm::enumerate(plan.interfaceOutputs))
    if (output.name == name)
      return ActivationNode{QueueActivationNodeKind::InterfaceOutput, index};
  for (auto [index, queue] : llvm::enumerate(plan.queues))
    if (queue.name == name)
      return ActivationNode{QueueActivationNodeKind::Queue, index};
  return std::nullopt;
}

std::optional<ActivationNode> tableActivationNode(const QueueGraphPlan &plan,
                                                  llvm::StringRef name) {
  for (auto [index, table] : llvm::enumerate(plan.tables))
    if (table.name == name)
      return ActivationNode{QueueActivationNodeKind::Table, index};
  return std::nullopt;
}

void collectExpressionTables(const QueueExpressionPlan &expression,
                             llvm::StringSet<> &tables) {
  if (!expression.table.empty())
    tables.insert(expression.table);
  for (const QueueExpressionPlan &nested : expression.nestedExpressions)
    collectExpressionTables(nested, tables);
}

struct InferredActivation {
  std::vector<ActivationEdge> wakeEdges;
  std::vector<ActivationEdge> workClosureEdges;
  std::vector<ActivationNode> initial;
};

llvm::Expected<InferredActivation> inferActivation(const QueueGraphPlan &plan) {
  std::vector<ActivationEdge> edges;
  std::vector<ActivationEdge> workClosureEdges;
  std::vector<ActivationNode> initial;
  for (auto [blockIndex, block] : llvm::enumerate(plan.blocks)) {
    if (block.kind == "source")
      continue;
    auto resolveRuleResource = [&](const QueueRuleResourcePlan &resource)
        -> llvm::Expected<ActivationNode> {
      if (resource.kind == "input_queue") {
        if (resource.ordinal >= block.inputs.size())
          return planError("activation input Queue ordinal is out of range");
        auto node = queueActivationNode(plan, block.inputs[resource.ordinal]);
        if (!node)
          return planError("activation input Queue is unresolved");
        return *node;
      }
      if (resource.kind == "output_queue") {
        if (resource.ordinal >= block.outputs.size())
          return planError("activation output Queue ordinal is out of range");
        auto node = queueActivationNode(plan, block.outputs[resource.ordinal]);
        if (!node)
          return planError("activation output Queue is unresolved");
        return *node;
      }
      if (resource.kind == "state") {
        auto node = tableActivationNode(plan, resource.resource);
        if (!node)
          return planError("activation Table is unresolved");
        return *node;
      }
      return planError("activation resource kind is unsupported");
    };
    std::vector<ActivationNode> wakeSources;
    std::vector<ActivationNode> transactionResources;
    if (block.hasActivationEvidence) {
      for (const QueueRuleResourcePlan &resource : block.activationSources) {
        auto node = resolveRuleResource(resource);
        if (!node)
          return node.takeError();
        wakeSources.push_back(*node);
      }
      for (const QueueRuleResourcePlan &resource : block.transactionResources) {
        auto node = resolveRuleResource(resource);
        if (!node)
          return node.takeError();
        transactionResources.push_back(*node);
      }
    } else {
      for (const std::string &queue : block.inputs) {
        auto node = queueActivationNode(plan, queue);
        if (!node)
          return planError("activation input Queue is unresolved");
        wakeSources.push_back(*node);
      }
      for (const std::string &queue : block.outputs) {
        auto node = queueActivationNode(plan, queue);
        if (!node)
          return planError("activation output Queue is unresolved");
        wakeSources.push_back(*node);
      }
      llvm::StringSet<> referencedTables;
      if (!block.table.empty())
        referencedTables.insert(block.table);
      for (const StateWritePlan &write : block.stateWrites)
        referencedTables.insert(write.table);
      for (const QueueExpressionPlan &expression : block.expressions)
        collectExpressionTables(expression, referencedTables);
      for (const auto &entry : referencedTables) {
        auto node = tableActivationNode(plan, entry.getKey());
        if (!node)
          return planError("activation Table is unresolved");
        wakeSources.push_back(*node);
      }
      transactionResources = wakeSources;
    }
    auto canonicalizeNodes = [](std::vector<ActivationNode> &nodes) {
      llvm::sort(nodes,
                 [](const ActivationNode &left, const ActivationNode &right) {
                   return activationNodeKey(left) < activationNodeKey(right);
                 });
      nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    };
    canonicalizeNodes(wakeSources);
    canonicalizeNodes(transactionResources);
    const ActivationNode worker{QueueActivationNodeKind::Block,
                                uint64_t(blockIndex)};
    for (const ActivationNode &source : wakeSources)
      edges.push_back({source, worker});
    for (const ActivationNode &resource : transactionResources)
      workClosureEdges.push_back({worker, resource});
    const bool initiallyActive = block.hasActivationEvidence
                                     ? block.initiallyActive
                                     : block.inputs.empty() &&
                                           block.kind != "sink" &&
                                           block.kind != "observe";
    if (initiallyActive)
      initial.push_back(worker);
  }
  llvm::sort(edges,
             [](const ActivationEdge &left, const ActivationEdge &right) {
               return activationEdgeKey(left) < activationEdgeKey(right);
             });
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  llvm::sort(workClosureEdges,
             [](const ActivationEdge &left, const ActivationEdge &right) {
               return activationEdgeKey(left) < activationEdgeKey(right);
             });
  workClosureEdges.erase(
      std::unique(workClosureEdges.begin(), workClosureEdges.end()),
      workClosureEdges.end());
  llvm::sort(initial,
             [](const ActivationNode &left, const ActivationNode &right) {
               return activationNodeKey(left) < activationNodeKey(right);
             });
  initial.erase(std::unique(initial.begin(), initial.end()), initial.end());
  return InferredActivation{std::move(edges), std::move(workClosureEdges),
                            std::move(initial)};
}

llvm::Error materializeActivation(QueueGraphPlan &plan) {
  auto inferred = inferActivation(plan);
  if (!inferred)
    return inferred.takeError();
  plan.activationEdges = std::move(inferred->wakeEdges);
  plan.workClosureEdges = std::move(inferred->workClosureEdges);
  plan.initialActivation = std::move(inferred->initial);
  return llvm::Error::success();
}

llvm::Expected<std::vector<QueueRuleResourcePlan>>
extractRuleResources(mlir::Operation *operation, llvm::StringRef name) {
  auto resources = operation->getAttrOfType<mlir::ArrayAttr>(name);
  if (!resources)
    return planError("lowered rule activation evidence is missing");
  std::vector<QueueRuleResourcePlan> result;
  result.reserve(resources.size());
  for (mlir::Attribute attribute : resources) {
    auto record = mlir::dyn_cast<mlir::DictionaryAttr>(attribute);
    auto kind = record ? record.getAs<ac::ActivationResourceKindAttr>("kind")
                       : ac::ActivationResourceKindAttr();
    if (!kind)
      return planError("lowered rule activation resource kind is missing");
    QueueRuleResourcePlan resource;
    switch (kind.getValue()) {
    case ac::ActivationResourceKind::InputQueue:
      resource.kind = "input_queue";
      break;
    case ac::ActivationResourceKind::OutputQueue:
      resource.kind = "output_queue";
      break;
    case ac::ActivationResourceKind::State:
      resource.kind = "state";
      break;
    }
    if (kind.getValue() == ac::ActivationResourceKind::State) {
      auto symbol = record.getAs<mlir::FlatSymbolRefAttr>("resource");
      if (!symbol)
        return planError("state activation resource is missing");
      resource.resource = symbol.getValue().str();
    } else {
      auto ordinal = record.getAs<mlir::IntegerAttr>("ordinal");
      if (!ordinal || ordinal.getInt() < 0)
        return planError("Queue activation ordinal is missing or invalid");
      resource.ordinal = static_cast<uint64_t>(ordinal.getInt());
    }
    result.push_back(std::move(resource));
  }
  return result;
}

llvm::Error extractRuleActivation(mlir::Operation *operation,
                                  QueueBlockPlan &block) {
  auto sources = extractRuleResources(operation, "ac.activation_sources");
  if (!sources)
    return sources.takeError();
  auto transaction =
      extractRuleResources(operation, "ac.transaction_resources");
  if (!transaction)
    return transaction.takeError();
  auto initial =
      operation->getAttrOfType<mlir::BoolAttr>("ac.initially_active");
  if (!initial)
    return planError("lowered rule activation evidence is incomplete");
  block.activationSources = std::move(*sources);
  block.transactionResources = std::move(*transaction);
  block.initiallyActive = initial.getValue();
  block.hasActivationEvidence = true;
  return llvm::Error::success();
}

class Extractor {
public:
  explicit Extractor(mlir::ModuleOp module) : module(module) {}

  llvm::Expected<QueueGraphPlan> run() {
    if (mlir::failed(mlir::verify(module)))
      return planError("QueueGraph input failed operation verification");
    auto epoch = module->getAttrOfType<mlir::StringAttr>("ac.contract_epoch");
    if (!epoch || epoch.getValue() != "0.5")
      return planError("module requires ac.contract_epoch exactly '0.5'");
    auto modelKind = module->getAttrOfType<mlir::StringAttr>("ac.model_kind");
    if (!modelKind || modelKind.getValue() != "queue_graph")
      return planError("module requires ac.model_kind exactly 'queue_graph'");
    if (!module.getOps<ac::SystemOp>().empty())
      return runStructured();
    for (mlir::Operation &operation : module.getBody()->getOperations()) {
      if (mlir::isa<ac::SystemOp, ac::ModuleOp, ac::ModuleExternOp>(operation))
        return planError(
            "structured system/module declaration is not legal in QueueGraph");
    }
    mlir::Operation *unclosed = nullptr;
    module.walk([&](mlir::Operation *operation) {
      if (!unclosed &&
          mlir::isa<ac::RuleOp, ac::TypeConstraintMarkerOp,
                    ac::ValueFactMarkerOp, ac::PendingObligationMarkerOp,
                    ac::VarDeclOp, ac::VarReadOp, ac::VarAssignOp,
                    ac::VarReadElementOp, ac::VarAssignElementOp,
                    ac::RuleConditionOp>(operation))
        unclosed = operation;
    });
    if (unclosed)
      return planError("unresolved rule or typed marker reached QueueGraph");
    bool unsupportedFiring = false;
    module.walk([&](ac::FiringOp firing) {
      unsigned proposals = 0;
      firing.getBody().walk([&](ac::TableProposeOp) { ++proposals; });
      unsupportedFiring |= proposals == 0;
    });
    if (unsupportedFiring)
      return planError("internal firing requires state proposals");
    mlir::LogicalResult loweredRuleProof = mlir::success();
    module.walk([&](ac::TransformOp transform) {
      if (mlir::failed(loweredRuleProof))
        return;
      loweredRuleProof = ac::verifyLoweredRuleTransformContract(transform);
    });
    if (mlir::failed(loweredRuleProof))
      return planError("lowered-rule proof verification failed");
    if (mlir::failed(acir::verifyFrozenFlatQueueGraph(module)))
      return planError(
          "QueueGraph requires verified epoch 0.5 topology freeze");
    auto system = module->getAttrOfType<mlir::StringAttr>("ac.system");
    if (!system || system.getValue().empty())
      return planError("module requires non-empty ac.system");
    plan.system = system.getValue().str();
    if (auto specialization =
            module->getAttrOfType<mlir::StringAttr>("ac.specialization")) {
      if (!isValidFingerprint(specialization.getValue()))
        return planError("module ac.specialization fingerprint is invalid");
      plan.specializationFingerprint = specialization.getValue().str();
    }
    if (auto error = extractBlock(*module.getBody(), {}))
      return std::move(error);
    if (auto error = materializeActivation(plan))
      return std::move(error);
    if (auto error = validateGraph())
      return std::move(error);
    return std::move(plan);
  }

private:
  llvm::Expected<QueueGraphPlan> extractDefinition(
      ac::ModuleOp definition, llvm::StringRef specialization,
      llvm::StringRef system,
      const llvm::StringMap<const QueueGraphPlan *> *available = nullptr) {
    Extractor nested(module);
    nested.plan.system = system.str();
    nested.plan.definition = definition.getSymName().str();
    auto definitionFingerprint = definition->getAttrOfType<mlir::StringAttr>(
        "ac.definition_fingerprint");
    if (!definitionFingerprint)
      return planError("module definition fingerprint is missing");
    nested.plan.definitionFingerprint = definitionFingerprint.getValue().str();
    nested.plan.specializationFingerprint = specialization.str();
    nested.plan.payloads = plan.payloads;
    nested.plan.enums = plan.enums;
    nested.plan.aggregates = plan.aggregates;
    if (available)
      nested.availableSpecializations = *available;

    mlir::Block &body = definition.getBody().front();
    for (auto [index, argument] : llvm::enumerate(body.getArguments())) {
      auto queue = mlir::dyn_cast<ac::QueueType>(argument.getType());
      if (!queue)
        return planError("module interface input must be ac.queue");
      std::string name = "input_" + std::to_string(index);
      nested.names[argument] = name;
      nested.plan.interfaceInputs.push_back(
          {std::move(name), printType(queue.getElementType())});
    }
    if (auto error = nested.extractBlock(body, {}))
      return std::move(error);
    auto returned = mlir::dyn_cast<ac::ReturnOp>(body.getTerminator());
    if (!returned)
      return planError("module definition must terminate with ac.return");
    for (mlir::Value value : returned.getOperands()) {
      auto name = queueName(value, nested.names);
      if (!name)
        return name.takeError();
      auto queue = mlir::cast<ac::QueueType>(value.getType());
      nested.plan.interfaceOutputs.push_back(
          {*name, printType(queue.getElementType())});
    }
    if (auto error = materializeActivation(nested.plan))
      return std::move(error);
    if (!available)
      if (auto error = verifyQueueGraphPlan(nested.plan))
        return std::move(error);
    return std::move(nested.plan);
  }

  llvm::Expected<QueueGraphPlan> runStructured() {
    if (mlir::failed(acir::verifyFrozenStructuredQueueGraph(module)))
      return planError(
          "QueueGraph requires verified structured epoch 0.5 topology freeze");
    ac::SystemOp selected;
    for (ac::SystemOp system : module.getOps<ac::SystemOp>())
      if (system.getSelected()) {
        selected = system;
        break;
      }
    if (!selected)
      return planError("structured QueueGraph has no selected system");
    mlir::SymbolTable symbols(module);
    auto root = mlir::dyn_cast_or_null<ac::ModuleOp>(
        symbols.lookup(selected.getRootAttr().getValue()));
    if (!root)
      return planError("structured QueueGraph root module is unresolved");

    for (ac::TypeScopeOp typeScope : module.getOps<ac::TypeScopeOp>())
      if (auto error = extractTypeScope(typeScope))
        return std::move(error);

    llvm::StringMap<ac::ModuleOp> definitions;
    for (ac::ModuleOp definition : module.getOps<ac::ModuleOp>())
      definitions[definition.getSymName()] = definition;
    llvm::StringMap<std::pair<ac::ModuleOp, std::string>> requested;
    for (ac::ModuleOp definition : module.getOps<ac::ModuleOp>())
      for (ac::InstanceOp instance :
           definition.getBody().front().getOps<ac::InstanceOp>()) {
        auto fingerprint =
            instance->getAttrOfType<mlir::StringAttr>("ac.specialization");
        auto target = definitions.find(instance.getDefinition());
        if (!fingerprint || target == definitions.end())
          return planError("structured instance specialization is incomplete");
        requested.try_emplace(fingerprint.getValue(), target->getValue(),
                              fingerprint.getValue().str());
      }

    llvm::StringMap<std::vector<std::string>> dependencies;
    llvm::StringMap<std::vector<std::string>> parents;
    llvm::StringMap<size_t> pendingDependencies;
    for (const auto &entry : requested) {
      llvm::StringSet<> unique;
      ac::ModuleOp definition = entry.getValue().first;
      for (ac::InstanceOp instance :
           definition.getBody().front().getOps<ac::InstanceOp>()) {
        auto fingerprint =
            instance->getAttrOfType<mlir::StringAttr>("ac.specialization");
        if (!fingerprint || !requested.contains(fingerprint.getValue()))
          return planError(
              "nested module references an unavailable specialization");
        if (unique.insert(fingerprint.getValue()).second)
          dependencies[entry.getKey()].push_back(fingerprint.getValue().str());
      }
      llvm::sort(dependencies[entry.getKey()]);
      pendingDependencies[entry.getKey()] = dependencies[entry.getKey()].size();
      for (const std::string &child : dependencies[entry.getKey()])
        parents[child].push_back(entry.getKey().str());
    }
    for (auto &entry : parents)
      llvm::sort(entry.getValue());

    std::set<std::string> ready;
    for (const auto &entry : requested)
      if (pendingDependencies[entry.getKey()] == 0)
        ready.insert(entry.getKey().str());
    llvm::StringMap<std::shared_ptr<QueueGraphPlan>> built;
    while (!ready.empty()) {
      std::string fingerprint = *ready.begin();
      ready.erase(ready.begin());
      auto request = requested.find(fingerprint);
      if (request == requested.end())
        return planError("specialization worklist identity is unresolved");
      llvm::StringMap<const QueueGraphPlan *> children;
      std::vector<std::shared_ptr<QueueGraphPlan>> childPlans;
      for (const std::string &child : dependencies[fingerprint]) {
        auto found = built.find(child);
        if (found == built.end())
          return planError("nested specialization dependency is not built");
        children[child] = found->getValue().get();
        childPlans.push_back(found->getValue());
      }
      auto extracted = extractDefinition(
          request->getValue().first, fingerprint, selected.getSymName(),
          children.empty() ? nullptr : &children);
      if (!extracted)
        return extracted.takeError();
      extracted->moduleSpecializations = std::move(childPlans);
      if (auto error = verifyQueueGraphPlan(*extracted))
        return std::move(error);
      auto stored = std::make_shared<QueueGraphPlan>(std::move(*extracted));
      built[fingerprint] = stored;
      for (const std::string &parent : parents[fingerprint]) {
        size_t &pending = pendingDependencies[parent];
        if (pending == 0)
          return planError("nested specialization dependency underflow");
        if (--pending == 0)
          ready.insert(parent);
      }
    }
    if (built.size() != requested.size())
      return planError("nested specialization graph is cyclic or incomplete");

    llvm::StringMap<const QueueGraphPlan *> available;
    std::vector<std::shared_ptr<QueueGraphPlan>> specializations;
    llvm::StringSet<> rootDependencies;
    for (ac::InstanceOp instance :
         root.getBody().front().getOps<ac::InstanceOp>()) {
      auto fingerprint =
          instance->getAttrOfType<mlir::StringAttr>("ac.specialization");
      auto found =
          fingerprint ? built.find(fingerprint.getValue()) : built.end();
      if (!fingerprint || found == built.end())
        return planError("root instance specialization is unavailable");
      available[fingerprint.getValue()] = found->getValue().get();
      if (rootDependencies.insert(fingerprint.getValue()).second)
        specializations.push_back(found->getValue());
    }
    llvm::sort(specializations, [](const auto &left, const auto &right) {
      return left->specializationFingerprint < right->specializationFingerprint;
    });

    auto rootSpecialization =
        root->getAttrOfType<mlir::StringAttr>("ac.specialization");
    if (!rootSpecialization)
      return planError("root specialization fingerprint is missing");
    auto extractedRoot = extractDefinition(root, rootSpecialization.getValue(),
                                           selected.getSymName(), &available);
    if (!extractedRoot)
      return extractedRoot.takeError();
    extractedRoot->moduleSpecializations = std::move(specializations);
    if (auto error = materializeActivation(*extractedRoot))
      return std::move(error);
    if (auto error = verifyQueueGraphPlan(*extractedRoot))
      return std::move(error);
    return std::move(*extractedRoot);
  }

  llvm::Error validateGraph() { return verifyQueueGraphPlan(plan); }

  llvm::Expected<uint64_t>
  valueWidth(mlir::Operation *from, mlir::Type type,
             llvm::SmallVectorImpl<mlir::Type> &active) {
    return mlirValueBitWidth(from, type, active);
  }

  llvm::Error recordAggregateType(mlir::Operation *from, mlir::Type type) {
    llvm::SmallVector<mlir::Type> active;
    auto width = valueWidth(from, type, active);
    if (!width)
      return width.takeError();
    std::string identity = printType(type);
    if (auto tuple = mlir::dyn_cast<mlir::TupleType>(type)) {
      if (!aggregateIdentities.insert(identity).second)
        return llvm::Error::success();
      QueueAggregatePlan aggregate{identity, "tuple", {}, tuple.size(), *width};
      for (mlir::Type element : tuple.getTypes()) {
        aggregate.elements.push_back(printType(element));
        if (mlir::isa<mlir::TupleType, ac::ValueArrayType>(element))
          if (auto error = recordAggregateType(from, element))
            return error;
      }
      plan.aggregates.push_back(std::move(aggregate));
    } else if (auto array = mlir::dyn_cast<ac::ValueArrayType>(type)) {
      if (!aggregateIdentities.insert(identity).second)
        return llvm::Error::success();
      QueueAggregatePlan aggregate{identity,
                                   "array",
                                   {printType(array.getElementType())},
                                   static_cast<uint64_t>(array.getLength()),
                                   *width};
      if (mlir::isa<mlir::TupleType, ac::ValueArrayType>(
              array.getElementType()))
        if (auto error = recordAggregateType(from, array.getElementType()))
          return error;
      plan.aggregates.push_back(std::move(aggregate));
    }
    return llvm::Error::success();
  }

  llvm::Error extractTypeScope(ac::TypeScopeOp typeScope) {
    for (mlir::Operation &declaration : typeScope.getBody().front()) {
      if (auto enumeration = mlir::dyn_cast<ac::EnumOp>(declaration)) {
        if (!enumIdentities.insert(enumeration.getSymName()).second)
          return planError("enum identities must be unique");
        QueueEnumPlan planEnum{enumeration.getSymName().str(), {}, 0};
        for (mlir::Attribute value : enumeration.getEnumerants())
          planEnum.enumerants.push_back(
              mlir::cast<mlir::StringAttr>(value).getValue().str());
        planEnum.width = std::max<uint64_t>(
            1, llvm::Log2_64_Ceil(planEnum.enumerants.size()));
        plan.enums.push_back(std::move(planEnum));
        continue;
      }
      auto structure = mlir::dyn_cast<ac::StructOp>(declaration);
      if (!structure)
        continue;
      if (!payloadIdentities.insert(structure.getSymName()).second)
        return planError("payload identities must be unique");
      QueuePayloadPlan payload{structure.getSymName().str(), {}};
      for (mlir::Attribute rawField : structure.getFields()) {
        auto field = mlir::dyn_cast<mlir::DictionaryAttr>(rawField);
        auto name =
            field ? field.getAs<mlir::StringAttr>("name") : mlir::StringAttr();
        auto type =
            field ? field.getAs<mlir::TypeAttr>("type") : mlir::TypeAttr();
        if (!name || !type)
          return planError("struct field requires name and type");
        if (mlir::isa<mlir::TupleType, ac::ValueArrayType>(type.getValue()))
          if (auto error = recordAggregateType(structure, type.getValue()))
            return error;
        llvm::SmallVector<mlir::Type> active;
        auto width = valueWidth(structure, type.getValue(), active);
        if (!width)
          return width.takeError();
        payload.fields.push_back(
            {name.getValue().str(), printType(type.getValue()), *width});
      }
      plan.payloads.push_back(std::move(payload));
    }
    return llvm::Error::success();
  }

  llvm::Error addQueue(mlir::Value value, llvm::StringRef name, uint64_t depth,
                       uint64_t latency, uint64_t rate,
                       llvm::ArrayRef<std::string> scope) {
    if (name.empty() || !queueIdentities.insert(name).second)
      return planError("Queue logical identities must be non-empty and unique");
    auto queue = mlir::dyn_cast<ac::QueueType>(value.getType());
    if (!queue || depth == 0 || latency == 0 || rate == 0 || rate > depth)
      return planError(
          "Queue plan requires typed positive depth/latency and rate <= depth");
    names[value] = name.str();
    plan.queues.push_back({name.str(), printType(queue.getElementType()),
                           scopePath(scope), depth, latency, rate});
    return llvm::Error::success();
  }

  llvm::Error addOutputs(mlir::Operation *op, mlir::ValueRange outputs,
                         llvm::ArrayRef<int64_t> depths,
                         llvm::ArrayRef<int64_t> latencies,
                         llvm::ArrayRef<std::string> scope,
                         std::vector<std::string> &result) {
    auto frozen = outputNames(op, outputs.size());
    if (!frozen)
      return frozen.takeError();
    if (depths.size() != outputs.size() || latencies.size() != outputs.size())
      return planError("Queue output metadata count mismatch");
    llvm::SmallVector<int64_t> defaultRates(outputs.size(), 1);
    llvm::ArrayRef<int64_t> rates = defaultRates;
    if (auto attribute =
            op->getAttrOfType<mlir::DenseI64ArrayAttr>("ac.output_rates"))
      rates = attribute.asArrayRef();
    if (rates.size() != outputs.size())
      return planError("Queue output rate count must match result count");
    for (size_t index = 0; index < outputs.size(); ++index) {
      if (depths[index] <= 0 || latencies[index] <= 0 || rates[index] <= 0 ||
          rates[index] > depths[index])
        return planError("Queue depth/latency must be positive and rate must "
                         "not exceed depth");
      auto error = addQueue(outputs[index], (*frozen)[index], depths[index],
                            latencies[index], rates[index], scope);
      if (error)
        return error;
    }
    result = std::move(*frozen);
    return llvm::Error::success();
  }

  void appendBlock(QueueBlockPlan block) {
    block.lexicalOrder = nextLexicalOrder++;
    plan.blocks.push_back(std::move(block));
  }

  llvm::Error extractBlock(mlir::Block &block, std::vector<std::string> scope) {
    for (mlir::Operation &operation : block) {
      if (auto typeScope = mlir::dyn_cast<ac::TypeScopeOp>(operation)) {
        if (auto error = extractTypeScope(typeScope))
          return error;
        continue;
      }
      if (auto instance = mlir::dyn_cast<ac::MemoryInstanceOp>(operation)) {
        plan.memoryInstances.push_back(
            {instance.getSymName().str(), printType(instance.getDataType()),
             uint64_t(instance.getEntries()), uint64_t(instance.getInit()),
             uint64_t(instance.getLatency()), instance.getStableId().str(),
             instance.getOwner().str()});
        continue;
      }
      if (auto table = mlir::dyn_cast<ac::TableOp>(operation)) {
        plan.tables.push_back(
            {table.getSymName().str(), printType(table.getEntryType()),
             uint64_t(table.getEntries()), uint64_t(table.getInit()),
             table.getStableId().str(), table.getOwner().str()});
        continue;
      }
      if (auto slot = mlir::dyn_cast<ac::SlotOp>(operation)) {
        auto input = queueName(slot.getInput(), names);
        if (!input)
          return input.takeError();
        plan.slots.push_back(
            {slot.getSymName().str(),
             printType(mlir::cast<ac::QueueType>(slot.getInput().getType())
                           .getElementType()),
             *input, scopePath(scope), slot.getStableId().str(),
             slot.getOwner().str()});
        continue;
      }
      if (auto match = mlir::dyn_cast<ac::TableMatchOp>(operation)) {
        const std::string name =
            "table_match_" + std::to_string(plan.tableMatches.size());
        QueueBlockPlan predicate;
        if (auto error = extractExpressions(match.getPredicate(), predicate))
          return error;
        if (predicate.yields.size() != 1)
          return planError("table.match predicate must yield one value");
        auto resultType = mlir::cast<ac::VarType>(match.getMask().getType());
        plan.tableMatches.push_back(
            {name, match.getTable().str(), scopePath(scope),
             printType(resultType.getElementType()),
             std::move(predicate.expressions), predicate.yields.front()});
        QueueExpressionPlan reference{
            "shared_match_" + std::to_string(plan.tableMatches.size() - 1),
            "table_match_ref",
            printType(resultType.getElementType()),
            {}};
        reference.field = name;
        reference.table = match.getTable().str();
        sharedExpressions.emplace_back(match.getMask(), std::move(reference));
        continue;
      }
      if (auto choose = mlir::dyn_cast<ac::TableChooseOp>(operation)) {
        auto matchValue = llvm::find_if(
            sharedExpressions, [&](const SharedExpression &candidate) {
              return candidate.first == choose.getMask() &&
                     candidate.second.kind == "table_match_ref";
            });
        if (matchValue == sharedExpressions.end())
          return planError("table.choose requires a shared table.match mask");
        const std::string name =
            "table_selection_" + std::to_string(plan.tableSelections.size());
        QueueBlockPlan key;
        if (choose.getPolicy() != "first") {
          if (auto error = extractExpressions(choose.getKey(), key))
            return error;
          if (key.yields.size() != 1)
            return planError("table.choose key must yield one value");
        }
        auto indexType = mlir::cast<ac::VarType>(choose.getIndex().getType());
        plan.tableSelections.push_back(
            {name, choose.getTable().str(), scopePath(scope),
             matchValue->second.field, choose.getPolicy().str(),
             printType(indexType.getElementType()), std::move(key.expressions),
             key.yields.empty() ? std::string() : key.yields.front()});
        QueueExpressionPlan indexReference{
            "shared_selection_" +
                std::to_string(plan.tableSelections.size() - 1) + "_index",
            "table_selection_index_ref",
            printType(indexType.getElementType()),
            {}};
        indexReference.field = name;
        indexReference.table = choose.getTable().str();
        sharedExpressions.emplace_back(choose.getIndex(),
                                       std::move(indexReference));
        auto validType = mlir::cast<ac::VarType>(choose.getValid().getType());
        QueueExpressionPlan validReference{
            "shared_selection_" +
                std::to_string(plan.tableSelections.size() - 1) + "_valid",
            "table_selection_valid_ref",
            printType(validType.getElementType()),
            {}};
        validReference.field = name;
        validReference.table = choose.getTable().str();
        sharedExpressions.emplace_back(choose.getValid(),
                                       std::move(validReference));
        continue;
      }
      if (auto source = mlir::dyn_cast<ac::SourceOp>(operation)) {
        std::vector<std::string> outputs;
        if (auto error = addOutputs(
                source, source->getResults(), {int64_t(source.getDepth())},
                {int64_t(source.getLatency())}, scope, outputs))
          return error;
        appendBlock({"source",
                     outputs.front(),
                     scopePath(scope),
                     {},
                     outputs,
                     {uint64_t(source.getDepth())},
                     {uint64_t(source.getLatency())}});
        continue;
      }
      if (auto firing = mlir::dyn_cast<ac::FiringOp>(operation)) {
        auto inputs = queueNames(firing.getInputs(), names);
        if (!inputs)
          return inputs.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(
                firing, firing.getOutputs(),
                firing.getOutputDepthsAttr().asArrayRef(),
                firing.getOutputLatenciesAttr().asArrayRef(), scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"firing",
                                 outputs.empty() ? firing.getStableId().str()
                                                 : outputs.front(),
                                 scopePath(scope), std::move(*inputs), outputs};
        for (int64_t value : firing.getOutputDepths())
          blockPlan.depths.push_back(value);
        for (int64_t value : firing.getOutputLatencies())
          blockPlan.latencies.push_back(value);
        blockPlan.region = printRegion(firing.getBody());
        auto priority =
            firing->getAttrOfType<mlir::IntegerAttr>("ac.rule_priority");
        if (!priority || priority.getInt() < 0)
          return planError("firing priority is missing or invalid");
        blockPlan.priority = static_cast<uint64_t>(priority.getInt());
        if (auto error = extractExpressions(firing.getBody(), blockPlan))
          return error;
        if (firing->hasAttr("ac.activation_sources"))
          if (auto error = extractRuleActivation(firing, blockPlan))
            return error;
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto transform = mlir::dyn_cast<ac::TransformOp>(operation)) {
        auto inputs = queueNames(transform.getInputs(), names);
        if (!inputs)
          return inputs.takeError();
        std::vector<std::string> outputs;
        if (auto error =
                addOutputs(transform, transform.getOutputs(),
                           transform.getOutputDepthsAttr().asArrayRef(),
                           transform.getOutputLatenciesAttr().asArrayRef(),
                           scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"transform", outputs.front(), scopePath(scope),
                                 std::move(*inputs), outputs};
        for (int64_t value : transform.getOutputDepths())
          blockPlan.depths.push_back(value);
        for (int64_t value : transform.getOutputLatencies())
          blockPlan.latencies.push_back(value);
        blockPlan.region = printRegion(transform.getBody());
        if (auto error = extractExpressions(transform.getBody(), blockPlan))
          return error;
        if (transform->hasAttr("ac.activation_sources"))
          if (auto error = extractRuleActivation(transform, blockPlan))
            return error;
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto broadcast = mlir::dyn_cast<ac::BroadcastOp>(operation)) {
        auto input = queueName(broadcast.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error =
                addOutputs(broadcast, broadcast.getOutputs(),
                           broadcast.getOutputDepthsAttr().asArrayRef(),
                           broadcast.getOutputLatenciesAttr().asArrayRef(),
                           scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"broadcast",
                                 "broadcast_" + *input,
                                 scopePath(scope),
                                 {*input},
                                 outputs};
        for (int64_t value : broadcast.getOutputDepths())
          blockPlan.depths.push_back(value);
        for (int64_t value : broadcast.getOutputLatencies())
          blockPlan.latencies.push_back(value);
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto fork = mlir::dyn_cast<ac::ForkOp>(operation)) {
        auto input = queueName(fork.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(fork, fork.getOutputs(),
                                    fork.getOutputDepthsAttr().asArrayRef(),
                                    fork.getOutputLatenciesAttr().asArrayRef(),
                                    scope, outputs))
          return error;
        QueueBlockPlan blockPlan{
            "fork", "fork_" + *input, scopePath(scope), {*input}, outputs};
        for (int64_t value : fork.getOutputDepths())
          blockPlan.depths.push_back(value);
        for (int64_t value : fork.getOutputLatencies())
          blockPlan.latencies.push_back(value);
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto route = mlir::dyn_cast<ac::RouteOp>(operation)) {
        auto input = queueName(route.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(route, route.getOutputs(),
                                    route.getOutputDepthsAttr().asArrayRef(),
                                    route.getOutputLatenciesAttr().asArrayRef(),
                                    scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"route",
                                 "route_" + outputs.front(),
                                 scopePath(scope),
                                 {*input},
                                 outputs};
        for (int64_t value : route.getOutputDepths())
          blockPlan.depths.push_back(value);
        for (int64_t value : route.getOutputLatencies())
          blockPlan.latencies.push_back(value);
        blockPlan.region = printRegion(route.getSelector());
        if (auto error = extractExpressions(route.getSelector(), blockPlan))
          return error;
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto select = mlir::dyn_cast<ac::SelectOp>(operation)) {
        auto inputs = queueNames(select.getInputs(), names);
        if (!inputs)
          return inputs.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(
                select, select->getResults(), {int64_t(select.getDepth())},
                {int64_t(select.getLatency())}, scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"select",
                                 outputs.front(),
                                 scopePath(scope),
                                 std::move(*inputs),
                                 outputs,
                                 {uint64_t(select.getDepth())},
                                 {uint64_t(select.getLatency())}};
        blockPlan.region = printRegion(select.getKey());
        if (auto error = extractExpressions(select.getKey(), blockPlan))
          return error;
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto merge = mlir::dyn_cast<ac::MergeOp>(operation)) {
        auto inputs = queueNames(merge.getInputs(), names);
        if (!inputs)
          return inputs.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(
                merge, merge->getResults(), {int64_t(merge.getDepth())},
                {int64_t(merge.getLatency())}, scope, outputs))
          return error;
        appendBlock({"merge",
                     outputs.front(),
                     scopePath(scope),
                     std::move(*inputs),
                     outputs,
                     {uint64_t(merge.getDepth())},
                     {uint64_t(merge.getLatency())},
                     merge.getPolicy().str()});
        continue;
      }
      if (auto barrier = mlir::dyn_cast<ac::BarrierOp>(operation)) {
        auto inputs = queueNames(barrier.getInputs(), names);
        if (!inputs)
          return inputs.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(
                barrier, barrier.getOutputs(),
                barrier.getOutputDepthsAttr().asArrayRef(),
                barrier.getOutputLatenciesAttr().asArrayRef(), scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"barrier", outputs.front(), scopePath(scope),
                                 std::move(*inputs), outputs};
        for (int64_t value : barrier.getOutputDepths())
          blockPlan.depths.push_back(value);
        for (int64_t value : barrier.getOutputLatencies())
          blockPlan.latencies.push_back(value);
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto reorder = mlir::dyn_cast<ac::ReorderOp>(operation)) {
        auto input = queueName(reorder.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(
                reorder, reorder->getResults(), {int64_t(reorder.getDepth())},
                {int64_t(reorder.getLatency())}, scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"reorder",
                                 outputs.front(),
                                 scopePath(scope),
                                 {*input},
                                 outputs,
                                 {uint64_t(reorder.getDepth())},
                                 {uint64_t(reorder.getLatency())}};
        blockPlan.capacity = reorder.getCapacity();
        blockPlan.start = reorder.getStart();
        blockPlan.region = printRegion(reorder.getKey());
        if (auto error = extractExpressions(reorder.getKey(), blockPlan))
          return error;
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto dependency = mlir::dyn_cast<ac::DependencyOp>(operation)) {
        auto input = queueName(dependency.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error =
                addOutputs(dependency, dependency->getResults(),
                           {int64_t(dependency.getDepth())},
                           {int64_t(dependency.getLatency())}, scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"dependency",
                                 outputs.front(),
                                 scopePath(scope),
                                 {*input},
                                 outputs,
                                 {uint64_t(dependency.getDepth())},
                                 {uint64_t(dependency.getLatency())}};
        blockPlan.capacity = dependency.getCapacity();
        blockPlan.noDependency = dependency.getNoDependency();
        blockPlan.resources = dependency.getResources();
        blockPlan.region = printRegion(dependency.getKey());
        std::vector<std::string> policyYields;
        for (mlir::Region *policy :
             {&dependency.getKey(), &dependency.getWaitsFor(),
              &dependency.getResource(), &dependency.getCost()}) {
          if (auto error = extractExpressions(*policy, blockPlan))
            return error;
          if (blockPlan.yields.size() != 1)
            return planError("dependency policy must yield one value");
          policyYields.push_back(blockPlan.yields.front());
        }
        blockPlan.yields = std::move(policyYields);
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto credit = mlir::dyn_cast<ac::CreditOp>(operation)) {
        auto input = queueName(credit.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error = addOutputs(
                credit, credit->getResults(), {int64_t(credit.getDepth())},
                {int64_t(credit.getLatency())}, scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"credit",
                                 outputs.front(),
                                 scopePath(scope),
                                 {*input},
                                 outputs,
                                 {uint64_t(credit.getDepth())},
                                 {uint64_t(credit.getLatency())}};
        blockPlan.credits = credit.getCredits();
        blockPlan.region = printRegion(credit.getCost());
        if (auto error = extractExpressions(credit.getCost(), blockPlan))
          return error;
        if (blockPlan.yields.size() != 1)
          return planError("credit cost must yield one value");
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto memory = mlir::dyn_cast<ac::MemoryRequestOp>(operation)) {
        auto input = queueName(memory.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error =
                addOutputs(memory, memory->getResults(),
                           {int64_t(memory.getDepth())}, {1}, scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"memory_request",
                                 outputs.front(),
                                 scopePath(scope),
                                 {*input},
                                 outputs,
                                 {uint64_t(memory.getDepth())},
                                 {1}};
        blockPlan.resultField = memory.getResultField().str();
        blockPlan.memoryInstance = memory.getInstance().str();
        blockPlan.endpointOrdinal = memory.getOrdinal();
        blockPlan.region = printRegion(memory.getAddress());
        std::vector<std::string> policyYields;
        for (mlir::Region *policy :
             {&memory.getAddress(), &memory.getWrite(), &memory.getData()}) {
          if (auto error = extractExpressions(*policy, blockPlan))
            return error;
          if (blockPlan.yields.size() != 1)
            return planError("memory policy must yield one value");
          policyYields.push_back(blockPlan.yields.front());
        }
        blockPlan.yields = std::move(policyYields);
        plan.memoryRequests.push_back(
            {blockPlan.memoryInstance, blockPlan.name, blockPlan.scope,
             blockPlan.inputs.front(), blockPlan.outputs.front(),
             blockPlan.endpointOrdinal, blockPlan.depths.front(),
             blockPlan.resultField});
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto read = mlir::dyn_cast<ac::TableReadOp>(operation)) {
        std::vector<std::string> inputs;
        if (read.getInput()) {
          auto input = queueName(read.getInput(), names);
          if (!input)
            return input.takeError();
          inputs.push_back(std::move(*input));
        }
        std::vector<std::string> outputs;
        if (auto error =
                addOutputs(read, read->getResults(), {int64_t(read.getDepth())},
                           {int64_t(read.getLatency())}, scope, outputs))
          return error;
        auto name = read->getAttrOfType<mlir::StringAttr>("ac.name");
        if (!name || name.getValue().empty())
          return planError("table.read requires frozen ac.name");
        QueueBlockPlan blockPlan{"table_read",
                                 name.getValue().str(),
                                 scopePath(scope),
                                 inputs,
                                 outputs,
                                 {uint64_t(read.getDepth())},
                                 {uint64_t(read.getLatency())}};
        blockPlan.table = read.getTable().str();
        std::vector<std::string> policyYields;
        for (mlir::Region *policy : {&read.getAddress(), &read.getWhen()}) {
          if (auto error =
                  extractExpressions(*policy, blockPlan, sharedExpressions))
            return error;
          if (blockPlan.yields.size() != 1)
            return planError("table read policy must yield one value");
          policyYields.push_back(blockPlan.yields.front());
        }
        blockPlan.yields = std::move(policyYields);
        plan.tableReads.push_back(
            {blockPlan.table, blockPlan.name, blockPlan.scope,
             inputs.empty() ? std::string() : inputs.front(), outputs.front(),
             uint64_t(read.getDepth()), uint64_t(read.getLatency())});
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto write = mlir::dyn_cast<ac::TableWriteOp>(operation)) {
        std::vector<std::string> inputs;
        if (write.getInput()) {
          auto input = queueName(write.getInput(), names);
          if (!input)
            return input.takeError();
          inputs.push_back(std::move(*input));
        }
        auto name = write->getAttrOfType<mlir::StringAttr>("ac.name");
        if (!name || name.getValue().empty())
          return planError("table.write requires frozen ac.name");
        QueueBlockPlan blockPlan{
            "table_write", name.getValue().str(), scopePath(scope), inputs, {}};
        blockPlan.table = write.getTable().str();
        blockPlan.writeMode = write.getMode().str();
        for (mlir::Attribute rawField : write.getWriteFields())
          blockPlan.writeFields.push_back(
              mlir::cast<mlir::StringAttr>(rawField).getValue().str());
        std::vector<std::string> policyYields;
        for (mlir::Region *policy :
             {&write.getAddress(), &write.getEnable(), &write.getValue()}) {
          if (auto error =
                  extractExpressions(*policy, blockPlan, sharedExpressions))
            return error;
          if (blockPlan.yields.size() != 1)
            return planError("table write policy must yield one value");
          policyYields.push_back(blockPlan.yields.front());
        }
        blockPlan.yields = std::move(policyYields);
        plan.tableWrites.push_back(
            {blockPlan.table, blockPlan.name, blockPlan.scope,
             inputs.empty() ? std::string() : inputs.front(),
             blockPlan.writeMode, blockPlan.writeFields});
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto write = mlir::dyn_cast<ac::TableMaskedWriteOp>(operation)) {
        auto name = write->getAttrOfType<mlir::StringAttr>("ac.name");
        if (!name || name.getValue().empty())
          return planError("table.masked_write requires frozen ac.name");
        QueueBlockPlan blockPlan{"table_masked_write",
                                 name.getValue().str(),
                                 scopePath(scope),
                                 {},
                                 {}};
        blockPlan.table = write.getTable().str();
        blockPlan.writeMode = write.getMode().str();
        for (mlir::Attribute rawField : write.getWriteFields())
          blockPlan.writeFields.push_back(
              mlir::cast<mlir::StringAttr>(rawField).getValue().str());
        auto matchValue = llvm::find_if(
            sharedExpressions, [&](const SharedExpression &candidate) {
              return candidate.first == write.getMask() &&
                     candidate.second.kind == "table_match_ref";
            });
        if (matchValue == sharedExpressions.end())
          return planError("masked table write requires a shared match mask");
        blockPlan.expressions.push_back(matchValue->second);
        std::vector<std::string> policyYields{matchValue->second.result};
        for (mlir::Region *policy : {&write.getEnable(), &write.getValue()}) {
          if (auto error =
                  extractExpressions(*policy, blockPlan, sharedExpressions))
            return error;
          if (blockPlan.yields.size() != 1)
            return planError("masked table write policy must yield one value");
          policyYields.push_back(blockPlan.yields.front());
        }
        blockPlan.yields = std::move(policyYields);
        plan.tableMaskedWrites.push_back({blockPlan.table, blockPlan.name,
                                          blockPlan.scope, blockPlan.writeMode,
                                          blockPlan.writeFields});
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto release = mlir::dyn_cast<ac::SlotReleaseOp>(operation)) {
        auto slot = llvm::find_if(plan.slots, [&](const SlotPlan &candidate) {
          return candidate.name == release.getSlot();
        });
        if (slot == plan.slots.end())
          return planError("slot.release references unknown slot");
        auto name = release->getAttrOfType<mlir::StringAttr>("ac.name");
        if (!name || name.getValue().empty())
          return planError("slot.release requires frozen ac.name");
        QueueBlockPlan blockPlan{
            "slot", name.getValue().str(), scopePath(scope), {slot->input}, {}};
        blockPlan.slot = slot->name;
        blockPlan.region = printRegion(release.getWhen());
        if (auto error = extractExpressions(release.getWhen(), blockPlan,
                                            sharedExpressions))
          return error;
        if (blockPlan.yields.size() != 1)
          return planError("slot release policy must yield one value");
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto feedback = mlir::dyn_cast<ac::FeedbackOp>(operation)) {
        auto input = queueName(feedback.getInput(), names);
        if (!input)
          return input.takeError();
        std::vector<std::string> outputs;
        if (auto error =
                addOutputs(feedback, feedback->getResults(),
                           {int64_t(feedback.getDepth())},
                           {int64_t(feedback.getLatency())}, scope, outputs))
          return error;
        QueueBlockPlan blockPlan{"feedback",
                                 outputs.front(),
                                 scopePath(scope),
                                 {*input},
                                 outputs,
                                 {uint64_t(feedback.getDepth())},
                                 {uint64_t(feedback.getLatency())},
                                 "",
                                 uint64_t(feedback.getMaxIterations())};
        blockPlan.region = printRegion(feedback.getBody());
        if (auto error = extractExpressions(feedback.getBody(), blockPlan))
          return error;
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (auto instance = mlir::dyn_cast<ac::InstanceOp>(operation)) {
        auto fingerprint =
            instance->getAttrOfType<mlir::StringAttr>("ac.specialization");
        auto specialization =
            fingerprint ? availableSpecializations.find(fingerprint.getValue())
                        : availableSpecializations.end();
        if (!fingerprint || specialization == availableSpecializations.end())
          return planError(
              "module instance references an unavailable specialization");
        const QueueGraphPlan *target = specialization->getValue();
        if (!target || target->definition != instance.getDefinition())
          return planError(
              "module instance specialization definition mismatch");
        auto inputs = queueNames(instance.getInputs(), names);
        if (!inputs)
          return inputs.takeError();
        if (inputs->size() != target->interfaceInputs.size() ||
            instance.getOutputs().size() != target->interfaceOutputs.size())
          return planError("module instance interface arity mismatch");
        for (auto [input, interface] :
             llvm::zip_equal(instance.getInputs(), target->interfaceInputs)) {
          auto queue = mlir::cast<ac::QueueType>(input.getType());
          if (printType(queue.getElementType()) != interface.payloadType)
            return planError("module instance input payload mismatch");
        }
        std::vector<std::string> outputs;
        for (auto [index, result] : llvm::enumerate(instance.getOutputs())) {
          const QueueInterfacePlan &interface = target->interfaceOutputs[index];
          auto source =
              llvm::find_if(target->queues, [&](const QueuePlan &queue) {
                return queue.name == interface.name;
              });
          if (source == target->queues.end())
            return planError(
                "module output must be produced by a local Queue operation");
          std::string name = instance.getSymName().str();
          if (instance.getOutputs().size() != 1)
            name += "_" + std::to_string(index);
          if (auto error = addQueue(result, name, source->depth,
                                    source->latency, source->rate, scope))
            return error;
          outputs.push_back(std::move(name));
        }
        plan.moduleInstances.push_back(
            {instance.getSymName().str(), instance.getDefinition().str(),
             fingerprint.getValue().str(), scopePath(scope), std::move(*inputs),
             std::move(outputs), nextLexicalOrder++});
        continue;
      }
      if (auto nested = mlir::dyn_cast<ac::ScopeOp>(operation)) {
        std::vector<std::string> nestedScope = scope;
        nestedScope.push_back(nested.getSymName().str());
        plan.scopes.push_back(scopePath(nestedScope));
        mlir::Block &body = nested.getBody().front();
        if (body.getNumArguments() != nested.getInputs().size())
          return planError("scope input arity mismatch");
        for (size_t index = 0; index < nested.getInputs().size(); ++index) {
          auto name = queueName(nested.getInputs()[index], names);
          if (!name)
            return name.takeError();
          names[body.getArgument(index)] = std::move(*name);
        }
        if (auto error = extractBlock(body, nestedScope))
          return error;
        auto yield = mlir::dyn_cast<ac::ScopeYieldOp>(body.getTerminator());
        bool invalidYield = !yield;
        if (yield)
          invalidYield = yield.getQueues().size() != nested.getOutputs().size();
        if (invalidYield)
          return planError("scope output arity mismatch");
        for (size_t index = 0; index < nested.getOutputs().size(); ++index) {
          auto name = queueName(yield.getQueues()[index], names);
          if (!name)
            return name.takeError();
          names[nested.getOutputs()[index]] = std::move(*name);
        }
        continue;
      }
      auto sink = mlir::dyn_cast<ac::SinkOp>(operation);
      if (sink) {
        auto input = queueName(sink.getInput(), names);
        if (!input)
          return input.takeError();
        auto name = sink->getAttrOfType<mlir::StringAttr>("ac.name");
        if (!name || name.getValue().empty())
          return planError("sink requires frozen ac.name");
        appendBlock(
            {"sink", name.getValue().str(), scopePath(scope), {*input}, {}});
        continue;
      }
      auto observe = mlir::dyn_cast<ac::ObserveOp>(operation);
      if (observe) {
        auto input = queueName(observe.getInput(), names);
        if (!input)
          return input.takeError();
        appendBlock({"observe",
                     observe.getName().str(),
                     scopePath(scope),
                     {*input},
                     {}});
        continue;
      }
      auto expect = mlir::dyn_cast<ac::ExpectOp>(operation);
      if (expect) {
        auto input = queueName(expect.getInput(), names);
        if (!input)
          return input.takeError();
        auto name = expect->getAttrOfType<mlir::StringAttr>("ac.name");
        if (!name || name.getValue().empty())
          return planError("expect requires frozen ac.name");
        QueueBlockPlan blockPlan{
            "expect", name.getValue().str(), scopePath(scope), {*input}, {}};
        blockPlan.message = expect.getMessage().str();
        blockPlan.region = printRegion(expect.getPredicate());
        if (auto error = extractExpressions(expect.getPredicate(), blockPlan))
          return error;
        appendBlock(std::move(blockPlan));
        continue;
      }
      if (mlir::isa<ac::ScopeYieldOp>(operation) ||
          operation.hasTrait<mlir::OpTrait::IsTerminator>() ||
          mlir::isa<ac::TypeScopeOp>(operation))
        continue;
      if (operation.getName().getDialectNamespace() == "ac")
        return planError("unsupported ACIR op in QueueGraph plan: " +
                         operation.getName().getStringRef());
    }
    return llvm::Error::success();
  }

  mlir::ModuleOp module;
  QueueGraphPlan plan;
  llvm::DenseMap<mlir::Value, std::string> names;
  std::vector<SharedExpression> sharedExpressions;
  llvm::StringSet<> queueIdentities;
  llvm::StringSet<> payloadIdentities;
  llvm::StringSet<> enumIdentities;
  llvm::StringSet<> aggregateIdentities;
  llvm::StringMap<const QueueGraphPlan *> availableSpecializations;
  uint64_t nextLexicalOrder = 0;
};

std::optional<llvm::StringRef> payloadTypeName(llvm::StringRef type) {
  constexpr llvm::StringLiteral prefix = "!ac.struct<@types::@";
  if (type.starts_with(prefix) && type.ends_with('>'))
    return type.drop_front(prefix.size()).drop_back();
  return std::nullopt;
}

std::optional<llvm::StringRef> enumTypeName(llvm::StringRef type) {
  constexpr llvm::StringLiteral prefix = "!ac.enum<@types::@";
  if (type.starts_with(prefix) && type.ends_with('>'))
    return type.drop_front(prefix.size()).drop_back();
  return std::nullopt;
}

llvm::Error verifyPayloadGraph(const QueueGraphPlan &plan) {
  llvm::StringMap<const QueueEnumPlan *> enums;
  for (const QueueEnumPlan &enumeration : plan.enums) {
    if (enumeration.name.empty() || enumeration.enumerants.empty() ||
        !enums.try_emplace(enumeration.name, &enumeration).second)
      return planError("enum identities must be complete and unique");
    llvm::StringSet<> enumerants;
    for (const std::string &enumerant : enumeration.enumerants)
      if (enumerant.empty() || !enumerants.insert(enumerant).second)
        return planError("enum enumerants must be non-empty and unique");
    const uint64_t expectedWidth = std::max<uint64_t>(
        1, llvm::Log2_64_Ceil(enumeration.enumerants.size()));
    if (enumeration.width != expectedWidth)
      return planError("enum encoding width is inconsistent");
  }
  llvm::StringMap<const QueuePayloadPlan *> payloads;
  for (const QueuePayloadPlan &payload : plan.payloads) {
    if (payload.name.empty() ||
        !payloads.try_emplace(payload.name, &payload).second)
      return planError("payload identities must be non-empty and unique");
    llvm::StringSet<> fields;
    for (const QueuePayloadFieldPlan &field : payload.fields) {
      if (field.name.empty() || field.type.empty() ||
          !fields.insert(field.name).second)
        return planError("payload fields must be complete and unique");
      if (std::optional<llvm::StringRef> enumeration = enumTypeName(field.type);
          enumeration && !enums.contains(*enumeration))
        return planError("nested enum payload type is unresolved");
    }
  }

  llvm::StringMap<unsigned> states;
  auto visit = [&](auto &self, llvm::StringRef name) -> llvm::Error {
    const unsigned state = states.lookup(name);
    if (state == 2)
      return llvm::Error::success();
    if (state == 1)
      return planError("nested payload definitions contain a cycle");
    auto payload = payloads.find(name);
    if (payload == payloads.end())
      return planError("nested payload type is unresolved");
    states[name] = 1;
    for (const QueuePayloadFieldPlan &field : payload->getValue()->fields)
      if (std::optional<llvm::StringRef> dependency =
              payloadTypeName(field.type))
        if (auto error = self(self, *dependency))
          return error;
    states[name] = 2;
    return llvm::Error::success();
  };
  for (const QueuePayloadPlan &payload : plan.payloads)
    if (auto error = visit(visit, payload.name))
      return error;

  llvm::StringMap<const QueueAggregatePlan *> aggregates;
  for (const QueueAggregatePlan &aggregate : plan.aggregates)
    if (aggregate.type.empty() || aggregate.width == 0 ||
        aggregate.width > 64 ||
        !aggregates.try_emplace(aggregate.type, &aggregate).second)
      return planError("aggregate type metadata must be complete and unique");

  llvm::StringSet<> widthActive;
  auto typeWidth = [&](auto &self,
                       llvm::StringRef type) -> llvm::Expected<uint64_t> {
    if (auto width = integerWidth(type))
      return *width;
    if (!widthActive.insert(type).second)
      return planError("recursive aggregate type has no finite width");
    auto finish =
        [&](llvm::Expected<uint64_t> result) -> llvm::Expected<uint64_t> {
      widthActive.erase(type);
      return result;
    };
    if (std::optional<llvm::StringRef> name = enumTypeName(type)) {
      auto found = enums.find(*name);
      return finish(found == enums.end()
                        ? llvm::Expected<uint64_t>(
                              planError("enum type metadata is unresolved"))
                        : llvm::Expected<uint64_t>(found->getValue()->width));
    }
    if (std::optional<llvm::StringRef> name = payloadTypeName(type)) {
      auto found = payloads.find(*name);
      if (found == payloads.end())
        return finish(planError("payload type metadata is unresolved"));
      uint64_t total = 0;
      for (const QueuePayloadFieldPlan &field : found->getValue()->fields) {
        auto width = self(self, field.type);
        if (!width)
          return finish(width.takeError());
        if (*width != field.width)
          return finish(planError("payload field width is inconsistent"));
        auto next = addBitWidths(total, *width);
        if (!next)
          return finish(next.takeError());
        total = *next;
      }
      return finish(total);
    }
    auto found = aggregates.find(type);
    if (found == aggregates.end())
      return finish(planError("aggregate type metadata is unresolved"));
    const QueueAggregatePlan &aggregate = *found->getValue();
    if (aggregate.kind == "tuple") {
      if (aggregate.length == 0 ||
          aggregate.length != aggregate.elements.size())
        return finish(planError("tuple aggregate metadata is malformed"));
      uint64_t total = 0;
      for (const std::string &element : aggregate.elements) {
        auto width = self(self, element);
        if (!width)
          return finish(width.takeError());
        auto next = addBitWidths(total, *width);
        if (!next)
          return finish(next.takeError());
        total = *next;
      }
      if (total != aggregate.width)
        return finish(planError("tuple aggregate width is inconsistent"));
      return finish(total);
    }
    if (aggregate.kind == "array") {
      if (aggregate.length == 0 || aggregate.elements.size() != 1)
        return finish(planError("value-array metadata is malformed"));
      auto width = self(self, aggregate.elements.front());
      if (!width)
        return finish(width.takeError());
      auto total = multiplyBitWidths(*width, aggregate.length);
      if (!total)
        return finish(total.takeError());
      if (*total != aggregate.width)
        return finish(planError("value-array width is inconsistent"));
      return finish(aggregate.width);
    }
    return finish(planError("aggregate kind is unsupported"));
  };
  for (const QueuePayloadPlan &payload : plan.payloads) {
    auto width =
        typeWidth(typeWidth, "!ac.struct<@types::@" + payload.name + ">");
    if (!width)
      return width.takeError();
  }
  return llvm::Error::success();
}

} // namespace

llvm::Expected<QueueGraphPlan> buildQueueGraphPlan(mlir::ModuleOp module) {
  return Extractor(module).run();
}

llvm::Error verifyQueueGraphPlan(const QueueGraphPlan &plan) {
  if (plan.system.empty() || plan.queues.empty() ||
      (plan.blocks.empty() && plan.moduleInstances.empty()))
    return planError("QueueGraph plan is incomplete");
  if (!plan.specializationFingerprint.empty() &&
      !isValidFingerprint(plan.specializationFingerprint))
    return planError("QueueGraph specialization fingerprint is invalid");
  if (auto error = verifyPayloadGraph(plan))
    return error;
  llvm::StringMap<const QueueEnumPlan *> enums;
  for (const QueueEnumPlan &enumeration : plan.enums)
    enums[enumeration.name] = &enumeration;
  auto valueWidth = [&](llvm::StringRef type) -> std::optional<uint64_t> {
    if (auto width = integerWidth(type))
      return *width;
    if (std::optional<llvm::StringRef> name = enumTypeName(type)) {
      auto found = enums.find(*name);
      return found == enums.end()
                 ? std::nullopt
                 : std::optional<uint64_t>(found->getValue()->width);
    }
    if (std::optional<llvm::StringRef> name = payloadTypeName(type)) {
      auto payload =
          llvm::find_if(plan.payloads, [&](const QueuePayloadPlan &candidate) {
            return candidate.name == *name;
          });
      if (payload == plan.payloads.end())
        return std::nullopt;
      return llvm::accumulate(
          payload->fields, uint64_t{0},
          [](uint64_t total, const QueuePayloadFieldPlan &field) {
            return total + field.width;
          });
    }
    auto aggregate = llvm::find_if(plan.aggregates,
                                   [&](const QueueAggregatePlan &candidate) {
                                     return candidate.type == type;
                                   });
    return aggregate == plan.aggregates.end()
               ? std::nullopt
               : std::optional<uint64_t>(aggregate->width);
  };
  const bool structured = !plan.definition.empty();
  if (structured && (plan.definitionFingerprint.empty() ||
                     !isValidFingerprint(plan.definitionFingerprint) ||
                     plan.specializationFingerprint.empty()))
    return planError("QueueGraph module specialization metadata is incomplete");
  if (!structured &&
      (!plan.definitionFingerprint.empty() || !plan.interfaceInputs.empty() ||
       !plan.interfaceOutputs.empty() || !plan.moduleInstances.empty() ||
       !plan.moduleSpecializations.empty()))
    return planError("flat QueueGraph cannot carry structured module metadata");
  llvm::StringMap<const QueueGraphPlan *> specializations;
  for (const std::shared_ptr<QueueGraphPlan> &specialization :
       plan.moduleSpecializations) {
    if (!specialization || specialization->definition.empty() ||
        specialization->specializationFingerprint.empty() ||
        !specializations
             .try_emplace(specialization->specializationFingerprint,
                          specialization.get())
             .second)
      return planError(
          "module specialization identities must be complete and unique");
    if (auto error = verifyQueueGraphPlan(*specialization))
      return error;
  }
  if (structured) {
    llvm::DenseSet<uint64_t> lexicalOrders;
    for (const QueueBlockPlan &block : plan.blocks)
      lexicalOrders.insert(block.lexicalOrder);
    for (const QueueModuleInstancePlan &instance : plan.moduleInstances)
      lexicalOrders.insert(instance.lexicalOrder);
    const size_t expected = plan.blocks.size() + plan.moduleInstances.size();
    if (lexicalOrders.size() != expected)
      return planError("structured QueueGraph lexical orders must be unique");
    for (uint64_t order = 0; order < expected; ++order)
      if (!lexicalOrders.contains(order))
        return planError(
            "structured QueueGraph lexical orders must be dense from zero");
  }

  llvm::StringSet<> queueNames;
  llvm::StringMap<const MemoryInstancePlan *> memoryInstances;
  llvm::StringMap<const TablePlan *> tables;
  for (const MemoryInstancePlan &instance : plan.memoryInstances) {
    if (instance.name.empty() ||
        !memoryInstances.try_emplace(instance.name, &instance).second)
      return planError(
          "memory instance identities must be non-empty and unique");
    if (instance.dataType.empty() || instance.entries == 0 ||
        instance.init != 0 || instance.latency == 0 ||
        instance.stableId.empty() || instance.ownerPath.empty())
      return planError("memory instance metadata is incomplete");
  }
  llvm::StringMap<llvm::DenseSet<uint64_t>> endpointOrdinals;
  for (const MemoryRequestPlan &request : plan.memoryRequests) {
    if (!memoryInstances.contains(request.instance))
      return planError("memory request references unknown instance '" +
                       request.instance + "'");
    if (!endpointOrdinals[request.instance].insert(request.ordinal).second)
      return planError("memory request endpoint ordinals must be unique");
  }
  for (const auto &entry : memoryInstances)
    if (!endpointOrdinals.contains(entry.getKey()))
      return planError("memory instance '" + entry.getKey() +
                       "' has no request endpoints");
  for (const auto &entry : endpointOrdinals)
    for (uint64_t ordinal = 0; ordinal < entry.getValue().size(); ++ordinal)
      if (!entry.getValue().contains(ordinal))
        return planError("memory request endpoint ordinals must be contiguous "
                         "from zero");
  for (const TablePlan &table : plan.tables) {
    if (table.name.empty() || !tables.try_emplace(table.name, &table).second)
      return planError("table identities must be non-empty and unique");
    if (table.entryType.empty() || table.entries == 0 || table.init != 0 ||
        table.stableId.empty() || table.ownerPath.empty())
      return planError("table metadata is incomplete");
  }
  llvm::StringMap<const TableMatchPlan *> tableMatches;
  for (const TableMatchPlan &match : plan.tableMatches) {
    const TablePlan *table = tables.lookup(match.table);
    auto width = integerWidth(match.resultType);
    if (match.name.empty() || !table || !width || *width != table->entries ||
        match.resultType.empty() || match.yield.empty() ||
        !tableMatches.try_emplace(match.name, &match).second)
      return planError("table match metadata is incomplete or duplicated");
  }
  llvm::StringMap<const TableSelectionPlan *> tableSelections;
  for (const TableSelectionPlan &selection : plan.tableSelections) {
    const TablePlan *table = tables.lookup(selection.table);
    const TableMatchPlan *match = tableMatches.lookup(selection.match);
    auto indexWidth = integerWidth(selection.indexType);
    const unsigned expectedIndexWidth =
        table ? std::max<unsigned>(1, llvm::Log2_64_Ceil(table->entries)) : 0;
    if (selection.name.empty() || !table || !match || !indexWidth ||
        *indexWidth != expectedIndexWidth || match->table != selection.table ||
        selection.indexType.empty() ||
        (selection.policy != "first" && selection.policy != "min" &&
         selection.policy != "max") ||
        (selection.policy == "first" &&
         (!selection.keyExpressions.empty() || !selection.keyYield.empty())) ||
        (selection.policy != "first" && selection.keyYield.empty()) ||
        !tableSelections.try_emplace(selection.name, &selection).second)
      return planError("table selection metadata is incomplete, duplicated, or "
                       "inconsistent");
  }
  auto verifySharedExpression =
      [&](auto &&self, const QueueExpressionPlan &expression) -> llvm::Error {
    if (expression.kind == "table_match_ref") {
      const TableMatchPlan *match = tableMatches.lookup(expression.field);
      if (!match)
        return planError("table_match_ref references unknown match target");
      if (expression.table != match->table)
        return planError("table_match_ref Table provenance is inconsistent");
      if (expression.type != match->resultType)
        return planError("table_match_ref field type is inconsistent");
    } else if (expression.kind == "table_selection_index_ref" ||
               expression.kind == "table_selection_valid_ref") {
      const TableSelectionPlan *selection =
          tableSelections.lookup(expression.field);
      if (!selection)
        return planError(
            "table_selection_ref references unknown selection target");
      if (expression.table != selection->table)
        return planError(
            "table_selection_ref Table provenance is inconsistent");
      const llvm::StringRef expected =
          expression.kind == "table_selection_index_ref"
              ? llvm::StringRef(selection->indexType)
              : llvm::StringRef("i1");
      if (expression.type != expected)
        return planError("table_selection_ref field type is inconsistent");
    }
    for (const QueueExpressionPlan &nested : expression.nestedExpressions)
      if (auto error = self(self, nested))
        return error;
    return llvm::Error::success();
  };
  auto verifySharedExpressions = [&](const auto &expressions) -> llvm::Error {
    for (const QueueExpressionPlan &expression : expressions)
      if (auto error =
              verifySharedExpression(verifySharedExpression, expression))
        return error;
    return llvm::Error::success();
  };
  for (const TableMatchPlan &match : plan.tableMatches)
    if (auto error = verifySharedExpressions(match.expressions))
      return error;
  for (const TableSelectionPlan &selection : plan.tableSelections)
    if (auto error = verifySharedExpressions(selection.keyExpressions))
      return error;
  llvm::StringMap<unsigned> tableReaders;
  llvm::StringMap<llvm::StringSet<>> tableWriterFields;
  llvm::StringSet<> tableReplaceWriters;
  llvm::StringMap<unsigned> tableFirings;
  std::optional<uint64_t> previousFiringPriority;
  auto verifyWriteFields = [&](llvm::StringRef tableName, llvm::StringRef mode,
                               const std::vector<std::string> &writeFields,
                               bool reserveOwnership = true) {
    const TablePlan *table = tables.lookup(tableName);
    if (!table || writeFields.empty())
      return false;
    llvm::StringSet<> allowed;
    llvm::StringMap<unsigned> ordinals;
    if (llvm::StringRef(table->entryType).starts_with("!ac.struct<")) {
      size_t marker = table->entryType.rfind('@');
      size_t end = table->entryType.rfind('>');
      if (marker == std::string::npos || end == std::string::npos ||
          marker >= end)
        return false;
      llvm::StringRef payloadName(table->entryType.data() + marker + 1,
                                  end - marker - 1);
      auto payload =
          llvm::find_if(plan.payloads, [&](const QueuePayloadPlan &item) {
            return item.name == payloadName;
          });
      if (payload == plan.payloads.end())
        return false;
      for (auto [ordinal, field] : llvm::enumerate(payload->fields)) {
        allowed.insert(field.name);
        ordinals[field.name] = ordinal;
      }
    } else {
      allowed.insert("$entry");
      ordinals["$entry"] = 0;
    }
    llvm::StringSet<> local;
    std::optional<unsigned> previousOrdinal;
    for (const std::string &field : writeFields) {
      if (field.empty() || !allowed.contains(field) ||
          !local.insert(field).second)
        return false;
      unsigned ordinal = ordinals.lookup(field);
      if (previousOrdinal && ordinal <= *previousOrdinal)
        return false;
      previousOrdinal = ordinal;
    }
    if (mode != "field" && mode != "replace")
      return false;
    if (mode == "replace")
      return writeFields.size() == allowed.size() &&
             (!reserveOwnership ||
              tableReplaceWriters.insert(tableName).second);
    if (reserveOwnership)
      for (const std::string &field : writeFields)
        if (!tableWriterFields[tableName].insert(field).second)
          return false;
    return true;
  };
  auto tableFieldCount = [&](const TablePlan &table) -> std::optional<size_t> {
    if (!llvm::StringRef(table.entryType).starts_with("!ac.struct<"))
      return size_t{1};
    size_t marker = table.entryType.rfind('@');
    size_t end = table.entryType.rfind('>');
    if (marker == std::string::npos || end == std::string::npos ||
        marker >= end)
      return std::nullopt;
    llvm::StringRef payloadName(table.entryType.data() + marker + 1,
                                end - marker - 1);
    auto payload =
        llvm::find_if(plan.payloads, [&](const QueuePayloadPlan &item) {
          return item.name == payloadName;
        });
    if (payload == plan.payloads.end())
      return std::nullopt;
    return payload->fields.size();
  };
  for (const TableReadPlan &read : plan.tableReads) {
    if (!tables.contains(read.table) || read.name.empty() ||
        read.output.empty() || read.depth == 0 || read.latency == 0)
      return planError("table read endpoint metadata is incomplete");
    ++tableReaders[read.table];
  }
  for (const TableWritePlan &write : plan.tableWrites) {
    if (!tables.contains(write.table) || write.name.empty())
      return planError("table write endpoint metadata is incomplete");
    if (!verifyWriteFields(write.table, write.mode, write.writeFields))
      return planError(
          "table write_fields are invalid or overlap another writer");
  }
  for (const TableMaskedWritePlan &write : plan.tableMaskedWrites) {
    if (!tables.contains(write.table) || write.name.empty())
      return planError("masked table write endpoint metadata is incomplete");
    if (write.mode != "field" ||
        !verifyWriteFields(write.table, write.mode, write.writeFields))
      return planError(
          "table write_fields are invalid or overlap another writer");
  }
  for (const QueueBlockPlan &block : plan.blocks) {
    if (block.kind != "firing")
      continue;
    if (previousFiringPriority && block.priority <= *previousFiringPriority)
      return planError("firing priorities must follow stable lexical order");
    previousFiringPriority = block.priority;
    if (block.stateWrites.empty() || block.outputs.size() > 1 ||
        block.guard.empty() || block.yields.size() != block.outputs.size() ||
        block.depths.size() != block.outputs.size() ||
        block.latencies.size() != block.outputs.size() ||
        (block.inputs.empty() && block.outputs.empty()))
      return planError("table firing metadata is incomplete or conflicting");
    const bool hasPresence =
        !block.outputPresence.empty() ||
        llvm::any_of(block.stateWrites,
                     [](const auto &write) { return !write.present.empty(); });
    if (hasPresence) {
      if (block.outputPresence.size() != block.outputs.size() ||
          llvm::any_of(block.stateWrites,
                       [](const auto &write) { return write.present.empty(); }))
        return planError("state firing SSA presence metadata is incomplete");
      for (auto [ordinal, output] : llvm::enumerate(block.outputPresence))
        if (output.ordinal != ordinal ||
            output.value != block.yields[ordinal] || output.present.empty())
          return planError("state firing output presence is not canonical");
    }
    llvm::StringMap<const StateWritePlan *> ownerWrites;
    for (const StateWritePlan &write : block.stateWrites) {
      if (!tables.contains(write.table) || write.index.empty() ||
          write.value.empty() || write.mode != "replace" ||
          llvm::any_of(plan.tableWrites,
                       [&](const TableWritePlan &endpoint) {
                         return endpoint.table == write.table;
                       }) ||
          llvm::any_of(plan.tableMaskedWrites,
                       [&](const TableMaskedWritePlan &endpoint) {
                         return endpoint.table == write.table;
                       }) ||
          !verifyWriteFields(write.table, write.mode, write.fields, false))
        return planError("state firing write metadata is invalid");
      auto [position, inserted] = ownerWrites.try_emplace(write.table, &write);
      (void)position;
      if (!inserted)
        return planError(
            "first multi-state firing slice permits one proposal per owner");
      if (inserted)
        ++tableFirings[write.table];
    }
    llvm::StringSet<> reservationOwners;
    for (const StateReservationPlan &reservation : block.stateReservations)
      if (reservationOwners.insert(reservation.table).second)
        ++tableReaders[reservation.table];
  }
  for (const auto &entry : tables)
    if (tableReaders[entry.getKey()] +
            (tableWriterFields.contains(entry.getKey()) ? 1U : 0U) +
            (tableReplaceWriters.contains(entry.getKey()) ? 1U : 0U) +
            tableFirings[entry.getKey()] ==
        0)
      return planError("table '" + entry.getKey() + "' has no endpoints");
  llvm::StringSet<> slotNames;
  for (const SlotPlan &slot : plan.slots)
    if (slot.name.empty() || !slotNames.insert(slot.name).second ||
        slot.payloadType.empty() || slot.input.empty() || slot.scope.empty() ||
        slot.stableId.empty() || slot.ownerPath.empty())
      return planError("slot metadata is incomplete or duplicated");
  llvm::StringMap<unsigned> producers;
  llvm::StringMap<unsigned> consumers;
  llvm::StringMap<unsigned> indegree;
  llvm::StringMap<std::vector<std::string>> successors;
  llvm::StringMap<std::string> queueTypes;
  for (const QueueInterfacePlan &input : plan.interfaceInputs) {
    if (input.name.empty() || input.payloadType.empty() ||
        !queueNames.insert(input.name).second)
      return planError("module input identities must be typed and unique");
    indegree[input.name] = 0;
    queueTypes[input.name] = input.payloadType;
    producers[input.name] = 1;
  }
  for (const QueuePlan &queue : plan.queues) {
    if (queue.name.empty() || !queueNames.insert(queue.name).second)
      return planError("Queue logical identities must be non-empty and unique");
    if (queue.payloadType.empty() || queue.depth == 0 || queue.latency == 0 ||
        queue.rate == 0 || queue.rate > queue.depth)
      return planError(
          "Queue plan requires typed positive depth/latency and rate <= depth");
    indegree[queue.name] = 0;
    queueTypes[queue.name] = queue.payloadType;
  }
  for (const QueueInterfacePlan &output : plan.interfaceOutputs) {
    if (output.name.empty() || output.payloadType.empty() ||
        !queueNames.contains(output.name) ||
        queueTypes.lookup(output.name) != output.payloadType)
      return planError(
          "module output must reference an exact typed local Queue");
    ++consumers[output.name];
  }

  llvm::StringSet<> instanceNames;
  for (const QueueModuleInstancePlan &instance : plan.moduleInstances) {
    const QueueGraphPlan *target =
        specializations.lookup(instance.specializationFingerprint);
    if (instance.name.empty() || !instanceNames.insert(instance.name).second ||
        instance.definition.empty() || !target ||
        target->definition != instance.definition || instance.scope.empty() ||
        instance.inputs.size() != target->interfaceInputs.size() ||
        instance.outputs.size() != target->interfaceOutputs.size())
      return planError(
          "module instance metadata is incomplete or inconsistent");
    for (auto [input, interface] :
         llvm::zip_equal(instance.inputs, target->interfaceInputs)) {
      if (!queueNames.contains(input) ||
          queueTypes.lookup(input) != interface.payloadType)
        return planError("module instance input Queue type is inconsistent");
      ++consumers[input];
    }
    for (auto [output, interface] :
         llvm::zip_equal(instance.outputs, target->interfaceOutputs)) {
      if (!queueNames.contains(output) ||
          queueTypes.lookup(output) != interface.payloadType)
        return planError("module instance output Queue type is inconsistent");
      ++producers[output];
    }
    for (const std::string &input : instance.inputs)
      for (const std::string &output : instance.outputs) {
        successors[input].push_back(output);
        ++indegree[output];
      }
  }

  auto verifyExpressionList =
      [&](auto &&self, const auto &expressions,
          llvm::ArrayRef<std::string> rootTypes) -> llvm::Error {
    llvm::StringMap<std::string> valueTypes;
    for (auto [index, type] : llvm::enumerate(rootTypes))
      valueTypes[index == 0 ? "item" : "item" + std::to_string(index)] = type;
    for (const QueueExpressionPlan &expression : expressions) {
      if (expression.result.empty() || expression.type.empty() ||
          valueTypes.contains(expression.result))
        return planError(
            "expression identities and result types must be closed");
      if (expression.kind == "enum_constant") {
        if (!expression.operands.empty() || expression.field.empty() ||
            expression.literal.empty())
          return planError("enum constant expression contract is malformed");
        std::optional<llvm::StringRef> name = enumTypeName(expression.type);
        auto enumeration = name ? enums.find(*name) : enums.end();
        uint64_t ordinal = 0;
        if (!name || enumeration == enums.end() ||
            llvm::StringRef(expression.literal).getAsInteger(10, ordinal) ||
            ordinal >= enumeration->getValue()->enumerants.size() ||
            enumeration->getValue()->enumerants[ordinal] != expression.field)
          return planError("enum constant expression is inconsistent");
      } else if (expression.kind == "tuple_create" ||
                 expression.kind == "array_create") {
        if (expression.operands.empty() || expression.width == 0)
          return planError("aggregate create expression is malformed");
        auto aggregate = llvm::find_if(
            plan.aggregates, [&](const QueueAggregatePlan &candidate) {
              return candidate.type == expression.type;
            });
        const llvm::StringRef expectedKind =
            expression.kind == "tuple_create" ? "tuple" : "array";
        if (aggregate == plan.aggregates.end() ||
            aggregate->kind != expectedKind)
          return planError("aggregate create result metadata is inconsistent");
        const size_t expectedArity = expression.kind == "tuple_create"
                                         ? aggregate->elements.size()
                                         : aggregate->length;
        if (expression.operands.size() != expectedArity)
          return planError("aggregate create operand arity is inconsistent");
        uint64_t total = 0;
        for (auto [index, operandName] : llvm::enumerate(expression.operands)) {
          auto operand = valueTypes.find(operandName);
          const llvm::StringRef expectedType =
              expression.kind == "tuple_create"
                  ? llvm::StringRef(aggregate->elements[index])
                  : llvm::StringRef(aggregate->elements.front());
          if (operand == valueTypes.end() ||
              operand->getValue() != expectedType)
            return planError("aggregate create operand type is inconsistent");
          auto width = operand == valueTypes.end()
                           ? std::optional<uint64_t>()
                           : valueWidth(operand->getValue());
          if (!width)
            return planError("aggregate operand type has no width");
          total += *width;
        }
        auto resultWidth = valueWidth(expression.type);
        if (!resultWidth || total != expression.width ||
            aggregate->width != expression.width ||
            *resultWidth != expression.width)
          return planError(
              "aggregate create expression widths are inconsistent");
      } else if (expression.kind == "aggregate_get") {
        if (expression.operands.size() != 1 || expression.width == 0)
          return planError("aggregate get expression is malformed");
        auto source = valueTypes.find(expression.operands.front());
        auto aggregate =
            source == valueTypes.end()
                ? plan.aggregates.end()
                : llvm::find_if(plan.aggregates,
                                [&](const QueueAggregatePlan &candidate) {
                                  return candidate.type == source->getValue();
                                });
        auto resultWidth = valueWidth(expression.type);
        if (aggregate == plan.aggregates.end() || !resultWidth ||
            expression.width != *resultWidth)
          return planError("aggregate get expression widths are inconsistent");
        bool exactElement = false;
        if (aggregate->kind == "tuple") {
          uint64_t offset = aggregate->width;
          for (const std::string &element : aggregate->elements) {
            auto width = valueWidth(element);
            if (!width)
              return planError("aggregate element type has no width");
            offset -= *width;
            exactElement |= expression.lsb == offset &&
                            expression.width == *width &&
                            expression.type == element;
          }
        } else if (aggregate->kind == "array") {
          auto width = valueWidth(aggregate->elements.front());
          if (!width)
            return planError("aggregate element type has no width");
          for (uint64_t index = 0; index < aggregate->length; ++index) {
            const uint64_t offset = (aggregate->length - index - 1) * *width;
            exactElement |= expression.lsb == offset &&
                            expression.width == *width &&
                            expression.type == aggregate->elements.front();
          }
        }
        if (!exactElement)
          return planError(
              "aggregate get must select one exact declared element");
      } else if (expression.kind == "masked_match") {
        if (expression.operands.size() != 1 || expression.type != "i1")
          return planError("masked_match expression contract is malformed");
        auto operand = valueTypes.find(expression.operands.front());
        auto inputWidth = operand == valueTypes.end()
                              ? std::optional<unsigned>()
                              : integerWidth(operand->getValue());
        if (!inputWidth || *inputWidth > 64)
          return planError("masked_match input must be an i1..i64 value");
        auto mask = parseExactWidthHex(expression.mask, *inputWidth);
        auto value = parseExactWidthHex(expression.value, *inputWidth);
        if (!mask || !value || (*value & ~*mask) != 0)
          return planError(
              "masked_match mask/value metadata is inconsistent");
      } else if (expression.kind == "priority_index" ||
                 expression.kind == "priority_valid") {
        if (expression.operands.size() != 1 ||
            (expression.predicate != "low" && expression.predicate != "high"))
          return planError("priority expression contract is malformed");
        auto operand = valueTypes.find(expression.operands.front());
        auto inputWidth = operand == valueTypes.end()
                              ? std::optional<unsigned>()
                              : integerWidth(operand->getValue());
        if (!inputWidth || *inputWidth == 0 || *inputWidth > 64)
          return planError(
              "priority expression input must be an i1..i64 value");
        const std::string expected =
            expression.kind == "priority_valid"
                ? "i1"
                : "i" + std::to_string(std::max<unsigned>(
                            1, llvm::Log2_64_Ceil(*inputWidth)));
        if (expression.type != expected)
          return planError("priority expression result type is inconsistent");
      } else if (expression.kind == "popcount") {
        if (expression.operands.size() != 1)
          return planError("popcount expression contract is malformed");
        auto operand = valueTypes.find(expression.operands.front());
        auto inputWidth = operand == valueTypes.end()
                              ? std::optional<unsigned>()
                              : integerWidth(operand->getValue());
        if (!inputWidth || *inputWidth == 0 || *inputWidth > 64)
          return planError("popcount input must be an i1..i64 value");
        const unsigned resultWidth =
            std::max(1u, static_cast<unsigned>(llvm::Log2_64(*inputWidth) + 1));
        if (expression.type != "i" + std::to_string(resultWidth))
          return planError("popcount expression result type is inconsistent");
      } else if (expression.kind == "count_zeros") {
        if (expression.operands.size() != 1)
          return planError("count_zeros expression contract is malformed");
        if (expression.predicate != "leading" &&
            expression.predicate != "trailing")
          return planError("count_zeros direction is malformed");
        auto operand = valueTypes.find(expression.operands.front());
        auto inputWidth = operand == valueTypes.end()
                              ? std::optional<unsigned>()
                              : integerWidth(operand->getValue());
        if (!inputWidth || *inputWidth == 0 || *inputWidth > 64)
          return planError("count_zeros input must be an i1..i64 value");
        const unsigned resultWidth =
            std::max(1u, static_cast<unsigned>(llvm::Log2_64(*inputWidth) + 1));
        if (expression.type != "i" + std::to_string(resultWidth))
          return planError(
              "count_zeros expression result type is inconsistent");
      } else if (expression.kind == "value_select") {
        if (expression.operands.size() != 3)
          return planError("value_select expression contract is malformed");
        auto condition = valueTypes.find(expression.operands[0]);
        auto trueValue = valueTypes.find(expression.operands[1]);
        auto falseValue = valueTypes.find(expression.operands[2]);
        if (condition == valueTypes.end() || condition->getValue() != "i1" ||
            trueValue == valueTypes.end() || falseValue == valueTypes.end() ||
            trueValue->getValue() != expression.type ||
            falseValue->getValue() != expression.type)
          return planError("value_select expression types are inconsistent");
      } else if (expression.kind == "bit_extract") {
        if (expression.operands.size() != 1 || expression.width == 0)
          return planError("bit_extract expression contract is malformed");
        auto input = valueTypes.find(expression.operands.front());
        auto inputWidth = input == valueTypes.end()
                              ? std::optional<unsigned>()
                              : integerWidth(input->getValue());
        auto resultWidth = integerWidth(expression.type);
        if (!inputWidth || !resultWidth || *inputWidth > 64 ||
            expression.lsb + expression.width > *inputWidth ||
            expression.width != *resultWidth)
          return planError("bit_extract expression widths are inconsistent");
      } else if (expression.kind == "bit_concat") {
        if (expression.operands.empty())
          return planError("bit_concat requires at least one operand");
        uint64_t totalWidth = 0;
        for (const std::string &operandName : expression.operands) {
          auto operand = valueTypes.find(operandName);
          auto width = operand == valueTypes.end()
                           ? std::optional<unsigned>()
                           : integerWidth(operand->getValue());
          if (!width || *width == 0 || *width > 64)
            return planError("bit_concat operand width is invalid");
          totalWidth += *width;
        }
        auto resultWidth = integerWidth(expression.type);
        if (!resultWidth || totalWidth == 0 || totalWidth > 64 ||
            totalWidth != *resultWidth)
          return planError("bit_concat result width is inconsistent");
      } else if (expression.kind == "bit_insert") {
        if (expression.operands.size() != 2)
          return planError("bit_insert expression contract is malformed");
        auto base = valueTypes.find(expression.operands[0]);
        auto value = valueTypes.find(expression.operands[1]);
        auto baseWidth = base == valueTypes.end()
                             ? std::optional<unsigned>()
                             : integerWidth(base->getValue());
        auto valueWidth = value == valueTypes.end()
                              ? std::optional<unsigned>()
                              : integerWidth(value->getValue());
        auto resultWidth = integerWidth(expression.type);
        if (!baseWidth || !valueWidth || !resultWidth || *baseWidth > 64 ||
            expression.lsb + *valueWidth > *baseWidth ||
            *resultWidth != *baseWidth)
          return planError("bit_insert expression widths are inconsistent");
      }
      valueTypes[expression.result] = expression.type;
      if (!expression.nestedExpressions.empty()) {
        const TablePlan *table = tables.lookup(expression.table);
        llvm::SmallVector<std::string> nestedRoots;
        if (table)
          nestedRoots.push_back(table->entryType);
        if (auto error = self(self, expression.nestedExpressions, nestedRoots))
          return error;
      }
    }
    return llvm::Error::success();
  };
  auto verifyTableGetConstraints =
      [&](auto &&self, const std::vector<QueueExpressionPlan> &expressions,
          llvm::ArrayRef<std::string> rootTypes) -> llvm::Error {
    llvm::StringMap<std::string> types;
    llvm::StringMap<ValueConstraint> constraints;
    for (auto [index, rootType] : llvm::enumerate(rootTypes)) {
      std::string identity =
          index == 0 ? "item" : "item" + std::to_string(index);
      types[identity] = rootType;
      constraints[identity] = planTypeConstraint(rootType);
    }
    for (const QueueExpressionPlan &expression : expressions) {
      types[expression.result] = expression.type;
      constraints[expression.result] =
          inferPlanConstraint(expression, constraints, types, tables);
      if (expression.kind == "table_get") {
        const TablePlan *table = tables.lookup(expression.table);
        auto index = expression.operands.size() == 1
                         ? constraints.find(expression.operands.front())
                         : constraints.end();
        if (!table || index == constraints.end() || table->entries == 0 ||
            !index->getValue().provesWithin(0, table->entries - 1))
          return planError("Table observation index is not statically safe");
      }
      if (!expression.nestedExpressions.empty()) {
        const TablePlan *table = tables.lookup(expression.table);
        llvm::SmallVector<std::string> nestedRoots;
        if (table)
          nestedRoots.push_back(table->entryType);
        if (auto error = self(self, expression.nestedExpressions, nestedRoots))
          return error;
      }
    }
    return llvm::Error::success();
  };
  for (const TableMatchPlan &match : plan.tableMatches) {
    const TablePlan *table = tables.lookup(match.table);
    llvm::SmallVector<std::string> roots;
    if (table)
      roots.push_back(table->entryType);
    if (auto error = verifyExpressionList(verifyExpressionList,
                                          match.expressions, roots))
      return error;
    if (auto error =
            verifyTableGetConstraints(verifyTableGetConstraints,
                                      match.expressions, roots))
      return error;
  }
  for (const TableSelectionPlan &selection : plan.tableSelections) {
    const TablePlan *table = tables.lookup(selection.table);
    llvm::SmallVector<std::string> roots;
    if (table)
      roots.push_back(table->entryType);
    if (auto error = verifyExpressionList(verifyExpressionList,
                                          selection.keyExpressions, roots))
      return error;
    if (auto error = verifyTableGetConstraints(
            verifyTableGetConstraints, selection.keyExpressions, roots))
      return error;
  }

  for (const QueueBlockPlan &block : plan.blocks) {
    llvm::SmallVector<std::string> roots;
    for (const std::string &input : block.inputs)
      if (auto found = queueTypes.find(input); found != queueTypes.end())
        roots.push_back(found->getValue());
    if (auto error = verifyExpressionList(verifyExpressionList,
                                          block.expressions, roots))
      return error;
    if (auto error = verifySharedExpressions(block.expressions))
      return error;
    if (auto error = verifyTableGetConstraints(
            verifyTableGetConstraints, block.expressions, roots))
      return error;
    llvm::StringMap<std::string> identities;
    llvm::StringMap<ValueConstraint> constraints;
    for (auto [index, rootType] : llvm::enumerate(roots)) {
      std::string identity =
          index == 0 ? "item" : "item" + std::to_string(index);
      identities[identity] = rootType;
      constraints[identity] = planTypeConstraint(rootType);
    }
    for (const QueueExpressionPlan &expression : block.expressions) {
      identities[expression.result] = expression.type;
      constraints[expression.result] =
          inferPlanConstraint(expression, constraints, identities, tables);
    }
    auto verifySafeIndex = [&](llvm::StringRef identity,
                               const TablePlan &table) -> bool {
      auto type = identities.find(identity);
      auto width = type == identities.end() ? std::optional<unsigned>()
                                            : integerWidth(type->getValue());
      if (!width || *width == 0 || *width > 64)
        return false;
      auto constraint = constraints.find(identity);
      return constraint != constraints.end() && table.entries != 0 &&
             constraint->getValue().provesWithin(0, table.entries - 1);
    };
    if (block.kind == "firing") {
      if (!identities.contains(block.guard) ||
          llvm::any_of(block.yields,
                       [&](const std::string &yield) {
                         return !identities.contains(yield);
                       }) ||
          llvm::any_of(block.stateWrites,
                       [&](const StateWritePlan &write) {
                         return !identities.contains(write.index) ||
                                !identities.contains(write.value) ||
                                (!write.present.empty() &&
                                 !identities.contains(write.present));
                       }) ||
          llvm::any_of(block.outputPresence,
                       [&](const OutputPresencePlan &output) {
                         return !identities.contains(output.value) ||
                                !identities.contains(output.present);
                       }))
        return planError("state firing value identities are not closed");
      if (identities.lookup(block.guard) != "i1")
        return planError("state firing functional condition must be i1");
      for (auto [output, yield] : llvm::zip_equal(block.outputs, block.yields))
        if (!queueTypes.contains(output) ||
            queueTypes.lookup(output) != identities.lookup(yield))
          return planError(
              "state firing output Queue and yielded value types must match");
      const bool candidateAlways = llvm::any_of(
          block.expressions, [&](const QueueExpressionPlan &expression) {
            return expression.result == block.guard &&
                   expression.kind == "constant" &&
                   expression.literal == "true";
          });
      auto isBooleanComplement = [&](llvm::StringRef candidate,
                                     llvm::StringRef base) {
        auto compare = llvm::find_if(
            block.expressions, [&](const QueueExpressionPlan &expression) {
              return expression.result == candidate &&
                     expression.kind == "cmp" && expression.predicate == "eq";
            });
        if (compare == block.expressions.end() || compare->operands.size() != 2)
          return false;
        auto isFalse = [&](llvm::StringRef identity) {
          return llvm::any_of(block.expressions,
                              [&](const QueueExpressionPlan &expression) {
                                return expression.result == identity &&
                                       expression.kind == "constant" &&
                                       expression.literal == "false";
                              });
        };
        return (compare->operands[0] == base &&
                isFalse(compare->operands[1])) ||
               (compare->operands[1] == base && isFalse(compare->operands[0]));
      };
      auto areBooleanComplements = [&](llvm::StringRef left,
                                       llvm::StringRef right) {
        return isBooleanComplement(left, right) ||
               isBooleanComplement(right, left);
      };
      llvm::SmallVector<std::string> divergentPresences;
      auto verifyEffectPresence = [&](llvm::StringRef present) {
        if (identities.lookup(present) != "i1")
          return false;
        if (present == block.guard)
          return true;
        if (!candidateAlways || block.inputs.size() != 1)
          return false;
        if (llvm::any_of(divergentPresences, [&](const std::string &value) {
              return value == present;
            }))
          return true;
        if (divergentPresences.size() >= 2 ||
            (!divergentPresences.empty() &&
             !areBooleanComplements(divergentPresences.front(), present)))
          return false;
        divergentPresences.push_back(present.str());
        return true;
      };
      for (const StateWritePlan &write : block.stateWrites) {
        const TablePlan *table = tables.lookup(write.table);
        if (!table || identities.lookup(write.value) != table->entryType ||
            !verifySafeIndex(write.index, *table))
          return planError(
              "state firing proposal type/index is not statically safe");
        if (!write.present.empty() && !verifyEffectPresence(write.present))
          return planError(
              "state firing proposal presence must imply its candidate");
      }
      for (const OutputPresencePlan &output : block.outputPresence)
        if (!verifyEffectPresence(output.present))
          return planError(
              "state firing output presence must imply its candidate");
      for (const StateReservationPlan &reservation : block.stateReservations) {
        const TablePlan *table = tables.lookup(reservation.table);
        if (!table || identities.lookup(reservation.predicate) != "i1")
          return planError("state snapshot reservation is malformed");
        if (table->entries > 64)
          return planError(
              "state snapshot reservation supports at most 64 entries");
        if (!verifyWriteFields(reservation.table, "field", reservation.fields,
                               false))
          return planError("state snapshot reservation fields are invalid");
        std::optional<size_t> fieldCount = tableFieldCount(*table);
        if (!fieldCount || *fieldCount == 0 || *fieldCount > 64 ||
            (reservation.fields.size() != *fieldCount &&
             table->entries * *fieldCount > 64))
          return planError(
              "field-qualified state reservation exceeds relation capacity");
        if (reservation.indexKind == "set") {
          if (!reservation.index.empty() || reservation.source.empty())
            return planError("snapshot-set reservation is malformed");
          auto source = llvm::find_if(
              block.expressions, [&](const QueueExpressionPlan &expression) {
                return expression.result == reservation.source &&
                       (expression.kind == "table_match" ||
                        expression.kind == "table_choose_index");
              });
          if (source == block.expressions.end() ||
              !llvm::any_of(source->nestedExpressions,
                            [&](const QueueExpressionPlan &expression) {
                              return expression.kind == "table_get" &&
                                     expression.table == reservation.table;
                            }))
            return planError(
                "snapshot-set source does not read its target table");
        } else if (reservation.indexKind == "all") {
          if (!reservation.index.empty() || !reservation.source.empty())
            return planError(
                "all-entry state snapshot must not carry an index");
        } else if ((reservation.indexKind != "static" &&
                    reservation.indexKind != "dynamic") ||
                   reservation.index.empty() || !reservation.source.empty() ||
                   !verifySafeIndex(reservation.index, *table)) {
          return planError("state snapshot reservation index is not safe");
        }
        auto containsState = [&](const auto &resources) {
          return llvm::any_of(resources, [&](const auto &resource) {
            return resource.kind == "state" &&
                   resource.resource == reservation.table;
          });
        };
        if (block.hasActivationEvidence &&
            !containsState(block.activationSources))
          return planError("state snapshot owner must be an activation source");
        const bool writable =
            llvm::any_of(block.stateWrites, [&](const StateWritePlan &write) {
              return write.table == reservation.table;
            });
        if (!writable && containsState(block.transactionResources))
          return planError(
              "reservation-only state must not be a transaction resource");
      }
    }
    if (block.kind == "memory_request" &&
        !memoryInstances.contains(block.memoryInstance))
      return planError("memory request block references unknown instance");
    if ((block.kind == "table_read" || block.kind == "table_write" ||
         block.kind == "table_masked_write" || block.kind == "firing") &&
        !tables.contains(block.table))
      return planError("table endpoint block references unknown table");
    if (block.kind == "table_read" || block.kind == "table_write") {
      const TablePlan *table = tables.lookup(block.table);
      const size_t expectedYields = block.kind == "table_read" ? 2 : 3;
      if (!table || block.yields.size() != expectedYields ||
          !verifySafeIndex(block.yields.front(), *table))
        return planError("Table endpoint address is not statically safe");
    }
    if (block.kind == "table_write") {
      auto endpoint =
          llvm::find_if(plan.tableWrites, [&](const TableWritePlan &write) {
            return write.name == block.name && write.table == block.table &&
                   write.scope == block.scope;
          });
      if (endpoint == plan.tableWrites.end() ||
          endpoint->mode != block.writeMode ||
          endpoint->writeFields != block.writeFields)
        return planError("table write block mode/fields are inconsistent");
    }
    if (block.kind == "table_masked_write") {
      auto endpoint = llvm::find_if(
          plan.tableMaskedWrites, [&](const TableMaskedWritePlan &write) {
            return write.name == block.name && write.table == block.table &&
                   write.scope == block.scope;
          });
      if (endpoint == plan.tableMaskedWrites.end() ||
          endpoint->mode != block.writeMode ||
          endpoint->writeFields != block.writeFields)
        return planError(
            "masked table write block mode/fields are inconsistent");
    }
    if (block.kind == "slot" && !slotNames.contains(block.slot))
      return planError("slot block references unknown slot");
    for (const QueueExpressionPlan &expression : block.expressions)
      if (expression.kind == "table_get" && !tables.contains(expression.table))
        return planError("table.get expression references unknown table");
    for (const std::string &input : block.inputs)
      if (!queueNames.contains(input))
        return planError("block input references unknown Queue '" + input +
                         "'");
    for (const std::string &output : block.outputs) {
      if (!queueNames.contains(output))
        return planError("block output references unknown Queue '" + output +
                         "'");
      ++producers[output];
    }
    if (block.kind != "observe" && block.kind != "expect")
      for (const std::string &input : block.inputs)
        ++consumers[input];
    for (const std::string &input : block.inputs)
      for (const std::string &output : block.outputs) {
        successors[input].push_back(output);
        ++indegree[output];
      }
  }

  for (const QueuePlan &queue : plan.queues) {
    if (producers[queue.name] != 1)
      return planError("Queue '" + queue.name +
                       "' must have exactly one producer");
    if (consumers[queue.name] == 0)
      return planError("Queue '" + queue.name +
                       "' has no consuming block; connect ac.sink");
    if (consumers[queue.name] > 1)
      return planError("Queue '" + queue.name +
                       "' has multiple consuming blocks; insert ac.broadcast");
  }
  for (const QueueInterfacePlan &input : plan.interfaceInputs) {
    if (consumers[input.name] == 0)
      return planError("module input Queue '" + input.name +
                       "' has no consuming block");
    if (consumers[input.name] > 1)
      return planError("module input Queue '" + input.name +
                       "' has multiple consuming blocks");
  }

  std::vector<std::string> ready;
  for (const auto &queue : queueNames)
    if (indegree[queue.getKey()] == 0)
      ready.push_back(queue.getKey().str());
  size_t visited = 0;
  for (size_t cursor = 0; cursor < ready.size(); ++cursor) {
    ++visited;
    auto found = successors.find(ready[cursor]);
    if (found == successors.end())
      continue;
    for (const std::string &successor : found->getValue())
      if (--indegree[successor] == 0)
        ready.push_back(successor);
  }
  if (visited != queueNames.size())
    return planError("QueueGraph contains a cycle; represent stateful loops "
                     "with ac.feedback");
  if (!plan.activationEdges.empty() || !plan.workClosureEdges.empty() ||
      !plan.initialActivation.empty()) {
    auto inferredActivation = inferActivation(plan);
    if (!inferredActivation)
      return inferredActivation.takeError();
    if (plan.activationEdges != inferredActivation->wakeEdges ||
        plan.workClosureEdges != inferredActivation->workClosureEdges ||
        plan.initialActivation != inferredActivation->initial)
      return planError("activation, Work closure, and initial frontier must "
                       "equal compiler inference");
  }
  return llvm::Error::success();
}

llvm::Expected<std::string> QueueGraphPlan::canonicalJson() const {
  auto expressionJson =
      [&](auto &&self,
          const QueueExpressionPlan &expression) -> llvm::json::Object {
    llvm::json::Array operands;
    for (const std::string &operand : expression.operands)
      operands.push_back(operand);
    llvm::json::Array nested;
    for (const QueueExpressionPlan &item : expression.nestedExpressions)
      nested.push_back(self(self, item));
    llvm::json::Array nestedYields;
    for (const std::string &yield : expression.nestedYields)
      nestedYields.push_back(yield);
    llvm::json::Object result{{"field", expression.field},
                              {"kind", expression.kind},
                              {"literal", expression.literal},
                              {"nested_expressions", std::move(nested)},
                              {"nested_yields", std::move(nestedYields)},
                              {"operands", std::move(operands)},
                              {"predicate", expression.predicate},
                              {"result", expression.result},
                              {"slot", expression.slot},
                              {"table", expression.table},
                              {"type", expression.type}};
    if (expression.kind == "bit_extract" || expression.kind == "bit_insert" ||
        expression.kind == "aggregate_get")
      result["lsb"] = expression.lsb;
    if (expression.kind == "bit_extract" ||
        expression.kind == "aggregate_get" ||
        expression.kind == "tuple_create" || expression.kind == "array_create")
      result["width"] = expression.width;
    if (expression.kind == "masked_match") {
      result["mask"] = expression.mask;
      result["value"] = expression.value;
    }
    return result;
  };
  llvm::json::Array payloadValues;
  for (const QueuePayloadPlan &payload : payloads) {
    llvm::json::Array fields;
    for (const QueuePayloadFieldPlan &field : payload.fields)
      fields.push_back(llvm::json::Object{
          {"name", field.name}, {"type", field.type}, {"width", field.width}});
    payloadValues.push_back(llvm::json::Object{{"fields", std::move(fields)},
                                               {"name", payload.name}});
  }
  llvm::json::Array enumValues;
  for (const QueueEnumPlan &enumeration : enums) {
    llvm::json::Array enumerants;
    for (const std::string &enumerant : enumeration.enumerants)
      enumerants.push_back(enumerant);
    enumValues.push_back(
        llvm::json::Object{{"enumerants", std::move(enumerants)},
                           {"name", enumeration.name},
                           {"width", enumeration.width}});
  }
  llvm::json::Array aggregateValues;
  for (const QueueAggregatePlan &aggregate : aggregates) {
    llvm::json::Array elements;
    for (const std::string &element : aggregate.elements)
      elements.push_back(element);
    aggregateValues.push_back(
        llvm::json::Object{{"elements", std::move(elements)},
                           {"kind", aggregate.kind},
                           {"length", aggregate.length},
                           {"type", aggregate.type},
                           {"width", aggregate.width}});
  }
  llvm::json::Array scopeValues;
  for (const std::string &scope : scopes)
    scopeValues.push_back(scope);
  llvm::json::Array queueValues;
  for (const QueuePlan &queue : queues)
    queueValues.push_back(
        llvm::json::Object{{"depth", queue.depth},
                           {"latency", queue.latency},
                           {"name", queue.name},
                           {"payload_type", queue.payloadType},
                           {"rate", queue.rate},
                           {"scope", queue.scope}});
  llvm::json::Array blockValues;
  for (const QueueBlockPlan &block : blocks) {
    auto resourceJson = [](const QueueRuleResourcePlan &resource) {
      return llvm::json::Object{{"kind", resource.kind},
                                {"ordinal", resource.ordinal},
                                {"resource", resource.resource}};
    };
    llvm::json::Array activationSources;
    for (const QueueRuleResourcePlan &resource : block.activationSources)
      activationSources.push_back(resourceJson(resource));
    llvm::json::Array transactionResources;
    for (const QueueRuleResourcePlan &resource : block.transactionResources)
      transactionResources.push_back(resourceJson(resource));
    llvm::json::Array inputs;
    for (const std::string &input : block.inputs)
      inputs.push_back(input);
    llvm::json::Array outputs;
    for (const std::string &output : block.outputs)
      outputs.push_back(output);
    llvm::json::Array depths;
    for (uint64_t depth : block.depths)
      depths.push_back(depth);
    llvm::json::Array latencies;
    for (uint64_t latency : block.latencies)
      latencies.push_back(latency);
    llvm::json::Array expressions;
    for (const QueueExpressionPlan &expression : block.expressions)
      expressions.push_back(expressionJson(expressionJson, expression));
    llvm::json::Array yields;
    for (const std::string &yield : block.yields)
      yields.push_back(yield);
    llvm::json::Array writeFields;
    for (const std::string &field : block.writeFields)
      writeFields.push_back(field);
    llvm::json::Array stateWrites;
    for (const StateWritePlan &write : block.stateWrites) {
      llvm::json::Array fields;
      for (const std::string &field : write.fields)
        fields.push_back(field);
      stateWrites.push_back(llvm::json::Object{{"fields", std::move(fields)},
                                               {"index", write.index},
                                               {"mode", write.mode},
                                               {"present", write.present},
                                               {"table", write.table},
                                               {"value", write.value}});
    }
    llvm::json::Array stateReservations;
    for (const StateReservationPlan &reservation : block.stateReservations) {
      llvm::json::Array fields;
      for (const std::string &field : reservation.fields)
        fields.push_back(field);
      stateReservations.push_back(
          llvm::json::Object{{"fields", std::move(fields)},
                             {"index", reservation.index},
                             {"index_kind", reservation.indexKind},
                             {"predicate", reservation.predicate},
                             {"source", reservation.source},
                             {"table", reservation.table}});
    }
    llvm::json::Array outputPresence;
    for (const OutputPresencePlan &output : block.outputPresence)
      outputPresence.push_back(llvm::json::Object{{"ordinal", output.ordinal},
                                                  {"present", output.present},
                                                  {"value", output.value}});
    blockValues.push_back(llvm::json::Object{
        {"activation_sources", std::move(activationSources)},
        {"capacity", block.capacity},
        {"credits", block.credits},
        {"depths", std::move(depths)},
        {"entries", block.entries},
        {"expressions", std::move(expressions)},
        {"inputs", std::move(inputs)},
        {"kind", block.kind},
        {"latencies", std::move(latencies)},
        {"lexical_order", block.lexicalOrder},
        {"max_iterations", block.maxIterations},
        {"message", block.message},
        {"memory_instance", block.memoryInstance},
        {"write_mode", block.writeMode},
        {"table", block.table},
        {"table_index", block.tableIndex},
        {"table_value", block.tableValue},
        {"slot", block.slot},
        {"name", block.name},
        {"no_dependency", block.noDependency},
        {"endpoint_ordinal", block.endpointOrdinal},
        {"outputs", std::move(outputs)},
        {"output_presence", std::move(outputPresence)},
        {"policy", block.policy},
        {"priority", block.priority},
        {"guard", block.guard},
        {"has_activation_evidence", block.hasActivationEvidence},
        {"initially_active", block.initiallyActive},
        {"region", block.region},
        {"result_field", block.resultField},
        {"resources", block.resources},
        {"scope", block.scope},
        {"start", block.start},
        {"state_reservations", std::move(stateReservations)},
        {"state_writes", std::move(stateWrites)},
        {"transaction_resources", std::move(transactionResources)},
        {"init", block.init},
        {"write_fields", std::move(writeFields)},
        {"yields", std::move(yields)}});
  }
  llvm::json::Array memoryInstanceValues;
  for (const MemoryInstancePlan &instance : memoryInstances)
    memoryInstanceValues.push_back(
        llvm::json::Object{{"data_type", instance.dataType},
                           {"entries", instance.entries},
                           {"init", instance.init},
                           {"latency", instance.latency},
                           {"name", instance.name},
                           {"owner_path", instance.ownerPath},
                           {"stable_id", instance.stableId}});
  llvm::json::Array memoryRequestValues;
  for (const MemoryRequestPlan &request : memoryRequests)
    memoryRequestValues.push_back(
        llvm::json::Object{{"depth", request.depth},
                           {"input", request.input},
                           {"instance", request.instance},
                           {"name", request.name},
                           {"ordinal", request.ordinal},
                           {"output", request.output},
                           {"result_field", request.resultField},
                           {"scope", request.scope}});
  llvm::json::Array tableValues;
  for (const TablePlan &table : tables)
    tableValues.push_back(llvm::json::Object{{"entries", table.entries},
                                             {"entry_type", table.entryType},
                                             {"init", table.init},
                                             {"name", table.name},
                                             {"owner_path", table.ownerPath},
                                             {"stable_id", table.stableId}});
  llvm::json::Array tableMatchValues;
  for (const TableMatchPlan &match : tableMatches) {
    llvm::json::Array expressions;
    for (const QueueExpressionPlan &expression : match.expressions)
      expressions.push_back(expressionJson(expressionJson, expression));
    tableMatchValues.push_back(
        llvm::json::Object{{"expressions", std::move(expressions)},
                           {"name", match.name},
                           {"result_type", match.resultType},
                           {"scope", match.scope},
                           {"table", match.table},
                           {"yield", match.yield}});
  }
  llvm::json::Array tableSelectionValues;
  for (const TableSelectionPlan &selection : tableSelections) {
    llvm::json::Array expressions;
    for (const QueueExpressionPlan &expression : selection.keyExpressions)
      expressions.push_back(expressionJson(expressionJson, expression));
    tableSelectionValues.push_back(
        llvm::json::Object{{"index_type", selection.indexType},
                           {"key_expressions", std::move(expressions)},
                           {"key_yield", selection.keyYield},
                           {"match", selection.match},
                           {"name", selection.name},
                           {"policy", selection.policy},
                           {"scope", selection.scope},
                           {"table", selection.table}});
  }
  llvm::json::Array tableReadValues;
  for (const TableReadPlan &read : tableReads)
    tableReadValues.push_back(llvm::json::Object{{"depth", read.depth},
                                                 {"input", read.input},
                                                 {"latency", read.latency},
                                                 {"name", read.name},
                                                 {"output", read.output},
                                                 {"scope", read.scope},
                                                 {"table", read.table}});
  llvm::json::Array tableWriteValues;
  for (const TableWritePlan &write : tableWrites) {
    llvm::json::Array writeFields;
    for (const std::string &field : write.writeFields)
      writeFields.push_back(field);
    tableWriteValues.push_back(
        llvm::json::Object{{"input", write.input},
                           {"mode", write.mode},
                           {"name", write.name},
                           {"scope", write.scope},
                           {"table", write.table},
                           {"write_fields", std::move(writeFields)}});
  }
  llvm::json::Array tableMaskedWriteValues;
  for (const TableMaskedWritePlan &write : tableMaskedWrites) {
    llvm::json::Array writeFields;
    for (const std::string &field : write.writeFields)
      writeFields.push_back(field);
    tableMaskedWriteValues.push_back(
        llvm::json::Object{{"name", write.name},
                           {"mode", write.mode},
                           {"scope", write.scope},
                           {"table", write.table},
                           {"write_fields", std::move(writeFields)}});
  }
  llvm::json::Array slotValues;
  for (const SlotPlan &slot : slots)
    slotValues.push_back(llvm::json::Object{{"input", slot.input},
                                            {"name", slot.name},
                                            {"owner_path", slot.ownerPath},
                                            {"payload_type", slot.payloadType},
                                            {"scope", slot.scope},
                                            {"stable_id", slot.stableId}});
  llvm::json::Array interfaceInputValues;
  for (const QueueInterfacePlan &input : interfaceInputs)
    interfaceInputValues.push_back(llvm::json::Object{
        {"name", input.name}, {"payload_type", input.payloadType}});
  llvm::json::Array interfaceOutputValues;
  for (const QueueInterfacePlan &output : interfaceOutputs)
    interfaceOutputValues.push_back(llvm::json::Object{
        {"name", output.name}, {"payload_type", output.payloadType}});
  llvm::json::Array moduleInstanceValues;
  for (const QueueModuleInstancePlan &instance : moduleInstances) {
    llvm::json::Array inputs;
    for (const std::string &input : instance.inputs)
      inputs.push_back(input);
    llvm::json::Array outputs;
    for (const std::string &output : instance.outputs)
      outputs.push_back(output);
    moduleInstanceValues.push_back(llvm::json::Object{
        {"definition", instance.definition},
        {"inputs", std::move(inputs)},
        {"lexical_order", instance.lexicalOrder},
        {"name", instance.name},
        {"outputs", std::move(outputs)},
        {"scope", instance.scope},
        {"specialization", instance.specializationFingerprint}});
  }
  llvm::json::Array moduleSpecializationValues;
  for (const std::shared_ptr<QueueGraphPlan> &specialization :
       moduleSpecializations) {
    if (!specialization)
      return planError("module specialization plan is null");
    auto serialized = specialization->canonicalJson();
    if (!serialized)
      return serialized.takeError();
    auto parsed = llvm::json::parse(*serialized);
    if (!parsed)
      return planError("module specialization canonical JSON is invalid");
    moduleSpecializationValues.push_back(std::move(*parsed));
  }
  auto activationNodeJson = [](const QueueActivationNodePlan &node) {
    return llvm::json::Object{{"index", node.index},
                              {"kind", activationKindName(node.kind)}};
  };
  llvm::json::Array activationEdgeValues;
  for (const QueueActivationEdgePlan &edge : activationEdges)
    activationEdgeValues.push_back(
        llvm::json::Object{{"source", activationNodeJson(edge.source)},
                           {"target", activationNodeJson(edge.target)}});
  llvm::json::Array workClosureEdgeValues;
  for (const QueueActivationEdgePlan &edge : workClosureEdges)
    workClosureEdgeValues.push_back(
        llvm::json::Object{{"source", activationNodeJson(edge.source)},
                           {"target", activationNodeJson(edge.target)}});
  llvm::json::Array initialActivationValues;
  for (const QueueActivationNodePlan &node : initialActivation)
    initialActivationValues.push_back(activationNodeJson(node));
  llvm::json::Object root{
      {"activation_edges", std::move(activationEdgeValues)},
      {"aggregates", std::move(aggregateValues)},
      {"blocks", std::move(blockValues)},
      {"contract_epoch", "0.5"},
      {"definition", definition.empty() ? llvm::json::Value(nullptr)
                                        : llvm::json::Value(definition)},
      {"definition_fingerprint",
       definitionFingerprint.empty()
           ? llvm::json::Value(nullptr)
           : llvm::json::Value(definitionFingerprint)},
      {"enums", std::move(enumValues)},
      {"interface_inputs", std::move(interfaceInputValues)},
      {"interface_outputs", std::move(interfaceOutputValues)},
      {"initial_activation", std::move(initialActivationValues)},
      {"memory_instances", std::move(memoryInstanceValues)},
      {"memory_requests", std::move(memoryRequestValues)},
      {"module_instances", std::move(moduleInstanceValues)},
      {"module_specializations", std::move(moduleSpecializationValues)},
      {"payloads", std::move(payloadValues)},
      {"queues", std::move(queueValues)},
      {"schema", "agentic-circuit-queue-graph-plan"},
      {"scopes", std::move(scopeValues)},
      {"slots", std::move(slotValues)},
      {"specialization", specializationFingerprint.empty()
                             ? llvm::json::Value(nullptr)
                             : llvm::json::Value(specializationFingerprint)},
      {"table_reads", std::move(tableReadValues)},
      {"table_matches", std::move(tableMatchValues)},
      {"table_masked_writes", std::move(tableMaskedWriteValues)},
      {"table_selections", std::move(tableSelectionValues)},
      {"table_writes", std::move(tableWriteValues)},
      {"tables", std::move(tableValues)},
      {"system", system},
      {"version", "0.5"}};
  root["work_closure_edges"] = std::move(workClosureEdgeValues);
  return bindings::canonicalizeJson(llvm::json::Value(std::move(root)));
}

} // namespace acir::codegen
