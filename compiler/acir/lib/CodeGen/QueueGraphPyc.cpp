#include "acir/CodeGen/QueueGraphPyc.h"
#include "acir/CodeGen/QueueBlockContract.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <bit>
#include <sstream>
#include <system_error>

namespace acir::codegen {
namespace {

llvm::Error pycError(const llvm::Twine &message) {
  return llvm::createStringError(
      std::make_error_code(std::errc::invalid_argument),
      "ACLOWER-PYC: " + message);
}

const QueuePayloadPlan *findPayload(const QueueGraphPlan &plan,
                                    llvm::StringRef type) {
  constexpr llvm::StringLiteral prefix = "!ac.struct<@types::@";
  if (!type.starts_with(prefix) || !type.ends_with('>'))
    return nullptr;
  llvm::StringRef name = type.drop_front(prefix.size()).drop_back();
  auto found =
      std::find_if(plan.payloads.begin(), plan.payloads.end(),
                   [&](const auto &payload) { return payload.name == name; });
  return found == plan.payloads.end() ? nullptr : &*found;
}

const QueueEnumPlan *findEnum(const QueueGraphPlan &plan, llvm::StringRef type) {
  constexpr llvm::StringLiteral prefix = "!ac.enum<@types::@";
  if (!type.starts_with(prefix) || !type.ends_with('>'))
    return nullptr;
  llvm::StringRef name = type.drop_front(prefix.size()).drop_back();
  auto found = std::find_if(
      plan.enums.begin(), plan.enums.end(),
      [&](const auto &enumeration) { return enumeration.name == name; });
  return found == plan.enums.end() ? nullptr : &*found;
}

const QueueAggregatePlan *findAggregate(const QueueGraphPlan &plan,
                                        llvm::StringRef type) {
  auto found = std::find_if(
      plan.aggregates.begin(), plan.aggregates.end(),
      [&](const auto &aggregate) { return aggregate.type == type; });
  return found == plan.aggregates.end() ? nullptr : &*found;
}

llvm::Expected<unsigned> typeWidth(const QueueGraphPlan &plan,
                                   llvm::StringRef type) {
  if (type.starts_with('i')) {
    unsigned width = 0;
    if (!type.drop_front().getAsInteger(10, width) && width > 0)
      return width;
  }
  if (const QueueEnumPlan *enumeration = findEnum(plan, type))
    return static_cast<unsigned>(enumeration->width);
  if (const QueueAggregatePlan *aggregate = findAggregate(plan, type))
    return static_cast<unsigned>(aggregate->width);
  const QueuePayloadPlan *payload = findPayload(plan, type);
  if (!payload)
    return pycError("unsupported PYC payload type '" + type + "'");
  unsigned total = 0;
  for (const QueuePayloadFieldPlan &field : payload->fields) {
    if (field.width == 0)
      return pycError("packed payload field width must be positive");
    total += field.width;
  }
  if (total == 0)
    return pycError("packed payload width must be positive");
  return total;
}

llvm::Expected<std::string> pycType(const QueueGraphPlan &plan,
                                    llvm::StringRef type) {
  auto width = typeWidth(plan, type);
  if (!width)
    return width.takeError();
  return "i" + std::to_string(*width);
}

struct FieldLayout {
  unsigned lsb = 0;
  unsigned width = 0;
  std::string type;
};

llvm::Expected<FieldLayout> fieldLayout(const QueueGraphPlan &plan,
                                        llvm::StringRef recordType,
                                        llvm::StringRef fieldName) {
  const QueuePayloadPlan *payload = findPayload(plan, recordType);
  if (!payload)
    return pycError("field access requires a packed struct payload");
  auto total = typeWidth(plan, recordType);
  if (!total)
    return total.takeError();
  unsigned cursor = *total;
  for (const QueuePayloadFieldPlan &field : payload->fields) {
    auto width = typeWidth(plan, field.type);
    if (!width)
      return width.takeError();
    cursor -= *width;
    if (field.name == fieldName)
      return FieldLayout{cursor, *width, field.type};
  }
  return pycError("unknown packed struct field '" + fieldName + "'");
}

const QueuePlan *findQueue(const QueueGraphPlan &plan, llvm::StringRef name) {
  auto found =
      std::find_if(plan.queues.begin(), plan.queues.end(),
                   [&](const QueuePlan &queue) { return queue.name == name; });
  return found == plan.queues.end() ? nullptr : &*found;
}

llvm::Expected<std::string> yieldedType(const QueueBlockPlan &block,
                                        llvm::StringRef yield,
                                        llvm::StringRef inputType) {
  if (yield == "item")
    return inputType.str();
  auto found = std::find_if(block.expressions.begin(), block.expressions.end(),
                            [&](const QueueExpressionPlan &expression) {
                              return expression.result == yield;
                            });
  if (found == block.expressions.end())
    return pycError("yield references unknown expression value");
  return found->type;
}

llvm::Expected<std::string>
emitTransform(const QueueGraphPlan &plan, const QueueBlockPlan &block,
              llvm::ArrayRef<std::string> inputData,
              llvm::ArrayRef<std::string> inputTypes, size_t yieldIndex,
              unsigned &nextValue, std::ostringstream &body) {
  if (inputData.size() != inputTypes.size() || inputData.empty())
    return pycError("transform input data/type arity mismatch");
  llvm::StringMap<std::string> values;
  llvm::StringMap<std::string> types;
  llvm::StringMap<std::pair<std::string, std::string>> priorityValues;
  for (size_t index = 0; index < inputData.size(); ++index) {
    std::string name = index == 0 ? "item" : "item" + std::to_string(index);
    values[name] = inputData[index];
    types[name] = inputTypes[index];
  }
  auto newValue = [&]() { return "%v" + std::to_string(nextValue++); };
  auto value = [&](llvm::StringRef name) -> llvm::Expected<std::string> {
    auto found = values.find(name);
    if (found == values.end())
      return pycError("transform expression references unknown value '" + name +
                      "'");
    return found->getValue();
  };
  auto valueType = [&](llvm::StringRef name) -> llvm::Expected<std::string> {
    auto found = types.find(name);
    if (found == types.end())
      return pycError("transform value has no type: '" + name + "'");
    return found->getValue();
  };
  for (const QueueExpressionPlan &expression : block.expressions) {
    std::string result;
    if (expression.kind == "enum_constant") {
      result = newValue();
      auto type = pycType(plan, expression.type);
      if (!type)
        return type.takeError();
      body << "    " << result << " = pyc.constant " << expression.literal
           << " : " << *type << "\n";
    } else if (expression.kind == "constant") {
      result = newValue();
      llvm::StringRef literal = expression.literal;
      auto type = pycType(plan, expression.type);
      if (!type)
        return type.takeError();
      body << "    " << result << " = pyc.constant "
           << literal.split(" : ").first.str() << " : " << *type << "\n";
    } else {
      if (expression.operands.empty())
        return pycError("transform expression operand is missing");
      auto first = value(expression.operands[0]);
      if (!first)
        return first.takeError();
      if (expression.kind == "masked_match") {
        if (expression.operands.size() != 1)
          return pycError("matches expression arity mismatch");
        auto inputType = valueType(expression.operands[0]);
        if (!inputType)
          return inputType.takeError();
        auto type = pycType(plan, *inputType);
        if (!type)
          return type.takeError();
        std::string mask = newValue();
        std::string masked = newValue();
        std::string expected = newValue();
        result = newValue();
        body << "    " << mask << " = pyc.constant " << expression.mask
             << " : " << *type << "\n";
        body << "    " << masked << " = pyc.and " << *first << ", " << mask
             << " : " << *type << ", " << *type << " -> " << *type
             << "\n";
        body << "    " << expected << " = pyc.constant " << expression.value
             << " : " << *type << "\n";
        body << "    " << result << " = pyc.eq " << masked << ", "
             << expected << " : " << *type << ", " << *type
             << " -> i1\n";
      } else if (expression.kind == "priority_index" ||
          expression.kind == "priority_valid") {
        if (expression.operands.size() != 1 ||
            (expression.predicate != "low" && expression.predicate != "high"))
          return pycError("priority encoder expression contract is malformed");
        auto inputType = valueType(expression.operands[0]);
        if (!inputType)
          return inputType.takeError();
        auto sourceType = pycType(plan, *inputType);
        auto inputWidth = typeWidth(plan, *inputType);
        if (!sourceType)
          return sourceType.takeError();
        if (!inputWidth)
          return inputWidth.takeError();
        unsigned indexWidth =
            std::max(1u, static_cast<unsigned>(std::bit_width(
                             static_cast<unsigned>(*inputWidth - 1))));
        std::string key = expression.operands[0] + "#" + expression.predicate;
        auto found = priorityValues.find(key);
        if (found == priorityValues.end()) {
          std::string encodedIndex = newValue();
          std::string encodedValid = newValue();
          body << "    " << encodedIndex << ", " << encodedValid
               << " = pyc.priority_encode " << *first << " {order = \""
               << expression.predicate << "\"} : " << *sourceType << " -> i"
               << indexWidth << ", i1\n";
          found =
              priorityValues.try_emplace(key, encodedIndex, encodedValid).first;
        }
        result = expression.kind == "priority_index" ? found->getValue().first
                                                     : found->getValue().second;
      } else if (expression.kind == "popcount") {
        if (expression.operands.size() != 1)
          return pycError("popcount expression arity mismatch");
        auto inputType = valueType(expression.operands[0]);
        if (!inputType)
          return inputType.takeError();
        auto sourceType = pycType(plan, *inputType);
        auto resultType = pycType(plan, expression.type);
        if (!sourceType)
          return sourceType.takeError();
        if (!resultType)
          return resultType.takeError();
        result = newValue();
        body << "    " << result << " = pyc.popcount " << *first << " : "
             << *sourceType << " -> " << *resultType << "\n";
      } else if (expression.kind == "count_zeros") {
        if (expression.operands.size() != 1)
          return pycError("count_zeros expression arity mismatch");
        auto inputType = valueType(expression.operands[0]);
        if (!inputType)
          return inputType.takeError();
        auto sourceType = pycType(plan, *inputType);
        auto resultType = pycType(plan, expression.type);
        if (!sourceType)
          return sourceType.takeError();
        if (!resultType)
          return resultType.takeError();
        result = newValue();
        body << "    " << result << " = pyc.count_zeros " << *first
             << " {direction = \"" << expression.predicate
             << "\"} : " << *sourceType << " -> " << *resultType << "\n";
      } else if (expression.kind == "bit_extract" ||
                 expression.kind == "aggregate_get") {
        if (expression.operands.size() != 1 || expression.width == 0)
          return pycError("extract expression contract is malformed");
        auto inputType = valueType(expression.operands[0]);
        auto sourceType =
            inputType ? pycType(plan, *inputType)
                      : llvm::Expected<std::string>(inputType.takeError());
        auto resultType = pycType(plan, expression.type);
        if (!sourceType)
          return sourceType.takeError();
        if (!resultType)
          return resultType.takeError();
        result = newValue();
        body << "    " << result << " = pyc.extract " << *first
             << " {lsb = " << expression.lsb << "} : " << *sourceType << " -> "
             << *resultType << "\n";
      } else if (expression.kind == "bit_concat" ||
                 expression.kind == "tuple_create" ||
                 expression.kind == "array_create") {
        if (expression.operands.empty())
          return pycError("concat/aggregate create requires at least one operand");
        result = newValue();
        body << "    " << result << " = pyc.concat(";
        for (auto [index, operandName] : llvm::enumerate(expression.operands)) {
          auto operandValue = value(operandName);
          if (!operandValue)
            return operandValue.takeError();
          if (index)
            body << ", ";
          body << *operandValue;
        }
        body << ") : (";
        for (auto [index, operandName] : llvm::enumerate(expression.operands)) {
          auto operandType = valueType(operandName);
          auto type =
              operandType
                  ? pycType(plan, *operandType)
                  : llvm::Expected<std::string>(operandType.takeError());
          if (!type)
            return type.takeError();
          if (index)
            body << ", ";
          body << *type;
        }
        auto resultType = pycType(plan, expression.type);
        if (!resultType)
          return resultType.takeError();
        body << ") -> " << *resultType << "\n";
      } else if (expression.kind == "bit_insert") {
        if (expression.operands.size() != 2)
          return pycError("bit_insert expression contract is malformed");
        auto inserted = value(expression.operands[1]);
        auto insertedType = valueType(expression.operands[1]);
        auto baseWidth = typeWidth(plan, expression.type);
        auto valueWidth =
            insertedType ? typeWidth(plan, *insertedType)
                         : llvm::Expected<unsigned>(insertedType.takeError());
        if (!inserted)
          return inserted.takeError();
        if (!baseWidth)
          return baseWidth.takeError();
        if (!valueWidth)
          return valueWidth.takeError();
        std::vector<std::pair<std::string, unsigned>> parts;
        const unsigned highWidth =
            *baseWidth - static_cast<unsigned>(expression.lsb) - *valueWidth;
        if (highWidth > 0) {
          std::string high = newValue();
          body << "    " << high << " = pyc.extract " << *first
               << " {lsb = " << expression.lsb + *valueWidth << "} : i"
               << *baseWidth << " -> i" << highWidth << "\n";
          parts.emplace_back(std::move(high), highWidth);
        }
        parts.emplace_back(*inserted, *valueWidth);
        if (expression.lsb > 0) {
          std::string low = newValue();
          body << "    " << low << " = pyc.extract " << *first
               << " {lsb = 0} : i" << *baseWidth << " -> i" << expression.lsb
               << "\n";
          parts.emplace_back(std::move(low), expression.lsb);
        }
        if (parts.size() == 1) {
          result = parts.front().first;
        } else {
          result = newValue();
          body << "    " << result << " = pyc.concat(";
          for (auto [index, part] : llvm::enumerate(parts)) {
            if (index)
              body << ", ";
            body << part.first;
          }
          body << ") : (";
          for (auto [index, part] : llvm::enumerate(parts)) {
            if (index)
              body << ", ";
            body << 'i' << part.second;
          }
          body << ") -> i" << *baseWidth << "\n";
        }
      } else if (expression.kind == "value_select") {
        if (expression.operands.size() != 3)
          return pycError("value_select expression arity mismatch");
        auto trueValue = value(expression.operands[1]);
        auto falseValue = value(expression.operands[2]);
        auto type = pycType(plan, expression.type);
        if (!trueValue)
          return trueValue.takeError();
        if (!falseValue)
          return falseValue.takeError();
        if (!type)
          return type.takeError();
        result = newValue();
        body << "    " << result << " = pyc.mux " << *first << ", "
             << *trueValue << ", " << *falseValue << " : i1, " << *type << ", "
             << *type << " -> " << *type << "\n";
      } else if (expression.kind == "not") {
        if (expression.operands.size() != 1)
          return pycError("unary transform expression arity mismatch");
        auto type = pycType(plan, expression.type);
        if (!type)
          return type.takeError();
        result = newValue();
        body << "    " << result << " = pyc.not " << *first << " : " << *type
             << "\n";
      } else if (expression.kind == "add" || expression.kind == "sub" ||
                 expression.kind == "mul" || expression.kind == "and" ||
                 expression.kind == "or" || expression.kind == "xor") {
        result = newValue();
        if (expression.operands.size() != 2)
          return pycError("binary transform expression arity mismatch");
        auto second = value(expression.operands[1]);
        if (!second)
          return second.takeError();
        auto type = pycType(plan, expression.type);
        if (!type)
          return type.takeError();
        body << "    " << result << " = pyc." << expression.kind << ' '
             << *first << ", " << *second << " : " << *type << ", " << *type
             << " -> " << *type << "\n";
      } else if (expression.kind == "shl" || expression.kind == "shr") {
        result = newValue();
        if (expression.operands.size() != 2)
          return pycError("shift transform expression arity mismatch");
        auto second = value(expression.operands[1]);
        if (!second)
          return second.takeError();
        auto type = pycType(plan, expression.type);
        if (!type)
          return type.takeError();
        body << "    " << result << " = pyc."
             << (expression.kind == "shl" ? "shl" : "lshr") << ' ' << *first
             << ", " << *second << " : " << *type << ", " << *type << "\n";
      } else if (expression.kind == "cmp") {
        if (expression.operands.size() != 2)
          return pycError("comparison expression arity mismatch");
        auto second = value(expression.operands[1]);
        auto firstType = valueType(expression.operands[0]);
        auto secondType = valueType(expression.operands[1]);
        if (!second)
          return second.takeError();
        if (!firstType)
          return firstType.takeError();
        if (!secondType)
          return secondType.takeError();
        if (*firstType != *secondType)
          return pycError("comparison operand types must match");
        auto type = pycType(plan, *firstType);
        if (!type)
          return type.takeError();

        llvm::StringRef opcode;
        std::string lhs = *first;
        std::string rhs = *second;
        bool negate = false;
        if (expression.predicate == "eq" || expression.predicate == "ne") {
          opcode = "eq";
          negate = expression.predicate == "ne";
        } else if (expression.predicate == "slt" ||
                   expression.predicate == "sge" ||
                   expression.predicate == "ult" ||
                   expression.predicate == "uge") {
          opcode = expression.predicate.starts_with("u") ? "ult" : "slt";
          negate = expression.predicate.ends_with("ge");
        } else if (expression.predicate == "sgt" ||
                   expression.predicate == "sle" ||
                   expression.predicate == "ugt" ||
                   expression.predicate == "ule") {
          opcode = expression.predicate.starts_with("u") ? "ult" : "slt";
          std::swap(lhs, rhs);
          negate = expression.predicate.ends_with("le");
        } else {
          return pycError("unsupported comparison predicate");
        }
        std::string compared = newValue();
        body << "    " << compared << " = pyc." << opcode.str() << ' ' << lhs
             << ", " << rhs << " : " << *type << ", " << *type << " -> i1\n";
        if (negate) {
          result = newValue();
          body << "    " << result << " = pyc.not " << compared << " : i1\n";
        } else {
          result = std::move(compared);
        }
      } else if (expression.kind == "get") {
        auto recordType = valueType(expression.operands[0]);
        if (!recordType)
          return recordType.takeError();
        auto layout = fieldLayout(plan, *recordType, expression.field);
        auto sourceType = pycType(plan, *recordType);
        auto resultType = pycType(plan, expression.type);
        if (!layout)
          return layout.takeError();
        if (!sourceType)
          return sourceType.takeError();
        if (!resultType)
          return resultType.takeError();
        result = newValue();
        body << "    " << result << " = pyc.extract " << *first
             << " {lsb = " << layout->lsb << "} : " << *sourceType << " -> "
             << *resultType << "\n";
      } else if (expression.kind == "with") {
        if (expression.operands.size() != 2)
          return pycError("packed field update arity mismatch");
        auto second = value(expression.operands[1]);
        auto recordType = valueType(expression.operands[0]);
        if (!second)
          return second.takeError();
        if (!recordType)
          return recordType.takeError();
        auto layout = fieldLayout(plan, *recordType, expression.field);
        auto totalWidth = typeWidth(plan, *recordType);
        if (!layout)
          return layout.takeError();
        if (!totalWidth)
          return totalWidth.takeError();
        std::vector<std::pair<std::string, unsigned>> parts;
        const unsigned highWidth = *totalWidth - layout->lsb - layout->width;
        if (highWidth > 0) {
          std::string high = newValue();
          body << "    " << high << " = pyc.extract " << *first
               << " {lsb = " << layout->lsb + layout->width << "} : i"
               << *totalWidth << " -> i" << highWidth << "\n";
          parts.emplace_back(std::move(high), highWidth);
        }
        parts.emplace_back(*second, layout->width);
        if (layout->lsb > 0) {
          std::string low = newValue();
          body << "    " << low << " = pyc.extract " << *first
               << " {lsb = 0} : i" << *totalWidth << " -> i" << layout->lsb
               << "\n";
          parts.emplace_back(std::move(low), layout->lsb);
        }
        if (parts.size() == 1) {
          result = parts.front().first;
        } else {
          result = newValue();
          body << "    " << result << " = pyc.concat(";
          for (auto [index, part] : llvm::enumerate(parts)) {
            if (index)
              body << ", ";
            body << part.first;
          }
          body << ") : (";
          for (auto [index, part] : llvm::enumerate(parts)) {
            if (index)
              body << ", ";
            body << 'i' << part.second;
          }
          body << ") -> i" << *totalWidth << "\n";
        }
      } else {
        return pycError("unsupported PYC transform expression");
      }
    }
    values[expression.result] = result;
    types[expression.result] = expression.type;
  }
  if (yieldIndex >= block.yields.size())
    return pycError("transform yield index is outside result arity");
  return value(block.yields[yieldIndex]);
}

constexpr llvm::StringLiteral kStructMetrics =
    "{\\\"ast_node_count\\\":0,\\\"collection_count\\\":0,"
    "\\\"collection_instance_count\\\":0,"
    "\\\"estimated_inline_cost\\\":0,\\\"hardware_call_count\\\":0,"
    "\\\"instance_count\\\":0,\\\"loop_count\\\":0,"
    "\\\"module_call_count\\\":0,"
    "\\\"module_family_collection_count\\\":0,"
    "\\\"repeat_pressure\\\":0,\\\"repeated_body_clusters\\\":[],"
    "\\\"source_loc\\\":0,\\\"state_alloc_count\\\":0,"
    "\\\"state_call_count\\\":0}";

} // namespace

llvm::Expected<std::string> generateQueueGraphPyc(const QueueGraphPlan &plan) {
  if (!plan.definition.empty())
    return pycError(
        "module-preserving QueueGraph PYC lowering is not implemented");
  if (!plan.tables.empty() || !plan.slots.empty())
    return pycError("unsupported provisional Table: PYC lowering is deferred");
  if (!plan.scopes.empty()) {
    const QueueBlockContract *scope = findQueueBlockContract("scope");
    if (!scope || !scope->pycAvailable)
      return pycError("official opcode has no PYC lowering: 'scope'");
  }
  struct TransformProducer {
    const QueueBlockPlan *block = nullptr;
    size_t index = 0;
  };
  struct RouteProducer {
    const QueueBlockPlan *block = nullptr;
    size_t index = 0;
  };
  struct SelectState {
    std::vector<std::string> conditions;
    std::string controlValid;
    std::string selectedValid;
    std::string selectorSafe;
  };
  struct MergeState {
    std::string nextWire;
    std::string enableWire;
    std::string cursor;
    std::string valid;
    std::string type;
  };
  struct ForkState {
    std::vector<std::string> nextWires;
    std::vector<std::string> enableWires;
    std::vector<std::string> delivered;
  };
  struct FeedbackState {
    std::string validNext;
    std::string validEnable;
    std::string valid;
    std::string dataNext;
    std::string dataEnable;
    std::string data;
    std::string iterationNext;
    std::string iterationEnable;
    std::string iteration;
    std::string selectedValid;
    std::string selectedIteration;
    std::string condition;
    std::string updated;
    std::string underLimit;
    std::string dataType;
    std::string iterationType;
  };
  struct ReorderSlotState {
    std::string next;
    std::string enable;
    std::string state;
    std::string valid;
    std::string key;
    std::string data;
    std::string free;
    std::string match;
  };
  struct ReorderState {
    std::vector<ReorderSlotState> slots;
    std::string expectedNext;
    std::string expectedEnable;
    std::string expected;
    std::string inputKey;
    std::string anyFree;
    std::string freeIndex;
    std::string freeIndexType;
    std::string outputMatch;
    std::string safeAdmission;
    std::string keyType;
    std::string dataType;
    std::string slotType;
  };
  struct DependencySlotState {
    std::string next;
    std::string enable;
    std::string state;
    std::string valid;
    std::string phase;
    std::string key;
    std::string predecessor;
    std::string resource;
    std::string remaining;
    std::string cost;
    std::string data;
    std::string free;
    std::string done;
  };
  struct DependencyState {
    std::vector<DependencySlotState> slots;
    std::string inputKey;
    std::string inputPredecessor;
    std::string inputResource;
    std::string inputCost;
    std::string anyFree;
    std::string freeIndex;
    std::string freeIndexType;
    std::string outputDone;
    std::string doneIndex;
    std::string safeAdmission;
    std::string keyType;
    std::string resourceType;
    std::string costType;
    std::string dataType;
    std::string slotType;
    uint64_t noDependency = 0;
    uint64_t resources = 0;
  };
  struct CreditSlotState {
    std::string next;
    std::string enable;
    std::string state;
    std::string valid;
    std::string remaining;
    std::string data;
    std::string free;
    std::string done;
  };
  struct CreditState {
    std::vector<CreditSlotState> slots;
    std::string inputCost;
    std::string anyFree;
    std::string freeIndex;
    std::string freeIndexType;
    std::string outputDone;
    std::string doneIndex;
    std::string safeAdmission;
    std::string costType;
    std::string dataType;
    std::string slotType;
  };
  std::vector<const QueueBlockPlan *> sources;
  std::vector<const QueueBlockPlan *> sinks;
  std::vector<const QueueBlockPlan *> observations;
  llvm::StringMap<TransformProducer> transformByOutput;
  llvm::StringMap<TransformProducer> barrierByOutput;
  llvm::StringMap<const QueueBlockPlan *> broadcastByOutput;
  llvm::StringMap<const QueueBlockPlan *> forkByOutput;
  llvm::StringMap<RouteProducer> routeByOutput;
  llvm::StringMap<const QueueBlockPlan *> selectByOutput;
  llvm::StringMap<const QueueBlockPlan *> mergeByOutput;
  llvm::StringMap<const QueueBlockPlan *> creditByOutput;
  llvm::StringMap<const QueueBlockPlan *> memoryByOutput;
  llvm::StringMap<const QueueBlockPlan *> dependencyByOutput;
  llvm::StringMap<const QueueBlockPlan *> reorderByOutput;
  llvm::StringMap<const QueueBlockPlan *> feedbackByOutput;
  for (const QueueBlockPlan &block : plan.blocks) {
    const QueueBlockContract *contract = findQueueBlockContract(block.kind);
    if (contract && contract->role == "verification")
      return pycError("verification-only opcode '" + contract->operation +
                      "' cannot appear in a design hierarchy; place it at "
                      "the PYC testbench boundary");
    if (!contract || !contract->pycAvailable)
      return pycError("official opcode has no PYC lowering: '" + block.kind +
                      "'");
    if (block.kind == "source")
      sources.push_back(&block);
    else if (block.kind == "sink")
      sinks.push_back(&block);
    else if (block.kind == "observe")
      observations.push_back(&block);
    else if (block.kind == "transform") {
      if (block.inputs.empty() || block.outputs.empty() ||
          block.yields.size() != block.outputs.size())
        return pycError("transform output arity is unsupported");
      for (auto [index, output] : llvm::enumerate(block.outputs))
        transformByOutput[output] = TransformProducer{&block, index};
    } else if (block.kind == "route") {
      if (block.inputs.size() != 1 || block.outputs.size() < 2)
        return pycError("route arity is unsupported");
      for (auto [index, output] : llvm::enumerate(block.outputs))
        routeByOutput[output] = RouteProducer{&block, index};
    } else if (block.kind == "select") {
      if (block.inputs.size() < 3 || block.outputs.size() != 1 ||
          block.yields.size() != 1)
        return pycError("select contract is unsupported");
      selectByOutput[block.outputs.front()] = &block;
    } else if (block.kind == "broadcast") {
      if (block.inputs.size() != 1 || block.outputs.size() < 2)
        return pycError("broadcast arity is unsupported");
      for (const std::string &output : block.outputs)
        broadcastByOutput[output] = &block;
    } else if (block.kind == "fork") {
      if (block.inputs.size() != 1 || block.outputs.size() < 2)
        return pycError("fork arity is unsupported");
      for (const std::string &output : block.outputs)
        forkByOutput[output] = &block;
    } else if (block.kind == "merge") {
      if (block.outputs.size() != 1 || block.inputs.size() < 2)
        return pycError("merge arity is unsupported");
      if (block.policy != "priority" && block.policy != "round_robin")
        return pycError("PYC merge policy must be priority or round_robin");
      mergeByOutput[block.outputs.front()] = &block;
    } else if (block.kind == "barrier") {
      if (block.inputs.size() < 2 ||
          block.outputs.size() != block.inputs.size())
        return pycError("barrier contract is unsupported");
      for (auto [index, output] : llvm::enumerate(block.outputs))
        barrierByOutput[output] = TransformProducer{&block, index};
    } else if (block.kind == "credit") {
      if (block.inputs.size() != 1 || block.outputs.size() != 1 ||
          block.yields.size() != 1 || block.credits == 0)
        return pycError("credit contract is unsupported");
      creditByOutput[block.outputs.front()] = &block;
    } else if (block.kind == "memory_request") {
      if (block.inputs.size() != 1 || block.outputs.size() != 1 ||
          block.yields.size() != 3 || block.memoryInstance.empty() ||
          block.resultField.empty())
        return pycError("memory contract is unsupported");
      memoryByOutput[block.outputs.front()] = &block;
    } else if (block.kind == "reorder") {
      if (block.inputs.size() != 1 || block.outputs.size() != 1 ||
          block.yields.size() != 1 || block.capacity == 0)
        return pycError("reorder contract is unsupported");
      reorderByOutput[block.outputs.front()] = &block;
    } else if (block.kind == "dependency") {
      if (block.inputs.size() != 1 || block.outputs.size() != 1 ||
          block.yields.size() != 4 || block.capacity == 0 ||
          block.resources == 0)
        return pycError("dependency contract is unsupported");
      dependencyByOutput[block.outputs.front()] = &block;
    } else if (block.kind == "feedback") {
      if (block.inputs.size() != 1 || block.outputs.size() != 1 ||
          block.yields.size() != 2 || block.maxIterations == 0)
        return pycError("feedback contract is unsupported");
      feedbackByOutput[block.outputs.front()] = &block;
    } else {
      return pycError(
          "PYC QueueGraph supports "
          "source/transform/broadcast/fork/route/select/"
          "merge/barrier/credit/memory_request/dependency/reorder/feedback/"
          "observe/sink");
    }
  }
  if (auto error = verifyQueueGraphPlan(plan))
    return std::move(error);
  if (sources.empty() || sinks.empty())
    return pycError("PYC lowering requires at least one source and one sink");
  for (const QueuePlan &queue : plan.queues) {
    if (auto width = typeWidth(plan, queue.payloadType); !width)
      return width.takeError();
    if (queue.latency == 0)
      return pycError("PYC Queue latency must be positive");
    if (queue.rate != 1)
      return pycError(
          "PYC Queue rate greater than one requires explicit lane lowering");
  }
  llvm::StringMap<size_t> sourceBoundary;
  std::vector<std::string> inputPortTypes;
  for (auto [index, source] : llvm::enumerate(sources)) {
    const QueuePlan *queue = findQueue(plan, source->outputs.front());
    if (!queue)
      return pycError("source Queue is missing");
    auto type = pycType(plan, queue->payloadType);
    if (!type)
      return type.takeError();
    sourceBoundary[source->outputs.front()] = index;
    inputPortTypes.push_back(std::move(*type));
  }
  std::vector<std::string> outputPortTypes;
  for (const QueueBlockPlan *sink : sinks) {
    const QueuePlan *queue = findQueue(plan, sink->inputs.front());
    if (!queue)
      return pycError("sink Queue is missing");
    auto type = pycType(plan, queue->payloadType);
    if (!type)
      return type.takeError();
    outputPortTypes.push_back(std::move(*type));
  }
  auto inputName = [&](size_t index, llvm::StringRef suffix) {
    return sources.size() == 1
               ? ("%in_" + suffix).str()
               : ("%in" + std::to_string(index) + "_" + suffix.str());
  };
  auto outputName = [&](size_t index, llvm::StringRef suffix) {
    return sinks.size() == 1
               ? ("%out_" + suffix).str()
               : ("%out" + std::to_string(index) + "_" + suffix.str());
  };

  llvm::StringMap<std::string> readyWires;
  llvm::StringMap<std::string> inputReady;
  llvm::StringMap<std::string> outputValid;
  llvm::StringMap<std::string> outputData;
  llvm::StringMap<std::string> routeSelector;
  llvm::StringMap<std::string> routeCondition;
  llvm::StringMap<SelectState> selectStates;
  llvm::StringMap<std::string> atomicTransformValid;
  llvm::StringMap<std::vector<std::string>> mergeGrants;
  llvm::StringMap<MergeState> mergeStates;
  llvm::StringMap<ForkState> forkStates;
  llvm::StringMap<FeedbackState> feedbackStates;
  llvm::StringMap<CreditState> creditStates;
  llvm::StringMap<std::string> memoryResponseValid;
  llvm::StringMap<std::string> memoryResponseData;
  llvm::StringMap<DependencyState> dependencyStates;
  llvm::StringMap<ReorderState> reorderStates;
  llvm::StringMap<std::string> forkOfferValid;
  std::ostringstream body;
  unsigned nextValue = 0;
  auto newValue = [&]() { return "%v" + std::to_string(nextValue++); };
  auto emitConstant = [&](uint64_t value, llvm::StringRef type) {
    std::string result = newValue();
    body << "    " << result << " = pyc.constant " << value << " : "
         << type.str() << "\n";
    return result;
  };
  auto emitBinary = [&](llvm::StringRef operation, llvm::StringRef lhs,
                        llvm::StringRef rhs, llvm::StringRef type) {
    std::string result = newValue();
    body << "    " << result << " = pyc." << operation.str() << ' ' << lhs.str()
         << ", " << rhs.str() << " : " << type.str() << ", " << type.str()
         << " -> "
         << (operation == "eq" || operation == "ult" || operation == "slt"
                 ? "i1"
                 : type.str())
         << "\n";
    return result;
  };
  auto emitNot = [&](llvm::StringRef value) {
    std::string result = newValue();
    body << "    " << result << " = pyc.not " << value.str() << " : i1\n";
    return result;
  };
  auto emitMux = [&](llvm::StringRef select, llvm::StringRef trueValue,
                     llvm::StringRef falseValue, llvm::StringRef type) {
    std::string result = newValue();
    body << "    " << result << " = pyc.mux " << select.str() << ", "
         << trueValue.str() << ", " << falseValue.str() << " : i1, "
         << type.str() << ", " << type.str() << " -> " << type.str() << "\n";
    return result;
  };
  auto emitExtract = [&](llvm::StringRef value, uint64_t lsb,
                         llvm::StringRef inputType,
                         llvm::StringRef resultType) {
    std::string result = newValue();
    body << "    " << result << " = pyc.extract " << value.str()
         << " {lsb = " << lsb << "} : " << inputType.str() << " -> "
         << resultType.str() << "\n";
    return result;
  };
  auto emitFieldReplace =
      [&](llvm::StringRef record, llvm::StringRef recordType,
          llvm::StringRef field,
          llvm::StringRef replacement) -> llvm::Expected<std::string> {
    auto layout = fieldLayout(plan, recordType, field);
    auto totalWidth = typeWidth(plan, recordType);
    if (!layout)
      return layout.takeError();
    if (!totalWidth)
      return totalWidth.takeError();
    std::vector<std::pair<std::string, unsigned>> parts;
    const unsigned highWidth = *totalWidth - layout->lsb - layout->width;
    if (highWidth > 0)
      parts.emplace_back(emitExtract(record, layout->lsb + layout->width,
                                     "i" + std::to_string(*totalWidth),
                                     "i" + std::to_string(highWidth)),
                         highWidth);
    parts.emplace_back(replacement.str(), layout->width);
    if (layout->lsb > 0)
      parts.emplace_back(emitExtract(record, 0,
                                     "i" + std::to_string(*totalWidth),
                                     "i" + std::to_string(layout->lsb)),
                         layout->lsb);
    if (parts.size() == 1)
      return parts.front().first;
    std::string result = newValue();
    body << "    " << result << " = pyc.concat(";
    for (auto [index, part] : llvm::enumerate(parts)) {
      if (index)
        body << ", ";
      body << part.first;
    }
    body << ") : (";
    for (auto [index, part] : llvm::enumerate(parts)) {
      if (index)
        body << ", ";
      body << 'i' << part.second;
    }
    body << ") -> i" << *totalWidth << "\n";
    return result;
  };
  auto reduceBalanced = [&](llvm::StringRef operation,
                            std::vector<std::string> values,
                            llvm::StringRef type) {
    while (values.size() > 1) {
      std::vector<std::string> next;
      next.reserve((values.size() + 1) / 2);
      for (size_t index = 0; index < values.size(); index += 2) {
        if (index + 1 == values.size())
          next.push_back(values[index]);
        else
          next.push_back(
              emitBinary(operation, values[index], values[index + 1], type));
      }
      values = std::move(next);
    }
    return values.front();
  };
  auto selectBalanced = [&](std::vector<std::string> valids,
                            std::vector<std::string> values,
                            llvm::StringRef type) {
    while (values.size() > 1) {
      std::vector<std::string> nextValids;
      std::vector<std::string> nextValues;
      nextValids.reserve((values.size() + 1) / 2);
      nextValues.reserve((values.size() + 1) / 2);
      for (size_t index = 0; index < values.size(); index += 2) {
        if (index + 1 == values.size()) {
          nextValids.push_back(valids[index]);
          nextValues.push_back(values[index]);
        } else {
          nextValues.push_back(
              emitMux(valids[index], values[index], values[index + 1], type));
          nextValids.push_back(
              emitBinary("or", valids[index], valids[index + 1], "i1"));
        }
      }
      valids = std::move(nextValids);
      values = std::move(nextValues);
    }
    return std::pair<std::string, std::string>{valids.front(), values.front()};
  };
  for (const QueuePlan &queue : plan.queues) {
    std::string ready = newValue();
    readyWires[queue.name] = ready;
    body << "    " << ready << " = pyc.wire : i1\n";
  }
  for (const QueuePlan &queue : plan.queues) {
    std::string producerValid;
    std::string producerData;
    auto source = sourceBoundary.find(queue.name);
    if (source != sourceBoundary.end()) {
      producerValid = inputName(source->getValue(), "valid");
      producerData = inputName(source->getValue(), "data");
    } else {
      auto transformProducer = transformByOutput.find(queue.name);
      auto barrierProducer = barrierByOutput.find(queue.name);
      auto broadcastProducer = broadcastByOutput.find(queue.name);
      auto forkProducer = forkByOutput.find(queue.name);
      auto routeProducer = routeByOutput.find(queue.name);
      auto selectProducer = selectByOutput.find(queue.name);
      auto mergeProducer = mergeByOutput.find(queue.name);
      auto creditProducer = creditByOutput.find(queue.name);
      auto memoryProducer = memoryByOutput.find(queue.name);
      auto dependencyProducer = dependencyByOutput.find(queue.name);
      auto reorderProducer = reorderByOutput.find(queue.name);
      auto feedbackProducer = feedbackByOutput.find(queue.name);
      if (transformProducer != transformByOutput.end()) {
        const TransformProducer &producer = transformProducer->getValue();
        const QueueBlockPlan &transform = *producer.block;
        std::vector<std::string> inputDataValues;
        std::vector<std::string> inputTypes;
        std::string allValid;
        for (const std::string &inputName : transform.inputs) {
          auto valid = outputValid.find(inputName);
          auto data = outputData.find(inputName);
          const QueuePlan *inputQueue = findQueue(plan, inputName);
          if (valid == outputValid.end() || data == outputData.end() ||
              !inputQueue)
            return pycError("Queue transforms are not in topological order");
          allValid = allValid.empty()
                         ? valid->getValue()
                         : emitBinary("and", allValid, valid->getValue(), "i1");
          inputDataValues.push_back(data->getValue());
          inputTypes.push_back(inputQueue->payloadType);
        }
        if (transform.inputs.size() == 1 && transform.outputs.size() == 1) {
          producerValid = allValid;
        } else {
          producerValid = newValue();
          body << "    " << producerValid << " = pyc.wire : i1\n";
          atomicTransformValid[queue.name] = producerValid;
        }
        auto transformed =
            emitTransform(plan, transform, inputDataValues, inputTypes,
                          producer.index, nextValue, body);
        if (!transformed)
          return transformed.takeError();
        producerData = std::move(*transformed);
      } else if (barrierProducer != barrierByOutput.end()) {
        const TransformProducer &producer = barrierProducer->getValue();
        const QueueBlockPlan &barrier = *producer.block;
        for (const std::string &inputName : barrier.inputs) {
          auto valid = outputValid.find(inputName);
          if (valid == outputValid.end())
            return pycError(
                "barrier input is not available in topological order");
        }
        auto data = outputData.find(barrier.inputs[producer.index]);
        if (data == outputData.end())
          return pycError("barrier input data is missing");
        producerValid = newValue();
        body << "    " << producerValid << " = pyc.wire : i1\n";
        atomicTransformValid[queue.name] = producerValid;
        producerData = data->getValue();
      } else if (broadcastProducer != broadcastByOutput.end()) {
        const QueueBlockPlan &broadcast = *broadcastProducer->getValue();
        auto valid = outputValid.find(broadcast.inputs.front());
        auto data = outputData.find(broadcast.inputs.front());
        if (valid == outputValid.end() || data == outputData.end())
          return pycError(
              "broadcast input is not available in topological order");
        producerValid = valid->getValue();
        producerData = data->getValue();
      } else if (forkProducer != forkByOutput.end()) {
        const QueueBlockPlan &fork = *forkProducer->getValue();
        auto valid = outputValid.find(fork.inputs.front());
        auto data = outputData.find(fork.inputs.front());
        if (valid == outputValid.end() || data == outputData.end())
          return pycError("fork input is not available in topological order");
        auto state = forkStates.find(fork.name);
        if (state == forkStates.end()) {
          ForkState created;
          std::string zero = emitConstant(0, "i1");
          for (size_t index = 0; index < fork.outputs.size(); ++index) {
            std::string nextWire = newValue();
            std::string enableWire = newValue();
            std::string delivered = newValue();
            body << "    " << nextWire << " = pyc.wire : i1\n";
            body << "    " << enableWire << " = pyc.wire : i1\n";
            body << "    " << delivered << " = pyc.reg %clk, %rst, "
                 << enableWire << ", " << nextWire << ", " << zero << " : i1\n";
            created.nextWires.push_back(std::move(nextWire));
            created.enableWires.push_back(std::move(enableWire));
            created.delivered.push_back(std::move(delivered));
          }
          forkStates[fork.name] = std::move(created);
          state = forkStates.find(fork.name);
        }
        auto output =
            std::find(fork.outputs.begin(), fork.outputs.end(), queue.name);
        if (output == fork.outputs.end())
          return pycError("fork output identity is missing");
        const size_t index = std::distance(fork.outputs.begin(), output);
        std::string notDelivered = emitNot(state->getValue().delivered[index]);
        producerValid =
            emitBinary("and", valid->getValue(), notDelivered, "i1");
        forkOfferValid[queue.name] = producerValid;
        producerData = data->getValue();
      } else if (routeProducer != routeByOutput.end()) {
        const RouteProducer &producer = routeProducer->getValue();
        const QueueBlockPlan &route = *producer.block;
        auto valid = outputValid.find(route.inputs.front());
        auto data = outputData.find(route.inputs.front());
        const QueuePlan *inputQueue = findQueue(plan, route.inputs.front());
        if (valid == outputValid.end() || data == outputData.end() ||
            !inputQueue)
          return pycError("route input is not available in topological order");
        auto selector = routeSelector.find(route.name);
        if (selector == routeSelector.end()) {
          auto selected =
              emitTransform(plan, route, {data->getValue()},
                            {inputQueue->payloadType}, 0, nextValue, body);
          if (!selected)
            return selected.takeError();
          routeSelector[route.name] = *selected;
          selector = routeSelector.find(route.name);
        }
        auto selectorType =
            yieldedType(route, route.yields.front(), inputQueue->payloadType);
        if (!selectorType)
          return selectorType.takeError();
        auto selectorPycType = pycType(plan, *selectorType);
        if (!selectorPycType)
          return selectorPycType.takeError();
        std::string index = emitConstant(producer.index, *selectorPycType);
        std::string condition =
            emitBinary("eq", selector->getValue(), index, *selectorPycType);
        routeCondition[queue.name] = condition;
        producerValid = emitBinary("and", valid->getValue(), condition, "i1");
        producerData = data->getValue();
      } else if (selectProducer != selectByOutput.end()) {
        const QueueBlockPlan &select = *selectProducer->getValue();
        auto controlValid = outputValid.find(select.inputs.front());
        auto controlData = outputData.find(select.inputs.front());
        const QueuePlan *controlQueue = findQueue(plan, select.inputs.front());
        if (controlValid == outputValid.end() ||
            controlData == outputData.end() || !controlQueue)
          return pycError(
              "select control is not available in topological order");
        auto selector =
            emitTransform(plan, select, {controlData->getValue()},
                          {controlQueue->payloadType}, 0, nextValue, body);
        if (!selector)
          return selector.takeError();
        auto selectorType = yieldedType(select, select.yields.front(),
                                        controlQueue->payloadType);
        if (!selectorType)
          return selectorType.takeError();
        auto selectorPycType = pycType(plan, *selectorType);
        if (!selectorPycType)
          return selectorPycType.takeError();
        auto outputType = pycType(plan, queue.payloadType);
        if (!outputType)
          return outputType.takeError();

        SelectState state;
        state.controlValid = controlValid->getValue();
        std::vector<std::string> selectedValidTerms;
        std::vector<std::string> dataValues;
        for (size_t index = 1; index < select.inputs.size(); ++index) {
          auto valid = outputValid.find(select.inputs[index]);
          auto data = outputData.find(select.inputs[index]);
          if (valid == outputValid.end() || data == outputData.end())
            return pycError(
                "select data input is not available in topological order");
          std::string indexValue = emitConstant(index - 1, *selectorPycType);
          std::string condition =
              emitBinary("eq", *selector, indexValue, *selectorPycType);
          state.conditions.push_back(condition);
          selectedValidTerms.push_back(
              emitBinary("and", valid->getValue(), condition, "i1"));
          dataValues.push_back(data->getValue());
        }
        state.selectedValid = reduceBalanced("or", selectedValidTerms, "i1");
        std::string selectedData = dataValues.back();
        for (size_t index = dataValues.size() - 1; index-- > 0;)
          selectedData = emitMux(state.conditions[index], dataValues[index],
                                 selectedData, *outputType);
        std::string anyCondition = reduceBalanced("or", state.conditions, "i1");
        std::string invalidSelector =
            emitBinary("and", state.controlValid, emitNot(anyCondition), "i1");
        state.selectorSafe = emitNot(invalidSelector);
        producerValid =
            emitBinary("and", state.controlValid, state.selectedValid, "i1");
        producerData = std::move(selectedData);
        selectStates[select.name] = std::move(state);
      } else if (mergeProducer != mergeByOutput.end()) {
        const QueueBlockPlan &merge = *mergeProducer->getValue();
        std::vector<std::string> valids;
        std::vector<std::string> dataValues;
        for (const std::string &input : merge.inputs) {
          auto valid = outputValid.find(input);
          auto data = outputData.find(input);
          if (valid == outputValid.end() || data == outputData.end())
            return pycError(
                "merge input is not available in topological order");
          valids.push_back(valid->getValue());
          dataValues.push_back(data->getValue());
        }
        std::string any = valids.front();
        for (size_t index = 1; index < valids.size(); ++index) {
          any = emitBinary("or", any, valids[index], "i1");
        }
        std::vector<std::string> grants;
        if (merge.policy == "priority") {
          grants.push_back(valids.front());
          std::string prior = valids.front();
          for (size_t index = 1; index < valids.size(); ++index) {
            std::string notPrior = emitNot(prior);
            grants.push_back(emitBinary("and", valids[index], notPrior, "i1"));
            prior = emitBinary("or", prior, valids[index], "i1");
          }
        } else {
          unsigned pointerWidth = 1;
          while ((uint64_t{1} << pointerWidth) < valids.size())
            ++pointerWidth;
          std::string pointerType = "i" + std::to_string(pointerWidth);
          std::string nextWire = newValue();
          std::string enableWire = newValue();
          body << "    " << nextWire << " = pyc.wire : " << pointerType << "\n";
          body << "    " << enableWire << " = pyc.wire : i1\n";
          std::string zeroPointer = emitConstant(0, pointerType);
          std::string cursor = newValue();
          body << "    " << cursor << " = pyc.reg %clk, %rst, " << enableWire
               << ", " << nextWire << ", " << zeroPointer << " : "
               << pointerType << "\n";
          std::string zeroGrant = emitConstant(0, "i1");
          std::vector<std::vector<std::string>> grantsByCursor;
          grantsByCursor.reserve(valids.size());
          for (size_t start = 0; start < valids.size(); ++start) {
            std::vector<std::string> cursorGrants(valids.size(), zeroGrant);
            std::string prior = zeroGrant;
            for (size_t offset = 0; offset < valids.size(); ++offset) {
              const size_t input = (start + offset) % valids.size();
              cursorGrants[input] =
                  emitBinary("and", valids[input], emitNot(prior), "i1");
              prior = emitBinary("or", prior, valids[input], "i1");
            }
            grantsByCursor.push_back(std::move(cursorGrants));
          }
          grants.reserve(valids.size());
          for (size_t input = 0; input < valids.size(); ++input) {
            std::string selected = zeroGrant;
            for (size_t start = valids.size(); start-- > 0;) {
              std::string startValue = emitConstant(start, pointerType);
              std::string atStart =
                  emitBinary("eq", cursor, startValue, pointerType);
              selected = emitMux(atStart, grantsByCursor[start][input],
                                 selected, "i1");
            }
            std::string qualified = newValue();
            body << "    " << qualified << " = pyc.alias " << selected
                 << " {num_inputs = " << valids.size() << ", lane = " << input
                 << ", primitive_id = \"control.rr_arbiter.v1\", "
                    "implementation_id = \"internal.reference.rr_arbiter.v1\", "
                    "qualification_report = "
                    "\"DF-09/smoke_v072/arbiter_candidates\"} : i1\n";
            grants.push_back(std::move(qualified));
          }
          mergeStates[merge.name] =
              MergeState{nextWire, enableWire, cursor, any, pointerType};
        }
        auto outputType = pycType(plan, queue.payloadType);
        if (!outputType)
          return outputType.takeError();
        std::string selectedData = dataValues.back();
        for (size_t index = dataValues.size() - 1; index-- > 0;)
          selectedData = emitMux(grants[index], dataValues[index], selectedData,
                                 *outputType);
        mergeGrants[merge.name] = grants;
        producerValid = any;
        producerData = selectedData;
      } else if (creditProducer != creditByOutput.end()) {
        const QueueBlockPlan &credit = *creditProducer->getValue();
        auto inputValidValue = outputValid.find(credit.inputs.front());
        auto inputDataValue = outputData.find(credit.inputs.front());
        const QueuePlan *inputQueue = findQueue(plan, credit.inputs.front());
        if (inputValidValue == outputValid.end() ||
            inputDataValue == outputData.end() || !inputQueue)
          return pycError("credit input is not available in topological order");
        auto dataType = pycType(plan, inputQueue->payloadType);
        auto dataWidth = typeWidth(plan, inputQueue->payloadType);
        if (!dataType)
          return dataType.takeError();
        if (!dataWidth)
          return dataWidth.takeError();
        auto costValue =
            emitTransform(plan, credit, {inputDataValue->getValue()},
                          {inputQueue->payloadType}, 0, nextValue, body);
        if (!costValue)
          return costValue.takeError();
        auto costType =
            yieldedType(credit, credit.yields.front(), inputQueue->payloadType);
        if (!costType)
          return costType.takeError();
        auto costPycType = pycType(plan, *costType);
        auto costWidth = typeWidth(plan, *costType);
        if (!costPycType)
          return costPycType.takeError();
        if (!costWidth)
          return costWidth.takeError();
        if (*costWidth == 0 || *costWidth > 64)
          return pycError("credit cost width is unsupported");

        CreditState state;
        state.inputCost = std::move(*costValue);
        state.costType = std::move(*costPycType);
        state.dataType = *dataType;
        state.slotType = "i" + std::to_string(1 + *costWidth + *dataWidth);
        std::string zeroSlot = emitConstant(0, state.slotType);
        std::string zeroCost = emitConstant(0, state.costType);
        std::vector<std::string> freeValues;
        std::vector<std::string> doneValues;
        std::vector<std::string> dataValues;
        for (uint64_t index = 0; index < credit.credits; ++index) {
          CreditSlotState slot;
          slot.next = newValue();
          slot.enable = newValue();
          slot.state = newValue();
          body << "    " << slot.next << " = pyc.wire : " << state.slotType
               << "\n";
          body << "    " << slot.enable << " = pyc.wire : i1\n";
          body << "    " << slot.state << " = pyc.reg %clk, %rst, "
               << slot.enable << ", " << slot.next << ", " << zeroSlot << " : "
               << state.slotType << "\n";
          slot.valid = emitExtract(slot.state, *costWidth + *dataWidth,
                                   state.slotType, "i1");
          slot.remaining = emitExtract(slot.state, *dataWidth, state.slotType,
                                       state.costType);
          slot.data =
              emitExtract(slot.state, 0, state.slotType, state.dataType);
          slot.free = emitNot(slot.valid);
          std::string atZero =
              emitBinary("eq", slot.remaining, zeroCost, state.costType);
          slot.done = emitBinary("and", slot.valid, atZero, "i1");
          freeValues.push_back(slot.free);
          doneValues.push_back(slot.done);
          dataValues.push_back(slot.data);
          state.slots.push_back(std::move(slot));
        }
        unsigned indexWidth = 1;
        while ((uint64_t{1} << indexWidth) < credit.credits)
          ++indexWidth;
        state.freeIndexType = "i" + std::to_string(indexWidth);
        std::vector<std::string> indices;
        indices.reserve(credit.credits);
        for (uint64_t index = 0; index < credit.credits; ++index)
          indices.push_back(emitConstant(index, state.freeIndexType));
        auto selectedFree =
            selectBalanced(freeValues, indices, state.freeIndexType);
        state.anyFree = std::move(selectedFree.first);
        state.freeIndex = std::move(selectedFree.second);
        auto selectedDoneIndex =
            selectBalanced(doneValues, indices, state.freeIndexType);
        state.doneIndex = std::move(selectedDoneIndex.second);
        auto selectedDoneData =
            selectBalanced(doneValues, dataValues, state.dataType);
        state.outputDone = std::move(selectedDoneData.first);
        std::string selectedData = std::move(selectedDoneData.second);
        std::string invalidCost =
            emitBinary("eq", state.inputCost, zeroCost, state.costType);
        std::string invalidAdmission =
            emitBinary("and", inputValidValue->getValue(), invalidCost, "i1");
        state.safeAdmission = emitNot(invalidAdmission);
        producerValid = state.outputDone;
        producerData = std::move(selectedData);
        creditStates[credit.name] = std::move(state);
      } else if (memoryProducer != memoryByOutput.end()) {
        const QueueBlockPlan &memory = *memoryProducer->getValue();
        const QueuePlan *inputQueue = findQueue(plan, memory.inputs.front());
        if (!inputQueue)
          return pycError("memory input Queue is missing");
        auto requestType = pycType(plan, inputQueue->payloadType);
        if (!requestType)
          return requestType.takeError();
        producerValid = newValue();
        producerData = newValue();
        body << "    " << producerValid << " = pyc.wire : i1\n";
        body << "    " << producerData << " = pyc.wire : " << *requestType
             << "\n";
        memoryResponseValid[memory.outputs.front()] = producerValid;
        memoryResponseData[memory.outputs.front()] = producerData;
      } else if (dependencyProducer != dependencyByOutput.end()) {
        const QueueBlockPlan &dependency = *dependencyProducer->getValue();
        auto inputValidValue = outputValid.find(dependency.inputs.front());
        auto inputDataValue = outputData.find(dependency.inputs.front());
        const QueuePlan *inputQueue =
            findQueue(plan, dependency.inputs.front());
        if (inputValidValue == outputValid.end() ||
            inputDataValue == outputData.end() || !inputQueue)
          return pycError(
              "dependency input is not available in topological order");
        auto dataType = pycType(plan, inputQueue->payloadType);
        auto dataWidth = typeWidth(plan, inputQueue->payloadType);
        if (!dataType)
          return dataType.takeError();
        if (!dataWidth)
          return dataWidth.takeError();

        std::vector<std::string> policyValues;
        std::vector<std::string> policyTypes;
        std::vector<unsigned> policyWidths;
        for (size_t index = 0; index < dependency.yields.size(); ++index) {
          auto value =
              emitTransform(plan, dependency, {inputDataValue->getValue()},
                            {inputQueue->payloadType}, index, nextValue, body);
          if (!value)
            return value.takeError();
          auto type = yieldedType(dependency, dependency.yields[index],
                                  inputQueue->payloadType);
          if (!type)
            return type.takeError();
          auto pycType = ::acir::codegen::pycType(plan, *type);
          auto width = typeWidth(plan, *type);
          if (!pycType)
            return pycType.takeError();
          if (!width)
            return width.takeError();
          if (*width == 0 || *width > 64)
            return pycError("dependency policy width is unsupported");
          policyValues.push_back(std::move(*value));
          policyTypes.push_back(std::move(*pycType));
          policyWidths.push_back(*width);
        }
        if (policyTypes[0] != policyTypes[1])
          return pycError("dependency key/predecessor types must match");
        if (policyWidths[1] < 64 &&
            dependency.noDependency >= (uint64_t{1} << policyWidths[1]))
          return pycError("dependency sentinel does not fit predecessor type");
        if (policyWidths[2] < 64 &&
            dependency.resources > (uint64_t{1} << policyWidths[2]))
          return pycError("dependency resources do not fit resource type");

        DependencyState state;
        state.inputKey = policyValues[0];
        state.inputPredecessor = policyValues[1];
        state.inputResource = policyValues[2];
        state.inputCost = policyValues[3];
        state.keyType = policyTypes[0];
        state.resourceType = policyTypes[2];
        state.costType = policyTypes[3];
        state.dataType = *dataType;
        state.noDependency = dependency.noDependency;
        state.resources = dependency.resources;
        const uint64_t slotWidth = 1 + 2 + 2 * policyWidths[0] +
                                   policyWidths[2] + 2 * policyWidths[3] +
                                   *dataWidth;
        state.slotType = "i" + std::to_string(slotWidth);
        std::string zeroSlot = emitConstant(0, state.slotType);
        std::string donePhase = emitConstant(2, "i2");

        const uint64_t costLsb = *dataWidth;
        const uint64_t remainingLsb = costLsb + policyWidths[3];
        const uint64_t resourceLsb = remainingLsb + policyWidths[3];
        const uint64_t predecessorLsb = resourceLsb + policyWidths[2];
        const uint64_t keyLsb = predecessorLsb + policyWidths[0];
        const uint64_t phaseLsb = keyLsb + policyWidths[0];
        const uint64_t validLsb = phaseLsb + 2;
        std::vector<std::string> freeValues;
        std::vector<std::string> doneValues;
        std::vector<std::string> dataValues;
        std::vector<std::string> duplicateValues;
        for (uint64_t index = 0; index < dependency.capacity; ++index) {
          DependencySlotState slot;
          slot.next = newValue();
          slot.enable = newValue();
          slot.state = newValue();
          body << "    " << slot.next << " = pyc.wire : " << state.slotType
               << "\n";
          body << "    " << slot.enable << " = pyc.wire : i1\n";
          body << "    " << slot.state << " = pyc.reg %clk, %rst, "
               << slot.enable << ", " << slot.next << ", " << zeroSlot << " : "
               << state.slotType << "\n";
          slot.valid = emitExtract(slot.state, validLsb, state.slotType, "i1");
          slot.phase = emitExtract(slot.state, phaseLsb, state.slotType, "i2");
          slot.key =
              emitExtract(slot.state, keyLsb, state.slotType, state.keyType);
          slot.predecessor = emitExtract(slot.state, predecessorLsb,
                                         state.slotType, state.keyType);
          slot.resource = emitExtract(slot.state, resourceLsb, state.slotType,
                                      state.resourceType);
          slot.remaining = emitExtract(slot.state, remainingLsb, state.slotType,
                                       state.costType);
          slot.cost =
              emitExtract(slot.state, costLsb, state.slotType, state.costType);
          slot.data =
              emitExtract(slot.state, 0, state.slotType, state.dataType);
          slot.free = emitNot(slot.valid);
          freeValues.push_back(slot.free);
          std::string isDone = emitBinary("eq", slot.phase, donePhase, "i2");
          slot.done = emitBinary("and", slot.valid, isDone, "i1");
          doneValues.push_back(slot.done);
          dataValues.push_back(slot.data);
          std::string sameInput =
              emitBinary("eq", slot.key, state.inputKey, state.keyType);
          duplicateValues.push_back(
              emitBinary("and", slot.valid, sameInput, "i1"));
          state.slots.push_back(std::move(slot));
        }

        unsigned indexWidth = 1;
        while ((uint64_t{1} << indexWidth) < dependency.capacity)
          ++indexWidth;
        state.freeIndexType = "i" + std::to_string(indexWidth);
        std::vector<std::string> indices;
        indices.reserve(dependency.capacity);
        for (uint64_t index = 0; index < dependency.capacity; ++index)
          indices.push_back(emitConstant(index, state.freeIndexType));
        auto selectedFree =
            selectBalanced(freeValues, indices, state.freeIndexType);
        state.anyFree = std::move(selectedFree.first);
        state.freeIndex = std::move(selectedFree.second);
        auto selectedDoneIndex =
            selectBalanced(doneValues, indices, state.freeIndexType);
        state.doneIndex = std::move(selectedDoneIndex.second);
        auto selectedDoneData =
            selectBalanced(doneValues, dataValues, state.dataType);
        state.outputDone = std::move(selectedDoneData.first);
        std::string selectedData = std::move(selectedDoneData.second);
        std::string duplicate = reduceBalanced("or", duplicateValues, "i1");
        std::string zeroCost = emitConstant(0, state.costType);
        std::string costIsZero =
            emitBinary("eq", state.inputCost, zeroCost, state.costType);
        std::string resourceInvalid = emitConstant(0, "i1");
        if (policyWidths[2] == 64 ||
            dependency.resources < (uint64_t{1} << policyWidths[2])) {
          std::string resourceLimit =
              emitConstant(dependency.resources, state.resourceType);
          std::string resourceValid = emitBinary(
              "ult", state.inputResource, resourceLimit, state.resourceType);
          resourceInvalid = emitNot(resourceValid);
        }
        std::string invalidInput =
            emitBinary("or", duplicate, costIsZero, "i1");
        invalidInput = emitBinary("or", invalidInput, resourceInvalid, "i1");
        std::string invalidAdmission =
            emitBinary("and", inputValidValue->getValue(), invalidInput, "i1");
        state.safeAdmission = emitNot(invalidAdmission);
        producerValid = state.outputDone;
        producerData = std::move(selectedData);
        dependencyStates[dependency.name] = std::move(state);
      } else if (reorderProducer != reorderByOutput.end()) {
        const QueueBlockPlan &reorder = *reorderProducer->getValue();
        auto inputValidValue = outputValid.find(reorder.inputs.front());
        auto inputDataValue = outputData.find(reorder.inputs.front());
        const QueuePlan *inputQueue = findQueue(plan, reorder.inputs.front());
        if (inputValidValue == outputValid.end() ||
            inputDataValue == outputData.end() || !inputQueue)
          return pycError(
              "reorder input is not available in topological order");
        auto dataType = pycType(plan, inputQueue->payloadType);
        if (!dataType)
          return dataType.takeError();
        auto keyValue =
            emitTransform(plan, reorder, {inputDataValue->getValue()},
                          {inputQueue->payloadType}, 0, nextValue, body);
        if (!keyValue)
          return keyValue.takeError();
        auto keyType = yieldedType(reorder, reorder.yields.front(),
                                   inputQueue->payloadType);
        if (!keyType)
          return keyType.takeError();
        auto keyPycType = pycType(plan, *keyType);
        auto keyWidth = typeWidth(plan, *keyType);
        auto dataWidth = typeWidth(plan, inputQueue->payloadType);
        if (!keyPycType)
          return keyPycType.takeError();
        if (!keyWidth)
          return keyWidth.takeError();
        if (!dataWidth)
          return dataWidth.takeError();
        if (*keyWidth == 0 || *keyWidth > 64 ||
            (*keyWidth < 64 && reorder.start >= (uint64_t{1} << *keyWidth)))
          return pycError("reorder start does not fit key type");

        ReorderState state;
        state.inputKey = std::move(*keyValue);
        state.keyType = *keyPycType;
        state.dataType = *dataType;
        state.slotType = "i" + std::to_string(1 + *keyWidth + *dataWidth);
        std::string zeroSlot = emitConstant(0, state.slotType);
        std::string initialExpected =
            emitConstant(reorder.start, state.keyType);
        state.expectedNext = newValue();
        state.expectedEnable = newValue();
        state.expected = newValue();
        body << "    " << state.expectedNext
             << " = pyc.wire : " << state.keyType << "\n";
        body << "    " << state.expectedEnable << " = pyc.wire : i1\n";
        body << "    " << state.expected << " = pyc.reg %clk, %rst, "
             << state.expectedEnable << ", " << state.expectedNext << ", "
             << initialExpected << " : " << state.keyType << "\n";

        std::vector<std::string> freeValues;
        std::vector<std::string> matchValues;
        std::vector<std::string> duplicateValues;
        std::vector<std::string> slotDataValues;
        for (uint64_t index = 0; index < reorder.capacity; ++index) {
          ReorderSlotState slot;
          slot.next = newValue();
          slot.enable = newValue();
          slot.state = newValue();
          body << "    " << slot.next << " = pyc.wire : " << state.slotType
               << "\n";
          body << "    " << slot.enable << " = pyc.wire : i1\n";
          body << "    " << slot.state << " = pyc.reg %clk, %rst, "
               << slot.enable << ", " << slot.next << ", " << zeroSlot << " : "
               << state.slotType << "\n";
          slot.valid = newValue();
          body << "    " << slot.valid << " = pyc.extract " << slot.state
               << " {lsb = " << (*keyWidth + *dataWidth)
               << "} : " << state.slotType << " -> i1\n";
          slot.key = newValue();
          body << "    " << slot.key << " = pyc.extract " << slot.state
               << " {lsb = " << *dataWidth << "} : " << state.slotType << " -> "
               << state.keyType << "\n";
          slot.data = newValue();
          body << "    " << slot.data << " = pyc.extract " << slot.state
               << " {lsb = 0} : " << state.slotType << " -> " << state.dataType
               << "\n";
          slot.free = emitNot(slot.valid);
          freeValues.push_back(slot.free);
          std::string expected =
              emitBinary("eq", slot.key, state.expected, state.keyType);
          slot.match = emitBinary("and", slot.valid, expected, "i1");
          matchValues.push_back(slot.match);
          slotDataValues.push_back(slot.data);
          std::string sameInput =
              emitBinary("eq", slot.key, state.inputKey, state.keyType);
          duplicateValues.push_back(
              emitBinary("and", slot.valid, sameInput, "i1"));
          state.slots.push_back(std::move(slot));
        }
        unsigned freeIndexWidth = 1;
        while ((uint64_t{1} << freeIndexWidth) < reorder.capacity)
          ++freeIndexWidth;
        state.freeIndexType = "i" + std::to_string(freeIndexWidth);
        std::vector<std::string> freeIndices;
        freeIndices.reserve(reorder.capacity);
        for (uint64_t index = 0; index < reorder.capacity; ++index)
          freeIndices.push_back(emitConstant(index, state.freeIndexType));
        auto selectedFree =
            selectBalanced(freeValues, freeIndices, state.freeIndexType);
        state.anyFree = std::move(selectedFree.first);
        state.freeIndex = std::move(selectedFree.second);
        auto selected =
            selectBalanced(matchValues, slotDataValues, state.dataType);
        state.outputMatch = std::move(selected.first);
        std::string selectedData = std::move(selected.second);
        std::string duplicate = reduceBalanced("or", duplicateValues, "i1");
        std::string stale =
            emitBinary("ult", state.inputKey, state.expected, state.keyType);
        std::string invalidKey = emitBinary("or", duplicate, stale, "i1");
        std::string invalidAdmission =
            emitBinary("and", inputValidValue->getValue(), invalidKey, "i1");
        state.safeAdmission = emitNot(invalidAdmission);
        producerValid = state.outputMatch;
        producerData = std::move(selectedData);
        reorderStates[reorder.name] = std::move(state);
      } else if (feedbackProducer != feedbackByOutput.end()) {
        const QueueBlockPlan &feedback = *feedbackProducer->getValue();
        auto inputValidValue = outputValid.find(feedback.inputs.front());
        auto inputDataValue = outputData.find(feedback.inputs.front());
        const QueuePlan *inputQueue = findQueue(plan, feedback.inputs.front());
        if (inputValidValue == outputValid.end() ||
            inputDataValue == outputData.end() || !inputQueue)
          return pycError(
              "feedback input is not available in topological order");
        auto dataType = pycType(plan, inputQueue->payloadType);
        if (!dataType)
          return dataType.takeError();
        unsigned iterationWidth = 1;
        while (iterationWidth < 64 &&
               (uint64_t{1} << iterationWidth) <= feedback.maxIterations)
          ++iterationWidth;
        std::string iterationType = "i" + std::to_string(iterationWidth);
        std::string zeroValid = emitConstant(0, "i1");
        std::string zeroData = emitConstant(0, *dataType);
        std::string zeroIteration = emitConstant(0, iterationType);
        FeedbackState state;
        state.validNext = newValue();
        state.validEnable = newValue();
        state.valid = newValue();
        body << "    " << state.validNext << " = pyc.wire : i1\n";
        body << "    " << state.validEnable << " = pyc.wire : i1\n";
        body << "    " << state.valid << " = pyc.reg %clk, %rst, "
             << state.validEnable << ", " << state.validNext << ", "
             << zeroValid << " : i1\n";
        state.dataNext = newValue();
        state.dataEnable = newValue();
        state.data = newValue();
        body << "    " << state.dataNext << " = pyc.wire : " << *dataType
             << "\n";
        body << "    " << state.dataEnable << " = pyc.wire : i1\n";
        body << "    " << state.data << " = pyc.reg %clk, %rst, "
             << state.dataEnable << ", " << state.dataNext << ", " << zeroData
             << " : " << *dataType << "\n";
        state.iterationNext = newValue();
        state.iterationEnable = newValue();
        state.iteration = newValue();
        body << "    " << state.iterationNext
             << " = pyc.wire : " << iterationType << "\n";
        body << "    " << state.iterationEnable << " = pyc.wire : i1\n";
        body << "    " << state.iteration << " = pyc.reg %clk, %rst, "
             << state.iterationEnable << ", " << state.iterationNext << ", "
             << zeroIteration << " : " << iterationType << "\n";
        std::string selectedData = emitMux(
            state.valid, state.data, inputDataValue->getValue(), *dataType);
        state.selectedValid =
            emitBinary("or", state.valid, inputValidValue->getValue(), "i1");
        state.selectedIteration =
            emitMux(state.valid, state.iteration, zeroIteration, iterationType);
        auto updated =
            emitTransform(plan, feedback, {selectedData},
                          {inputQueue->payloadType}, 0, nextValue, body);
        if (!updated)
          return updated.takeError();
        state.updated = std::move(*updated);
        auto condition =
            emitTransform(plan, feedback, {selectedData},
                          {inputQueue->payloadType}, 1, nextValue, body);
        if (!condition)
          return condition.takeError();
        auto conditionType =
            yieldedType(feedback, feedback.yields[1], inputQueue->payloadType);
        if (!conditionType)
          return conditionType.takeError();
        auto conditionPycType = pycType(plan, *conditionType);
        if (!conditionPycType)
          return conditionPycType.takeError();
        if (*conditionPycType != "i1")
          return pycError("feedback condition must lower to i1");
        state.condition = std::move(*condition);
        std::string limit = emitConstant(feedback.maxIterations, iterationType);
        state.underLimit =
            emitBinary("ult", state.selectedIteration, limit, iterationType);
        std::string done = emitNot(state.condition);
        producerValid = emitBinary("and", state.selectedValid, done, "i1");
        producerData = std::move(selectedData);
        state.dataType = *dataType;
        state.iterationType = std::move(iterationType);
        feedbackStates[feedback.name] = std::move(state);
      } else {
        return pycError("Queue has no supported producer: '" + queue.name +
                        "'");
      }
    }
    auto dataType = pycType(plan, queue.payloadType);
    if (!dataType)
      return dataType.takeError();
    std::vector<std::string> stageReady;
    for (uint64_t stage = 0; stage + 1 < queue.latency; ++stage) {
      std::string ready = newValue();
      body << "    " << ready << " = pyc.wire : i1\n";
      stageReady.push_back(std::move(ready));
    }
    stageReady.push_back(readyWires[queue.name]);
    std::string currentValid = producerValid;
    std::string currentData = producerData;
    std::string firstReady;
    for (uint64_t stage = 0; stage < queue.latency; ++stage) {
      std::string inReady = newValue();
      std::string outValid = newValue();
      std::string outData = newValue();
      const uint64_t depth = stage == 0 ? queue.depth : 1;
      body << "    " << inReady << ", " << outValid << ", " << outData
           << " = pyc.fifo %clk, %rst, " << currentValid << ", " << currentData
           << ", " << stageReady[stage] << " {depth = " << depth
           << "} : " << *dataType << "\n";
      if (stage == 0)
        firstReady = inReady;
      else
        body << "    pyc.assign " << stageReady[stage - 1] << ", " << inReady
             << " : i1\n";
      currentValid = std::move(outValid);
      currentData = std::move(outData);
    }
    inputReady[queue.name] = std::move(firstReady);
    outputValid[queue.name] = std::move(currentValid);
    outputData[queue.name] = std::move(currentData);
  }
  for (const MemoryInstancePlan &instance : plan.memoryInstances) {
    std::vector<const QueueBlockPlan *> endpoints;
    for (const QueueBlockPlan &block : plan.blocks)
      if (block.kind == "memory_request" &&
          block.memoryInstance == instance.name)
        endpoints.push_back(&block);
    llvm::sort(endpoints,
               [](const QueueBlockPlan *left, const QueueBlockPlan *right) {
                 return left->endpointOrdinal < right->endpointOrdinal;
               });
    if (endpoints.empty())
      return pycError("memory instance has no endpoints");
    const QueuePlan *firstInput =
        findQueue(plan, endpoints.front()->inputs.front());
    if (!firstInput)
      return pycError("memory endpoint input Queue is missing");
    auto requestType = pycType(plan, firstInput->payloadType);
    auto dataType = pycType(plan, instance.dataType);
    auto dataWidth = typeWidth(plan, instance.dataType);
    if (!requestType)
      return requestType.takeError();
    if (!dataType)
      return dataType.takeError();
    if (!dataWidth)
      return dataWidth.takeError();

    std::vector<std::string> endpointValids;
    std::vector<std::string> endpointData;
    std::vector<std::string> addresses;
    std::vector<unsigned> addressWidths;
    std::vector<std::string> writes;
    std::vector<std::string> writeData;
    unsigned addressWidth = 1;
    for (const QueueBlockPlan *endpoint : endpoints) {
      auto valid = outputValid.find(endpoint->inputs.front());
      auto data = outputData.find(endpoint->inputs.front());
      const QueuePlan *input = findQueue(plan, endpoint->inputs.front());
      if (valid == outputValid.end() || data == outputData.end() || !input)
        return pycError("memory inputs are not available after Queue lowering");
      endpointValids.push_back(valid->getValue());
      endpointData.push_back(data->getValue());
      for (size_t policy = 0; policy < 3; ++policy) {
        auto value =
            emitTransform(plan, *endpoint, {data->getValue()},
                          {input->payloadType}, policy, nextValue, body);
        if (!value)
          return value.takeError();
        auto yielded = yieldedType(*endpoint, endpoint->yields[policy],
                                   input->payloadType);
        if (!yielded)
          return yielded.takeError();
        auto width = typeWidth(plan, *yielded);
        if (!width)
          return width.takeError();
        if (policy == 0) {
          addresses.push_back(std::move(*value));
          addressWidths.push_back(*width);
          addressWidth = std::max(addressWidth, *width);
        } else if (policy == 1) {
          auto type = pycType(plan, *yielded);
          if (!type)
            return type.takeError();
          if (*type != "i1")
            return pycError("memory write policy must lower to i1");
          writes.push_back(std::move(*value));
        } else {
          if (*width != *dataWidth)
            return pycError(
                "memory data policy must match instance data width");
          writeData.push_back(std::move(*value));
        }
      }
    }
    const std::string addressType = "i" + std::to_string(addressWidth);
    for (size_t index = 0; index < addresses.size(); ++index) {
      if (addressWidths[index] == addressWidth)
        continue;
      std::string zero = emitConstant(
          0, "i" + std::to_string(addressWidth - addressWidths[index]));
      std::string extended = newValue();
      body << "    " << extended << " = pyc.concat(" << zero << ", "
           << addresses[index] << ") : (i"
           << addressWidth - addressWidths[index] << ", i"
           << addressWidths[index] << ") -> " << addressType << "\n";
      addresses[index] = std::move(extended);
    }

    std::string zeroI1 = emitConstant(0, "i1");
    std::string oneI1 = emitConstant(1, "i1");
    std::string busyNext = newValue();
    std::string busyEnable = newValue();
    body << "    " << busyNext << " = pyc.wire : i1\n";
    body << "    " << busyEnable << " = pyc.wire : i1\n";
    std::string busy = newValue();
    body << "    " << busy << " = pyc.reg %clk, %rst, " << busyEnable << ", "
         << busyNext << ", " << zeroI1 << " : i1\n";
    std::string idle = emitNot(busy);
    std::vector<std::string> grants;
    std::string noHigher = oneI1;
    for (size_t index = 0; index < endpoints.size(); ++index) {
      std::string ready = emitBinary("and", idle, noHigher, "i1");
      body << "    pyc.assign " << readyWires[endpoints[index]->inputs.front()]
           << ", " << ready << " : i1\n";
      grants.push_back(emitBinary("and", endpointValids[index], ready, "i1"));
      noHigher =
          emitBinary("and", noHigher, emitNot(endpointValids[index]), "i1");
    }
    std::string issue = reduceBalanced("or", grants, "i1");
    auto selectedAddress =
        selectBalanced(grants, addresses, addressType).second;
    auto selectedWrite = selectBalanced(grants, writes, "i1").second;
    auto selectedWriteData =
        selectBalanced(grants, writeData, *dataType).second;
    auto selectedRequest =
        selectBalanced(grants, endpointData, *requestType).second;
    const bool fullAddressRange =
        addressWidth < 64 && instance.entries == (uint64_t{1} << addressWidth);
    if (!fullAddressRange) {
      std::string addressLimit = emitConstant(instance.entries, addressType);
      std::string addressSafe =
          emitBinary("ult", selectedAddress, addressLimit, addressType);
      std::string accessSafe =
          emitBinary("or", emitNot(issue), addressSafe, "i1");
      body << "    pyc.assert " << accessSafe
           << " {msg = \"memory_address_out_of_range\"}\n";
    }

    unsigned ownerWidth = 1;
    while ((uint64_t{1} << ownerWidth) < endpoints.size())
      ++ownerWidth;
    std::string ownerType = "i" + std::to_string(ownerWidth);
    std::vector<std::string> ownerConstants;
    for (size_t index = 0; index < endpoints.size(); ++index)
      ownerConstants.push_back(emitConstant(index, ownerType));
    std::string selectedOwner =
        selectBalanced(grants, ownerConstants, ownerType).second;
    std::string ownerNext = newValue();
    std::string ownerEnable = newValue();
    body << "    " << ownerNext << " = pyc.wire : " << ownerType << "\n";
    body << "    " << ownerEnable << " = pyc.wire : i1\n";
    std::string owner = newValue();
    body << "    " << owner << " = pyc.reg %clk, %rst, " << ownerEnable << ", "
         << ownerNext << ", " << ownerConstants.front() << " : " << ownerType
         << "\n";
    body << "    pyc.assign " << ownerNext << ", " << selectedOwner << " : "
         << ownerType << "\n";
    body << "    pyc.assign " << ownerEnable << ", " << issue << " : i1\n";

    std::string requestNext = newValue();
    std::string requestEnable = newValue();
    body << "    " << requestNext << " = pyc.wire : " << *requestType << "\n";
    body << "    " << requestEnable << " = pyc.wire : i1\n";
    std::string zeroRequest = emitConstant(0, *requestType);
    std::string pendingRequest = newValue();
    body << "    " << pendingRequest << " = pyc.reg %clk, %rst, "
         << requestEnable << ", " << requestNext << ", " << zeroRequest << " : "
         << *requestType << "\n";
    body << "    pyc.assign " << requestNext << ", " << selectedRequest << " : "
         << *requestType << "\n";
    body << "    pyc.assign " << requestEnable << ", " << issue << " : i1\n";

    std::string writeValid = emitBinary("and", issue, selectedWrite, "i1");
    const unsigned strobeWidth = (*dataWidth + 7) / 8;
    const uint64_t strobeValue = (uint64_t{1} << strobeWidth) - 1;
    std::string strobeType = "i" + std::to_string(strobeWidth);
    std::string strobe = emitConstant(strobeValue, strobeType);
    std::string readData = newValue();
    body << "    " << readData << " = pyc.sync_mem %clk, %rst, " << issue
         << ", " << selectedAddress << ", " << writeValid << ", "
         << selectedAddress << ", " << selectedWriteData << ", " << strobe
         << " {depth = " << instance.entries << ", name = \"" << instance.name
         << "\"} : " << addressType << ", " << *dataType << ", " << strobeType
         << "\n";

    std::string responseMature = busy;
    if (instance.latency > 1) {
      const unsigned latencyWidth = std::max(
          1u, static_cast<unsigned>(std::bit_width(instance.latency - 1)));
      const std::string latencyType = "i" + std::to_string(latencyWidth);
      std::string zeroLatency = emitConstant(0, latencyType);
      std::string oneLatency = emitConstant(1, latencyType);
      std::string initialLatency =
          emitConstant(instance.latency - 1, latencyType);
      std::string latencyNext = newValue();
      std::string latencyEnable = newValue();
      body << "    " << latencyNext << " = pyc.wire : " << latencyType << "\n";
      body << "    " << latencyEnable << " = pyc.wire : i1\n";
      std::string remainingLatency = newValue();
      body << "    " << remainingLatency << " = pyc.reg %clk, %rst, "
           << latencyEnable << ", " << latencyNext << ", " << zeroLatency
           << " : " << latencyType << "\n";
      std::string latencyIsZero =
          emitBinary("eq", remainingLatency, zeroLatency, latencyType);
      std::string latencyActive =
          emitBinary("and", busy, emitNot(latencyIsZero), "i1");
      std::string decremented =
          emitBinary("sub", remainingLatency, oneLatency, latencyType);
      std::string nextLatency =
          emitMux(issue, initialLatency, decremented, latencyType);
      std::string updateLatency = emitBinary("or", issue, latencyActive, "i1");
      body << "    pyc.assign " << latencyNext << ", " << nextLatency << " : "
           << latencyType << "\n";
      body << "    pyc.assign " << latencyEnable << ", " << updateLatency
           << " : i1\n";
      responseMature = emitBinary("and", busy, latencyIsZero, "i1");
    }

    std::vector<std::string> accepted;
    for (size_t index = 0; index < endpoints.size(); ++index) {
      std::string selected =
          emitBinary("eq", owner, ownerConstants[index], ownerType);
      std::string responseValid =
          emitBinary("and", responseMature, selected, "i1");
      auto response = emitFieldReplace(pendingRequest, firstInput->payloadType,
                                       endpoints[index]->resultField, readData);
      if (!response)
        return response.takeError();
      body << "    pyc.assign "
           << memoryResponseValid[endpoints[index]->outputs.front()] << ", "
           << responseValid << " : i1\n";
      body << "    pyc.assign "
           << memoryResponseData[endpoints[index]->outputs.front()] << ", "
           << *response << " : " << *requestType << "\n";
      accepted.push_back(
          emitBinary("and", responseValid,
                     inputReady[endpoints[index]->outputs.front()], "i1"));
    }
    std::string responseAccepted = reduceBalanced("or", accepted, "i1");
    std::string retained =
        emitBinary("and", busy, emitNot(responseAccepted), "i1");
    std::string nextBusy = emitBinary("or", retained, issue, "i1");
    std::string updateBusy = emitBinary("or", responseAccepted, issue, "i1");
    body << "    pyc.assign " << busyNext << ", " << nextBusy << " : i1\n";
    body << "    pyc.assign " << busyEnable << ", " << updateBusy << " : i1\n";
  }
  for (const QueueBlockPlan &block : plan.blocks) {
    if (block.kind == "transform") {
      if (block.inputs.size() == 1 && block.outputs.size() == 1) {
        body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
             << inputReady[block.outputs.front()] << " : i1\n";
      } else {
        std::string allReady = inputReady[block.outputs.front()];
        for (size_t index = 1; index < block.outputs.size(); ++index)
          allReady = emitBinary("and", allReady,
                                inputReady[block.outputs[index]], "i1");
        std::string allValid = outputValid[block.inputs.front()];
        for (size_t index = 1; index < block.inputs.size(); ++index)
          allValid = emitBinary("and", allValid,
                                outputValid[block.inputs[index]], "i1");
        for (auto [index, input] : llvm::enumerate(block.inputs)) {
          std::string inputCanFire = allReady;
          for (auto [otherIndex, other] : llvm::enumerate(block.inputs)) {
            if (otherIndex == index)
              continue;
            inputCanFire =
                emitBinary("and", inputCanFire, outputValid[other], "i1");
          }
          body << "    pyc.assign " << readyWires[input] << ", " << inputCanFire
               << " : i1\n";
        }
        for (auto [index, output] : llvm::enumerate(block.outputs)) {
          auto validWire = atomicTransformValid.find(output);
          if (validWire == atomicTransformValid.end())
            return pycError("atomic transform valid wire is missing");
          std::string outputCanFire = allValid;
          for (auto [otherIndex, other] : llvm::enumerate(block.outputs)) {
            if (otherIndex == index)
              continue;
            outputCanFire =
                emitBinary("and", outputCanFire, inputReady[other], "i1");
          }
          body << "    pyc.assign " << validWire->getValue() << ", "
               << outputCanFire << " : i1\n";
        }
      }
    } else if (block.kind == "barrier") {
      std::string allReady = inputReady[block.outputs.front()];
      for (size_t index = 1; index < block.outputs.size(); ++index)
        allReady =
            emitBinary("and", allReady, inputReady[block.outputs[index]], "i1");
      std::string allValid = outputValid[block.inputs.front()];
      for (size_t index = 1; index < block.inputs.size(); ++index)
        allValid =
            emitBinary("and", allValid, outputValid[block.inputs[index]], "i1");
      for (auto [index, input] : llvm::enumerate(block.inputs)) {
        std::string inputCanFire = allReady;
        for (auto [otherIndex, other] : llvm::enumerate(block.inputs)) {
          if (otherIndex == index)
            continue;
          inputCanFire =
              emitBinary("and", inputCanFire, outputValid[other], "i1");
        }
        body << "    pyc.assign " << readyWires[input] << ", " << inputCanFire
             << " : i1\n";
      }
      for (auto [index, output] : llvm::enumerate(block.outputs)) {
        auto validWire = atomicTransformValid.find(output);
        if (validWire == atomicTransformValid.end())
          return pycError("barrier valid wire is missing");
        std::string outputCanFire = allValid;
        for (auto [otherIndex, other] : llvm::enumerate(block.outputs)) {
          if (otherIndex == index)
            continue;
          outputCanFire =
              emitBinary("and", outputCanFire, inputReady[other], "i1");
        }
        body << "    pyc.assign " << validWire->getValue() << ", "
             << outputCanFire << " : i1\n";
      }
    } else if (block.kind == "broadcast") {
      std::string allReady = inputReady[block.outputs.front()];
      for (size_t index = 1; index < block.outputs.size(); ++index)
        allReady =
            emitBinary("and", allReady, inputReady[block.outputs[index]], "i1");
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << allReady << " : i1\n";
    } else if (block.kind == "fork") {
      auto state = forkStates.find(block.name);
      auto inputValidValue = outputValid.find(block.inputs.front());
      if (state == forkStates.end() || inputValidValue == outputValid.end())
        return pycError("fork state or input valid is missing");
      std::vector<std::string> deliveredNow;
      for (auto [index, output] : llvm::enumerate(block.outputs)) {
        std::string accepted =
            emitBinary("and", forkOfferValid[output], inputReady[output], "i1");
        deliveredNow.push_back(emitBinary(
            "or", state->getValue().delivered[index], accepted, "i1"));
      }
      std::string deliveredAll = deliveredNow.front();
      for (size_t index = 1; index < deliveredNow.size(); ++index)
        deliveredAll =
            emitBinary("and", deliveredAll, deliveredNow[index], "i1");
      std::string complete =
          emitBinary("and", inputValidValue->getValue(), deliveredAll, "i1");
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << complete << " : i1\n";
      std::string zero = emitConstant(0, "i1");
      for (size_t index = 0; index < block.outputs.size(); ++index) {
        std::string next = emitMux(complete, zero, deliveredNow[index], "i1");
        body << "    pyc.assign " << state->getValue().nextWires[index] << ", "
             << next << " : i1\n";
        body << "    pyc.assign " << state->getValue().enableWires[index]
             << ", " << inputValidValue->getValue() << " : i1\n";
      }
    } else if (block.kind == "route") {
      std::string selectedReady = emitConstant(0, "i1");
      for (size_t index = block.outputs.size(); index-- > 0;) {
        auto condition = routeCondition.find(block.outputs[index]);
        if (condition == routeCondition.end())
          return pycError("route output condition is missing");
        selectedReady =
            emitMux(condition->getValue(), inputReady[block.outputs[index]],
                    selectedReady, "i1");
      }
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << selectedReady << " : i1\n";
    } else if (block.kind == "select") {
      auto state = selectStates.find(block.name);
      if (state == selectStates.end())
        return pycError("select state is missing");
      const std::string &outputReady = inputReady[block.outputs.front()];
      std::string controlReady =
          emitBinary("and", outputReady, state->getValue().selectedValid, "i1");
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << controlReady << " : i1\n";
      body << "    pyc.assert " << state->getValue().selectorSafe
           << " {msg = \"select_selector_out_of_range\"}\n";
      for (size_t index = 1; index < block.inputs.size(); ++index) {
        std::string selected =
            emitBinary("and", state->getValue().controlValid,
                       state->getValue().conditions[index - 1], "i1");
        std::string ready = emitBinary("and", outputReady, selected, "i1");
        body << "    pyc.assign " << readyWires[block.inputs[index]] << ", "
             << ready << " : i1\n";
      }
    } else if (block.kind == "merge") {
      auto grants = mergeGrants.find(block.name);
      if (grants == mergeGrants.end() ||
          grants->getValue().size() != block.inputs.size())
        return pycError("merge grant plan is missing");
      const std::string &ready = inputReady[block.outputs.front()];
      for (auto [index, input] : llvm::enumerate(block.inputs)) {
        std::string accepted =
            emitBinary("and", ready, grants->getValue()[index], "i1");
        body << "    pyc.assign " << readyWires[input] << ", " << accepted
             << " : i1\n";
      }
      auto state = mergeStates.find(block.name);
      if (state != mergeStates.end()) {
        std::string accepted =
            emitBinary("and", ready, state->getValue().valid, "i1");
        body << "    pyc.assign " << state->getValue().enableWire << ", "
             << accepted << " : i1\n";
        std::string next = state->getValue().cursor;
        for (size_t index = block.inputs.size(); index-- > 0;) {
          std::string nextValue = emitConstant(
              (index + 1) % block.inputs.size(), state->getValue().type);
          next = emitMux(grants->getValue()[index], nextValue, next,
                         state->getValue().type);
        }
        body << "    pyc.assign " << state->getValue().nextWire << ", " << next
             << " : " << state->getValue().type << "\n";
      }
    } else if (block.kind == "credit") {
      auto state = creditStates.find(block.name);
      auto inputValidValue = outputValid.find(block.inputs.front());
      auto inputDataValue = outputData.find(block.inputs.front());
      if (state == creditStates.end() || inputValidValue == outputValid.end() ||
          inputDataValue == outputData.end())
        return pycError("credit state or input is missing");
      const std::string &outputReady = inputReady[block.outputs.front()];
      std::string retire =
          emitBinary("and", state->getValue().outputDone, outputReady, "i1");
      std::string inputReadyValue =
          emitBinary("and", state->getValue().anyFree,
                     state->getValue().safeAdmission, "i1");
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << inputReadyValue << " : i1\n";
      body << "    pyc.assert " << state->getValue().safeAdmission
           << " {msg = \"credit_nonpositive_cost\"}\n";
      std::string admit =
          emitBinary("and", inputValidValue->getValue(), inputReadyValue, "i1");
      std::string zeroValid = emitConstant(0, "i1");
      std::string oneValid = emitConstant(1, "i1");
      std::string oneCost = emitConstant(1, state->getValue().costType);
      std::vector<std::string> freeGrants;
      std::vector<std::string> doneGrants;
      for (size_t index = 0; index < state->getValue().slots.size(); ++index) {
        std::string indexValue =
            emitConstant(index, state->getValue().freeIndexType);
        freeGrants.push_back(emitBinary("eq", state->getValue().freeIndex,
                                        indexValue,
                                        state->getValue().freeIndexType));
        doneGrants.push_back(emitBinary("eq", state->getValue().doneIndex,
                                        indexValue,
                                        state->getValue().freeIndexType));
      }
      for (auto [index, slot] : llvm::enumerate(state->getValue().slots)) {
        std::string admitSlot =
            emitBinary("and", admit, freeGrants[index], "i1");
        std::string retireSlot =
            emitBinary("and", retire, doneGrants[index], "i1");
        std::string active =
            emitBinary("and", slot.valid, emitNot(slot.done), "i1");
        std::string decremented = emitBinary("sub", slot.remaining, oneCost,
                                             state->getValue().costType);
        std::string nextRemaining = emitMux(active, decremented, slot.remaining,
                                            state->getValue().costType);
        nextRemaining = emitMux(admitSlot, state->getValue().inputCost,
                                nextRemaining, state->getValue().costType);
        std::string nextValid =
            emitMux(retireSlot, zeroValid, slot.valid, "i1");
        nextValid = emitMux(admitSlot, oneValid, nextValid, "i1");
        std::string nextData = emitMux(admitSlot, inputDataValue->getValue(),
                                       slot.data, state->getValue().dataType);
        std::string nextState = newValue();
        body << "    " << nextState << " = pyc.concat(" << nextValid << ", "
             << nextRemaining << ", " << nextData << ") : (i1, "
             << state->getValue().costType << ", " << state->getValue().dataType
             << ") -> " << state->getValue().slotType << "\n";
        std::string changed =
            reduceBalanced("or", {admitSlot, retireSlot, active}, "i1");
        body << "    pyc.assign " << slot.next << ", " << nextState << " : "
             << state->getValue().slotType << "\n";
        body << "    pyc.assign " << slot.enable << ", " << changed
             << " : i1\n";
      }
    } else if (block.kind == "memory_request") {
      // Shared memory-instance arbitration and endpoint ready/response demux
      // are emitted once above, after every Queue signal is available.
    } else if (block.kind == "dependency") {
      auto state = dependencyStates.find(block.name);
      auto inputValidValue = outputValid.find(block.inputs.front());
      auto inputDataValue = outputData.find(block.inputs.front());
      if (state == dependencyStates.end() ||
          inputValidValue == outputValid.end() ||
          inputDataValue == outputData.end())
        return pycError("dependency state or input is missing");
      const std::string &outputReady = inputReady[block.outputs.front()];
      std::string retire =
          emitBinary("and", state->getValue().outputDone, outputReady, "i1");
      std::string inputReadyValue =
          emitBinary("and", state->getValue().anyFree,
                     state->getValue().safeAdmission, "i1");
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << inputReadyValue << " : i1\n";
      std::string admit =
          emitBinary("and", inputValidValue->getValue(), inputReadyValue, "i1");
      std::string zeroValid = emitConstant(0, "i1");
      std::string oneValid = emitConstant(1, "i1");
      std::string waitingPhase = emitConstant(0, "i2");
      std::string executingPhase = emitConstant(1, "i2");
      std::string donePhase = emitConstant(2, "i2");
      std::string zeroCost = emitConstant(0, state->getValue().costType);
      std::string oneCost = emitConstant(1, state->getValue().costType);
      std::string noDependency = emitConstant(state->getValue().noDependency,
                                              state->getValue().keyType);

      std::vector<std::string> freeGrants;
      std::vector<std::string> doneGrants;
      std::vector<std::string> indexValues;
      freeGrants.reserve(state->getValue().slots.size());
      doneGrants.reserve(state->getValue().slots.size());
      indexValues.reserve(state->getValue().slots.size());
      for (size_t index = 0; index < state->getValue().slots.size(); ++index) {
        std::string indexValue =
            emitConstant(index, state->getValue().freeIndexType);
        indexValues.push_back(indexValue);
        freeGrants.push_back(emitBinary("eq", state->getValue().freeIndex,
                                        indexValue,
                                        state->getValue().freeIndexType));
        doneGrants.push_back(emitBinary("eq", state->getValue().doneIndex,
                                        indexValue,
                                        state->getValue().freeIndexType));
      }

      std::vector<std::string> waitingValues;
      std::vector<std::string> executingValues;
      std::vector<std::string> completeValues;
      std::vector<std::string> decrementValues;
      std::vector<std::string> decrementedValues;
      std::vector<std::string> dependencyReadyValues;
      for (const DependencySlotState &slot : state->getValue().slots) {
        std::string isWaiting =
            emitBinary("eq", slot.phase, waitingPhase, "i2");
        waitingValues.push_back(emitBinary("and", slot.valid, isWaiting, "i1"));
        std::string isExecuting =
            emitBinary("eq", slot.phase, executingPhase, "i2");
        isExecuting = emitBinary("and", slot.valid, isExecuting, "i1");
        executingValues.push_back(isExecuting);
        std::string sentinel = emitBinary("eq", slot.predecessor, noDependency,
                                          state->getValue().keyType);
        std::vector<std::string> predecessorMatches;
        predecessorMatches.reserve(state->getValue().slots.size());
        for (const DependencySlotState &candidate : state->getValue().slots) {
          std::string same = emitBinary("eq", candidate.key, slot.predecessor,
                                        state->getValue().keyType);
          predecessorMatches.push_back(
              emitBinary("and", candidate.done, same, "i1"));
        }
        std::string predecessorDone =
            reduceBalanced("or", predecessorMatches, "i1");
        dependencyReadyValues.push_back(
            emitBinary("or", sentinel, predecessorDone, "i1"));
        std::string atOne = emitBinary("eq", slot.remaining, oneCost,
                                       state->getValue().costType);
        completeValues.push_back(emitBinary("and", isExecuting, atOne, "i1"));
        std::string notAtOne = emitNot(atOne);
        decrementValues.push_back(
            emitBinary("and", isExecuting, notAtOne, "i1"));
        decrementedValues.push_back(emitBinary("sub", slot.remaining, oneCost,
                                               state->getValue().costType));
      }

      std::string noIssue = emitConstant(0, "i1");
      std::vector<std::string> issueValues(state->getValue().slots.size(),
                                           noIssue);
      for (uint64_t resource = 0; resource < state->getValue().resources;
           ++resource) {
        std::string resourceValue =
            emitConstant(resource, state->getValue().resourceType);
        std::vector<std::string> candidates;
        std::vector<std::string> blockers;
        candidates.reserve(state->getValue().slots.size());
        blockers.reserve(state->getValue().slots.size());
        for (auto [index, slot] : llvm::enumerate(state->getValue().slots)) {
          std::string sameResource =
              emitBinary("eq", slot.resource, resourceValue,
                         state->getValue().resourceType);
          std::string ready = emitBinary("and", waitingValues[index],
                                         dependencyReadyValues[index], "i1");
          candidates.push_back(emitBinary("and", ready, sameResource, "i1"));
          std::string stillExecuting =
              emitBinary("and", executingValues[index],
                         emitNot(completeValues[index]), "i1");
          blockers.push_back(
              emitBinary("and", stillExecuting, sameResource, "i1"));
        }
        auto selected = selectBalanced(candidates, indexValues,
                                       state->getValue().freeIndexType);
        std::string resourceBusy = reduceBalanced("or", blockers, "i1");
        std::string resourceCanIssue =
            emitBinary("and", selected.first, emitNot(resourceBusy), "i1");
        for (size_t index = 0; index < state->getValue().slots.size();
             ++index) {
          std::string selectedSlot =
              emitBinary("eq", selected.second, indexValues[index],
                         state->getValue().freeIndexType);
          std::string issue =
              emitBinary("and", resourceCanIssue, selectedSlot, "i1");
          issueValues[index] =
              emitBinary("or", issueValues[index], issue, "i1");
        }
      }

      for (auto [index, slot] : llvm::enumerate(state->getValue().slots)) {
        std::string admitSlot =
            emitBinary("and", admit, freeGrants[index], "i1");
        std::string retireSlot =
            emitBinary("and", retire, doneGrants[index], "i1");
        const std::string &issue = issueValues[index];
        const std::string &complete = completeValues[index];
        const std::string &decrement = decrementValues[index];
        const std::string &decremented = decrementedValues[index];

        std::string nextValid =
            emitMux(retireSlot, zeroValid, slot.valid, "i1");
        nextValid = emitMux(admitSlot, oneValid, nextValid, "i1");
        std::string nextPhase = emitMux(complete, donePhase, slot.phase, "i2");
        nextPhase = emitMux(issue, executingPhase, nextPhase, "i2");
        nextPhase = emitMux(admitSlot, waitingPhase, nextPhase, "i2");
        std::string nextRemaining = emitMux(
            decrement, decremented, slot.remaining, state->getValue().costType);
        nextRemaining = emitMux(complete, zeroCost, nextRemaining,
                                state->getValue().costType);
        nextRemaining = emitMux(issue, slot.cost, nextRemaining,
                                state->getValue().costType);
        nextRemaining = emitMux(admitSlot, zeroCost, nextRemaining,
                                state->getValue().costType);
        std::string nextKey = emitMux(admitSlot, state->getValue().inputKey,
                                      slot.key, state->getValue().keyType);
        std::string nextPredecessor =
            emitMux(admitSlot, state->getValue().inputPredecessor,
                    slot.predecessor, state->getValue().keyType);
        std::string nextResource =
            emitMux(admitSlot, state->getValue().inputResource, slot.resource,
                    state->getValue().resourceType);
        std::string nextCost = emitMux(admitSlot, state->getValue().inputCost,
                                       slot.cost, state->getValue().costType);
        std::string nextData = emitMux(admitSlot, inputDataValue->getValue(),
                                       slot.data, state->getValue().dataType);
        std::string nextState = newValue();
        body << "    " << nextState << " = pyc.concat(" << nextValid << ", "
             << nextPhase << ", " << nextKey << ", " << nextPredecessor << ", "
             << nextResource << ", " << nextRemaining << ", " << nextCost
             << ", " << nextData << ") : (i1, i2, " << state->getValue().keyType
             << ", " << state->getValue().keyType << ", "
             << state->getValue().resourceType << ", "
             << state->getValue().costType << ", " << state->getValue().costType
             << ", " << state->getValue().dataType << ") -> "
             << state->getValue().slotType << "\n";
        std::vector<std::string> changes = {admitSlot, retireSlot, issue,
                                            complete, decrement};
        std::string changed = reduceBalanced("or", changes, "i1");
        body << "    pyc.assign " << slot.next << ", " << nextState << " : "
             << state->getValue().slotType << "\n";
        body << "    pyc.assign " << slot.enable << ", " << changed
             << " : i1\n";
      }
    } else if (block.kind == "reorder") {
      auto state = reorderStates.find(block.name);
      auto inputValidValue = outputValid.find(block.inputs.front());
      auto inputDataValue = outputData.find(block.inputs.front());
      if (state == reorderStates.end() ||
          inputValidValue == outputValid.end() ||
          inputDataValue == outputData.end())
        return pycError("reorder state or input is missing");
      const std::string &outputReady = inputReady[block.outputs.front()];
      std::string retire =
          emitBinary("and", state->getValue().outputMatch, outputReady, "i1");
      std::string inputReadyValue =
          emitBinary("and", state->getValue().anyFree,
                     state->getValue().safeAdmission, "i1");
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << inputReadyValue << " : i1\n";
      std::string admit =
          emitBinary("and", inputValidValue->getValue(), inputReadyValue, "i1");
      std::string zero = emitConstant(0, "i1");
      std::string one = emitConstant(1, "i1");
      std::string admittedState = newValue();
      body << "    " << admittedState << " = pyc.concat(" << one << ", "
           << state->getValue().inputKey << ", " << inputDataValue->getValue()
           << ") : (i1, " << state->getValue().keyType << ", "
           << state->getValue().dataType << ") -> "
           << state->getValue().slotType << "\n";
      std::vector<std::string> grants;
      grants.reserve(state->getValue().slots.size());
      for (size_t index = 0; index < state->getValue().slots.size(); ++index) {
        std::string indexValue =
            emitConstant(index, state->getValue().freeIndexType);
        grants.push_back(emitBinary("eq", state->getValue().freeIndex,
                                    indexValue,
                                    state->getValue().freeIndexType));
      }
      for (auto [index, slot] : llvm::enumerate(state->getValue().slots)) {
        std::string admitSlot = emitBinary("and", admit, grants[index], "i1");
        std::string retireSlot = emitBinary("and", retire, slot.match, "i1");
        std::string changed = emitBinary("or", admitSlot, retireSlot, "i1");
        std::string clearedState = newValue();
        body << "    " << clearedState << " = pyc.concat(" << zero << ", "
             << slot.key << ", " << slot.data << ") : (i1, "
             << state->getValue().keyType << ", " << state->getValue().dataType
             << ") -> " << state->getValue().slotType << "\n";
        std::string afterRetire = emitMux(retireSlot, clearedState, slot.state,
                                          state->getValue().slotType);
        std::string nextState = emitMux(admitSlot, admittedState, afterRetire,
                                        state->getValue().slotType);
        body << "    pyc.assign " << slot.next << ", " << nextState << " : "
             << state->getValue().slotType << "\n";
        body << "    pyc.assign " << slot.enable << ", " << changed
             << " : i1\n";
      }
      std::string keyOne = emitConstant(1, state->getValue().keyType);
      std::string nextExpected = emitBinary("add", state->getValue().expected,
                                            keyOne, state->getValue().keyType);
      body << "    pyc.assign " << state->getValue().expectedNext << ", "
           << nextExpected << " : " << state->getValue().keyType << "\n";
      body << "    pyc.assign " << state->getValue().expectedEnable << ", "
           << retire << " : i1\n";
    } else if (block.kind == "feedback") {
      auto state = feedbackStates.find(block.name);
      if (state == feedbackStates.end())
        return pycError("feedback state is missing");
      std::string notInternal = emitNot(state->getValue().valid);
      std::string canProceed =
          emitMux(state->getValue().condition, state->getValue().underLimit,
                  inputReady[block.outputs.front()], "i1");
      std::string accepted =
          emitBinary("and", state->getValue().selectedValid, canProceed, "i1");
      std::string externalReady =
          emitBinary("and", notInternal, canProceed, "i1");
      body << "    pyc.assign " << readyWires[block.inputs.front()] << ", "
           << externalReady << " : i1\n";
      std::string continueAccepted =
          emitBinary("and", accepted, state->getValue().condition, "i1");
      body << "    pyc.assign " << state->getValue().validNext << ", "
           << state->getValue().condition << " : i1\n";
      body << "    pyc.assign " << state->getValue().validEnable << ", "
           << accepted << " : i1\n";
      body << "    pyc.assign " << state->getValue().dataNext << ", "
           << state->getValue().updated << " : " << state->getValue().dataType
           << "\n";
      body << "    pyc.assign " << state->getValue().dataEnable << ", "
           << continueAccepted << " : i1\n";
      std::string one = emitConstant(1, state->getValue().iterationType);
      std::string nextIteration =
          emitBinary("add", state->getValue().selectedIteration, one,
                     state->getValue().iterationType);
      body << "    pyc.assign " << state->getValue().iterationNext << ", "
           << nextIteration << " : " << state->getValue().iterationType << "\n";
      body << "    pyc.assign " << state->getValue().iterationEnable << ", "
           << continueAccepted << " : i1\n";
      std::string atLimit = emitNot(state->getValue().underLimit);
      std::string limitCondition =
          emitBinary("and", state->getValue().condition, atLimit, "i1");
      std::string limitViolation = emitBinary(
          "and", state->getValue().selectedValid, limitCondition, "i1");
      std::string limitOk = emitNot(limitViolation);
      body << "    pyc.assert " << limitOk
           << " {msg = \"feedback_iteration_limit\"}\n";
    }
  }
  for (auto [index, sink] : llvm::enumerate(sinks))
    body << "    pyc.assign " << readyWires[sink->inputs.front()] << ", "
         << outputName(index, "ready") << " : i1\n";
  for (const QueueBlockPlan *observation : observations) {
    const QueuePlan *queue = findQueue(plan, observation->inputs.front());
    auto type = queue ? pycType(plan, queue->payloadType)
                      : llvm::Expected<std::string>(
                            pycError("observation Queue is missing"));
    if (!type)
      return type.takeError();
    std::string alias = newValue();
    body << "    " << alias << " = pyc.alias "
         << outputData[observation->inputs.front()] << " {pyc.name = \""
         << observation->name << "\"} : " << *type << "\n";
  }

  llvm::StringRef top = plan.system;
  std::vector<std::string> arguments = {"%clk: !pyc.clock", "%rst: !pyc.reset"};
  std::vector<std::string> argumentNames = {"clk", "rst"};
  for (size_t index = 0; index < sources.size(); ++index) {
    arguments.push_back(inputName(index, "valid") + ": i1");
    arguments.push_back(inputName(index, "data") + ": " +
                        inputPortTypes[index]);
    argumentNames.push_back(inputName(index, "valid").substr(1));
    argumentNames.push_back(inputName(index, "data").substr(1));
  }
  for (size_t index = 0; index < sinks.size(); ++index) {
    arguments.push_back(outputName(index, "ready") + ": i1");
    argumentNames.push_back(outputName(index, "ready").substr(1));
  }
  std::vector<std::string> resultTypes;
  std::vector<std::string> resultNames;
  std::vector<std::string> returnValues;
  for (auto [index, sink] : llvm::enumerate(sinks)) {
    resultTypes.push_back("i1");
    resultTypes.push_back(outputPortTypes[index]);
    resultNames.push_back(outputName(index, "valid").substr(1));
    resultNames.push_back(outputName(index, "data").substr(1));
    returnValues.push_back(outputValid[sink->inputs.front()]);
    returnValues.push_back(outputData[sink->inputs.front()]);
  }
  for (auto [index, source] : llvm::enumerate(sources)) {
    resultTypes.push_back("i1");
    resultNames.push_back(inputName(index, "ready").substr(1));
    returnValues.push_back(inputReady[source->outputs.front()]);
  }
  auto writeList =
      [](std::ostringstream &stream, const std::vector<std::string> &values,
         llvm::StringRef prefix = {}, llvm::StringRef suffix = {}) {
        for (auto [index, value] : llvm::enumerate(values)) {
          if (index)
            stream << ", ";
          stream << prefix.str() << value << suffix.str();
        }
      };
  std::ostringstream output;
  output << "module attributes {pyc.top = @" << top.str()
         << ", pyc.frontend.contract = \"pycircuit\"} {\n  func.func @"
         << top.str() << '(';
  writeList(output, arguments);
  output << ") -> (";
  writeList(output, resultTypes);
  output << ") attributes {arg_names = [";
  writeList(output, argumentNames, "\"", "\"");
  output << "], result_names = [";
  writeList(output, resultNames, "\"", "\"");
  output << "], pyc.value_params = [], pyc.value_param_types = [], "
            "pyc.kind = \"module\", pyc.inline = \"false\", "
            "pyc.params = \"{}\", pyc.base = \""
         << top.str() << "\", pyc.struct.metrics = \"" << kStructMetrics.str()
         << "\", pyc.struct.collections = \"[]\"} {\n"
         << body.str() << "    func.return ";
  writeList(output, returnValues);
  output << " : ";
  writeList(output, resultTypes);
  output << "\n  }\n}\n";
  return output.str();
}

} // namespace acir::codegen
