#include "acir/CodeGen/QueueGraphGenerator.h"
#include "acir/CodeGen/QueueBlockContract.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace acir::codegen {
namespace {

llvm::Error generatorError(const llvm::Twine &message) {
  return llvm::createStringError(
      std::make_error_code(std::errc::invalid_argument),
      "ACLOWER-QUEUE-CXX: " + message);
}

template <typename... Values>
void appendInitializer(std::vector<std::string> &initializers,
                       const Values &...values) {
  std::string initializer;
  llvm::raw_string_ostream output(initializer);
  (output << ... << values);
  output.flush();
  initializers.push_back(std::move(initializer));
}

std::string identifier(llvm::StringRef value) {
  std::string result;
  for (char character : value)
    result.push_back(
        std::isalnum(static_cast<unsigned char>(character)) ? character : '_');
  if (result.empty() ||
      std::isdigit(static_cast<unsigned char>(result.front())))
    result.insert(result.begin(), '_');
  return result;
}

std::string className(llvm::StringRef value) {
  std::string result;
  bool capitalize = true;
  for (char character : value) {
    if (!std::isalnum(static_cast<unsigned char>(character))) {
      capitalize = true;
      continue;
    }
    result.push_back(capitalize ? static_cast<char>(std::toupper(
                                      static_cast<unsigned char>(character)))
                                : character);
    capitalize = false;
  }
  if (result.empty() ||
      std::isdigit(static_cast<unsigned char>(result.front())))
    result.insert(result.begin(), '_');
  return result;
}

std::string cppStringLiteral(llvm::StringRef value) {
  std::string result = "\"";
  for (char character : value) {
    switch (character) {
    case '\\':
      result.append("\\\\");
      break;
    case '"':
      result.append("\\\"");
      break;
    case '\n':
      result.append("\\n");
      break;
    case '\r':
      result.append("\\r");
      break;
    case '\t':
      result.append("\\t");
      break;
    default:
      result.push_back(character);
      break;
    }
  }
  result.push_back('"');
  return result;
}

std::optional<llvm::StringRef> structTypeName(llvm::StringRef type) {
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

llvm::Expected<std::vector<const QueuePayloadPlan *>>
payloadEmissionOrder(const QueueGraphPlan &plan) {
  llvm::StringMap<const QueuePayloadPlan *> byName;
  for (const QueuePayloadPlan &payload : plan.payloads)
    if (!byName.try_emplace(payload.name, &payload).second)
      return generatorError("payload identities are duplicated");

  std::vector<const QueuePayloadPlan *> result;
  llvm::StringSet<> emitted;
  while (result.size() != plan.payloads.size()) {
    const size_t before = result.size();
    for (const QueuePayloadPlan &payload : plan.payloads) {
      if (emitted.contains(payload.name))
        continue;
      bool blocked = false;
      for (const QueuePayloadFieldPlan &field : payload.fields) {
        std::optional<llvm::StringRef> dependency = structTypeName(field.type);
        if (!dependency)
          continue;
        if (!byName.contains(*dependency))
          return generatorError("nested payload type is unresolved");
        blocked |= !emitted.contains(*dependency);
      }
      if (blocked)
        continue;
      emitted.insert(payload.name);
      result.push_back(&payload);
    }
    if (result.size() == before)
      return generatorError("nested payload definitions contain a cycle");
  }
  return result;
}

llvm::StringRef enumStorage(uint64_t width) {
  if (width <= 8)
    return "std::uint8_t";
  if (width <= 16)
    return "std::uint16_t";
  if (width <= 32)
    return "std::uint32_t";
  return "std::uint64_t";
}

llvm::Expected<std::string> cppType(llvm::StringRef type) {
  if (type.starts_with('i')) {
    unsigned width = 0;
    if (!type.drop_front().getAsInteger(10, width) && width > 0) {
      if (width <= 64)
        return "gfsim::UInt<" + std::to_string(width) + ">";
    }
  }
  if (std::optional<llvm::StringRef> name = structTypeName(type))
    return name->str();
  if (std::optional<llvm::StringRef> name = enumTypeName(type))
    return name->str();
  return generatorError("no C++ storage realization for ACIR type '" + type +
                        "'");
}

llvm::Expected<std::string>
cppPayloadFieldType(const QueueGraphPlan &plan,
                    const QueuePayloadFieldPlan &field) {
  auto aggregate =
      llvm::find_if(plan.aggregates, [&](const QueueAggregatePlan &candidate) {
        return candidate.type == field.type;
      });
  if (aggregate != plan.aggregates.end()) {
    if (field.width == 0 || field.width > 64 || field.width != aggregate->width)
      return generatorError("aggregate payload field width is unsupported");
    return "gfsim::UInt<" + std::to_string(field.width) + ">";
  }
  return cppType(field.type);
}

const QueuePayloadPlan *findPayloadType(const QueueGraphPlan &plan,
                                        llvm::StringRef type) {
  std::optional<llvm::StringRef> name = structTypeName(type);
  if (!name)
    return nullptr;
  auto found =
      llvm::find_if(plan.payloads, [&](const QueuePayloadPlan &payload) {
        return payload.name == *name;
      });
  return found == plan.payloads.end() ? nullptr : &*found;
}

const QueueEnumPlan *findEnumType(const QueueGraphPlan &plan,
                                  llvm::StringRef type) {
  std::optional<llvm::StringRef> name = enumTypeName(type);
  if (!name)
    return nullptr;
  auto found = llvm::find_if(plan.enums, [&](const QueueEnumPlan &enumeration) {
    return enumeration.name == *name;
  });
  return found == plan.enums.end() ? nullptr : &*found;
}

const QueueAggregatePlan *findAggregateType(const QueueGraphPlan &plan,
                                            llvm::StringRef type) {
  auto found =
      llvm::find_if(plan.aggregates, [&](const QueueAggregatePlan &aggregate) {
        return aggregate.type == type;
      });
  return found == plan.aggregates.end() ? nullptr : &*found;
}

llvm::Expected<uint64_t> generatedTypeWidth(const QueueGraphPlan &plan,
                                            llvm::StringRef type) {
  if (type.starts_with('i')) {
    uint64_t width = 0;
    if (!type.drop_front().getAsInteger(10, width) && width > 0 && width <= 64)
      return width;
  }
  if (const QueueEnumPlan *enumeration = findEnumType(plan, type))
    return enumeration->width;
  if (const QueueAggregatePlan *aggregate = findAggregateType(plan, type))
    return aggregate->width;
  if (const QueuePayloadPlan *payload = findPayloadType(plan, type)) {
    uint64_t width = 0;
    for (const QueuePayloadFieldPlan &field : payload->fields) {
      auto fieldWidth = generatedTypeWidth(plan, field.type);
      if (!fieldWidth)
        return fieldWidth.takeError();
      if (field.width != 0 && field.width != *fieldWidth)
        return generatorError("payload field width disagrees with its type");
      width += *fieldWidth;
    }
    if (width > 0 && width <= 64)
      return width;
  }
  return generatorError("no packed width for ACIR type '" + type + "'");
}

llvm::Expected<std::string> emitPackedValueImpl(const QueueGraphPlan &plan,
                                                llvm::StringRef type,
                                                llvm::StringRef value,
                                                llvm::StringSet<> &active) {
  if (type.starts_with('i') || findAggregateType(plan, type))
    return value.str();
  if (const QueueEnumPlan *enumeration = findEnumType(plan, type))
    return "gfsim::UInt<" + std::to_string(enumeration->width) +
           ">{static_cast<std::uint64_t>(" + value.str() + ")}";
  const QueuePayloadPlan *payload = findPayloadType(plan, type);
  if (!payload)
    return generatorError("cannot pack ACIR type '" + type + "'");
  if (!active.insert(payload->name).second)
    return generatorError("cannot pack recursive struct type '" + type + "'");
  std::vector<std::string> fields;
  fields.reserve(payload->fields.size());
  for (const QueuePayloadFieldPlan &field : payload->fields) {
    auto packed = emitPackedValueImpl(
        plan, field.type, "(" + value.str() + ")." + field.name, active);
    if (!packed) {
      active.erase(payload->name);
      return packed.takeError();
    }
    fields.push_back(std::move(*packed));
  }
  active.erase(payload->name);
  if (fields.empty())
    return generatorError("cannot pack an empty struct type '" + type + "'");
  if (fields.size() == 1)
    return fields.front();
  std::string result = "gfsim::bitConcat(";
  for (auto [index, field] : llvm::enumerate(fields)) {
    if (index)
      result.append(", ");
    result.append(field);
  }
  result.push_back(')');
  return result;
}

llvm::Expected<std::string> emitPackedValue(const QueueGraphPlan &plan,
                                            llvm::StringRef type,
                                            llvm::StringRef value) {
  llvm::StringSet<> active;
  return emitPackedValueImpl(plan, type, value, active);
}

llvm::Expected<std::string> emitUnpackedValueImpl(const QueueGraphPlan &plan,
                                                  llvm::StringRef type,
                                                  llvm::StringRef value,
                                                  llvm::StringSet<> &active) {
  if (type.starts_with('i') || findAggregateType(plan, type))
    return value.str();
  if (const QueueEnumPlan *enumeration = findEnumType(plan, type))
    return "static_cast<" + enumeration->name +
           ">(static_cast<std::uint64_t>((" + value.str() + ").value()))";
  const QueuePayloadPlan *payload = findPayloadType(plan, type);
  if (!payload)
    return generatorError("cannot unpack ACIR type '" + type + "'");
  if (!active.insert(payload->name).second)
    return generatorError("cannot unpack recursive struct type '" + type + "'");
  auto totalWidth = generatedTypeWidth(plan, type);
  if (!totalWidth) {
    active.erase(payload->name);
    return totalWidth.takeError();
  }
  uint64_t cursor = *totalWidth;
  std::string result = "[&]() { " + payload->name + " unpacked{}; ";
  for (const QueuePayloadFieldPlan &field : payload->fields) {
    auto fieldWidth = generatedTypeWidth(plan, field.type);
    if (!fieldWidth) {
      active.erase(payload->name);
      return fieldWidth.takeError();
    }
    cursor -= *fieldWidth;
    const std::string extracted =
        "gfsim::bitExtract<" + std::to_string(*fieldWidth) + ">(" +
        value.str() + ", " + std::to_string(cursor) + ")";
    auto unpacked = emitUnpackedValueImpl(plan, field.type, extracted, active);
    if (!unpacked) {
      active.erase(payload->name);
      return unpacked.takeError();
    }
    result.append("unpacked.")
        .append(field.name)
        .append(" = ")
        .append(*unpacked)
        .append("; ");
  }
  active.erase(payload->name);
  result.append("return unpacked; }()");
  return result;
}

llvm::Expected<std::string> emitUnpackedValue(const QueueGraphPlan &plan,
                                              llvm::StringRef type,
                                              llvm::StringRef value) {
  llvm::StringSet<> active;
  return emitUnpackedValueImpl(plan, type, value, active);
}

std::vector<std::string> pathParts(llvm::StringRef path) {
  std::vector<std::string> result;
  while (!path.empty()) {
    path = path.ltrim('/');
    if (path.empty())
      break;
    auto split = path.split('/');
    result.push_back(split.first.str());
    path = split.second;
  }
  return result;
}

std::string commonPath(llvm::StringRef left, llvm::StringRef right) {
  std::vector<std::string> lhs = pathParts(left);
  std::vector<std::string> rhs = pathParts(right);
  std::string result;
  for (size_t index = 0; index < std::min(lhs.size(), rhs.size()); ++index) {
    if (lhs[index] != rhs[index])
      break;
    result.push_back('/');
    result.append(lhs[index]);
  }
  return result.empty() ? "/" : result;
}

llvm::Expected<std::string>
emitExpressionBody(const QueueGraphPlan &plan, const QueueBlockPlan &block,
                   llvm::StringRef yield, unsigned indent,
                   bool qualifyTables = false, bool checkedTableAccess = true,
                   llvm::ArrayRef<std::string> additionalNeeded = {},
                   llvm::StringRef returnExpression = {}) {
  std::ostringstream output;
  std::string padding(indent, ' ');
  llvm::StringMap<std::string> priorityEncodings;
  llvm::StringMap<std::pair<std::string, std::string>> tableChoices;
  llvm::StringSet<> needed;
  needed.insert(yield);
  for (const std::string &name : additionalNeeded)
    needed.insert(name);
  for (const QueueExpressionPlan &expression : llvm::reverse(block.expressions))
    if (needed.contains(expression.result)) {
      for (const std::string &operand : expression.operands)
        needed.insert(operand);
      if (expression.kind == "snapshot_set")
        needed.insert(expression.field);
    }
  for (const QueueExpressionPlan &expression : block.expressions) {
    if (!needed.contains(expression.result))
      continue;
    auto operand = [&](size_t index) -> llvm::Expected<llvm::StringRef> {
      if (index >= expression.operands.size())
        return generatorError("expression operand arity mismatch");
      return llvm::StringRef(expression.operands[index]);
    };
    if (expression.kind == "enum_constant") {
      std::optional<llvm::StringRef> type = enumTypeName(expression.type);
      if (!type || expression.field.empty())
        return generatorError("enum constant expression is malformed");
      output << padding << "auto " << expression.result << " = " << type->str()
             << "::" << expression.field << ";\n";
      continue;
    }
    if (expression.kind == "constant") {
      llvm::StringRef literal = expression.literal;
      output << padding << "auto " << expression.result << " = "
             << literal.split(" : ").first.str() << ";\n";
      continue;
    }
    if (expression.kind == "slot_get_valid") {
      output << padding << "auto " << expression.result << " = slot_"
             << identifier(expression.slot) << "->valid;\n";
      continue;
    }
    if (expression.kind == "slot_get_value") {
      output << padding << "auto " << expression.result << " = slot_"
             << identifier(expression.slot) << "->value;\n";
      continue;
    }
    if (expression.kind == "table_match_ref") {
      output << padding << "auto " << expression.result << " = "
             << identifier(expression.field) << "->get(epoch);\n";
      continue;
    }
    if (expression.kind == "table_selection_index_ref" ||
        expression.kind == "table_selection_valid_ref") {
      output << padding << "auto " << expression.result << " = "
             << identifier(expression.field) << "->get(epoch)."
             << (expression.kind == "table_selection_index_ref" ? "index"
                                                                : "valid")
             << ";\n";
      continue;
    }
    if (expression.kind == "table_match") {
      if (expression.nestedYields.size() != 1)
        return generatorError("table.match predicate yield is missing");
      QueueBlockPlan nested;
      nested.expressions = expression.nestedExpressions;
      nested.yields = expression.nestedYields;
      auto predicate =
          emitExpressionBody(plan, nested, nested.yields.front(), indent + 8,
                             qualifyTables, checkedTableAccess);
      if (!predicate)
        return predicate.takeError();
      std::vector<const QueueExpressionPlan *> snapshotSets;
      for (const QueueExpressionPlan &candidate : block.expressions)
        if (candidate.kind == "snapshot_set" &&
            candidate.field == expression.result)
          snapshotSets.push_back(&candidate);
      const std::string table =
          qualifyTables ? "table_" + identifier(expression.table) : "table";
      output << padding << "std::uint64_t " << expression.result << " = 0;\n";
      for (const QueueExpressionPlan *snapshotSet : snapshotSets)
        output << padding << "std::uint64_t " << snapshotSet->result
               << " = 0;\n";
      output << padding << "for (std::size_t index = 0; index < " << table
             << "->size(); ++index) {\n"
             << padding << "  const auto &item = " << table << "->at(index);\n";
      for (auto [setIndex, snapshotSet] : llvm::enumerate(snapshotSets)) {
        std::vector<const QueueExpressionPlan *> reads;
        for (const QueueExpressionPlan &candidate :
             expression.nestedExpressions)
          if (candidate.kind == "table_get" &&
              candidate.table == snapshotSet->table)
            reads.push_back(&candidate);
        if (reads.empty())
          return generatorError("snapshot-set target read is missing");
        for (auto [readIndex, read] : llvm::enumerate(reads)) {
          if (read->operands.size() != 1)
            return generatorError("snapshot-set TableGet index is malformed");
          QueueBlockPlan indexExpression;
          indexExpression.expressions = expression.nestedExpressions;
          indexExpression.yields = {read->operands.front()};
          auto indexBody =
              emitExpressionBody(plan, indexExpression, read->operands.front(),
                                 indent + 4, qualifyTables, checkedTableAccess);
          if (!indexBody)
            return indexBody.takeError();
          output << padding << "  const auto snapshot_index_" << setIndex << '_'
                 << readIndex << " = [&]() {\n"
                 << *indexBody << padding << "  }();\n"
                 << padding << "  " << snapshotSet->result
                 << " |= std::uint64_t{1} << static_cast<std::size_t>("
                 << "snapshot_index_" << setIndex << '_' << readIndex << ");\n";
        }
      }
      output << padding << "  if ([&]() {\n"
             << *predicate << padding << "  }())\n"
             << padding << "    " << expression.result
             << " |= (std::uint64_t{1} << index);\n"
             << padding << "}\n";
      continue;
    }
    if (expression.kind == "snapshot_set") {
      auto source = llvm::find_if(
          block.expressions, [&](const QueueExpressionPlan &candidate) {
            return candidate.result == expression.field &&
                   (candidate.kind == "table_match" ||
                    candidate.kind == "table_choose_index");
          });
      if (source == block.expressions.end())
        return generatorError("snapshot-set source evaluation is missing");
      continue;
    }
    auto first = operand(0);
    if (!first)
      return first.takeError();
    if (expression.kind == "masked_match") {
      output << padding << "auto " << expression.result << " = ("
             << first->str() << " & std::uint64_t{" << expression.mask
             << "}) == std::uint64_t{" << expression.value << "};\n";
      continue;
    }
    if (expression.kind == "get") {
      output << padding << "auto " << expression.result << " = " << first->str()
             << '.' << expression.field << ";\n";
      continue;
    }
    if (expression.kind == "table_get") {
      const std::string table =
          qualifyTables ? "table_" + identifier(expression.table) : "table";
      output << padding << "auto " << expression.result << " = " << table
             << (checkedTableAccess ? "->checkedAt(static_cast<size_t>("
                                    : "->at(static_cast<size_t>(")
             << first->str() << "));\n";
      continue;
    }
    if (expression.kind == "popcount") {
      output << padding << "auto " << expression.result
             << " = gfsim::populationCount(" << first->str() << ");\n";
      continue;
    }
    if (expression.kind == "count_zeros") {
      output << padding << "auto " << expression.result
             << (expression.predicate == "trailing"
                     ? " = gfsim::countTrailingZeros("
                     : " = gfsim::countLeadingZeros(")
             << first->str() << ");\n";
      continue;
    }
    if (expression.kind == "table_choose_index" ||
        expression.kind == "table_choose_valid") {
      QueueBlockPlan nested;
      nested.expressions = expression.nestedExpressions;
      nested.yields = expression.nestedYields;
      std::string choiceKey =
          expression.table + "#" + first->str() + "#" + expression.predicate;
      for (const QueueExpressionPlan &keyExpression : nested.expressions) {
        choiceKey.append("#")
            .append(keyExpression.kind)
            .append(":")
            .append(keyExpression.field)
            .append(":")
            .append(keyExpression.literal)
            .append(":")
            .append(keyExpression.predicate)
            .append(":")
            .append(keyExpression.table);
        for (const std::string &operandName : keyExpression.operands)
          choiceKey.append(":").append(operandName);
      }
      if (auto cached = tableChoices.find(choiceKey);
          cached != tableChoices.end()) {
        output << padding << "auto " << expression.result << " = "
               << (expression.kind == "table_choose_index"
                       ? cached->second.first
                       : cached->second.second)
               << ";\n";
        continue;
      }
      const std::string choice = "choice_" + identifier(expression.result);
      const std::string choiceIndex = choice + "_index";
      const std::string choiceValid = choice + "_valid";
      const std::string choiceBest = choice + "_best";
      std::vector<const QueueExpressionPlan *> snapshotSets;
      for (const QueueExpressionPlan &candidate : block.expressions)
        if (candidate.kind == "snapshot_set" &&
            candidate.field == expression.result)
          snapshotSets.push_back(&candidate);
      for (const QueueExpressionPlan *snapshotSet : snapshotSets)
        output << padding << "std::uint64_t " << snapshotSet->result
               << " = 0;\n";
      output << padding << "std::uint64_t " << choiceIndex << " = 0;\n"
             << padding << "bool " << choiceValid << " = false;\n"
             << padding << "std::uint64_t " << choiceBest << " = 0;\n"
             << padding << "for (std::size_t index = 0; index < "
             << (qualifyTables ? "table_" + identifier(expression.table)
                               : std::string("table"))
             << "->size(); ++index) {\n"
             << padding << "  if ((static_cast<std::uint64_t>(" << first->str()
             << ") & (std::uint64_t{1} << index)) == 0) continue;\n";
      if (expression.predicate != "first")
        output << padding << "  const auto &item = "
               << (qualifyTables ? "table_" + identifier(expression.table)
                                 : std::string("table"))
               << "->at(index);\n";
      for (auto [setIndex, snapshotSet] : llvm::enumerate(snapshotSets)) {
        std::vector<const QueueExpressionPlan *> reads;
        for (const QueueExpressionPlan &candidate : nested.expressions)
          if (candidate.kind == "table_get" &&
              candidate.table == snapshotSet->table)
            reads.push_back(&candidate);
        if (reads.empty())
          return generatorError(
              "snapshot-set choose-key target read is missing");
        for (auto [readIndex, read] : llvm::enumerate(reads)) {
          if (read->operands.size() != 1)
            return generatorError(
                "snapshot-set choose-key TableGet index is malformed");
          QueueBlockPlan indexExpression;
          indexExpression.expressions = nested.expressions;
          indexExpression.yields = {read->operands.front()};
          auto indexBody =
              emitExpressionBody(plan, indexExpression, read->operands.front(),
                                 indent + 4, qualifyTables, checkedTableAccess);
          if (!indexBody)
            return indexBody.takeError();
          output << padding << "  const auto snapshot_index_" << setIndex << '_'
                 << readIndex << " = [&]() {\n"
                 << *indexBody << padding << "  }();\n"
                 << padding << "  " << snapshotSet->result
                 << " |= std::uint64_t{1} << static_cast<std::size_t>("
                 << "snapshot_index_" << setIndex << '_' << readIndex << ");\n";
        }
      }
      if (expression.predicate == "first") {
        output << padding << "  " << choiceIndex << " = index;\n"
               << padding << "  " << choiceValid << " = true;\n"
               << padding << "  break;\n";
      } else {
        if (nested.yields.size() != 1)
          return generatorError("table.choose key yield is missing");
        auto key =
            emitExpressionBody(plan, nested, nested.yields.front(), indent + 6,
                               qualifyTables, checkedTableAccess);
        if (!key)
          return key.takeError();
        const char *comparison = expression.predicate == "min" ? "<" : ">";
        output << padding << "  auto key = [&]() {\n"
               << *key << padding << "  }();\n"
               << padding << "  if (!" << choiceValid
               << " || static_cast<std::uint64_t>(key) " << comparison << " "
               << choiceBest << ") {\n"
               << padding << "    " << choiceIndex << " = index;\n"
               << padding << "    " << choiceValid << " = true;\n"
               << padding << "    " << choiceBest
               << " = static_cast<std::uint64_t>(key);\n"
               << padding << "  }\n";
      }
      output << padding << "}\n"
             << padding << "auto " << expression.result << " = "
             << (expression.kind == "table_choose_index" ? choiceIndex
                                                         : choiceValid)
             << ";\n";
      tableChoices[choiceKey] = {choiceIndex, choiceValid};
      continue;
    }
    if (expression.kind == "not") {
      output << padding << "auto " << expression.result << " = ~"
             << first->str() << ";\n";
      continue;
    }
    if (expression.kind == "priority_index" ||
        expression.kind == "priority_valid") {
      std::string key = first->str() + "#" + expression.predicate;
      auto [entry, inserted] = priorityEncodings.try_emplace(
          key, "priority_" + identifier(expression.result));
      if (inserted)
        output << padding << "auto " << entry->getValue()
               << " = gfsim::priorityEncode(" << first->str() << ", "
               << (expression.predicate == "low" ? "true" : "false") << ");\n";
      output << padding << "auto " << expression.result << " = "
             << entry->getValue() << "."
             << (expression.kind == "priority_index" ? "index" : "valid")
             << ";\n";
      continue;
    }
    if (expression.kind == "bit_extract") {
      output << padding << "auto " << expression.result
             << " = gfsim::bitExtract<" << expression.width << ">("
             << first->str() << ", " << expression.lsb << ");\n";
      continue;
    }
    if (expression.kind == "aggregate_get") {
      const std::string extracted =
          "gfsim::bitExtract<" + std::to_string(expression.width) + ">(" +
          first->str() + ", " + std::to_string(expression.lsb) + ")";
      auto unpacked = emitUnpackedValue(plan, expression.type, extracted);
      if (!unpacked)
        return unpacked.takeError();
      output << padding << "auto " << expression.result << " = " << *unpacked
             << ";\n";
      continue;
    }
    if (expression.kind == "bit_concat") {
      output << padding << "auto " << expression.result
             << " = gfsim::bitConcat(";
      for (auto [index, value] : llvm::enumerate(expression.operands)) {
        if (index)
          output << ", ";
        output << value;
      }
      output << ");\n";
      continue;
    }
    if (expression.kind == "tuple_create" ||
        expression.kind == "array_create") {
      const QueueAggregatePlan *aggregate =
          findAggregateType(plan, expression.type);
      if (!aggregate)
        return generatorError("aggregate create type is unresolved");
      const bool tuple = expression.kind == "tuple_create";
      if ((tuple && aggregate->kind != "tuple") ||
          (!tuple && aggregate->kind != "array"))
        return generatorError("aggregate create kind disagrees with its type");
      if ((tuple && aggregate->elements.size() != expression.operands.size()) ||
          (!tuple && (aggregate->elements.size() != 1 ||
                      aggregate->length != expression.operands.size())))
        return generatorError("aggregate create operand arity mismatch");
      output << padding << "auto " << expression.result
             << " = gfsim::bitConcat(";
      for (auto [index, value] : llvm::enumerate(expression.operands)) {
        auto packed = emitPackedValue(
            plan, tuple ? aggregate->elements[index] : aggregate->elements[0],
            value);
        if (!packed)
          return packed.takeError();
        if (index)
          output << ", ";
        output << *packed;
      }
      output << ");\n";
      continue;
    }
    if (expression.kind == "bit_insert") {
      auto value = operand(1);
      if (!value)
        return value.takeError();
      output << padding << "auto " << expression.result
             << " = gfsim::bitInsert(" << first->str() << ", " << value->str()
             << ", " << expression.lsb << ");\n";
      continue;
    }
    if (expression.kind == "value_select") {
      auto trueValue = operand(1);
      auto falseValue = operand(2);
      if (!trueValue)
        return trueValue.takeError();
      if (!falseValue)
        return falseValue.takeError();
      output << padding << "auto " << expression.result << " = " << first->str()
             << " ? " << trueValue->str() << " : " << falseValue->str()
             << ";\n";
      continue;
    }
    auto second = operand(1);
    if (!second)
      return second.takeError();
    if (expression.kind == "with") {
      output << padding << "auto " << expression.result << " = " << first->str()
             << ";\n";
      output << padding << expression.result << '.' << expression.field << " = "
             << second->str() << ";\n";
      continue;
    }
    llvm::StringRef operation;
    if (expression.kind == "add")
      operation = "+";
    else if (expression.kind == "sub")
      operation = "-";
    else if (expression.kind == "mul")
      operation = "*";
    else if (expression.kind == "and")
      operation = "&";
    else if (expression.kind == "or")
      operation = "|";
    else if (expression.kind == "xor")
      operation = "^";
    else if (expression.kind == "shl")
      operation = "<<";
    else if (expression.kind == "shr")
      operation = ">>";
    else if (expression.kind == "cmp") {
      operation = llvm::StringSwitch<llvm::StringRef>(expression.predicate)
                      .Case("eq", "==")
                      .Case("ne", "!=")
                      .Case("slt", "<")
                      .Case("sle", "<=")
                      .Case("sgt", ">")
                      .Case("sge", ">=")
                      .Case("ult", "<")
                      .Case("ule", "<=")
                      .Case("ugt", ">")
                      .Case("uge", ">=")
                      .Default("");
    }
    if (operation.empty())
      return generatorError("unsupported Var expression kind '" +
                            expression.kind + "'");
    output << padding << "auto " << expression.result << " = ";
    if (expression.kind == "cmp" && expression.predicate.starts_with("s"))
      output << "gfsim::signedValue(" << first->str() << ") " << operation.str()
             << " gfsim::signedValue(" << second->str() << ")";
    else
      output << first->str() << ' ' << operation.str() << ' ' << second->str();
    output << ";\n";
  }
  output << padding << "return "
         << (returnExpression.empty() ? yield : returnExpression).str()
         << ";\n";
  return output.str();
}

bool referencesTable(const std::vector<QueueExpressionPlan> &expressions,
                     llvm::StringRef table) {
  for (const QueueExpressionPlan &expression : expressions) {
    if (expression.table == table ||
        referencesTable(expression.nestedExpressions, table))
      return true;
  }
  return false;
}

const QueuePlan *findQueue(const QueueGraphPlan &plan, llvm::StringRef name) {
  auto found =
      std::find_if(plan.queues.begin(), plan.queues.end(),
                   [&](const QueuePlan &queue) { return queue.name == name; });
  return found == plan.queues.end() ? nullptr : &*found;
}

const TablePlan *findTable(const QueueGraphPlan &plan, llvm::StringRef name) {
  auto found =
      std::find_if(plan.tables.begin(), plan.tables.end(),
                   [&](const TablePlan &table) { return table.name == name; });
  return found == plan.tables.end() ? nullptr : &*found;
}

struct ReservationFieldEncoding {
  uint64_t mask = 0;
  unsigned count = 0;
  bool complete = false;
};

llvm::Expected<ReservationFieldEncoding>
reservationFieldMask(const QueueGraphPlan &plan, const TablePlan &table,
                     llvm::ArrayRef<std::string> fields) {
  if (fields.empty())
    return generatorError("state reservation fields are empty");
  if (!llvm::StringRef(table.entryType).starts_with("!ac.struct<")) {
    if (fields.size() != 1 || fields.front() != "$entry")
      return generatorError("scalar state reservation requires $entry");
    return ReservationFieldEncoding{1, 1, true};
  }
  size_t marker = table.entryType.rfind('@');
  size_t end = table.entryType.rfind('>');
  if (marker == std::string::npos || end == std::string::npos || marker >= end)
    return generatorError("state reservation Entry type is malformed");
  llvm::StringRef payloadName(table.entryType.data() + marker + 1,
                              end - marker - 1);
  auto payload =
      llvm::find_if(plan.payloads, [&](const QueuePayloadPlan &candidate) {
        return candidate.name == payloadName;
      });
  if (payload == plan.payloads.end() || payload->fields.size() > 64)
    return generatorError("state reservation Entry requires at most 64 fields");
  uint64_t mask = 0;
  for (const std::string &field : fields) {
    auto declared = llvm::find_if(payload->fields,
                                  [&](const QueuePayloadFieldPlan &candidate) {
                                    return candidate.name == field;
                                  });
    if (declared == payload->fields.end())
      return generatorError("state reservation field is not declared");
    mask |= uint64_t{1} << static_cast<unsigned>(
                std::distance(payload->fields.begin(), declared));
  }
  const unsigned count = static_cast<unsigned>(payload->fields.size());
  const uint64_t completeMask =
      count == 64 ? ~uint64_t{0} : (uint64_t{1} << count) - 1;
  if (mask != completeMask && table.entries * count > 64)
    return generatorError(
        "field-qualified state reservation exceeds the 64-bit relation");
  return ReservationFieldEncoding{mask, count, mask == completeMask};
}

const StateWritePlan *findStateWrite(const QueueBlockPlan &block,
                                     llvm::StringRef table) {
  auto found =
      llvm::find_if(block.stateWrites, [&](const StateWritePlan &write) {
        return write.table == table;
      });
  return found == block.stateWrites.end() ? nullptr : &*found;
}

std::vector<const StateReservationPlan *>
findStateReservations(const QueueBlockPlan &block, llvm::StringRef table) {
  std::vector<const StateReservationPlan *> result;
  for (const StateReservationPlan &reservation : block.stateReservations)
    if (reservation.table == table)
      result.push_back(&reservation);
  return result;
}

std::vector<const TablePlan *> stateOwnerTables(const QueueGraphPlan &plan,
                                                const QueueBlockPlan &block) {
  std::vector<const TablePlan *> result;
  for (const TablePlan &table : plan.tables)
    if (findStateWrite(block, table.name) ||
        !findStateReservations(block, table.name).empty())
      result.push_back(&table);
  return result;
}

std::vector<const TablePlan *> readOnlyTables(const QueueGraphPlan &plan,
                                              const QueueBlockPlan &block) {
  llvm::StringSet<> owned;
  for (const TablePlan *table : stateOwnerTables(plan, block))
    owned.insert(table->name);
  std::vector<const TablePlan *> result;
  for (const TablePlan &table : plan.tables)
    if (!owned.contains(table.name) &&
        referencesTable(block.expressions, table.name))
      result.push_back(&table);
  return result;
}

const SlotPlan *findSlot(const QueueGraphPlan &plan, llvm::StringRef name) {
  auto found =
      std::find_if(plan.slots.begin(), plan.slots.end(),
                   [&](const SlotPlan &slot) { return slot.name == name; });
  return found == plan.slots.end() ? nullptr : &*found;
}

bool isRuntimeBlock(const QueueBlockPlan &block) {
  return block.kind != "source";
}

llvm::Error emitStructuredMergePolicy(std::ostringstream &output,
                                      const QueueGraphPlan &specialization,
                                      llvm::StringRef policyName,
                                      llvm::StringRef entryType,
                                      llvm::ArrayRef<std::string> fields) {
  output << "struct " << policyName.str()
         << " {\n  static constexpr std::array<size_t, " << fields.size()
         << "> fields{";
  for (auto [fieldIndex, field] : llvm::enumerate(fields)) {
    if (fieldIndex)
      output << ", ";
    if (field == "$entry") {
      output << 0;
      continue;
    }
    auto payload = llvm::find_if(specialization.payloads,
                                 [&](const QueuePayloadPlan &candidate) {
                                   return candidate.name == entryType;
                                 });
    if (payload == specialization.payloads.end())
      return generatorError("structured specialization Entry payload missing");
    auto declared = llvm::find_if(payload->fields,
                                  [&](const QueuePayloadFieldPlan &candidate) {
                                    return candidate.name == field;
                                  });
    if (declared == payload->fields.end())
      return generatorError("structured specialization write field missing");
    output << std::distance(payload->fields.begin(), declared);
  }
  output << "};\n  void operator()(" << entryType.str() << " &target, const "
         << entryType.str() << " &value) const {\n";
  for (const std::string &field : fields)
    if (field == "$entry")
      output << "    target = value;\n";
    else
      output << "    target." << field << " = value." << field << ";\n";
  output << "  }\n};\n\n";
  return llvm::Error::success();
}

llvm::Expected<std::string>
generateStructuredQueueGraphCpp(const QueueGraphPlan &plan) {
  if (auto error = verifyQueueGraphPlan(plan))
    return std::move(error);
  if (plan.definition.empty() || plan.moduleSpecializations.empty() ||
      plan.moduleInstances.empty())
    return generatorError("structured QueueGraph plan is incomplete");

  struct SpecializationFrame {
    const QueueGraphPlan *specialization = nullptr;
    bool expanded = false;
  };
  std::vector<SpecializationFrame> pending;
  for (const std::shared_ptr<QueueGraphPlan> &specialization :
       llvm::reverse(plan.moduleSpecializations))
    pending.push_back({specialization.get(), false});
  llvm::StringSet<> scheduledSpecializations;
  std::vector<const QueueGraphPlan *> emissionOrder;
  while (!pending.empty()) {
    SpecializationFrame frame = pending.back();
    pending.pop_back();
    if (!frame.specialization)
      return generatorError("nested specialization plan is null");
    if (frame.expanded) {
      emissionOrder.push_back(frame.specialization);
      continue;
    }
    if (!scheduledSpecializations
             .insert(frame.specialization->specializationFingerprint)
             .second)
      continue;
    pending.push_back({frame.specialization, true});
    for (const std::shared_ptr<QueueGraphPlan> &child :
         llvm::reverse(frame.specialization->moduleSpecializations))
      pending.push_back({child.get(), false});
  }

  llvm::StringMap<const QueueGraphPlan *> specializations;
  for (const QueueGraphPlan *specialization : emissionOrder) {
    const bool pureTransform =
        specialization && specialization->blocks.size() == 1 &&
        specialization->blocks.front().kind == "transform" &&
        specialization->tables.empty() &&
        specialization->interfaceInputs.size() == 1 &&
        specialization->interfaceOutputs.size() == 1 &&
        specialization->blocks.front().inputs.size() == 1 &&
        specialization->blocks.front().outputs.size() == 1 &&
        specialization->blocks.front().yields.size() == 1;
    const bool statefulModule =
        specialization && !specialization->blocks.empty() &&
        !specialization->tables.empty() &&
        llvm::all_of(specialization->blocks, [&](const QueueBlockPlan &block) {
          return block.kind == "firing" && !block.stateWrites.empty() &&
                 llvm::all_of(block.stateWrites,
                              [&](const StateWritePlan &write) {
                                return findTable(*specialization,
                                                 write.table) != nullptr;
                              }) &&
                 block.outputs.size() <= 1 &&
                 block.yields.size() == block.outputs.size();
        });
    const bool nestedWrapper = specialization &&
                               specialization->blocks.empty() &&
                               specialization->tables.empty() &&
                               !specialization->moduleInstances.empty() &&
                               specialization->scopes.empty();
    const bool mixedNested =
        specialization && specialization->blocks.size() == 1 &&
        specialization->blocks.front().kind == "transform" &&
        specialization->tables.empty() &&
        !specialization->moduleInstances.empty() &&
        specialization->scopes.size() == 1 &&
        specialization->blocks.front().scope == specialization->scopes.front();
    const bool localShape =
        nestedWrapper || mixedNested ||
        (specialization && specialization->moduleInstances.empty() &&
         specialization->scopes.size() == 1 &&
         llvm::none_of(specialization->blocks,
                       [&](const QueueBlockPlan &block) {
                         return block.scope != specialization->scopes.front();
                       }));
    if (!specialization ||
        (!pureTransform && !statefulModule && !nestedWrapper && !mixedNested) ||
        !localShape || !specialization->slots.empty() ||
        !specialization->memoryInstances.empty())
      return generatorError(
          "structured QueueGraph specialization requires a pure 1x1 "
          "transform, stateful firing module, or direct nested wrapper");
    llvm::StringSet<> interfaceQueues;
    for (const QueueInterfacePlan &input : specialization->interfaceInputs)
      interfaceQueues.insert(input.name);
    for (const QueueInterfacePlan &output : specialization->interfaceOutputs)
      interfaceQueues.insert(output.name);
    if (mixedNested)
      for (const QueuePlan &queue : specialization->queues)
        interfaceQueues.insert(queue.name);
    for (const QueueBlockPlan &block : specialization->blocks)
      if (llvm::any_of(block.inputs,
                       [&](const std::string &name) {
                         return !interfaceQueues.contains(name);
                       }) ||
          llvm::any_of(block.outputs, [&](const std::string &name) {
            return !interfaceQueues.contains(name);
          }))
        return generatorError(
            "first multi-rule specialization slice requires direct interface "
            "Queue bindings");
    specializations[specialization->specializationFingerprint] = specialization;
  }

  for (const QueueBlockPlan &block : plan.blocks)
    if (block.kind != "source" && block.kind != "broadcast" &&
        block.kind != "sink" && block.kind != "observe")
      return generatorError(
          "first structured QueueGraph root supports source, broadcast, "
          "sink, and observe blocks");

  auto specializationClassName = [](const QueueGraphPlan &specialization) {
    llvm::StringRef fingerprint(specialization.specializationFingerprint);
    fingerprint.consume_front("sha256:");
    return className(specialization.definition) + "_" + fingerprint.str();
  };
  llvm::StringMap<uint64_t> specializationObjectCounts;
  for (const QueueGraphPlan *specialization : emissionOrder) {
    llvm::StringSet<> exportedQueues;
    for (const QueueInterfacePlan &output : specialization->interfaceOutputs)
      exportedQueues.insert(output.name);
    uint64_t count =
        specialization->blocks.size() + specialization->tables.size() +
        llvm::count_if(specialization->queues, [&](const QueuePlan &queue) {
          return !exportedQueues.contains(queue.name);
        });
    for (const QueueModuleInstancePlan &instance :
         specialization->moduleInstances) {
      auto child =
          specializationObjectCounts.find(instance.specializationFingerprint);
      if (child == specializationObjectCounts.end())
        return generatorError(
            "nested specialization object count dependency is unavailable");
      count += child->getValue();
    }
    specializationObjectCounts[specialization->specializationFingerprint] =
        count;
  }
  auto specializationObjectCount = [&](const QueueGraphPlan &specialization) {
    return specializationObjectCounts.lookup(
        specialization.specializationFingerprint);
  };

  llvm::StringMap<std::string> queueMembers;
  llvm::StringMap<std::string> queueOwners;
  llvm::StringMap<uint64_t> queueIds;
  for (auto [index, queue] : llvm::enumerate(plan.queues)) {
    queueMembers[queue.name] = identifier(queue.name) + "_";
    queueOwners[queue.name] = queue.scope;
    queueIds[queue.name] = index;
  }
  for (const QueueBlockPlan &block : plan.blocks)
    for (const std::string &input : block.inputs)
      queueOwners[input] = commonPath(queueOwners[input], block.scope);
  for (const QueueModuleInstancePlan &instance : plan.moduleInstances)
    for (const std::string &input : instance.inputs)
      queueOwners[input] = commonPath(queueOwners[input], instance.scope);

  llvm::StringMap<std::string> scopeMembers;
  for (auto [index, scope] : llvm::enumerate(plan.scopes))
    scopeMembers[scope] = "scope_" + std::to_string(index) + "_";
  auto modulePointer =
      [&](llvm::StringRef path) -> llvm::Expected<std::string> {
    if (path == "/")
      return std::string("this");
    auto found = scopeMembers.find(path);
    if (found == scopeMembers.end())
      return generatorError("unknown structured scope path '" + path + "'");
    return "&" + found->getValue();
  };
  auto attach = [&](llvm::StringRef path,
                    llvm::StringRef member) -> llvm::Expected<std::string> {
    if (path == "/")
      return "    attachChild(" + member.str() + ");";
    auto found = scopeMembers.find(path);
    if (found == scopeMembers.end())
      return generatorError("unknown structured attachment scope '" + path +
                            "'");
    return "    " + found->getValue() + ".attachChild(" + member.str() + ");";
  };

  std::vector<const QueueBlockPlan *> runtimeBlocks;
  for (const QueueBlockPlan &block : plan.blocks)
    if (isRuntimeBlock(block))
      runtimeBlocks.push_back(&block);
  struct DispatchItem {
    uint64_t lexicalOrder = 0;
    const QueueBlockPlan *block = nullptr;
    size_t instanceIndex = 0;
  };
  std::vector<DispatchItem> dispatchItems;
  for (const QueueBlockPlan *block : runtimeBlocks)
    dispatchItems.push_back({block->lexicalOrder, block, 0});
  for (auto [index, instance] : llvm::enumerate(plan.moduleInstances))
    dispatchItems.push_back({instance.lexicalOrder, nullptr, index});
  llvm::sort(dispatchItems,
             [](const DispatchItem &left, const DispatchItem &right) {
               return left.lexicalOrder < right.lexicalOrder;
             });
  uint64_t nextId = plan.queues.size();
  llvm::DenseMap<const QueueBlockPlan *, uint64_t> blockIds;
  std::vector<std::vector<uint64_t>> instanceObjectIds(
      plan.moduleInstances.size());
  for (const DispatchItem &item : dispatchItems) {
    if (item.block)
      blockIds[item.block] = nextId++;
    else {
      const QueueModuleInstancePlan &instance =
          plan.moduleInstances[item.instanceIndex];
      const QueueGraphPlan *specialization =
          specializations.lookup(instance.specializationFingerprint);
      for (uint64_t index = 0;
           index < specializationObjectCount(*specialization); ++index)
        instanceObjectIds[item.instanceIndex].push_back(nextId++);
    }
  }

  auto resolveRootActivation =
      [&](const QueueActivationNodePlan &node) -> llvm::Expected<uint64_t> {
    if (node.kind == QueueActivationNodeKind::Queue) {
      if (node.index >= plan.queues.size())
        return generatorError("root activation Queue index is out of range");
      return queueIds.lookup(plan.queues[node.index].name);
    }
    if (node.kind == QueueActivationNodeKind::Block) {
      if (node.index >= plan.blocks.size())
        return generatorError("root activation block index is out of range");
      auto found = blockIds.find(&plan.blocks[node.index]);
      if (found == blockIds.end())
        return generatorError("root activation block has no runtime object");
      return found->second;
    }
    return generatorError(
        "structured root activation contains an unsupported node kind");
  };
  std::set<std::pair<uint64_t, uint64_t>> activationEdges;
  std::set<std::pair<uint64_t, uint64_t>> physicalWorkClosureEdges;
  std::set<uint64_t> initialActivation;
  const bool activationComplete = true;
  for (const QueueActivationEdgePlan &edge : plan.activationEdges) {
    auto source = resolveRootActivation(edge.source);
    auto target = resolveRootActivation(edge.target);
    if (!source)
      return source.takeError();
    if (!target)
      return target.takeError();
    activationEdges.emplace(*source, *target);
  }
  for (const QueueActivationNodePlan &node : plan.initialActivation) {
    auto resolved = resolveRootActivation(node);
    if (!resolved)
      return resolved.takeError();
    initialActivation.insert(*resolved);
  }
  for (const QueueActivationEdgePlan &edge : plan.workClosureEdges) {
    auto source = resolveRootActivation(edge.source);
    auto target = resolveRootActivation(edge.target);
    if (!source)
      return source.takeError();
    if (!target)
      return target.takeError();
    physicalWorkClosureEdges.emplace(*source, *target);
  }
  auto instantiateActivation =
      [&](auto &&self, const QueueGraphPlan &specialization,
          llvm::ArrayRef<uint64_t> objectIds,
          const llvm::StringMap<uint64_t> &interfaceBindings) -> llvm::Error {
    llvm::StringSet<> exported;
    for (const QueueInterfacePlan &output : specialization.interfaceOutputs)
      exported.insert(output.name);
    std::vector<const QueuePlan *> internalQueues;
    llvm::StringMap<uint64_t> internalQueueIndices;
    for (const QueuePlan &queue : specialization.queues) {
      if (exported.contains(queue.name))
        continue;
      internalQueueIndices[queue.name] = internalQueues.size();
      internalQueues.push_back(&queue);
    }
    const uint64_t blockOffset = internalQueues.size();
    const uint64_t tableOffset = blockOffset + specialization.blocks.size();
    uint64_t childOffset = tableOffset + specialization.tables.size();
    if (childOffset > objectIds.size())
      return generatorError(
          "activation specialization object layout is incomplete");
    auto resolveQueue = [&](llvm::StringRef name) -> llvm::Expected<uint64_t> {
      auto interface = interfaceBindings.find(name);
      if (interface != interfaceBindings.end())
        return interface->getValue();
      auto internal = internalQueueIndices.find(name);
      if (internal == internalQueueIndices.end() ||
          internal->getValue() >= objectIds.size())
        return generatorError("activation Queue binding is unresolved");
      return objectIds[internal->getValue()];
    };
    auto resolveNode =
        [&](const QueueActivationNodePlan &node) -> llvm::Expected<uint64_t> {
      if (node.kind == QueueActivationNodeKind::InterfaceInput) {
        if (node.index >= specialization.interfaceInputs.size())
          return generatorError(
              "activation interface input index is out of range");
        return resolveQueue(specialization.interfaceInputs[node.index].name);
      }
      if (node.kind == QueueActivationNodeKind::InterfaceOutput) {
        if (node.index >= specialization.interfaceOutputs.size())
          return generatorError(
              "activation interface output index is out of range");
        return resolveQueue(specialization.interfaceOutputs[node.index].name);
      }
      if (node.kind == QueueActivationNodeKind::Queue) {
        if (node.index >= specialization.queues.size())
          return generatorError("activation Queue index is out of range");
        return resolveQueue(specialization.queues[node.index].name);
      }
      if (node.kind == QueueActivationNodeKind::Block) {
        const uint64_t local = blockOffset + node.index;
        if (node.index >= specialization.blocks.size() ||
            local >= objectIds.size())
          return generatorError("activation block index is out of range");
        return objectIds[local];
      }
      if (node.kind == QueueActivationNodeKind::Table) {
        const uint64_t local = tableOffset + node.index;
        if (node.index >= specialization.tables.size() ||
            local >= objectIds.size())
          return generatorError("activation Table index is out of range");
        return objectIds[local];
      }
      return generatorError("activation node kind is unsupported");
    };
    for (const QueueActivationEdgePlan &edge : specialization.activationEdges) {
      auto source = resolveNode(edge.source);
      auto target = resolveNode(edge.target);
      if (!source)
        return source.takeError();
      if (!target)
        return target.takeError();
      activationEdges.emplace(*source, *target);
    }
    for (const QueueActivationEdgePlan &edge :
         specialization.workClosureEdges) {
      auto source = resolveNode(edge.source);
      auto target = resolveNode(edge.target);
      if (!source)
        return source.takeError();
      if (!target)
        return target.takeError();
      physicalWorkClosureEdges.emplace(*source, *target);
    }
    for (const QueueActivationNodePlan &node :
         specialization.initialActivation) {
      auto resolved = resolveNode(node);
      if (!resolved)
        return resolved.takeError();
      initialActivation.insert(*resolved);
    }
    for (const QueueModuleInstancePlan &instance :
         specialization.moduleInstances) {
      const QueueGraphPlan *child =
          specializations.lookup(instance.specializationFingerprint);
      if (!child)
        return generatorError("nested activation specialization is missing");
      const uint64_t childCount = specializationObjectCount(*child);
      if (childOffset + childCount > objectIds.size())
        return generatorError("nested activation ID partition is incomplete");
      llvm::StringMap<uint64_t> childBindings;
      for (auto [index, input] : llvm::enumerate(instance.inputs)) {
        auto resolved = resolveQueue(input);
        if (!resolved)
          return resolved.takeError();
        childBindings[child->interfaceInputs[index].name] = *resolved;
      }
      for (auto [index, outputName] : llvm::enumerate(instance.outputs)) {
        auto resolved = resolveQueue(outputName);
        if (!resolved)
          return resolved.takeError();
        childBindings[child->interfaceOutputs[index].name] = *resolved;
      }
      if (auto error =
              self(self, *child, objectIds.slice(childOffset, childCount),
                   childBindings))
        return error;
      childOffset += childCount;
    }
    if (childOffset != objectIds.size())
      return generatorError("activation ID partition has unused objects");
    return llvm::Error::success();
  };
  for (auto [instanceIndex, instance] : llvm::enumerate(plan.moduleInstances)) {
    const QueueGraphPlan *specialization =
        specializations.lookup(instance.specializationFingerprint);
    if (!specialization)
      return generatorError("activation specialization is missing");
    llvm::StringMap<uint64_t> bindings;
    for (auto [index, input] : llvm::enumerate(instance.inputs))
      bindings[specialization->interfaceInputs[index].name] =
          queueIds.lookup(input);
    for (auto [index, outputName] : llvm::enumerate(instance.outputs))
      bindings[specialization->interfaceOutputs[index].name] =
          queueIds.lookup(outputName);
    if (auto error = instantiateActivation(
            instantiateActivation, *specialization,
            llvm::ArrayRef(instanceObjectIds[instanceIndex]), bindings))
      return std::move(error);
  }
  std::vector<uint32_t> activationOffsets(nextId + 1, 0);
  std::vector<uint64_t> activationTargets;
  activationTargets.reserve(activationEdges.size());
  for (auto [source, target] : activationEdges) {
    if (source >= nextId || target >= nextId)
      return generatorError("activation endpoint is outside dispatch rows");
    ++activationOffsets[source + 1];
    activationTargets.push_back(target);
  }
  for (size_t index = 1; index < activationOffsets.size(); ++index)
    activationOffsets[index] += activationOffsets[index - 1];
  std::vector<uint32_t> workClosureOffsets(nextId + 1, 0);
  std::vector<uint64_t> workClosureTargets;
  workClosureTargets.reserve(physicalWorkClosureEdges.size());
  for (auto [worker, resource] : physicalWorkClosureEdges) {
    if (worker >= nextId || resource >= nextId)
      return generatorError("Work closure endpoint is outside dispatch rows");
    ++workClosureOffsets[worker + 1];
    workClosureTargets.push_back(resource);
  }
  for (size_t index = 1; index < workClosureOffsets.size(); ++index)
    workClosureOffsets[index] += workClosureOffsets[index - 1];

  std::ostringstream output;
  output << "// Generated from hierarchy-preserving frozen ACIR QueueGraph "
            "plan; do not edit.\n"
            "#include \"gfsim/bits.h\"\n"
            "#include \"gfsim/dispatch.h\"\n"
            "#include \"gfsim/object.h\"\n"
            "#include \"gfsim/queue.h\"\n"
            "#include \"gfsim/queue_blocks.h\"\n\n"
            "#include <array>\n#include <cstdint>\n#include <limits>\n"
            "#include <optional>\n#include <string>\n#include <tuple>\n"
            "#include <utility>\n#include <vector>\n\n"
            "namespace ac_generated {\n\n";
  for (const QueueEnumPlan &enumeration : plan.enums) {
    output << "enum class " << enumeration.name << " : "
           << enumStorage(enumeration.width).str() << " {\n";
    for (auto [index, enumerant] : llvm::enumerate(enumeration.enumerants))
      output << "  " << enumerant << " = " << index << ",\n";
    output << "};\n\n";
  }
  auto payloadOrder = payloadEmissionOrder(plan);
  if (!payloadOrder)
    return payloadOrder.takeError();
  for (const QueuePayloadPlan *payload : *payloadOrder) {
    output << "struct " << payload->name << " {\n";
    for (const QueuePayloadFieldPlan &field : payload->fields) {
      auto type = cppPayloadFieldType(plan, field);
      if (!type)
        return type.takeError();
      output << "  " << *type << ' ' << field.name << "{};\n";
    }
    output << "  bool operator==(const " << payload->name
           << " &) const = default;\n};\n\n";
  }

  auto emitStatefulSpecialization =
      [&](const QueueGraphPlan &specialization,
          const std::string &implementation) -> llvm::Error {
    const std::string localScope =
        pathParts(specialization.scopes.front()).back();
    std::vector<std::string> tableTypes;
    llvm::StringMap<size_t> tableIndices;
    for (auto [index, table] : llvm::enumerate(specialization.tables)) {
      if (table.ownerPath != specialization.scopes.front())
        return generatorError(
            "stateful specialization Tables must share the firing scope");
      auto type = cppType(table.entryType);
      if (!type)
        return type.takeError();
      tableIndices[table.name] = index;
      tableTypes.push_back(std::move(*type));
    }
    llvm::StringMap<std::string> portTypes;
    llvm::StringMap<std::string> portParameters;
    for (auto [index, input] :
         llvm::enumerate(specialization.interfaceInputs)) {
      auto type = cppType(input.payloadType);
      if (!type)
        return type.takeError();
      portTypes[input.name] = *type;
      portParameters[input.name] = "input_" + std::to_string(index);
    }
    for (auto [index, result] :
         llvm::enumerate(specialization.interfaceOutputs)) {
      auto type = cppType(result.payloadType);
      if (!type)
        return type.takeError();
      portTypes[result.name] = *type;
      portParameters[result.name] = "output_" + std::to_string(index);
    }
    auto policyName = [&](size_t blockIndex) {
      return implementation + "_block_" + std::to_string(blockIndex) +
             "_policy";
    };
    auto mergeName = [&](size_t blockIndex, size_t writeIndex) {
      return implementation + "_block_" + std::to_string(blockIndex) +
             "_merge_policy_" + std::to_string(writeIndex);
    };
    auto queueTypes = [&](llvm::ArrayRef<std::string> queues) {
      std::vector<std::string> result;
      for (const std::string &queue : queues)
        result.push_back(portTypes.lookup(queue));
      return result;
    };
    auto tableBindings = [&](const QueueBlockPlan &firing) {
      std::vector<size_t> result;
      for (const TablePlan *table : stateOwnerTables(specialization, firing))
        result.push_back(tableIndices.lookup(table->name));
      return result;
    };
    auto readOnlyTableBindings = [&](const QueueBlockPlan &firing) {
      std::vector<size_t> result;
      for (const TablePlan *table : readOnlyTables(specialization, firing))
        result.push_back(tableIndices.lookup(table->name));
      return result;
    };
    auto tupleType = [](llvm::ArrayRef<std::string> types) {
      std::string result = "std::tuple<";
      for (auto [index, type] : llvm::enumerate(types)) {
        if (index)
          result.append(", ");
        result.append(type);
      }
      result.push_back('>');
      return result;
    };

    for (auto [blockIndex, firing] : llvm::enumerate(specialization.blocks)) {
      const std::vector<size_t> tables = tableBindings(firing);
      const std::vector<size_t> readOnlyTables = readOnlyTableBindings(firing);
      std::vector<std::string> writeTypes;
      for (size_t table : tables)
        writeTypes.push_back(tableTypes[table]);
      const std::vector<std::string> inputTypes = queueTypes(firing.inputs);
      const std::vector<std::string> outputTypes = queueTypes(firing.outputs);
      const bool oneOwner = tables.size() == 1;
      llvm::StringMap<size_t> writeOrdinals;
      for (auto [writeIndex, write] : llvm::enumerate(firing.stateWrites))
        writeOrdinals[write.table] = writeIndex;

      QueueBlockPlan evaluation = firing;
      std::vector<std::string> additional{firing.guard};
      std::string tupleResult = "std::tuple{";
      for (auto [writeIndex, write] : llvm::enumerate(firing.stateWrites)) {
        if (writeIndex)
          tupleResult.append(", ");
        tupleResult.append(write.index)
            .append(", ")
            .append(write.value)
            .append(", ")
            .append(write.present);
        additional.push_back(write.index);
        additional.push_back(write.value);
        additional.push_back(write.present);
      }
      for (auto [outputIndex, yield] : llvm::enumerate(firing.yields)) {
        const std::string &present = firing.outputPresence[outputIndex].present;
        tupleResult.append(", ").append(yield).append(", ").append(present);
        additional.push_back(yield);
        additional.push_back(present);
      }
      for (auto [ownerIndex, tableIndex] : llvm::enumerate(tables)) {
        const TablePlan &table = specialization.tables[tableIndex];
        size_t reservationIndex = 0;
        for (const StateReservationPlan *reservation :
             findStateReservations(firing, table.name)) {
          if (reservation->indexKind == "all") {
            ++reservationIndex;
            continue;
          }
          if (reservation->indexKind == "set") {
            const std::string result = "snapshot_set_" +
                                       std::to_string(ownerIndex) + "_" +
                                       std::to_string(reservationIndex++);
            QueueExpressionPlan expression{result, "snapshot_set", "i64", {}};
            expression.field = reservation->source;
            expression.table = reservation->table;
            evaluation.expressions.push_back(std::move(expression));
            tupleResult.append(", ").append(result);
            additional.push_back(result);
            continue;
          }
          ++reservationIndex;
          tupleResult.append(", ").append(reservation->index);
          additional.push_back(reservation->index);
        }
      }
      tupleResult.append(", ").append(firing.guard).push_back('}');
      auto body = emitExpressionBody(specialization, evaluation,
                                     firing.stateWrites.front().index, 6, true,
                                     false, additional, tupleResult);
      if (!body)
        return body.takeError();

      std::string planType;
      if (oneOwner) {
        planType = "gfsim::TableTransitionPlan<" + writeTypes.front();
        for (const std::string &type : outputTypes)
          planType.append(", ").append(type);
        planType.push_back('>');
      } else {
        planType = "gfsim::StateTransitionPlan<" + tupleType(writeTypes) +
                   ", " + tupleType(outputTypes) + ">";
      }
      output << "struct " << policyName(blockIndex) << " {\n";
      for (size_t table : readOnlyTables)
        output << "  const gfsim::SimTable<" << tableTypes[table]
               << "> *read_table_"
               << identifier(specialization.tables[table].name) << "{};\n";
      output << "  std::optional<" << planType
             << "> operator()(gfsim::Epoch epoch, ";
      if (oneOwner) {
        output << "const gfsim::SimTable<" << writeTypes.front()
               << "> &table_ref";
      } else {
        output << "std::tuple<";
        for (auto [index, type] : llvm::enumerate(writeTypes)) {
          if (index)
            output << ", ";
          output << "const gfsim::SimTable<" << type << "> *";
        }
        output << "> table_refs";
      }
      for (auto [inputIndex, type] : llvm::enumerate(inputTypes)) {
        output << ", const " << type << " &item";
        if (inputIndex)
          output << inputIndex;
      }
      output << ") const {\n";
      if (oneOwner) {
        output << "    const auto *table_"
               << identifier(specialization.tables[tables.front()].name)
               << " = &table_ref;\n";
      } else {
        for (auto [ownerIndex, tableIndex] : llvm::enumerate(tables))
          output << "    const auto *table_"
                 << identifier(specialization.tables[tableIndex].name)
                 << " = std::get<" << ownerIndex << ">(table_refs);\n";
      }
      for (size_t table : readOnlyTables)
        output << "    const auto *table_"
               << identifier(specialization.tables[table].name)
               << " = read_table_"
               << identifier(specialization.tables[table].name) << ";\n";
      output << "    auto [";
      for (size_t writeIndex = 0; writeIndex < firing.stateWrites.size();
           ++writeIndex) {
        if (writeIndex)
          output << ", ";
        output << "proposal_index" << writeIndex << ", proposal_value"
               << writeIndex << ", proposal_present" << writeIndex;
      }
      for (size_t outputIndex = 0; outputIndex < outputTypes.size();
           ++outputIndex)
        output << ", output_value" << outputIndex << ", output_present"
               << outputIndex;
      for (auto [ownerIndex, tableIndex] : llvm::enumerate(tables)) {
        size_t reservationIndex = 0;
        for (const StateReservationPlan *reservation : findStateReservations(
                 firing, specialization.tables[tableIndex].name)) {
          if (reservation->indexKind == "all") {
            ++reservationIndex;
            continue;
          }
          output << ", "
                 << (reservation->indexKind == "set" ? "snapshot_set_"
                                                     : "reservation_index")
                 << ownerIndex << '_' << reservationIndex++;
        }
      }
      output << ", condition] = [&]() {\n"
             << *body << "    }();\n"
             << "    if (!condition)\n      return std::nullopt;\n"
             << "    return " << planType;
      if (oneOwner) {
        output << "{proposal_present0 ? std::vector<std::pair<size_t, "
               << writeTypes.front()
               << ">>{{{static_cast<size_t>(proposal_index0), "
                  "proposal_value0}}} : std::vector<std::pair<size_t, "
               << writeTypes.front() << ">>{}, {";
      } else {
        output << "{{";
        for (auto [ownerIndex, type] : llvm::enumerate(writeTypes)) {
          if (ownerIndex)
            output << ", ";
          const TablePlan &table = specialization.tables[tables[ownerIndex]];
          auto ordinal = writeOrdinals.find(table.name);
          if (ordinal == writeOrdinals.end()) {
            output << "std::optional<std::pair<size_t, " << type << ">>{}";
            continue;
          }
          const size_t writeIndex = ordinal->getValue();
          output << "proposal_present" << writeIndex
                 << " ? std::optional<std::pair<size_t, " << type
                 << ">>{std::in_place, static_cast<size_t>(proposal_index"
                 << writeIndex << "), proposal_value" << writeIndex
                 << "} : std::optional<std::pair<size_t, " << type << ">>{}";
        }
        output << "}, {";
      }
      for (auto [outputIndex, type] : llvm::enumerate(outputTypes)) {
        if (outputIndex)
          output << ", ";
        output << "output_present" << outputIndex << " ? std::optional<" << type
               << ">{output_value" << outputIndex << "} : std::optional<"
               << type << ">{}";
      }
      output << "}, {";
      for (size_t ownerIndex = 0; ownerIndex < writeTypes.size();
           ++ownerIndex) {
        if (ownerIndex)
          output << ", ";
        const TablePlan &table = specialization.tables[tables[ownerIndex]];
        output << "gfsim::StateReservation{}";
        size_t reservationIndex = 0;
        for (const StateReservationPlan *reservation :
             findStateReservations(firing, table.name)) {
          auto fieldMask =
              reservationFieldMask(plan, table, reservation->fields);
          if (!fieldMask)
            return fieldMask.takeError();
          output << " | "
                 << (fieldMask->complete
                         ? "gfsim::StateReservation("
                         : "gfsim::StateReservation::forFields(");
          if (reservation->indexKind == "all") {
            output << (table.entries == 64
                           ? "~std::uint64_t{0}"
                           : "((std::uint64_t{1} << " +
                                 std::to_string(table.entries) + ") - 1)");
            ++reservationIndex;
          } else if (reservation->indexKind == "set") {
            output << "snapshot_set_" << ownerIndex << '_'
                   << reservationIndex++;
          } else {
            output << "(std::uint64_t{1} << static_cast<std::size_t>("
                   << "reservation_index" << ownerIndex << '_'
                   << reservationIndex++ << "))";
          }
          if (fieldMask->complete)
            output << ")";
          else
            output << ", std::uint64_t{" << fieldMask->mask << "}, "
                   << fieldMask->count << ")";
        }
      }
      output << "}};\n  }\n};\n\n";
      for (auto [ownerIndex, tableIndex] : llvm::enumerate(tables)) {
        const TablePlan &table = specialization.tables[tableIndex];
        const StateWritePlan *write = findStateWrite(firing, table.name);
        const std::vector<std::string> fields =
            write ? write->fields : std::vector<std::string>{"$entry"};
        if (auto error = emitStructuredMergePolicy(
                output, specialization, mergeName(blockIndex, ownerIndex),
                writeTypes[ownerIndex], fields))
          return error;
      }
    }
    auto emitQueueTuple = [&](llvm::ArrayRef<std::string> queues) {
      output << "std::tuple{";
      for (auto [index, queue] : llvm::enumerate(queues)) {
        if (index)
          output << ", ";
        output << '&' << portParameters.lookup(queue);
      }
      output << '}';
    };
    output << "class " << implementation
           << " final : public gfsim::Module {\npublic:\n  " << implementation
           << "(std::string name";
    for (size_t index = 0; index < specialization.blocks.size(); ++index)
      output << ", gfsim::ObjectId block_" << index << "_id";
    for (size_t index = 0; index < specialization.tables.size(); ++index)
      output << ", gfsim::ObjectId table_" << index << "_id";
    output << ", gfsim::SimObject *parent";
    for (auto [index, input] : llvm::enumerate(specialization.interfaceInputs))
      output << ", gfsim::SimQueue<" << portTypes.lookup(input.name)
             << "> &input_" << index;
    for (auto [index, result] :
         llvm::enumerate(specialization.interfaceOutputs))
      output << ", gfsim::SimQueue<" << portTypes.lookup(result.name)
             << "> &output_" << index;
    output << ")\n      : gfsim::Module(std::move(name), "
              "gfsim::kInvalidObjectId, parent),\n        scope_(\""
           << localScope << "\", gfsim::kInvalidObjectId, this)";
    for (auto [index, table] : llvm::enumerate(specialization.tables))
      output << ",\n        table_" << index << "_(\"" << table.name
             << "\", table_" << index << "_id, &scope_, " << table.entries
             << ")";
    for (auto [blockIndex, firing] : llvm::enumerate(specialization.blocks)) {
      const std::vector<size_t> tables = tableBindings(firing);
      const std::vector<size_t> readOnlyTables = readOnlyTableBindings(firing);
      std::string policy = policyName(blockIndex) + "{";
      for (auto [index, table] : llvm::enumerate(readOnlyTables)) {
        if (index)
          policy.append(", ");
        policy.append("&table_").append(std::to_string(table)).append("_");
      }
      policy.push_back('}');
      output << ",\n        block_" << blockIndex << "_(\"firing_"
             << firing.name << "\", block_" << blockIndex << "_id, &scope_, ";
      if (tables.size() == 1) {
        output << "table_" << tables.front() << "_, ";
      } else {
        output << "std::tuple{";
        for (auto [index, table] : llvm::enumerate(tables)) {
          if (index)
            output << ", ";
          output << "&table_" << table << '_';
        }
        output << "}, ";
      }
      emitQueueTuple(firing.inputs);
      output << ", ";
      emitQueueTuple(firing.outputs);
      if (tables.size() == 1) {
        output << ", gfsim::TableWriteMode::Replace, " << policy << ", "
               << mergeName(blockIndex, 0) << "{})";
      } else {
        output << ", std::array{";
        for (size_t index = 0; index < tables.size(); ++index) {
          if (index)
            output << ", ";
          output << "gfsim::TableWriteMode::Replace";
        }
        output << "}, " << policy << ", std::tuple{";
        for (size_t index = 0; index < tables.size(); ++index) {
          if (index)
            output << ", ";
          output << mergeName(blockIndex, index) << "{}";
        }
        output << "})";
      }
    }
    output << " {\n    attachChild(scope_);\n";
    for (size_t index = 0; index < specialization.tables.size(); ++index)
      output << "    scope_.attachChild(table_" << index << "_);\n";
    for (size_t index = 0; index < specialization.blocks.size(); ++index)
      output << "    scope_.attachChild(block_" << index << "_);\n";
    output << "  }\n\n  gfsim::DispatchRow dispatch_row(size_t index) {\n"
           << "    switch (index) {\n";
    for (size_t index = 0; index < specialization.blocks.size(); ++index)
      output << "    case " << index
             << ": return gfsim::makeDispatchRow(&block_" << index << "_);\n";
    for (size_t index = 0; index < specialization.tables.size(); ++index)
      output << "    case " << specialization.blocks.size() + index
             << ": return gfsim::makeDispatchRow(&table_" << index << "_);\n";
    output << "    default: return {};\n    }\n  }\n\nprivate:\n"
           << "  gfsim::Module scope_;\n";
    for (auto [index, type] : llvm::enumerate(tableTypes))
      output << "  gfsim::SimTable<" << type << "> table_" << index << "_;\n";
    for (auto [blockIndex, firing] : llvm::enumerate(specialization.blocks)) {
      const std::vector<size_t> tables = tableBindings(firing);
      const std::vector<std::string> inputTypes = queueTypes(firing.inputs);
      const std::vector<std::string> outputTypes = queueTypes(firing.outputs);
      if (tables.size() == 1) {
        output << "  gfsim::QueueTableTransition<" << policyName(blockIndex)
               << ", " << tableTypes[tables.front()] << ", "
               << tupleType(inputTypes) << ", " << tupleType(outputTypes)
               << ", " << mergeName(blockIndex, 0) << "> block_" << blockIndex
               << "_;\n";
      } else {
        std::vector<std::string> writeTypes;
        for (size_t table : tables)
          writeTypes.push_back(tableTypes[table]);
        output << "  gfsim::QueueStateTransition<" << policyName(blockIndex)
               << ", " << tupleType(writeTypes) << ", " << tupleType(inputTypes)
               << ", " << tupleType(outputTypes) << ", std::tuple<";
        for (size_t index = 0; index < tables.size(); ++index) {
          if (index)
            output << ", ";
          output << mergeName(blockIndex, index);
        }
        output << ">> block_" << blockIndex << "_;\n";
      }
    }
    output << "};\n\n";
    return llvm::Error::success();
  };

  auto emitNestedWrapper =
      [&](const QueueGraphPlan &specialization,
          const std::string &implementation) -> llvm::Error {
    llvm::StringMap<std::string> portTypes;
    llvm::StringMap<std::string> portParameters;
    for (auto [index, input] :
         llvm::enumerate(specialization.interfaceInputs)) {
      auto type = cppType(input.payloadType);
      if (!type)
        return type.takeError();
      portTypes[input.name] = *type;
      portParameters[input.name] = "input_" + std::to_string(index);
    }
    for (auto [index, result] :
         llvm::enumerate(specialization.interfaceOutputs)) {
      auto type = cppType(result.payloadType);
      if (!type)
        return type.takeError();
      portTypes[result.name] = *type;
      portParameters[result.name] = "output_" + std::to_string(index);
    }
    const uint64_t objectCount = specializationObjectCount(specialization);
    output << "class " << implementation
           << " final : public gfsim::Module {\npublic:\n  " << implementation
           << "(std::string name";
    for (uint64_t index = 0; index < objectCount; ++index)
      output << ", gfsim::ObjectId object_" << index << "_id";
    output << ", gfsim::SimObject *parent";
    for (auto [index, input] : llvm::enumerate(specialization.interfaceInputs))
      output << ", gfsim::SimQueue<" << portTypes.lookup(input.name)
             << "> &input_" << index;
    for (auto [index, result] :
         llvm::enumerate(specialization.interfaceOutputs))
      output << ", gfsim::SimQueue<" << portTypes.lookup(result.name)
             << "> &output_" << index;
    output << ")\n      : gfsim::Module(std::move(name), "
              "gfsim::kInvalidObjectId, parent)";
    uint64_t objectOffset = 0;
    for (auto [instanceIndex, instance] :
         llvm::enumerate(specialization.moduleInstances)) {
      const QueueGraphPlan *child =
          specializations.lookup(instance.specializationFingerprint);
      if (!child)
        return generatorError("nested wrapper child specialization is missing");
      const uint64_t childCount = specializationObjectCount(*child);
      output << ",\n        child_" << instanceIndex << "_(\"" << instance.name
             << "\"";
      for (uint64_t index = 0; index < childCount; ++index)
        output << ", object_" << objectOffset + index << "_id";
      output << ", this";
      for (const std::string &input : instance.inputs)
        output << ", " << portParameters.lookup(input);
      for (const std::string &result : instance.outputs)
        output << ", " << portParameters.lookup(result);
      output << ')';
      objectOffset += childCount;
    }
    if (objectOffset != objectCount)
      return generatorError("nested wrapper object ID partition is incomplete");
    output << " {\n";
    for (size_t index = 0; index < specialization.moduleInstances.size();
         ++index)
      output << "    attachChild(child_" << index << "_);\n";
    output << "  }\n\n  gfsim::DispatchRow dispatch_row(size_t index) {\n";
    objectOffset = 0;
    for (auto [instanceIndex, instance] :
         llvm::enumerate(specialization.moduleInstances)) {
      const QueueGraphPlan *child =
          specializations.lookup(instance.specializationFingerprint);
      const uint64_t childCount = specializationObjectCount(*child);
      output << "    if (index < " << objectOffset + childCount
             << ") return child_" << instanceIndex << "_.dispatch_row(index - "
             << objectOffset << ");\n";
      objectOffset += childCount;
    }
    output << "    return {};\n  }\n\nprivate:\n";
    for (auto [instanceIndex, instance] :
         llvm::enumerate(specialization.moduleInstances)) {
      const QueueGraphPlan *child =
          specializations.lookup(instance.specializationFingerprint);
      output << "  " << specializationClassName(*child) << " child_"
             << instanceIndex << "_;\n";
    }
    output << "};\n\n";
    return llvm::Error::success();
  };

  auto emitMixedNested = [&](const QueueGraphPlan &specialization,
                             const std::string &implementation) -> llvm::Error {
    const QueueBlockPlan &block = specialization.blocks.front();
    llvm::StringMap<std::string> queueTypes;
    llvm::StringMap<std::string> queueExpressions;
    for (auto [index, input] :
         llvm::enumerate(specialization.interfaceInputs)) {
      auto type = cppType(input.payloadType);
      if (!type)
        return type.takeError();
      queueTypes[input.name] = *type;
      queueExpressions[input.name] = "input_" + std::to_string(index);
    }
    for (auto [index, result] :
         llvm::enumerate(specialization.interfaceOutputs)) {
      auto type = cppType(result.payloadType);
      if (!type)
        return type.takeError();
      queueTypes[result.name] = *type;
      queueExpressions[result.name] = "output_" + std::to_string(index);
    }
    llvm::StringSet<> exported;
    for (const QueueInterfacePlan &result : specialization.interfaceOutputs)
      exported.insert(result.name);
    std::vector<const QueuePlan *> internalQueues;
    for (const QueuePlan &queue : specialization.queues) {
      if (exported.contains(queue.name))
        continue;
      auto type = cppType(queue.payloadType);
      if (!type)
        return type.takeError();
      const size_t index = internalQueues.size();
      internalQueues.push_back(&queue);
      queueTypes[queue.name] = *type;
      queueExpressions[queue.name] = "queue_" + std::to_string(index) + "_";
    }
    if (block.inputs.size() != 1 || block.outputs.size() != 1 ||
        internalQueues.empty())
      return generatorError("mixed nested module requires one local transform "
                            "and internal Queue");
    const std::string inputType = queueTypes.lookup(block.inputs.front());
    const std::string outputType = queueTypes.lookup(block.outputs.front());
    const QueuePlan *outputQueue =
        findQueue(specialization, block.outputs.front());
    if (!outputQueue)
      return generatorError("mixed nested transform output Queue is missing");
    const std::string policy = implementation + "_local_policy";
    output << "struct " << policy << " {\n  " << outputType
           << " operator()(const " << inputType << " &item) const {\n";
    auto body =
        emitExpressionBody(specialization, block, block.yields.front(), 4);
    if (!body)
      return body.takeError();
    output << *body << "  }\n};\n\n"
           << "class " << implementation
           << " final : public gfsim::Module {\npublic:\n  " << implementation
           << "(std::string name";
    const uint64_t objectCount = specializationObjectCount(specialization);
    for (uint64_t index = 0; index < objectCount; ++index)
      output << ", gfsim::ObjectId object_" << index << "_id";
    output << ", gfsim::SimObject *parent";
    for (auto [index, input] : llvm::enumerate(specialization.interfaceInputs))
      output << ", gfsim::SimQueue<" << queueTypes.lookup(input.name)
             << "> &input_" << index;
    for (auto [index, result] :
         llvm::enumerate(specialization.interfaceOutputs))
      output << ", gfsim::SimQueue<" << queueTypes.lookup(result.name)
             << "> &output_" << index;
    output << ")\n      : gfsim::Module(std::move(name), "
              "gfsim::kInvalidObjectId, parent),\n        scope_(\""
           << pathParts(block.scope).back()
           << "\", gfsim::kInvalidObjectId, this)";
    for (auto [index, queue] : llvm::enumerate(internalQueues))
      output << ",\n        queue_" << index << "_(\"" << queue->name
             << "\", object_" << index << "_id, this, " << queue->depth
             << ", std::numeric_limits<size_t>::max(), nullptr, "
             << queue->latency << ", " << queue->rate << ")";
    const uint64_t blockId = internalQueues.size();
    output << ",\n        block_(\"transform_" << block.name << "\", object_"
           << blockId << "_id, &scope_, "
           << queueExpressions.lookup(block.inputs.front()) << ", "
           << queueExpressions.lookup(block.outputs.front()) << ")";
    uint64_t childOffset = blockId + 1;
    for (auto [instanceIndex, instance] :
         llvm::enumerate(specialization.moduleInstances)) {
      const QueueGraphPlan *child =
          specializations.lookup(instance.specializationFingerprint);
      if (!child)
        return generatorError("mixed nested child specialization is missing");
      const uint64_t childCount = specializationObjectCount(*child);
      output << ",\n        child_" << instanceIndex << "_(\"" << instance.name
             << "\"";
      for (uint64_t index = 0; index < childCount; ++index)
        output << ", object_" << childOffset + index << "_id";
      output << ", this";
      for (const std::string &input : instance.inputs)
        output << ", " << queueExpressions.lookup(input);
      for (const std::string &result : instance.outputs)
        output << ", " << queueExpressions.lookup(result);
      output << ')';
      childOffset += childCount;
    }
    if (childOffset != objectCount)
      return generatorError("mixed nested object ID partition is incomplete");
    output << " {\n    attachChild(scope_);\n";
    for (size_t index = 0; index < internalQueues.size(); ++index)
      output << "    attachChild(queue_" << index << "_);\n";
    output << "    scope_.attachChild(block_);\n";
    for (size_t index = 0; index < specialization.moduleInstances.size();
         ++index)
      output << "    attachChild(child_" << index << "_);\n";
    output << "  }\n\n  gfsim::DispatchRow dispatch_row(size_t index) {\n";
    for (size_t index = 0; index < internalQueues.size(); ++index)
      output << "    if (index == " << index
             << ") return gfsim::makeDispatchRow(&queue_" << index << "_);\n";
    output << "    if (index == " << blockId
           << ") return gfsim::makeDispatchRow(&block_);\n";
    childOffset = blockId + 1;
    for (auto [instanceIndex, instance] :
         llvm::enumerate(specialization.moduleInstances)) {
      const QueueGraphPlan *child =
          specializations.lookup(instance.specializationFingerprint);
      const uint64_t childCount = specializationObjectCount(*child);
      output << "    if (index < " << childOffset + childCount
             << ") return child_" << instanceIndex << "_.dispatch_row(index - "
             << childOffset << ");\n";
      childOffset += childCount;
    }
    output << "    return {};\n  }\n\nprivate:\n  gfsim::Module scope_;\n";
    for (auto [index, queue] : llvm::enumerate(internalQueues))
      output << "  gfsim::SimQueue<" << queueTypes.lookup(queue->name)
             << "> queue_" << index << "_;\n";
    output << "  gfsim::QueueTransform<" << inputType << ", " << outputType
           << ", " << policy << ", " << outputQueue->rate << "> block_;\n";
    for (auto [instanceIndex, instance] :
         llvm::enumerate(specialization.moduleInstances)) {
      const QueueGraphPlan *child =
          specializations.lookup(instance.specializationFingerprint);
      output << "  " << specializationClassName(*child) << " child_"
             << instanceIndex << "_;\n";
    }
    output << "};\n\n";
    return llvm::Error::success();
  };

  for (const QueueGraphPlan *specialization : emissionOrder) {
    const std::string implementation = specializationClassName(*specialization);
    if (!specialization->moduleInstances.empty()) {
      auto error = specialization->blocks.empty()
                       ? emitNestedWrapper(*specialization, implementation)
                       : emitMixedNested(*specialization, implementation);
      if (error)
        return std::move(error);
      continue;
    }
    const QueueBlockPlan &block = specialization->blocks.front();
    const std::string localScope = pathParts(block.scope).back();
    if (block.kind == "firing") {
      if (auto error =
              emitStatefulSpecialization(*specialization, implementation))
        return std::move(error);
      continue;
    }
    auto inputType =
        cppType(specialization->interfaceInputs.front().payloadType);
    auto outputType =
        cppType(specialization->interfaceOutputs.front().payloadType);
    if (!inputType)
      return inputType.takeError();
    if (!outputType)
      return outputType.takeError();
    const QueuePlan *outputQueue = findQueue(
        *specialization, specialization->interfaceOutputs.front().name);
    if (!outputQueue)
      return generatorError("specialization output Queue metadata is missing");
    if (block.kind == "transform") {
      output << "struct " << implementation << "_policy {\n  " << *outputType
             << " operator()(const " << *inputType << " &item) const {\n";
      auto body =
          emitExpressionBody(*specialization, block, block.yields.front(), 4);
      if (!body)
        return body.takeError();
      output << *body << "  }\n};\n\n"
             << "class " << implementation
             << " final : public gfsim::Module {\npublic:\n  " << implementation
             << "(std::string name, gfsim::ObjectId block_id, "
                "gfsim::SimObject *parent, gfsim::SimQueue<"
             << *inputType << "> &input, gfsim::SimQueue<" << *outputType
             << "> &output)\n      : gfsim::Module(std::move(name), "
                "gfsim::kInvalidObjectId, parent),\n        scope_(\""
             << localScope
             << "\", gfsim::kInvalidObjectId, this),\n        "
                "block_(\"transform_"
             << block.name << "\", block_id, &scope_, input, output) {\n"
             << "    attachChild(scope_);\n    scope_.attachChild(block_);\n  "
                "}\n\n"
             << "  gfsim::DispatchRow dispatch_row(size_t index) {\n"
             << "    return index == 0 ? gfsim::makeDispatchRow(&block_) : "
                "gfsim::DispatchRow{};\n  }\n\nprivate:\n"
             << "  gfsim::Module scope_;\n"
             << "  gfsim::QueueTransform<" << *inputType << ", " << *outputType
             << ", " << implementation << "_policy, " << outputQueue->rate
             << "> block_;\n};\n\n";
      continue;
    }

    return generatorError(
        "structured specialization contains an unsupported block");
  }

  const std::string modelClass = className(plan.system);
  output << "class " << modelClass
         << " final : public gfsim::Module {\npublic:\n  " << modelClass
         << "() : gfsim::Module(\"" << plan.system
         << "\", gfsim::kInvalidObjectId, nullptr),\n";
  std::vector<std::string> initializers;
  for (const std::string &scope : plan.scopes) {
    llvm::StringRef parent = llvm::StringRef(scope).rsplit('/').first;
    if (parent.empty())
      parent = "/";
    auto parentPointer = modulePointer(parent);
    if (!parentPointer)
      return parentPointer.takeError();
    appendInitializer(initializers, scopeMembers[scope], "(\"",
                      pathParts(scope).back(), "\", gfsim::kInvalidObjectId, ",
                      *parentPointer, ")");
  }
  for (const QueuePlan &queue : plan.queues) {
    auto type = cppType(queue.payloadType);
    auto parent = modulePointer(queueOwners[queue.name]);
    if (!type)
      return type.takeError();
    if (!parent)
      return parent.takeError();
    appendInitializer(initializers, queueMembers[queue.name], "(\"", queue.name,
                      "\", ", queueIds[queue.name], ", ", *parent, ", ",
                      queue.depth,
                      ", std::numeric_limits<size_t>::max(), nullptr, ",
                      queue.latency, ", ", queue.rate, ")");
  }
  for (auto [index, instance] : llvm::enumerate(plan.moduleInstances)) {
    const QueueGraphPlan *specialization =
        specializations.lookup(instance.specializationFingerprint);
    if (!specialization)
      return generatorError("structured module specialization is missing");
    auto parent = modulePointer(instance.scope);
    if (!parent)
      return parent.takeError();
    appendInitializer(initializers, "instance_", index, "_(\"", instance.name,
                      "\"");
    for (uint64_t objectId : instanceObjectIds[index])
      initializers.back().append(", ").append(std::to_string(objectId));
    initializers.back().append(", ").append(*parent);
    for (const std::string &input : instance.inputs)
      initializers.back().append(", ").append(queueMembers[input]);
    for (const std::string &outputName : instance.outputs)
      initializers.back().append(", ").append(queueMembers[outputName]);
    initializers.back().append(")");
  }
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    auto parent = modulePointer(block->scope);
    if (!parent)
      return parent.takeError();
    const std::string member = "block_" + std::to_string(index) + "_";
    const std::string instanceName = block->kind + "_" + block->name;
    if (block->kind == "broadcast") {
      const QueuePlan *input = findQueue(plan, block->inputs.front());
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(generatorError(
                              "structured broadcast input is missing"));
      if (!type)
        return type.takeError();
      std::string outputs;
      for (auto [outputIndex, name] : llvm::enumerate(block->outputs)) {
        if (outputIndex)
          outputs.append(", ");
        outputs.append("&").append(queueMembers[name]);
      }
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[block], ", ", *parent, ", ",
                        queueMembers[block->inputs.front()],
                        ", std::array<gfsim::SimQueue<", *type, "> *, ",
                        block->outputs.size(), ">{", outputs, "})");
    } else if (block->kind == "sink" || block->kind == "observe") {
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[block], ", ", *parent, ", ",
                        queueMembers[block->inputs.front()], ")");
    }
  }
  for (auto [index, initializer] : llvm::enumerate(initializers))
    output << "        " << initializer
           << (index + 1 == initializers.size() ? "\n" : ",\n");
  output << "  {\n    setPath(\"/" << plan.system << "\");\n";
  for (const std::string &scope : plan.scopes) {
    llvm::StringRef parent = llvm::StringRef(scope).rsplit('/').first;
    if (parent.empty())
      parent = "/";
    auto line = attach(parent, scopeMembers[scope]);
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (const QueuePlan &queue : plan.queues) {
    auto line = attach(queueOwners[queue.name], queueMembers[queue.name]);
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (auto [index, instance] : llvm::enumerate(plan.moduleInstances)) {
    auto line =
        attach(instance.scope, "instance_" + std::to_string(index) + "_");
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    auto line = attach(block->scope, "block_" + std::to_string(index) + "_");
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  output << "  }\n\n";
  for (const QueueBlockPlan &block : plan.blocks)
    if (block.kind == "source") {
      const QueuePlan *queue = findQueue(plan, block.outputs.front());
      auto type = queue ? cppType(queue->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("structured source is missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::SimQueue<" << *type << "> &" << block.outputs.front()
             << "() { return " << queueMembers[block.outputs.front()] << "; }\n"
             << "  bool offer_" << identifier(block.outputs.front())
             << "(gfsim::SimSystem &system, " << *type << " value) {\n"
             << "    if (!" << queueMembers[block.outputs.front()]
             << ".canProposePush() || !system.scheduleExternalXfer("
             << queueMembers[block.outputs.front()] << ".id()))\n"
             << "      return false;\n"
             << "    return " << queueMembers[block.outputs.front()]
             << ".proposePush(std::move(value));\n  }\n";
    }
  for (auto [index, result] : llvm::enumerate(plan.interfaceOutputs)) {
    const QueuePlan *queue = findQueue(plan, result.name);
    auto type = queue ? cppType(queue->payloadType)
                      : llvm::Expected<std::string>(
                            generatorError("structured result is missing"));
    if (!type)
      return type.takeError();
    output << "  const gfsim::SimQueue<" << *type << "> &result_" << index
           << "() const { return " << queueMembers[result.name] << "; }\n"
           << "  std::optional<" << *type << "> try_take_result_" << index
           << "(gfsim::SimSystem &system) {\n"
           << "    if (!" << queueMembers[result.name]
           << ".canProposePop() || !system.scheduleExternalXfer("
           << queueMembers[result.name] << ".id()))\n"
           << "      return std::nullopt;\n"
           << "    return " << queueMembers[result.name]
           << ".proposePop();\n  }\n";
  }
  size_t sinkIndex = 0;
  for (auto [index, block] : llvm::enumerate(runtimeBlocks))
    if (block->kind == "sink") {
      const QueuePlan *queue = findQueue(plan, block->inputs.front());
      auto type = queue ? cppType(queue->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("structured sink is missing"));
      if (!type)
        return type.takeError();
      output << "  const std::vector<" << *type << "> &sink_" << sinkIndex
             << "_values() const { return block_" << index
             << "_.received(); }\n";
      ++sinkIndex;
    }
  output << "\n  std::array<gfsim::DispatchRow, " << nextId
         << "> dispatch_rows() {\n    return {\n";
  for (const QueuePlan &queue : plan.queues)
    output << "        gfsim::makeDispatchRow(&" << queueMembers[queue.name]
           << "),\n";
  for (const DispatchItem &item : dispatchItems) {
    if (item.block) {
      const size_t index = static_cast<size_t>(
          llvm::find(runtimeBlocks, item.block) - runtimeBlocks.begin());
      output << "        gfsim::makeDispatchRow(&block_" << index << "_),\n";
    } else {
      const QueueModuleInstancePlan &instance =
          plan.moduleInstances[item.instanceIndex];
      const QueueGraphPlan *specialization =
          specializations.lookup(instance.specializationFingerprint);
      for (uint64_t index = 0;
           index < specializationObjectCount(*specialization); ++index)
        output << "        instance_" << item.instanceIndex << "_.dispatch_row("
               << index << "),\n";
    }
  }
  output << "    };\n  }\n\n"
         << "  static constexpr std::array<uint32_t, "
         << activationOffsets.size()
         << "> activation_offsets() {\n    return {";
  for (auto [index, offset] : llvm::enumerate(activationOffsets)) {
    if (index)
      output << ", ";
    output << offset;
  }
  output << "};\n  }\n\n"
         << "  static constexpr bool activation_complete() { return "
         << (activationComplete ? "true" : "false") << "; }\n\n"
         << "  static constexpr std::array<gfsim::ObjectId, "
         << activationTargets.size()
         << "> activation_targets() {\n    return {";
  for (auto [index, target] : llvm::enumerate(activationTargets)) {
    if (index)
      output << ", ";
    output << target;
  }
  output << "};\n  }\n\n"
         << "  static constexpr std::array<uint32_t, "
         << workClosureOffsets.size()
         << "> work_closure_offsets() {\n    return {";
  for (auto [index, offset] : llvm::enumerate(workClosureOffsets)) {
    if (index)
      output << ", ";
    output << offset;
  }
  output << "};\n  }\n\n"
         << "  static constexpr std::array<gfsim::ObjectId, "
         << workClosureTargets.size()
         << "> work_closure_targets() {\n    return {";
  for (auto [index, target] : llvm::enumerate(workClosureTargets)) {
    if (index)
      output << ", ";
    output << target;
  }
  output << "};\n  }\n\n"
         << "  static constexpr std::array<gfsim::ObjectId, "
         << initialActivation.size() << "> initial_work_ids() {\n    return {";
  for (auto [index, target] : llvm::enumerate(initialActivation)) {
    if (index)
      output << ", ";
    output << target;
  }
  output << "};\n  }\n\n"
         << "  static bool schedule_initial_work(gfsim::SimSystem &system) {\n"
         << "    for (gfsim::ObjectId id : initial_work_ids())\n"
         << "      if (!system.scheduleWork(id, system.currentEpoch()))\n"
         << "        return false;\n"
         << "    return true;\n  }\n\nprivate:\n";
  for (const std::string &scope : plan.scopes)
    output << "  gfsim::Module " << scopeMembers[scope] << ";\n";
  for (const QueuePlan &queue : plan.queues) {
    auto type = cppType(queue.payloadType);
    if (!type)
      return type.takeError();
    output << "  gfsim::SimQueue<" << *type << "> " << queueMembers[queue.name]
           << ";\n";
  }
  for (auto [index, instance] : llvm::enumerate(plan.moduleInstances)) {
    const QueueGraphPlan *specialization =
        specializations.lookup(instance.specializationFingerprint);
    output << "  " << specializationClassName(*specialization) << " instance_"
           << index << "_;\n";
  }
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    const QueuePlan *input = findQueue(plan, block->inputs.front());
    auto type = input ? cppType(input->payloadType)
                      : llvm::Expected<std::string>(
                            generatorError("structured block input missing"));
    if (!type)
      return type.takeError();
    if (block->kind == "broadcast")
      output << "  gfsim::QueueBroadcast<" << *type << ", "
             << block->outputs.size() << "> block_" << index << "_;\n";
    else if (block->kind == "sink")
      output << "  gfsim::QueueSink<" << *type << "> block_" << index << "_;\n";
    else if (block->kind == "observe")
      output << "  gfsim::QueueObserve<" << *type << "> block_" << index
             << "_;\n";
  }
  output << "};\n\n} // namespace ac_generated\n";
  return output.str();
}

} // namespace

llvm::Expected<std::string> generateQueueGraphCpp(const QueueGraphPlan &plan) {
  if (!plan.definition.empty())
    return generateStructuredQueueGraphCpp(plan);
  if (plan.system.empty() || plan.queues.empty() || plan.blocks.empty())
    return generatorError("QueueGraph plan is incomplete");
  if (!plan.scopes.empty()) {
    const QueueBlockContract *scope = findQueueBlockContract("scope");
    if (!scope || !scope->gfsimAvailable)
      return generatorError("official opcode has no gfsim lowering: 'scope'");
  }
  for (const QueueBlockPlan &block : plan.blocks) {
    const QueueBlockContract *contract = findQueueBlockContract(block.kind);
    if (!contract || !contract->gfsimAvailable)
      return generatorError("official opcode has no gfsim lowering: '" +
                            block.kind + "'");
    if (block.kind == "reorder" &&
        (block.inputs.size() != 1 || block.outputs.size() != 1 ||
         block.yields.size() != 1 || block.capacity == 0))
      return generatorError("reorder contract is unsupported");
    if (block.kind == "dependency" &&
        (block.inputs.size() != 1 || block.outputs.size() != 1 ||
         block.yields.size() != 4 || block.capacity == 0 ||
         block.resources == 0))
      return generatorError("dependency contract is unsupported");
    if (block.kind == "credit" &&
        (block.inputs.size() != 1 || block.outputs.size() != 1 ||
         block.yields.size() != 1 || block.credits == 0))
      return generatorError("credit contract is unsupported");
    if (block.kind == "barrier" &&
        (block.inputs.size() < 2 ||
         block.outputs.size() != block.inputs.size() ||
         block.depths.size() != block.outputs.size() ||
         block.latencies.size() != block.outputs.size()))
      return generatorError("barrier contract is unsupported");
    if (block.kind == "select" &&
        (block.inputs.size() < 3 || block.outputs.size() != 1 ||
         block.yields.size() != 1))
      return generatorError("select contract is unsupported");
    if (block.kind == "expect" &&
        (block.inputs.size() != 1 || !block.outputs.empty() ||
         block.yields.size() != 1 || block.message.empty()))
      return generatorError("expect contract is unsupported");
    if (block.kind == "memory_request" &&
        (block.inputs.size() != 1 || block.outputs.size() != 1 ||
         block.yields.size() != 3 || block.memoryInstance.empty() ||
         block.resultField.empty()))
      return generatorError("memory contract is unsupported");
    if (block.kind == "table_read" &&
        (block.inputs.size() > 1 || block.outputs.size() != 1 ||
         block.yields.size() != 2 || block.table.empty()))
      return generatorError("table read contract is unsupported");
    if (block.kind == "table_write" &&
        (block.inputs.size() > 1 || !block.outputs.empty() ||
         block.yields.size() != 3 || block.table.empty() ||
         (block.writeMode != "field" && block.writeMode != "replace")))
      return generatorError("table write contract is unsupported");
    if (block.kind == "table_masked_write" &&
        (!block.inputs.empty() || !block.outputs.empty() ||
         block.yields.size() != 3 || block.table.empty() ||
         block.writeMode != "field"))
      return generatorError("masked table write contract is unsupported");
    if (block.kind == "firing" &&
        (block.outputs.size() > 1 ||
         block.yields.size() != block.outputs.size() || block.table.empty() ||
         block.tableIndex.empty() || block.tableValue.empty() ||
         block.guard.empty() || block.writeMode != "replace" ||
         block.writeFields.empty()))
      return generatorError("table firing contract is unsupported");
    if (block.kind == "slot" &&
        (block.inputs.size() != 1 || !block.outputs.empty() ||
         block.yields.size() != 1 || block.slot.empty()))
      return generatorError("slot contract is unsupported");
  }
  if (auto error = verifyQueueGraphPlan(plan))
    return std::move(error);

  llvm::StringMap<std::string> queueMembers;
  llvm::StringMap<std::string> queueOwners;
  for (const QueuePlan &queue : plan.queues) {
    if (queueMembers.contains(queue.name))
      return generatorError("Queue names must be unique");
    queueMembers[queue.name] = identifier(queue.name) + "_";
    queueOwners[queue.name] = queue.scope;
  }
  for (const QueueBlockPlan &block : plan.blocks)
    for (const std::string &input : block.inputs) {
      auto owner = queueOwners.find(input);
      if (owner == queueOwners.end())
        return generatorError("block input references unknown Queue '" + input +
                              "'");
      owner->getValue() = commonPath(owner->getValue(), block.scope);
    }

  llvm::StringMap<std::string> scopeMembers;
  for (auto [index, scope] : llvm::enumerate(plan.scopes))
    scopeMembers[scope] = "scope_" + std::to_string(index) + "_";
  auto modulePointer =
      [&](llvm::StringRef path) -> llvm::Expected<std::string> {
    if (path == "/")
      return std::string("this");
    auto found = scopeMembers.find(path);
    if (found == scopeMembers.end())
      return generatorError("unknown scope path '" + path + "'");
    return "&" + found->getValue();
  };
  auto attach = [&](llvm::StringRef path,
                    llvm::StringRef member) -> llvm::Expected<std::string> {
    if (path == "/")
      return "    attachChild(" + member.str() + ");";
    auto found = scopeMembers.find(path);
    if (found == scopeMembers.end())
      return generatorError("unknown attachment scope '" + path + "'");
    return "    " + found->getValue() + ".attachChild(" + member.str() + ");";
  };

  std::vector<const QueueBlockPlan *> runtimeBlocks;
  for (const QueueBlockPlan &block : plan.blocks)
    if (isRuntimeBlock(block) && block.kind != "memory_request")
      runtimeBlocks.push_back(&block);
  llvm::StringMap<std::vector<const QueueBlockPlan *>> memoryEndpoints;
  for (const QueueBlockPlan &block : plan.blocks)
    if (block.kind == "memory_request")
      memoryEndpoints[block.memoryInstance].push_back(&block);
  for (auto &entry : memoryEndpoints)
    llvm::sort(entry.getValue(),
               [](const QueueBlockPlan *left, const QueueBlockPlan *right) {
                 return left->endpointOrdinal < right->endpointOrdinal;
               });
  llvm::StringMap<uint64_t> queueIds;
  for (auto [index, queue] : llvm::enumerate(plan.queues))
    queueIds[queue.name] = index;
  uint64_t nextId = plan.queues.size();
  llvm::DenseMap<size_t, uint64_t> feedbackStateIds;
  for (auto [index, block] : llvm::enumerate(runtimeBlocks))
    if (block->kind == "feedback")
      feedbackStateIds[index] = nextId++;
  llvm::StringMap<uint64_t> blockIds;
  for (auto [index, block] : llvm::enumerate(runtimeBlocks))
    blockIds[block->name + "#" + std::to_string(index)] = nextId++;
  llvm::StringMap<uint64_t> memoryIds;
  for (const MemoryInstancePlan &instance : plan.memoryInstances)
    memoryIds[instance.name] = nextId++;
  llvm::StringMap<uint64_t> tableIds;
  llvm::StringMap<std::string> tableMembers;
  for (auto [index, table] : llvm::enumerate(plan.tables)) {
    tableIds[table.name] = nextId++;
    tableMembers[table.name] = "table_" + std::to_string(index) + "_";
  }

  std::ostringstream output;
  output << "// Generated from frozen ACIR QueueGraph plan; do not edit.\n";
  if (!plan.specializationFingerprint.empty())
    output << "// Specialization: " << plan.specializationFingerprint << "\n";
  output << "#include \"gfsim/bits.h\"\n"
            "#include \"gfsim/dispatch.h\"\n"
            "#include \"gfsim/object.h\"\n"
            "#include \"gfsim/count_zeros.h\"\n"
            "#include \"gfsim/popcount.h\"\n"
            "#include \"gfsim/priority_encode.h\"\n"
            "#include \"gfsim/queue.h\"\n"
            "#include \"gfsim/queue_blocks.h\"\n\n"
            "#include <array>\n#include <cstdint>\n#include <limits>\n"
            "#include <optional>\n#include <tuple>\n\n"
            "namespace ac_generated {\n\n";
  for (const QueueEnumPlan &enumeration : plan.enums) {
    output << "enum class " << enumeration.name << " : "
           << enumStorage(enumeration.width).str() << " {\n";
    for (auto [index, enumerant] : llvm::enumerate(enumeration.enumerants))
      output << "  " << enumerant << " = " << index << ",\n";
    output << "};\n\n";
  }
  auto payloadOrder = payloadEmissionOrder(plan);
  if (!payloadOrder)
    return payloadOrder.takeError();
  for (const QueuePayloadPlan *payload : *payloadOrder) {
    output << "struct " << payload->name << " {\n";
    for (const QueuePayloadFieldPlan &field : payload->fields) {
      auto type = cppPayloadFieldType(plan, field);
      if (!type)
        return type.takeError();
      output << "  " << *type << ' ' << field.name << "{};\n";
    }
    output << "  bool operator==(const " << payload->name
           << " &) const = default;\n";
    output << "};\n\n";
  }

  for (const TableMatchPlan &match : plan.tableMatches) {
    const TablePlan *table = findTable(plan, match.table);
    auto entryType = table ? cppType(table->entryType)
                           : llvm::Expected<std::string>(
                                 generatorError("table.match Table missing"));
    if (!entryType)
      return entryType.takeError();
    QueueBlockPlan predicate;
    predicate.expressions = match.expressions;
    predicate.yields = {match.yield};
    auto body = emitExpressionBody(plan, predicate, match.yield, 4);
    if (!body)
      return body.takeError();
    output << "struct " << identifier(match.name) << "_predicate_policy {\n"
           << "  gfsim::SimTable<" << *entryType << "> *table{};\n";
    for (const SlotPlan &slot : plan.slots) {
      auto type = cppType(slot.payloadType);
      if (!type)
        return type.takeError();
      output << "  gfsim::SlotState<" << *type << "> *slot_"
             << identifier(slot.name) << "{};\n";
    }
    output << "  bool operator()(const " << *entryType << " &item) const {\n"
           << *body << "  }\n};\n"
           << "using " << identifier(match.name) << "_cache = "
           << "gfsim::TableMatchCache<" << *entryType << ", "
           << identifier(match.name) << "_predicate_policy>;\n\n";
  }
  for (const TableSelectionPlan &selection : plan.tableSelections) {
    const TablePlan *table = findTable(plan, selection.table);
    auto entryType = table ? cppType(table->entryType)
                           : llvm::Expected<std::string>(
                                 generatorError("table.choose Table missing"));
    if (!entryType)
      return entryType.takeError();
    output << "struct " << identifier(selection.name) << "_mask_policy {\n"
           << "  " << identifier(selection.match) << "_cache *match{};\n"
           << "  std::uint64_t operator()(gfsim::Epoch epoch) const {\n"
           << "    return match->get(epoch);\n  }\n};\n";
    output << "struct " << identifier(selection.name) << "_key_policy {\n"
           << "  std::uint64_t operator()(const " << *entryType
           << " &item) const {\n";
    if (selection.policy == "first") {
      output << "    return 0;\n";
    } else {
      QueueBlockPlan key;
      key.expressions = selection.keyExpressions;
      key.yields = {selection.keyYield};
      output << "    return static_cast<std::uint64_t>([&]() {\n";
      auto body = emitExpressionBody(plan, key, selection.keyYield, 6);
      if (!body)
        return body.takeError();
      output << *body;
      output << "    }());\n";
    }
    output << "  }\n};\n"
           << "using " << identifier(selection.name) << "_cache = "
           << "gfsim::TableSelectionCache<" << *entryType << ", "
           << identifier(selection.name) << "_mask_policy, "
           << identifier(selection.name) << "_key_policy>;\n"
           << "inline constexpr auto " << identifier(selection.name)
           << "_choose_policy = gfsim::TableChoosePolicy::"
           << (selection.policy == "first" ? "First"
               : selection.policy == "min" ? "Min"
                                           : "Max")
           << ";\n\n";
  }

  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    if (block->kind != "transform" && block->kind != "route" &&
        block->kind != "select" && block->kind != "expect" &&
        block->kind != "dependency" && block->kind != "credit" &&
        block->kind != "reorder" && block->kind != "feedback" &&
        block->kind != "table_read" && block->kind != "table_write" &&
        block->kind != "table_masked_write" && block->kind != "firing" &&
        block->kind != "slot")
      continue;
    if (block->kind == "slot") {
      const SlotPlan *slot = findSlot(plan, block->slot);
      auto payloadType = slot ? cppType(slot->payloadType)
                              : llvm::Expected<std::string>(
                                    generatorError("slot declaration missing"));
      if (!payloadType)
        return payloadType.takeError();
      output << "struct block_" << index << "_release_policy {\n";
      for (const SlotPlan &candidate : plan.slots) {
        auto type = cppType(candidate.payloadType);
        if (!type)
          return type.takeError();
        output << "  gfsim::SlotState<" << *type << "> *slot_"
               << identifier(candidate.name) << "{};\n";
      }
      for (const TablePlan &table : plan.tables) {
        if (!referencesTable(block->expressions, table.name))
          continue;
        auto type = cppType(table.entryType);
        if (!type)
          return type.takeError();
        output << "  gfsim::SimTable<" << *type << "> *table_"
               << identifier(table.name) << "{};\n";
      }
      for (const TableMatchPlan &match : plan.tableMatches)
        output << "  " << identifier(match.name) << "_cache *"
               << identifier(match.name) << "{};\n";
      for (const TableSelectionPlan &selection : plan.tableSelections)
        output << "  " << identifier(selection.name) << "_cache *"
               << identifier(selection.name) << "{};\n";
      output << "  bool operator()(gfsim::Epoch epoch) const {\n";
      auto body =
          emitExpressionBody(plan, *block, block->yields.front(), 4, true);
      if (!body)
        return body.takeError();
      output << *body << "  }\n};\n\n";
      continue;
    }
    if (block->kind == "firing") {
      const std::vector<const TablePlan *> ownerTables =
          stateOwnerTables(plan, *block);
      if (ownerTables.size() > 1) {
        const std::vector<const TablePlan *> readTables =
            readOnlyTables(plan, *block);
        std::vector<std::string> tableTypes;
        for (const TablePlan *table : ownerTables) {
          auto type = table ? cppType(table->entryType)
                            : llvm::Expected<std::string>(generatorError(
                                  "state firing owner Table missing"));
          if (!type)
            return type.takeError();
          tableTypes.push_back(std::move(*type));
        }
        llvm::StringMap<size_t> writeOrdinals;
        for (auto [writeIndex, write] : llvm::enumerate(block->stateWrites))
          writeOrdinals[write.table] = writeIndex;
        std::vector<std::string> inputTypes;
        for (const std::string &inputName : block->inputs) {
          const QueuePlan *input = findQueue(plan, inputName);
          auto type = input ? cppType(input->payloadType)
                            : llvm::Expected<std::string>(
                                  generatorError("state firing input missing"));
          if (!type)
            return type.takeError();
          inputTypes.push_back(std::move(*type));
        }
        std::vector<std::string> outputTypes;
        for (const std::string &outputName : block->outputs) {
          const QueuePlan *result = findQueue(plan, outputName);
          auto type = result ? cppType(result->payloadType)
                             : llvm::Expected<std::string>(generatorError(
                                   "state firing output missing"));
          if (!type)
            return type.takeError();
          outputTypes.push_back(std::move(*type));
        }
        QueueBlockPlan evaluation = *block;
        std::vector<std::string> additional{block->guard};
        std::string tupleResult = "std::tuple{";
        for (auto [writeIndex, write] : llvm::enumerate(block->stateWrites)) {
          if (writeIndex)
            tupleResult.append(", ");
          tupleResult.append(write.index)
              .append(", ")
              .append(write.value)
              .append(", ")
              .append(write.present);
          additional.push_back(write.index);
          additional.push_back(write.value);
          additional.push_back(write.present);
        }
        for (auto [outputIndex, yield] : llvm::enumerate(block->yields)) {
          const std::string &present =
              block->outputPresence[outputIndex].present;
          tupleResult.append(", ").append(yield).append(", ").append(present);
          additional.push_back(yield);
          additional.push_back(present);
        }
        for (auto [ownerIndex, table] : llvm::enumerate(ownerTables)) {
          size_t reservationIndex = 0;
          for (const StateReservationPlan *reservation :
               findStateReservations(*block, table->name)) {
            if (reservation->indexKind == "all") {
              ++reservationIndex;
              continue;
            }
            if (reservation->indexKind == "set") {
              const std::string result = "snapshot_set_" +
                                         std::to_string(ownerIndex) + "_" +
                                         std::to_string(reservationIndex++);
              QueueExpressionPlan expression{result, "snapshot_set", "i64", {}};
              expression.field = reservation->source;
              expression.table = reservation->table;
              evaluation.expressions.push_back(std::move(expression));
              tupleResult.append(", ").append(result);
              additional.push_back(result);
              continue;
            }
            ++reservationIndex;
            tupleResult.append(", ").append(reservation->index);
            additional.push_back(reservation->index);
          }
        }
        tupleResult.append(", ").append(block->guard).push_back('}');
        auto evaluationBody = emitExpressionBody(
            plan, evaluation, block->stateWrites.front().index, 6, true, false,
            additional, tupleResult);
        if (!evaluationBody)
          return evaluationBody.takeError();

        std::string tableTuple = "std::tuple<";
        for (auto [typeIndex, type] : llvm::enumerate(tableTypes)) {
          if (typeIndex)
            tableTuple.append(", ");
          tableTuple.append(type);
        }
        tableTuple.push_back('>');
        std::string outputTuple = "std::tuple<";
        for (auto [typeIndex, type] : llvm::enumerate(outputTypes)) {
          if (typeIndex)
            outputTuple.append(", ");
          outputTuple.append(type);
        }
        outputTuple.push_back('>');
        const std::string planType = "gfsim::StateTransitionPlan<" +
                                     tableTuple + ", " + outputTuple + ">";
        output << "struct block_" << index << "_policy {\n";
        for (const TablePlan *readTable : readTables) {
          auto readType = cppType(readTable->entryType);
          if (!readType)
            return readType.takeError();
          output << "  const gfsim::SimTable<" << *readType << "> *read_table_"
                 << identifier(readTable->name) << "{};\n";
        }
        output << "  std::optional<" << planType
               << "> operator()(gfsim::Epoch epoch, std::tuple<";
        for (auto [typeIndex, type] : llvm::enumerate(tableTypes)) {
          if (typeIndex)
            output << ", ";
          output << "const gfsim::SimTable<" << type << "> *";
        }
        output << "> table_refs";
        for (auto [inputIndex, type] : llvm::enumerate(inputTypes)) {
          output << ", const " << type << " &item";
          if (inputIndex)
            output << inputIndex;
        }
        output << ") const {\n";
        for (auto [ownerIndex, table] : llvm::enumerate(ownerTables))
          output << "    const auto *table_" << identifier(table->name)
                 << " = std::get<" << ownerIndex << ">(table_refs);\n";
        for (const TablePlan *readTable : readTables)
          output << "    const auto *table_" << identifier(readTable->name)
                 << " = read_table_" << identifier(readTable->name) << ";\n";
        output << "    auto [";
        for (size_t writeIndex = 0; writeIndex < block->stateWrites.size();
             ++writeIndex) {
          if (writeIndex)
            output << ", ";
          output << "proposal_index" << writeIndex << ", proposal_value"
                 << writeIndex << ", proposal_present" << writeIndex;
        }
        for (size_t outputIndex = 0; outputIndex < outputTypes.size();
             ++outputIndex)
          output << ", output_value" << outputIndex << ", output_present"
                 << outputIndex;
        for (auto [ownerIndex, table] : llvm::enumerate(ownerTables)) {
          size_t reservationIndex = 0;
          for (const StateReservationPlan *reservation :
               findStateReservations(*block, table->name)) {
            if (reservation->indexKind == "all") {
              ++reservationIndex;
              continue;
            }
            output << ", "
                   << (reservation->indexKind == "set" ? "snapshot_set_"
                                                       : "reservation_index")
                   << ownerIndex << '_' << reservationIndex++;
          }
        }
        output << ", condition] = [&]() {\n"
               << *evaluationBody << "    }();\n"
               << "    if (!condition)\n"
               << "      return std::nullopt;\n"
               << "    return " << planType << "{{";
        for (auto [ownerIndex, type] : llvm::enumerate(tableTypes)) {
          if (ownerIndex)
            output << ", ";
          auto ordinal = writeOrdinals.find(ownerTables[ownerIndex]->name);
          if (ordinal == writeOrdinals.end()) {
            output << "std::optional<std::pair<size_t, " << type << ">>{}";
            continue;
          }
          const size_t writeIndex = ordinal->getValue();
          output << "proposal_present" << writeIndex
                 << " ? std::optional<std::pair<size_t, " << type
                 << ">>{std::in_place, static_cast<size_t>(proposal_index"
                 << writeIndex << "), proposal_value" << writeIndex
                 << "} : std::optional<std::pair<size_t, " << type << ">>{}";
        }
        output << "}, {";
        for (auto [outputIndex, type] : llvm::enumerate(outputTypes)) {
          if (outputIndex)
            output << ", ";
          output << "output_present" << outputIndex << " ? std::optional<"
                 << type << ">{output_value" << outputIndex
                 << "} : std::optional<" << type << ">{}";
        }
        output << "}, {";
        for (size_t ownerIndex = 0; ownerIndex < tableTypes.size();
             ++ownerIndex) {
          if (ownerIndex)
            output << ", ";
          const TablePlan &table = *ownerTables[ownerIndex];
          output << "gfsim::StateReservation{}";
          size_t reservationIndex = 0;
          for (const StateReservationPlan *reservation :
               findStateReservations(*block, table.name)) {
            auto fieldMask =
                reservationFieldMask(plan, table, reservation->fields);
            if (!fieldMask)
              return fieldMask.takeError();
            output << " | "
                   << (fieldMask->complete
                           ? "gfsim::StateReservation("
                           : "gfsim::StateReservation::forFields(");
            if (reservation->indexKind == "all") {
              output << (table.entries == 64
                             ? "~std::uint64_t{0}"
                             : "((std::uint64_t{1} << " +
                                   std::to_string(table.entries) + ") - 1)");
              ++reservationIndex;
            } else if (reservation->indexKind == "set") {
              output << "snapshot_set_" << ownerIndex << '_'
                     << reservationIndex++;
            } else {
              output << "(std::uint64_t{1} << static_cast<std::size_t>("
                     << "reservation_index" << ownerIndex << '_'
                     << reservationIndex++ << "))";
            }
            if (fieldMask->complete)
              output << ")";
            else
              output << ", std::uint64_t{" << fieldMask->mask << "}, "
                     << fieldMask->count << ")";
          }
        }
        output << "}};\n  }\n};\n\n";

        for (auto [ownerIndex, table] : llvm::enumerate(ownerTables)) {
          const std::string &entryType = tableTypes[ownerIndex];
          const StateWritePlan *write = findStateWrite(*block, table->name);
          const std::vector<std::string> fields =
              write ? write->fields : std::vector<std::string>{"$entry"};
          output << "struct block_" << index << "_merge_policy_" << ownerIndex
                 << " {\n  static constexpr std::array<size_t, "
                 << fields.size() << "> fields{";
          for (auto [fieldIndex, field] : llvm::enumerate(fields)) {
            if (fieldIndex)
              output << ", ";
            if (field == "$entry") {
              output << 0;
              continue;
            }
            auto payload = llvm::find_if(
                plan.payloads, [&](const QueuePayloadPlan &candidate) {
                  return candidate.name == entryType;
                });
            if (payload == plan.payloads.end())
              return generatorError("state firing Entry payload is missing");
            auto declared = llvm::find_if(
                payload->fields, [&](const QueuePayloadFieldPlan &candidate) {
                  return candidate.name == field;
                });
            if (declared == payload->fields.end())
              return generatorError("state firing write field is missing");
            output << std::distance(payload->fields.begin(), declared);
          }
          output << "};\n  void operator()(" << entryType << " &target, const "
                 << entryType << " &value) const {\n";
          for (const std::string &field : fields)
            if (field == "$entry")
              output << "    target = value;\n";
            else
              output << "    target." << field << " = value." << field << ";\n";
          output << "  }\n};\n\n";
        }
        continue;
      }
      const TablePlan *table = findTable(plan, block->table);
      auto entryType = table ? cppType(table->entryType)
                             : llvm::Expected<std::string>(generatorError(
                                   "table firing Table missing"));
      if (!entryType)
        return entryType.takeError();
      const std::vector<const TablePlan *> readTables =
          readOnlyTables(plan, *block);
      std::vector<std::string> inputTypes;
      for (const std::string &inputName : block->inputs) {
        const QueuePlan *input = findQueue(plan, inputName);
        auto inputType = input ? cppType(input->payloadType)
                               : llvm::Expected<std::string>(generatorError(
                                     "table firing input missing"));
        if (!inputType)
          return inputType.takeError();
        inputTypes.push_back(std::move(*inputType));
      }
      std::vector<std::string> outputTypes;
      for (const std::string &outputName : block->outputs) {
        const QueuePlan *result = findQueue(plan, outputName);
        auto outputType = result ? cppType(result->payloadType)
                                 : llvm::Expected<std::string>(generatorError(
                                       "table firing output missing"));
        if (!outputType)
          return outputType.takeError();
        outputTypes.push_back(std::move(*outputType));
      }
      const std::string &writePresent = block->stateWrites.front().present;
      QueueBlockPlan evaluation = *block;
      std::vector<std::string> additional{block->tableValue, writePresent,
                                          block->guard};
      std::string tupleResult = "std::tuple{" + block->tableIndex + ", " +
                                block->tableValue + ", " + writePresent;
      for (auto [outputIndex, yield] : llvm::enumerate(block->yields)) {
        const std::string &present = block->outputPresence[outputIndex].present;
        tupleResult.append(", ").append(yield).append(", ").append(present);
        additional.push_back(yield);
        additional.push_back(present);
      }
      size_t snapshotOrdinal = 0;
      for (const StateReservationPlan *reservation :
           findStateReservations(*block, block->table)) {
        if (reservation->indexKind == "all") {
          ++snapshotOrdinal;
          continue;
        }
        if (reservation->indexKind == "set") {
          const std::string result =
              "snapshot_set_0_" + std::to_string(snapshotOrdinal++);
          QueueExpressionPlan expression{result, "snapshot_set", "i64", {}};
          expression.field = reservation->source;
          expression.table = reservation->table;
          evaluation.expressions.push_back(std::move(expression));
          tupleResult.append(", ").append(result);
          additional.push_back(result);
          continue;
        }
        ++snapshotOrdinal;
        tupleResult.append(", ").append(reservation->index);
        additional.push_back(reservation->index);
      }
      tupleResult.append(", ").append(block->guard);
      tupleResult.push_back('}');
      auto evaluationBody =
          emitExpressionBody(plan, evaluation, block->tableIndex, 6, true,
                             false, additional, tupleResult);
      if (!evaluationBody)
        return evaluationBody.takeError();
      std::string planType = "gfsim::TableTransitionPlan<" + *entryType;
      for (const std::string &outputType : outputTypes)
        planType.append(", ").append(outputType);
      planType.push_back('>');
      output << "struct block_" << index << "_policy {\n";
      for (const TablePlan *readTable : readTables) {
        auto readType = cppType(readTable->entryType);
        if (!readType)
          return readType.takeError();
        output << "  const gfsim::SimTable<" << *readType << "> *read_table_"
               << identifier(readTable->name) << "{};\n";
      }
      output << "  std::optional<" << planType
             << "> operator()(gfsim::Epoch epoch, "
             << "const gfsim::SimTable<" << *entryType << "> &table_ref";
      for (auto [inputIndex, inputType] : llvm::enumerate(inputTypes)) {
        output << ", const " << inputType << " &item";
        if (inputIndex)
          output << inputIndex;
      }
      output << ") const {\n"
             << "    const auto *table_" << identifier(block->table)
             << " = &table_ref;\n";
      for (const TablePlan *readTable : readTables)
        output << "    const auto *table_" << identifier(readTable->name)
               << " = read_table_" << identifier(readTable->name) << ";\n";
      output << "    auto [proposal_index, proposal_value, proposal_present";
      for (size_t outputIndex = 0; outputIndex < outputTypes.size();
           ++outputIndex)
        output << ", output_value" << outputIndex << ", output_present"
               << outputIndex;
      size_t reservationIndex = 0;
      for (const StateReservationPlan *reservation :
           findStateReservations(*block, block->table)) {
        if (reservation->indexKind == "all") {
          ++reservationIndex;
          continue;
        }
        output << ", "
               << (reservation->indexKind == "set" ? "snapshot_set_0_"
                                                   : "reservation_index")
               << reservationIndex++;
      }
      output << ", condition] = [&]() {\n"
             << *evaluationBody << "    }();\n"
             << "    if (!condition)\n"
             << "      return std::nullopt;\n"
             << "    return " << planType
             << "{proposal_present ? std::vector<std::pair<size_t, "
             << *entryType
             << ">>{{{static_cast<size_t>(proposal_index), proposal_value}}} "
                ": std::vector<std::pair<size_t, "
             << *entryType << ">>{}, {";
      for (auto [outputIndex, outputType] : llvm::enumerate(outputTypes)) {
        if (outputIndex)
          output << ", ";
        output << "output_present" << outputIndex << " ? std::optional<"
               << outputType << ">{output_value" << outputIndex
               << "} : std::optional<" << outputType << ">{}";
      }
      output << "}, gfsim::StateReservation{}";
      reservationIndex = 0;
      for (const StateReservationPlan *reservation :
           findStateReservations(*block, block->table)) {
        auto fieldMask =
            reservationFieldMask(plan, *table, reservation->fields);
        if (!fieldMask)
          return fieldMask.takeError();
        output << " | "
               << (fieldMask->complete ? "gfsim::StateReservation("
                                       : "gfsim::StateReservation::forFields(");
        if (reservation->indexKind == "all") {
          output << (table->entries == 64
                         ? "~std::uint64_t{0}"
                         : "((std::uint64_t{1} << " +
                               std::to_string(table->entries) + ") - 1)");
          ++reservationIndex;
        } else if (reservation->indexKind == "set") {
          output << "snapshot_set_0_" << reservationIndex++;
        } else {
          output << "(std::uint64_t{1} << static_cast<std::size_t>("
                 << "reservation_index" << reservationIndex++ << "))";
        }
        if (fieldMask->complete)
          output << ")";
        else
          output << ", std::uint64_t{" << fieldMask->mask << "}, "
                 << fieldMask->count << ")";
      }
      output << "};\n  }\n};\n\n";
      output << "struct block_" << index
             << "_merge_policy {\n  static constexpr std::array<size_t, "
             << block->writeFields.size() << "> fields{";
      for (auto [fieldIndex, field] : llvm::enumerate(block->writeFields)) {
        if (fieldIndex)
          output << ", ";
        if (field == "$entry") {
          output << 0;
          continue;
        }
        auto payload = llvm::find_if(plan.payloads,
                                     [&](const QueuePayloadPlan &candidate) {
                                       return candidate.name == *entryType;
                                     });
        if (payload == plan.payloads.end())
          return generatorError("table firing Entry payload is missing");
        auto declared = llvm::find_if(
            payload->fields, [&](const QueuePayloadFieldPlan &candidate) {
              return candidate.name == field;
            });
        if (declared == payload->fields.end())
          return generatorError("table firing write field is missing");
        output << std::distance(payload->fields.begin(), declared);
      }
      output << "};\n  void operator()(" << *entryType << " &target, const "
             << *entryType << " &value) const {\n";
      for (const std::string &field : block->writeFields) {
        if (field == "$entry")
          output << "    target = value;\n";
        else
          output << "    target." << field << " = value." << field << ";\n";
      }
      output << "  }\n};\n\n";
      continue;
    }
    if (block->kind == "table_read" || block->kind == "table_write" ||
        block->kind == "table_masked_write") {
      const TablePlan *table = findTable(plan, block->table);
      auto entryType = table ? cppType(table->entryType)
                             : llvm::Expected<std::string>(
                                   generatorError("table declaration missing"));
      if (!entryType)
        return entryType.takeError();
      std::string inputType;
      if (!block->inputs.empty()) {
        const QueuePlan *input = findQueue(plan, block->inputs.front());
        auto type = input ? cppType(input->payloadType)
                          : llvm::Expected<std::string>(
                                generatorError("table input Queue missing"));
        if (!type)
          return type.takeError();
        inputType = std::move(*type);
      }
      const std::vector<llvm::StringRef> policyNames =
          block->kind == "table_read"
              ? std::vector<llvm::StringRef>{"address", "when"}
          : block->kind == "table_masked_write"
              ? std::vector<llvm::StringRef>{"mask", "enable", "value"}
              : std::vector<llvm::StringRef>{"address", "enable", "value"};
      for (auto [policyIndex, policyName] : llvm::enumerate(policyNames)) {
        llvm::StringRef resultType = table->entryType;
        if (block->yields[policyIndex] != "item") {
          auto expression = std::find_if(
              block->expressions.begin(), block->expressions.end(),
              [&](const QueueExpressionPlan &candidate) {
                return candidate.result == block->yields[policyIndex];
              });
          if (expression == block->expressions.end())
            return generatorError("table policy yield type is missing");
          resultType = expression->type;
        } else if (!block->inputs.empty()) {
          const QueuePlan *input = findQueue(plan, block->inputs.front());
          resultType = input->payloadType;
        }
        auto resultCppType = cppType(resultType);
        if (!resultCppType)
          return resultCppType.takeError();
        output << "struct block_" << index << '_' << policyName.str()
               << "_policy {\n  gfsim::SimTable<" << *entryType
               << "> *table{};\n";
        for (const SlotPlan &slot : plan.slots) {
          auto type = cppType(slot.payloadType);
          if (!type)
            return type.takeError();
          output << "  gfsim::SlotState<" << *type << "> *slot_"
                 << identifier(slot.name) << "{};\n";
        }
        for (const TableMatchPlan &match : plan.tableMatches)
          output << "  " << identifier(match.name) << "_cache *"
                 << identifier(match.name) << "{};\n";
        for (const TableSelectionPlan &selection : plan.tableSelections)
          output << "  " << identifier(selection.name) << "_cache *"
                 << identifier(selection.name) << "{};\n";
        output << "  " << *resultCppType << " operator()(gfsim::Epoch epoch";
        if (!inputType.empty())
          output << ", const " << inputType << " &item";
        else if (block->kind == "table_masked_write" && policyName == "value")
          output << ", const " << *entryType << " &item";
        output << ") const {\n";
        auto body =
            emitExpressionBody(plan, *block, block->yields[policyIndex], 4);
        if (!body)
          return body.takeError();
        output << *body << "  }\n};\n\n";
      }
      if (block->kind == "table_write" || block->kind == "table_masked_write") {
        output << "struct block_" << index
               << "_merge_policy {\n  static constexpr std::array<size_t, "
               << block->writeFields.size() << "> fields{";
        for (auto [fieldIndex, field] : llvm::enumerate(block->writeFields)) {
          if (fieldIndex)
            output << ", ";
          if (field == "$entry") {
            output << 0;
            continue;
          }
          auto payload = llvm::find_if(plan.payloads,
                                       [&](const QueuePayloadPlan &candidate) {
                                         return candidate.name == *entryType;
                                       });
          if (payload == plan.payloads.end())
            return generatorError("table Entry payload is missing");
          auto declared = llvm::find_if(
              payload->fields, [&](const QueuePayloadFieldPlan &candidate) {
                return candidate.name == field;
              });
          if (declared == payload->fields.end())
            return generatorError("table write field is missing");
          output << std::distance(payload->fields.begin(), declared);
        }
        output << "};\n  void operator()(" << *entryType << " &target, const "
               << *entryType << " &value) const {\n";
        for (const std::string &field : block->writeFields) {
          if (field == "$entry")
            output << "    target = value;\n";
          else
            output << "    target." << field << " = value." << field << ";\n";
        }
        output << "  }\n};\n\n";
      }
      continue;
    }
    if (block->kind == "dependency") {
      const QueuePlan *input = findQueue(plan, block->inputs.front());
      if (!input)
        return generatorError("dependency input Queue is missing");
      auto inputType = cppType(input->payloadType);
      if (!inputType)
        return inputType.takeError();
      constexpr llvm::StringLiteral policyNames[] = {"key", "dependency",
                                                     "resource", "cost"};
      for (auto [policyIndex, policyName] : llvm::enumerate(policyNames)) {
        llvm::StringRef resultType = input->payloadType;
        if (block->yields[policyIndex] != "item") {
          auto expression = std::find_if(
              block->expressions.begin(), block->expressions.end(),
              [&](const QueueExpressionPlan &candidate) {
                return candidate.result == block->yields[policyIndex];
              });
          if (expression == block->expressions.end())
            return generatorError("dependency policy yield type is missing");
          resultType = expression->type;
        }
        auto resultCppType = cppType(resultType);
        if (!resultCppType)
          return resultCppType.takeError();
        output << "struct block_" << index << '_' << policyName.str()
               << "_policy {\n  " << *resultCppType << " operator()(const "
               << *inputType << " &item) const {\n";
        auto body =
            emitExpressionBody(plan, *block, block->yields[policyIndex], 4);
        if (!body)
          return body.takeError();
        output << *body << "  }\n};\n\n";
      }
      continue;
    }
    if (block->kind == "transform" &&
        (block->inputs.size() != 1 || block->outputs.size() != 1)) {
      if (block->inputs.empty() || block->outputs.empty() ||
          block->outputs.size() != block->yields.size())
        return generatorError("atomic transform arity is inconsistent");
      std::vector<std::string> inputTypes;
      std::vector<std::string> outputTypes;
      for (const std::string &inputName : block->inputs) {
        const QueuePlan *input = findQueue(plan, inputName);
        if (!input)
          return generatorError("atomic transform input Queue is missing");
        auto type = cppType(input->payloadType);
        if (!type)
          return type.takeError();
        inputTypes.push_back(std::move(*type));
      }
      for (const std::string &outputName : block->outputs) {
        const QueuePlan *result = findQueue(plan, outputName);
        if (!result)
          return generatorError("atomic transform output Queue is missing");
        auto type = cppType(result->payloadType);
        if (!type)
          return type.takeError();
        outputTypes.push_back(std::move(*type));
      }
      output << "struct block_" << index << "_policy {\n  std::tuple<";
      for (auto [typeIndex, type] : llvm::enumerate(outputTypes)) {
        if (typeIndex)
          output << ", ";
        output << type;
      }
      output << "> operator()(";
      for (auto [typeIndex, type] : llvm::enumerate(inputTypes)) {
        if (typeIndex)
          output << ", ";
        output << "const " << type << " &item";
        if (typeIndex)
          output << typeIndex;
      }
      output << ") const {\n    return {\n";
      for (auto [yieldIndex, yield] : llvm::enumerate(block->yields)) {
        output << "      [&]() -> " << outputTypes[yieldIndex] << " {\n";
        auto body = emitExpressionBody(plan, *block, yield, 8);
        if (!body)
          return body.takeError();
        output << *body << "      }()"
               << (yieldIndex + 1 == block->yields.size() ? "\n" : ",\n");
      }
      output << "    };\n  }\n};\n\n";
      continue;
    }
    const size_t expectedYields = block->kind == "feedback" ? 2 : 1;
    if (block->yields.size() != expectedYields ||
        (block->kind != "select" && block->inputs.size() != 1))
      return generatorError("Queue policy arity is unsupported");
    const QueuePlan *input = findQueue(plan, block->inputs.front());
    if (!input)
      return generatorError("policy input Queue is missing");
    auto inputType = cppType(input->payloadType);
    if (!inputType)
      return inputType.takeError();
    std::string policy =
        "block_" + std::to_string(index) +
        (block->kind == "feedback" ? "_update_policy" : "_policy");
    output << "struct " << policy << " {\n  ";
    if (block->kind == "route" || block->kind == "select")
      output << "size_t";
    else if (block->kind == "expect")
      output << "bool";
    else if (block->kind == "reorder" || block->kind == "credit") {
      llvm::StringRef keyType = input->payloadType;
      if (block->yields.front() != "item") {
        auto expression =
            std::find_if(block->expressions.begin(), block->expressions.end(),
                         [&](const QueueExpressionPlan &candidate) {
                           return candidate.result == block->yields.front();
                         });
        if (expression == block->expressions.end())
          return generatorError(block->kind + " yield type is missing");
        keyType = expression->type;
      }
      auto keyCppType = cppType(keyType);
      if (!keyCppType)
        return keyCppType.takeError();
      output << *keyCppType;
    } else {
      const QueuePlan *result = findQueue(plan, block->outputs.front());
      if (!result)
        return generatorError("transform output Queue is missing");
      auto resultType = cppType(result->payloadType);
      if (!resultType)
        return resultType.takeError();
      output << *resultType;
    }
    output << " operator()(const " << *inputType << " &item) const {\n";
    auto body = emitExpressionBody(plan, *block, block->yields.front(), 4);
    if (!body)
      return body.takeError();
    if (block->kind == "route" || block->kind == "select")
      output << "    return static_cast<size_t>([&]() {\n"
             << *body << "    }());\n";
    else
      output << *body;
    output << "  }\n};\n\n";
    if (block->kind == "feedback") {
      output << "struct block_" << index
             << "_condition_policy {\n  bool operator()(const " << *inputType
             << " &item) const {\n";
      auto condition = emitExpressionBody(plan, *block, block->yields[1], 4);
      if (!condition)
        return condition.takeError();
      output << *condition << "  }\n};\n\n";
    }
  }

  for (auto [memoryIndex, instance] : llvm::enumerate(plan.memoryInstances)) {
    auto found = memoryEndpoints.find(instance.name);
    if (found == memoryEndpoints.end() || found->getValue().empty())
      return generatorError("memory instance has no endpoints");
    const auto &endpoints = found->getValue();
    const QueuePlan *input = findQueue(plan, endpoints.front()->inputs.front());
    if (!input)
      return generatorError("memory endpoint input Queue is missing");
    auto inputType = cppType(input->payloadType);
    auto dataType = cppType(instance.dataType);
    if (!inputType)
      return inputType.takeError();
    if (!dataType)
      return dataType.takeError();
    constexpr llvm::StringLiteral policyNames[] = {"address", "write", "data"};
    const std::array<std::string, 3> resultTypes = {"std::uint64_t", "bool",
                                                    *dataType};
    for (auto [policyIndex, policyName] : llvm::enumerate(policyNames)) {
      output << "struct memory_" << memoryIndex << '_' << policyName.str()
             << "_policy {\n  " << resultTypes[policyIndex]
             << " operator()(size_t endpoint, const " << *inputType
             << " &item) const {\n    switch (endpoint) {\n";
      for (const QueueBlockPlan *endpoint : endpoints) {
        output << "    case " << endpoint->endpointOrdinal << ": {\n";
        if (policyIndex == 0)
          output << "      return static_cast<std::uint64_t>([&]() {\n";
        auto body =
            emitExpressionBody(plan, *endpoint, endpoint->yields[policyIndex],
                               policyIndex == 0 ? 8 : 6);
        if (!body)
          return body.takeError();
        output << *body;
        if (policyIndex == 0)
          output << "      }());\n";
        output << "    }\n";
      }
      output << "    default: return {};\n    }\n  }\n};\n\n";
    }
    output << "struct memory_" << memoryIndex << "_response_policy {\n  "
           << *inputType << " operator()(size_t endpoint, const " << *inputType
           << " &item, const " << *dataType
           << " &old_data) const {\n    auto result = item;\n"
              "    switch (endpoint) {\n";
    for (const QueueBlockPlan *endpoint : endpoints)
      output << "    case " << endpoint->endpointOrdinal << ": result."
             << endpoint->resultField << " = old_data; break;\n";
    output << "    default: break;\n    }\n    return result;\n  }\n};\n\n";
  }

  std::string modelClass = className(plan.system);
  output << "class " << modelClass
         << " final : public gfsim::Module {\npublic:\n  " << modelClass
         << "() : gfsim::Module(\"" << plan.system
         << "\", gfsim::kInvalidObjectId, nullptr),\n";
  std::vector<std::string> initializers;
  for (const std::string &scope : plan.scopes) {
    llvm::StringRef parent = llvm::StringRef(scope).rsplit('/').first;
    if (parent.empty())
      parent = "/";
    auto parentPointer = modulePointer(parent);
    if (!parentPointer)
      return parentPointer.takeError();
    appendInitializer(initializers, scopeMembers[scope], "(\"",
                      pathParts(scope).back(), "\", gfsim::kInvalidObjectId, ",
                      *parentPointer, ")");
  }
  for (const QueuePlan &queue : plan.queues) {
    auto type = cppType(queue.payloadType);
    auto parent = modulePointer(queueOwners[queue.name]);
    if (!type)
      return type.takeError();
    if (!parent)
      return parent.takeError();
    appendInitializer(initializers, queueMembers[queue.name], "(\"", queue.name,
                      "\", ", queueIds[queue.name], ", ", *parent, ", ",
                      queue.depth,
                      ", std::numeric_limits<size_t>::max(), nullptr, ",
                      queue.latency, ", ", queue.rate, ")");
  }
  for (const TablePlan &table : plan.tables) {
    auto parent = modulePointer(table.ownerPath);
    if (!parent)
      return parent.takeError();
    appendInitializer(initializers, tableMembers[table.name], "(\"", table.name,
                      "\", ", tableIds[table.name], ", ", *parent, ", ",
                      table.entries, ")");
  }
  std::string slotPolicyPointers;
  for (auto [index, slot] : llvm::enumerate(plan.slots)) {
    (void)slot;
    slotPolicyPointers.append(", &slot_")
        .append(std::to_string(index))
        .append("_state_");
  }
  std::string sharedPolicyPointers;
  for (const TableMatchPlan &match : plan.tableMatches)
    sharedPolicyPointers.append(", &")
        .append(identifier(match.name))
        .append("_");
  for (const TableSelectionPlan &selection : plan.tableSelections)
    sharedPolicyPointers.append(", &")
        .append(identifier(selection.name))
        .append("_");
  for (const TableMatchPlan &match : plan.tableMatches) {
    auto table = tableMembers.find(match.table);
    if (table == tableMembers.end())
      return generatorError("table.match declaration is missing");
    appendInitializer(initializers, identifier(match.name), "_(",
                      table->getValue(), ", ", identifier(match.name),
                      "_predicate_policy{&", table->getValue(),
                      slotPolicyPointers, "})");
  }
  for (const TableSelectionPlan &selection : plan.tableSelections) {
    auto table = tableMembers.find(selection.table);
    if (table == tableMembers.end())
      return generatorError("table.choose declaration is missing");
    appendInitializer(initializers, identifier(selection.name), "_(",
                      table->getValue(), ", ", identifier(selection.name),
                      "_mask_policy{&", identifier(selection.match), "_}, ",
                      identifier(selection.name), "_key_policy{}, ",
                      identifier(selection.name), "_choose_policy)");
  }
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    auto state = feedbackStateIds.find(index);
    if (state == feedbackStateIds.end())
      continue;
    const QueuePlan *input = findQueue(plan, block->inputs[0]);
    auto type = input ? cppType(input->payloadType)
                      : llvm::Expected<std::string>(
                            generatorError("feedback input Queue is missing"));
    auto parent = modulePointer(block->scope);
    if (!type)
      return type.takeError();
    if (!parent)
      return parent.takeError();
    appendInitializer(initializers, "block_", index,
                      "_state_(\"feedback_state_", block->name, "\", ",
                      state->second, ", ", *parent,
                      ", 1, std::numeric_limits<size_t>::max(), nullptr, 1)");
  }
  size_t sinkIndex = 0;
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    auto parent = modulePointer(block->scope);
    if (!parent)
      return parent.takeError();
    std::string member = "block_" + std::to_string(index) + "_";
    std::string key = block->name + "#" + std::to_string(index);
    std::string instanceName = block->kind + "_" + block->name;
    if (block->kind == "firing") {
      std::string inputs;
      for (size_t input = 0; input < block->inputs.size(); ++input) {
        if (input)
          inputs.append(", ");
        inputs.append("&").append(queueMembers[block->inputs[input]]);
      }
      std::string outputs;
      for (size_t outputIndex = 0; outputIndex < block->outputs.size();
           ++outputIndex) {
        if (outputIndex)
          outputs.append(", ");
        outputs.append("&").append(queueMembers[block->outputs[outputIndex]]);
      }
      std::string policy = "block_" + std::to_string(index) + "_policy{";
      for (auto [readIndex, readTable] :
           llvm::enumerate(readOnlyTables(plan, *block))) {
        auto table = tableMembers.find(readTable->name);
        if (table == tableMembers.end())
          return generatorError("read-only state declaration is missing");
        if (readIndex)
          policy.append(", ");
        policy.append("&").append(table->getValue());
      }
      policy.push_back('}');
      const std::vector<const TablePlan *> ownerTables =
          stateOwnerTables(plan, *block);
      if (ownerTables.size() > 1) {
        std::string tables;
        std::string modes;
        std::string merges;
        for (auto [ownerIndex, owner] : llvm::enumerate(ownerTables)) {
          auto table = tableMembers.find(owner->name);
          if (table == tableMembers.end())
            return generatorError("state firing declaration is missing");
          if (ownerIndex) {
            tables.append(", ");
            modes.append(", ");
            merges.append(", ");
          }
          const StateWritePlan *write = findStateWrite(*block, owner->name);
          tables.append("&").append(table->getValue());
          modes.append("gfsim::TableWriteMode::")
              .append(!write || write->mode == "replace" ? "Replace"
                                                         : "FieldMerge");
          merges.append("block_")
              .append(std::to_string(index))
              .append("_merge_policy_")
              .append(std::to_string(ownerIndex))
              .append("{}");
        }
        appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                          blockIds[key], ", ", *parent, ", std::tuple{", tables,
                          "}, std::tuple{", inputs, "}, std::tuple{", outputs,
                          "}, std::array{", modes, "}, ", policy,
                          ", std::tuple{", merges, "})");
      } else {
        auto table = tableMembers.find(block->table);
        if (table == tableMembers.end())
          return generatorError("table firing declaration is missing");
        appendInitializer(
            initializers, member, "(\"", instanceName, "\", ", blockIds[key],
            ", ", *parent, ", ", table->getValue(), ", std::tuple{", inputs,
            "}, std::tuple{", outputs, "}, gfsim::TableWriteMode::",
            block->writeMode == "replace" ? "Replace" : "FieldMerge", ", ",
            policy, ", block_", index, "_merge_policy{})");
      }
    } else if (block->kind == "transform") {
      if (block->inputs.size() == 1 && block->outputs.size() == 1) {
        appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                          blockIds[key], ", ", *parent, ", ",
                          queueMembers[block->inputs[0]], ", ",
                          queueMembers[block->outputs[0]], ")");
      } else {
        std::string inputs;
        std::string outputs;
        for (size_t operand = 0; operand < block->inputs.size(); ++operand) {
          if (operand)
            inputs.append(", ");
          inputs.append("&").append(queueMembers[block->inputs[operand]]);
        }
        for (size_t result = 0; result < block->outputs.size(); ++result) {
          if (result)
            outputs.append(", ");
          outputs.append("&").append(queueMembers[block->outputs[result]]);
        }
        appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                          blockIds[key], ", ", *parent, ", std::tuple{", inputs,
                          "}, std::tuple{", outputs, "})");
      }
    } else if (block->kind == "broadcast" || block->kind == "fork" ||
               block->kind == "route") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(generatorError(
                              "topology input Queue is missing"));
      if (!type)
        return type.takeError();
      std::string outputs;
      for (auto [outputIndex, name] : llvm::enumerate(block->outputs)) {
        if (outputIndex)
          outputs.append(", ");
        outputs.append("&").append(queueMembers[name]);
      }
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ",
                        queueMembers[block->inputs[0]],
                        ", std::array<gfsim::SimQueue<", *type, "> *, ",
                        block->outputs.size(), ">{", outputs, "})");
    } else if (block->kind == "select") {
      const QueuePlan *result = findQueue(plan, block->outputs[0]);
      auto type = result ? cppType(result->payloadType)
                         : llvm::Expected<std::string>(generatorError(
                               "select output Queue is missing"));
      if (!type)
        return type.takeError();
      std::string inputs;
      for (size_t input = 1; input < block->inputs.size(); ++input) {
        if (input > 1)
          inputs.append(", ");
        inputs.append("&").append(queueMembers[block->inputs[input]]);
      }
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ",
                        queueMembers[block->inputs[0]],
                        ", std::array<gfsim::SimQueue<", *type, "> *, ",
                        block->inputs.size() - 1, ">{", inputs, "}, ",
                        queueMembers[block->outputs[0]], ")");
    } else if (block->kind == "merge") {
      const QueuePlan *result = findQueue(plan, block->outputs[0]);
      auto type = result ? cppType(result->payloadType)
                         : llvm::Expected<std::string>(
                               generatorError("merge output Queue is missing"));
      if (!type)
        return type.takeError();
      std::string inputs;
      for (auto [inputIndex, name] : llvm::enumerate(block->inputs)) {
        if (inputIndex)
          inputs.append(", ");
        inputs.append("&").append(queueMembers[name]);
      }
      std::string policy = block->policy == "priority"
                               ? "gfsim::QueueMergePolicy::Priority"
                               : "gfsim::QueueMergePolicy::RoundRobin";
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent,
                        ", std::array<gfsim::SimQueue<", *type, "> *, ",
                        block->inputs.size(), ">{", inputs, "}, ",
                        queueMembers[block->outputs[0]], ", ", policy, ")");
    } else if (block->kind == "barrier") {
      std::string inputs;
      std::string outputs;
      for (size_t operand = 0; operand < block->inputs.size(); ++operand) {
        if (operand)
          inputs.append(", ");
        inputs.append("&").append(queueMembers[block->inputs[operand]]);
        if (operand)
          outputs.append(", ");
        outputs.append("&").append(queueMembers[block->outputs[operand]]);
      }
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", std::tuple{", inputs,
                        "}, std::tuple{", outputs, "})");
    } else if (block->kind == "reorder") {
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ",
                        queueMembers[block->inputs[0]], ", ",
                        queueMembers[block->outputs[0]], ", ", block->capacity,
                        ", ", block->start, ")");
    } else if (block->kind == "dependency") {
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ",
                        queueMembers[block->inputs[0]], ", ",
                        queueMembers[block->outputs[0]], ", ", block->capacity,
                        ", ", block->resources, ", ", block->noDependency, ")");
    } else if (block->kind == "credit") {
      appendInitializer(
          initializers, member, "(\"", instanceName, "\", ", blockIds[key],
          ", ", *parent, ", ", queueMembers[block->inputs[0]], ", ",
          queueMembers[block->outputs[0]], ", ", block->credits, ")");
    } else if (block->kind == "feedback") {
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ",
                        queueMembers[block->inputs[0]], ", block_", index,
                        "_state_, ", queueMembers[block->outputs[0]], ", ",
                        block->maxIterations, ")");
    } else if (block->kind == "expect") {
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ",
                        queueMembers[block->inputs[0]], ", ",
                        cppStringLiteral(block->message), ")");
    } else if (block->kind == "table_read") {
      auto table = tableMembers.find(block->table);
      if (table == tableMembers.end())
        return generatorError("table read declaration is missing");
      if (block->inputs.empty())
        appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                          blockIds[key], ", ", *parent, ", ", table->getValue(),
                          ", ", queueMembers[block->outputs[0]], ", block_",
                          index, "_address_policy{&", table->getValue(),
                          slotPolicyPointers, sharedPolicyPointers, "}, block_",
                          index, "_when_policy{&", table->getValue(),
                          slotPolicyPointers, sharedPolicyPointers, "})");
      else
        appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                          blockIds[key], ", ", *parent, ", ", table->getValue(),
                          ", ", queueMembers[block->inputs[0]], ", ",
                          queueMembers[block->outputs[0]], ", block_", index,
                          "_address_policy{&", table->getValue(),
                          slotPolicyPointers, sharedPolicyPointers, "}, block_",
                          index, "_when_policy{&", table->getValue(),
                          slotPolicyPointers, sharedPolicyPointers, "})");
    } else if (block->kind == "table_write") {
      auto table = tableMembers.find(block->table);
      if (table == tableMembers.end())
        return generatorError("table write declaration is missing");
      if (block->inputs.empty())
        appendInitializer(
            initializers, member, "(\"", instanceName, "\", ", blockIds[key],
            ", ", *parent, ", ", table->getValue(), ", block_", index,
            "_address_policy{&", table->getValue(), slotPolicyPointers,
            sharedPolicyPointers, "}, block_", index, "_enable_policy{&",
            table->getValue(), slotPolicyPointers, sharedPolicyPointers,
            "}, block_", index, "_value_policy{&", table->getValue(),
            slotPolicyPointers, sharedPolicyPointers, "}, block_", index,
            "_merge_policy{}, gfsim::TableWriteMode::",
            block->writeMode == "replace" ? "Replace" : "FieldMerge", ")");
      else
        appendInitializer(
            initializers, member, "(\"", instanceName, "\", ", blockIds[key],
            ", ", *parent, ", ", table->getValue(), ", ",
            queueMembers[block->inputs[0]], ", block_", index,
            "_address_policy{&", table->getValue(), slotPolicyPointers,
            sharedPolicyPointers, "}, block_", index, "_enable_policy{&",
            table->getValue(), slotPolicyPointers, sharedPolicyPointers,
            "}, block_", index, "_value_policy{&", table->getValue(),
            slotPolicyPointers, sharedPolicyPointers, "}, block_", index,
            "_merge_policy{}, gfsim::TableWriteMode::",
            block->writeMode == "replace" ? "Replace" : "FieldMerge", ")");
    } else if (block->kind == "table_masked_write") {
      auto table = tableMembers.find(block->table);
      if (table == tableMembers.end())
        return generatorError("masked table write declaration is missing");
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ", table->getValue(),
                        ", block_", index, "_mask_policy{&", table->getValue(),
                        slotPolicyPointers, sharedPolicyPointers, "}, block_",
                        index, "_enable_policy{&", table->getValue(),
                        slotPolicyPointers, sharedPolicyPointers, "}, block_",
                        index, "_value_policy{&", table->getValue(),
                        slotPolicyPointers, sharedPolicyPointers, "}, block_",
                        index, "_merge_policy{})");
    } else if (block->kind == "slot") {
      const SlotPlan *slot = findSlot(plan, block->slot);
      if (!slot)
        return generatorError("slot declaration is missing");
      auto slotIndex = static_cast<size_t>(slot - plan.slots.data());
      std::string policyPointers = slotPolicyPointers;
      for (const TablePlan &table : plan.tables)
        if (referencesTable(block->expressions, table.name))
          policyPointers.append(", &").append(tableMembers[table.name]);
      policyPointers.append(sharedPolicyPointers);
      appendInitializer(
          initializers, member, "(\"", instanceName, "\", ", blockIds[key],
          ", ", *parent, ", ", queueMembers[block->inputs[0]], ", slot_",
          slotIndex, "_state_, block_", index, "_release_policy{",
          policyPointers.empty() ? std::string() : policyPointers.substr(2),
          "})");
    } else if (block->kind == "sink" || block->kind == "observe") {
      appendInitializer(initializers, member, "(\"", instanceName, "\", ",
                        blockIds[key], ", ", *parent, ", ",
                        queueMembers[block->inputs[0]], ")");
      ++sinkIndex;
    } else {
      return generatorError("unsupported native Queue block '" + block->kind +
                            "'");
    }
  }
  for (auto [memoryIndex, instance] : llvm::enumerate(plan.memoryInstances)) {
    auto found = memoryEndpoints.find(instance.name);
    if (found == memoryEndpoints.end() || found->getValue().empty())
      return generatorError("memory instance has no endpoints");
    const auto &endpoints = found->getValue();
    const QueuePlan *input = findQueue(plan, endpoints.front()->inputs.front());
    auto type = input ? cppType(input->payloadType)
                      : llvm::Expected<std::string>(
                            generatorError("memory input missing"));
    auto parent = modulePointer(instance.ownerPath);
    if (!type)
      return type.takeError();
    if (!parent)
      return parent.takeError();
    std::string inputs;
    std::string outputs;
    for (auto [index, endpoint] : llvm::enumerate(endpoints)) {
      if (index) {
        inputs.append(", ");
        outputs.append(", ");
      }
      inputs.append("&").append(queueMembers[endpoint->inputs.front()]);
      outputs.append("&").append(queueMembers[endpoint->outputs.front()]);
    }
    appendInitializer(initializers, "memory_", memoryIndex, "_(\"memory_",
                      instance.name, "\", ", memoryIds[instance.name], ", ",
                      *parent, ", std::array<gfsim::SimQueue<", *type, "> *, ",
                      endpoints.size(), ">{", inputs,
                      "}, std::array<gfsim::SimQueue<", *type, "> *, ",
                      endpoints.size(), ">{", outputs, "}, ", instance.entries,
                      ", ", instance.init, ", ", instance.latency, ")");
  }
  for (auto [index, initializer] : llvm::enumerate(initializers))
    output << "        " << initializer
           << (index + 1 == initializers.size() ? "\n" : ",\n");
  output << "  {\n    setPath(\"/" << plan.system << "\");\n";
  for (const std::string &scope : plan.scopes) {
    llvm::StringRef parent = llvm::StringRef(scope).rsplit('/').first;
    if (parent.empty())
      parent = "/";
    auto line = attach(parent, scopeMembers[scope]);
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (const TablePlan &table : plan.tables) {
    auto line = attach(table.ownerPath, tableMembers[table.name]);
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (const QueuePlan &queue : plan.queues) {
    auto line = attach(queueOwners[queue.name], queueMembers[queue.name]);
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    if (!feedbackStateIds.contains(index))
      continue;
    auto line =
        attach(block->scope, "block_" + std::to_string(index) + "_state_");
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (auto [memoryIndex, instance] : llvm::enumerate(plan.memoryInstances)) {
    auto line = attach(instance.ownerPath,
                       "memory_" + std::to_string(memoryIndex) + "_");
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    auto line = attach(block->scope, "block_" + std::to_string(index) + "_");
    if (!line)
      return line.takeError();
    output << *line << '\n';
  }
  output << "  }\n\n  void reset() override {\n    gfsim::Module::reset();\n";
  for (const TableMatchPlan &match : plan.tableMatches)
    output << "    " << identifier(match.name) << "_.reset();\n";
  for (const TableSelectionPlan &selection : plan.tableSelections)
    output << "    " << identifier(selection.name) << "_.reset();\n";
  output << "  }\n\n";
  for (const QueueBlockPlan &block : plan.blocks)
    if (block.kind == "source") {
      const QueuePlan *queue = findQueue(plan, block.outputs.front());
      auto type = queue ? cppType(queue->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("source Queue is missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::SimQueue<" << *type << "> &" << block.outputs.front()
             << "() { return " << queueMembers[block.outputs.front()]
             << "; }\n";
    }
  for (const TablePlan &table : plan.tables) {
    auto type = cppType(table.entryType);
    if (!type)
      return type.takeError();
    output << "  const gfsim::SimTable<" << *type << "> &table_"
           << identifier(table.name) << "() const { return "
           << tableMembers[table.name] << "; }\n";
  }
  sinkIndex = 0;
  size_t observationIndex = 0;
  for (auto [index, block] : llvm::enumerate(runtimeBlocks))
    if (block->kind == "sink") {
      const QueuePlan *queue = findQueue(plan, block->inputs.front());
      auto type = queue ? cppType(queue->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("sink Queue is missing"));
      if (!type)
        return type.takeError();
      output << "  const std::vector<" << *type << "> &sink_" << sinkIndex
             << "_values() const { return block_" << index
             << "_.received(); }\n"
             << "  gfsim::ObjectId sink_" << sinkIndex
             << "_id() const { return block_" << index << "_.id(); }\n";
      ++sinkIndex;
    } else if (block->kind == "observe") {
      const QueuePlan *queue = findQueue(plan, block->inputs.front());
      auto type = queue ? cppType(queue->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("observation Queue is missing"));
      if (!type)
        return type.takeError();
      output << "  const std::vector<" << *type << "> &observation_"
             << observationIndex << "_values() const { return block_" << index
             << "_.observed(); }\n";
      ++observationIndex;
    }
  size_t dependencyIndex = 0;
  size_t reorderIndex = 0;
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    if (block->kind == "dependency") {
      output << "  size_t dependency_" << dependencyIndex
             << "_active() const { return block_" << index << "_.active(); }\n"
             << "  size_t dependency_" << dependencyIndex
             << "_resource_active(size_t resource) const { return block_"
             << index << "_.resourceActive(resource); }\n";
      ++dependencyIndex;
    } else if (block->kind == "reorder") {
      output << "  size_t reorder_" << reorderIndex
             << "_active() const { return block_" << index << "_.active(); }\n";
      ++reorderIndex;
    }
  }
  output << "\n  std::array<gfsim::DispatchRow, " << nextId
         << "> dispatch_rows() {\n    return {\n";
  for (const QueuePlan &queue : plan.queues)
    output << "        gfsim::makeDispatchRow(&" << queueMembers[queue.name]
           << "),\n";
  for (auto [index, block] : llvm::enumerate(runtimeBlocks))
    if (feedbackStateIds.contains(index))
      output << "        gfsim::makeDispatchRow(&block_" << index
             << "_state_),\n";
  for (size_t index = 0; index < runtimeBlocks.size(); ++index) {
    output << "        gfsim::makeDispatchRow(&block_" << index << "_),\n";
  }
  for (size_t index = 0; index < plan.memoryInstances.size(); ++index)
    output << "        gfsim::makeDispatchRow(&memory_" << index << "_),\n";
  for (const TablePlan &table : plan.tables)
    output << "        gfsim::makeDispatchRow(&" << tableMembers[table.name]
           << "),\n";
  output << "    };\n  }\n\nprivate:\n";
  for (const std::string &scope : plan.scopes)
    output << "  gfsim::Module " << scopeMembers[scope] << ";\n";
  for (const QueuePlan &queue : plan.queues) {
    auto type = cppType(queue.payloadType);
    if (!type)
      return type.takeError();
    output << "  gfsim::SimQueue<" << *type << "> " << queueMembers[queue.name]
           << ";\n";
  }
  for (auto [index, slot] : llvm::enumerate(plan.slots)) {
    auto type = cppType(slot.payloadType);
    if (!type)
      return type.takeError();
    output << "  gfsim::SlotState<" << *type << "> slot_" << index
           << "_state_;\n";
  }
  for (const TablePlan &table : plan.tables) {
    auto type = cppType(table.entryType);
    if (!type)
      return type.takeError();
    output << "  gfsim::SimTable<" << *type << "> " << tableMembers[table.name]
           << ";\n";
  }
  for (const TableMatchPlan &match : plan.tableMatches)
    output << "  " << identifier(match.name) << "_cache "
           << identifier(match.name) << "_;\n";
  for (const TableSelectionPlan &selection : plan.tableSelections)
    output << "  " << identifier(selection.name) << "_cache "
           << identifier(selection.name) << "_;\n";
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    if (!feedbackStateIds.contains(index))
      continue;
    const QueuePlan *input = findQueue(plan, block->inputs[0]);
    auto type = input ? cppType(input->payloadType)
                      : llvm::Expected<std::string>(
                            generatorError("feedback state type is missing"));
    if (!type)
      return type.takeError();
    output << "  gfsim::SimQueue<gfsim::FeedbackToken<" << *type << ">> block_"
           << index << "_state_;\n";
  }
  sinkIndex = 0;
  for (auto [index, block] : llvm::enumerate(runtimeBlocks)) {
    if (block->kind == "firing") {
      const std::vector<const TablePlan *> ownerTables =
          stateOwnerTables(plan, *block);
      if (ownerTables.size() > 1) {
        output << "  gfsim::QueueStateTransition<block_" << index
               << "_policy, std::tuple<";
        for (auto [ownerIndex, table] : llvm::enumerate(ownerTables)) {
          auto type = table ? cppType(table->entryType)
                            : llvm::Expected<std::string>(
                                  generatorError("state firing Table missing"));
          if (!type)
            return type.takeError();
          if (ownerIndex)
            output << ", ";
          output << *type;
        }
        output << ">, std::tuple<";
        for (auto [inputIndex, inputName] : llvm::enumerate(block->inputs)) {
          const QueuePlan *input = findQueue(plan, inputName);
          auto type = input ? cppType(input->payloadType)
                            : llvm::Expected<std::string>(
                                  generatorError("state firing input missing"));
          if (!type)
            return type.takeError();
          if (inputIndex)
            output << ", ";
          output << *type;
        }
        output << ">, std::tuple<";
        for (auto [outputIndex, outputName] : llvm::enumerate(block->outputs)) {
          const QueuePlan *result = findQueue(plan, outputName);
          auto type = result ? cppType(result->payloadType)
                             : llvm::Expected<std::string>(generatorError(
                                   "state firing output missing"));
          if (!type)
            return type.takeError();
          if (outputIndex)
            output << ", ";
          output << *type;
        }
        output << ">, std::tuple<";
        for (size_t ownerIndex = 0; ownerIndex < ownerTables.size();
             ++ownerIndex) {
          if (ownerIndex)
            output << ", ";
          output << "block_" << index << "_merge_policy_" << ownerIndex;
        }
        output << ">> block_" << index << "_;\n";
        continue;
      }
      const TablePlan *table = findTable(plan, block->table);
      auto entryType = table ? cppType(table->entryType)
                             : llvm::Expected<std::string>(generatorError(
                                   "table firing Table missing"));
      if (!entryType)
        return entryType.takeError();
      output << "  gfsim::QueueTableTransition<block_" << index << "_policy, "
             << *entryType << ", std::tuple<";
      for (auto [inputIndex, inputName] : llvm::enumerate(block->inputs)) {
        const QueuePlan *input = findQueue(plan, inputName);
        auto inputType = input ? cppType(input->payloadType)
                               : llvm::Expected<std::string>(generatorError(
                                     "table firing input missing"));
        if (!inputType)
          return inputType.takeError();
        if (inputIndex)
          output << ", ";
        output << *inputType;
      }
      output << ">, std::tuple<";
      for (auto [outputIndex, outputName] : llvm::enumerate(block->outputs)) {
        const QueuePlan *result = findQueue(plan, outputName);
        auto resultType = result ? cppType(result->payloadType)
                                 : llvm::Expected<std::string>(generatorError(
                                       "table firing output missing"));
        if (!resultType)
          return resultType.takeError();
        if (outputIndex)
          output << ", ";
        output << *resultType;
      }
      output << ">, block_" << index << "_merge_policy> block_" << index
             << "_;\n";
    } else if (block->kind == "transform") {
      if (block->inputs.size() == 1 && block->outputs.size() == 1) {
        const QueuePlan *input = findQueue(plan, block->inputs[0]);
        const QueuePlan *result = findQueue(plan, block->outputs[0]);
        auto inputType = input ? cppType(input->payloadType)
                               : llvm::Expected<std::string>(
                                     generatorError("transform input missing"));
        auto resultType = result ? cppType(result->payloadType)
                                 : llvm::Expected<std::string>(generatorError(
                                       "transform output missing"));
        if (!inputType)
          return inputType.takeError();
        if (!resultType)
          return resultType.takeError();
        output << "  gfsim::QueueTransform<" << *inputType << ", "
               << *resultType << ", block_" << index << "_policy, "
               << result->rate << "> block_" << index << "_;\n";
      } else {
        output << "  gfsim::QueueAtomicTransform<block_" << index
               << "_policy, std::tuple<";
        for (auto [inputIndex, inputName] : llvm::enumerate(block->inputs)) {
          const QueuePlan *input = findQueue(plan, inputName);
          auto type = input ? cppType(input->payloadType)
                            : llvm::Expected<std::string>(
                                  generatorError("atomic input missing"));
          if (!type)
            return type.takeError();
          if (inputIndex)
            output << ", ";
          output << *type;
        }
        output << ">, std::tuple<";
        for (auto [outputIndex, outputName] : llvm::enumerate(block->outputs)) {
          const QueuePlan *result = findQueue(plan, outputName);
          auto type = result ? cppType(result->payloadType)
                             : llvm::Expected<std::string>(generatorError(
                                   "atomic transform output missing"));
          if (!type)
            return type.takeError();
          if (outputIndex)
            output << ", ";
          output << *type;
        }
        output << ">> block_" << index << "_;\n";
      }
    } else if (block->kind == "broadcast" || block->kind == "fork" ||
               block->kind == "route") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("route input missing"));
      if (!type)
        return type.takeError();
      if (block->kind == "broadcast")
        output << "  gfsim::QueueBroadcast<" << *type << ", "
               << block->outputs.size() << "> block_" << index << "_;\n";
      else if (block->kind == "fork")
        output << "  gfsim::QueueFork<" << *type << ", "
               << block->outputs.size() << "> block_" << index << "_;\n";
      else
        output << "  gfsim::QueueRoute<" << *type << ", "
               << block->outputs.size() << ", block_" << index
               << "_policy> block_" << index << "_;\n";
    } else if (block->kind == "select") {
      const QueuePlan *control = findQueue(plan, block->inputs[0]);
      const QueuePlan *result = findQueue(plan, block->outputs[0]);
      auto controlType = control ? cppType(control->payloadType)
                                 : llvm::Expected<std::string>(generatorError(
                                       "select control input missing"));
      auto dataType = result ? cppType(result->payloadType)
                             : llvm::Expected<std::string>(
                                   generatorError("select output missing"));
      if (!controlType)
        return controlType.takeError();
      if (!dataType)
        return dataType.takeError();
      output << "  gfsim::QueueSelect<" << *controlType << ", " << *dataType
             << ", " << block->inputs.size() - 1 << ", block_" << index
             << "_policy> block_" << index << "_;\n";
    } else if (block->kind == "merge") {
      const QueuePlan *result = findQueue(plan, block->outputs[0]);
      auto type = result ? cppType(result->payloadType)
                         : llvm::Expected<std::string>(
                               generatorError("merge output missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::QueueMerge<" << *type << ", " << block->inputs.size()
             << "> block_" << index << "_;\n";
    } else if (block->kind == "barrier") {
      output << "  gfsim::QueueBarrier<std::tuple<";
      for (auto [inputIndex, inputName] : llvm::enumerate(block->inputs)) {
        const QueuePlan *input = findQueue(plan, inputName);
        auto type = input ? cppType(input->payloadType)
                          : llvm::Expected<std::string>(
                                generatorError("barrier input missing"));
        if (!type)
          return type.takeError();
        if (inputIndex)
          output << ", ";
        output << *type;
      }
      output << ">> block_" << index << "_;\n";
    } else if (block->kind == "reorder") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("reorder input missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::QueueReorder<" << *type << ", block_" << index
             << "_policy> block_" << index << "_;\n";
    } else if (block->kind == "dependency") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("dependency input missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::QueueDependency<" << *type << ", block_" << index
             << "_key_policy, block_" << index << "_dependency_policy, block_"
             << index << "_resource_policy, block_" << index
             << "_cost_policy> block_" << index << "_;\n";
    } else if (block->kind == "credit") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("credit input missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::QueueCredit<" << *type << ", block_" << index
             << "_policy> block_" << index << "_;\n";
    } else if (block->kind == "feedback") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("feedback input missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::QueueFeedback<" << *type << ", block_" << index
             << "_update_policy, block_" << index << "_condition_policy> block_"
             << index << "_;\n";
    } else if (block->kind == "expect") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("expect input missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::QueueExpect<" << *type << ", block_" << index
             << "_policy> block_" << index << "_;\n";
    } else if (block->kind == "table_read") {
      const TablePlan *table = findTable(plan, block->table);
      auto entryType = table ? cppType(table->entryType)
                             : llvm::Expected<std::string>(
                                   generatorError("table declaration missing"));
      if (!entryType)
        return entryType.takeError();
      if (block->inputs.empty()) {
        output << "  gfsim::TableReadSource<" << *entryType << ", block_"
               << index << "_address_policy, block_" << index
               << "_when_policy> block_" << index << "_;\n";
      } else {
        const QueuePlan *input = findQueue(plan, block->inputs.front());
        auto inputType = input ? cppType(input->payloadType)
                               : llvm::Expected<std::string>(generatorError(
                                     "table read input missing"));
        if (!inputType)
          return inputType.takeError();
        output << "  gfsim::QueueTableRead<" << *inputType << ", " << *entryType
               << ", block_" << index << "_address_policy, block_" << index
               << "_when_policy> block_" << index << "_;\n";
      }
    } else if (block->kind == "table_write") {
      const TablePlan *table = findTable(plan, block->table);
      auto entryType = table ? cppType(table->entryType)
                             : llvm::Expected<std::string>(
                                   generatorError("table declaration missing"));
      if (!entryType)
        return entryType.takeError();
      if (block->inputs.empty()) {
        output << "  gfsim::TableWriteSource<" << *entryType << ", block_"
               << index << "_address_policy, block_" << index
               << "_enable_policy, block_" << index << "_value_policy, block_"
               << index << "_merge_policy> block_" << index << "_;\n";
      } else {
        const QueuePlan *input = findQueue(plan, block->inputs.front());
        auto inputType = input ? cppType(input->payloadType)
                               : llvm::Expected<std::string>(generatorError(
                                     "table write input missing"));
        if (!inputType)
          return inputType.takeError();
        output << "  gfsim::QueueTableWrite<" << *inputType << ", "
               << *entryType << ", block_" << index << "_address_policy, block_"
               << index << "_enable_policy, block_" << index
               << "_value_policy, block_" << index << "_merge_policy> block_"
               << index << "_;\n";
      }
    } else if (block->kind == "table_masked_write") {
      const TablePlan *table = findTable(plan, block->table);
      auto entryType = table ? cppType(table->entryType)
                             : llvm::Expected<std::string>(
                                   generatorError("table declaration missing"));
      if (!entryType)
        return entryType.takeError();
      output << "  gfsim::TableMaskedWriteSource<" << *entryType << ", block_"
             << index << "_mask_policy, block_" << index
             << "_enable_policy, block_" << index << "_value_policy, block_"
             << index << "_merge_policy> block_" << index << "_;\n";
    } else if (block->kind == "slot") {
      const SlotPlan *slot = findSlot(plan, block->slot);
      auto type = slot ? cppType(slot->payloadType)
                       : llvm::Expected<std::string>(
                             generatorError("slot declaration missing"));
      if (!type)
        return type.takeError();
      output << "  gfsim::QueueSlot<" << *type << ", block_" << index
             << "_release_policy> block_" << index << "_;\n";
    } else if (block->kind == "sink" || block->kind == "observe") {
      const QueuePlan *input = findQueue(plan, block->inputs[0]);
      auto type = input ? cppType(input->payloadType)
                        : llvm::Expected<std::string>(
                              generatorError("sink input missing"));
      if (!type)
        return type.takeError();
      if (block->kind == "sink") {
        output << "  gfsim::QueueSink<" << *type << "> block_" << index
               << "_;\n";
        ++sinkIndex;
      } else {
        output << "  gfsim::QueueObserve<" << *type << "> block_" << index
               << "_;\n";
      }
    }
  }
  for (auto [memoryIndex, instance] : llvm::enumerate(plan.memoryInstances)) {
    auto found = memoryEndpoints.find(instance.name);
    if (found == memoryEndpoints.end() || found->getValue().empty())
      return generatorError("memory instance has no endpoints");
    const auto &endpoints = found->getValue();
    const QueuePlan *input = findQueue(plan, endpoints.front()->inputs.front());
    auto type = input ? cppType(input->payloadType)
                      : llvm::Expected<std::string>(
                            generatorError("memory input missing"));
    auto dataType = cppType(instance.dataType);
    if (!type)
      return type.takeError();
    if (!dataType)
      return dataType.takeError();
    output << "  gfsim::QueueMemoryArbiter<" << *type << ", " << *dataType
           << ", " << endpoints.size() << ", memory_" << memoryIndex
           << "_address_policy, memory_" << memoryIndex
           << "_write_policy, memory_" << memoryIndex << "_data_policy, memory_"
           << memoryIndex << "_response_policy> memory_" << memoryIndex
           << "_;\n";
  }
  output << "};\n\n} // namespace ac_generated\n";
  return output.str();
}

} // namespace acir::codegen
