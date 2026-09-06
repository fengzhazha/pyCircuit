#include "acir/Dialect/ACIR/ACIRDialect.h"
#include "acir/Dialect/ACIR/ACIRTypes.h"

#include "mlir/AsmParser/AsmParser.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"
#include "gtest/gtest.h"

#include <array>

namespace acir::ac {
namespace {

TEST(ACIRTypesTest, PublicTypeInventoryRoundTrips) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();

  struct TypeCase {
    llvm::StringLiteral spelling;
    mlir::TypeID typeID;
  };
  const std::array<TypeCase, 18> cases = {{
      {"!ac.struct<@types::@Struct>", StructType::getTypeID()},
      {"!ac.packet<@types::@Packet>", PacketType::getTypeID()},
      {"!ac.transaction<@types::@Transaction>", TransactionType::getTypeID()},
      {"!ac.enum<@types::@Enum>", EnumType::getTypeID()},
      {"!ac.union<@types::@Union>", UnionType::getTypeID()},
      {"!ac.optional<i8>", OptionalType::getTypeID()},
      {"!ac.list<i8>", ListType::getTypeID()},
      {"!ac.vector<4 x i8>", VectorType::getTypeID()},
      {"!ac.value_array<4 x i8>", ValueArrayType::getTypeID()},
      {"!ac.flow<i8, @protocol>", FlowType::getTypeID()},
      {"!ac.endpoint<@interface, @role>", EndpointType::getTypeID()},
      {"!ac.resource_ref<@resource_type, @role>", ResourceRefType::getTypeID()},
      {"!ac.channel<i8, @protocol>", ChannelType::getTypeID()},
      {"!ac.duration<cycles>", DurationType::getTypeID()},
      {"!ac.rate<bytes, cycles>", RateType::getTypeID()},
      {"!ac.event<i8>", EventType::getTypeID()},
      {"!ac.address<@space>", AddressType::getTypeID()},
      {"!ac.resource_token<@resource>", ResourceTokenType::getTypeID()},
  }};

  for (const TypeCase &testCase : cases) {
    mlir::Type type = mlir::parseType(testCase.spelling, &context);
    ASSERT_TRUE(type) << testCase.spelling.str();
    EXPECT_EQ(type.getTypeID(), testCase.typeID) << testCase.spelling.str();

    std::string printed;
    llvm::raw_string_ostream(printed) << type;
    EXPECT_EQ(printed, testCase.spelling) << testCase.spelling.str();
    EXPECT_EQ(type, mlir::parseType(printed, &context));
  }
}

TEST(ACIRTypesTest, QueueVarTypesRoundTripWithImmutablePayloads) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();

  struct TypeCase {
    llvm::StringLiteral spelling;
    mlir::TypeID typeID;
  };
  const std::array<TypeCase, 4> cases = {{
      {"!ac.var<i32>", VarType::getTypeID()},
      {"!ac.queue<!ac.struct<@types::@Token>>", QueueType::getTypeID()},
      {"!ac.var<tuple<i3, i5>>", VarType::getTypeID()},
      {"!ac.queue<!ac.value_array<4 x i8>>", QueueType::getTypeID()},
  }};

  for (const TypeCase &testCase : cases) {
    mlir::Type type = mlir::parseType(testCase.spelling, &context);
    ASSERT_TRUE(type) << testCase.spelling.str();
    EXPECT_EQ(type.getTypeID(), testCase.typeID) << testCase.spelling.str();

    std::string printed;
    llvm::raw_string_ostream(printed) << type;
    EXPECT_EQ(printed, testCase.spelling) << testCase.spelling.str();
    EXPECT_EQ(type, mlir::parseType(printed, &context));
  }
}

TEST(ACIRTypesTest, StaticQueueVarCollectionsRoundTrip) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();

  struct TypeCase {
    llvm::StringLiteral spelling;
    mlir::TypeID typeID;
  };
  const std::array<TypeCase, 4> cases = {{
      {"!ac.array<4 x !ac.queue<i32>>", ArrayType::getTypeID()},
      {"!ac.map<[\"cube\", \"scalar\", \"vector\"], !ac.queue<i32>>",
       MapType::getTypeID()},
      {"!ac.set<4 x !ac.var<i1>>", SetType::getTypeID()},
      {"!ac.array<2 x !ac.map<[\"left\", \"right\"], "
       "!ac.queue<!ac.struct<@types::@Token>>>>",
       ArrayType::getTypeID()},
  }};

  for (const TypeCase &testCase : cases) {
    mlir::Type type = mlir::parseType(testCase.spelling, &context);
    ASSERT_TRUE(type) << testCase.spelling.str();
    EXPECT_EQ(type.getTypeID(), testCase.typeID) << testCase.spelling.str();

    std::string printed;
    llvm::raw_string_ostream(printed) << type;
    EXPECT_EQ(printed, testCase.spelling) << testCase.spelling.str();
    EXPECT_EQ(type, mlir::parseType(printed, &context));
  }
}

TEST(ACIRTypesTest, EveryUnitCategoryIsChecked) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::ScopedDiagnosticHandler suppressExpectedDiagnostics(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });
  auto location = mlir::UnknownLoc::get(&context);
  auto emitError = [location] { return mlir::emitError(location); };

  const std::array timeUnits = {
      Unit::Ticks,        Unit::Cycles,       Unit::Seconds,
      Unit::Milliseconds, Unit::Microseconds, Unit::Nanoseconds,
      Unit::Picoseconds,
  };
  const std::array dataUnits = {Unit::Bytes, Unit::Bits, Unit::Entries,
                                Unit::Packets, Unit::Transactions};

  for (Unit timeUnit : timeUnits) {
    EXPECT_TRUE(DurationType::getChecked(emitError, &context, timeUnit));
    EXPECT_TRUE(
        RateType::getChecked(emitError, &context, Unit::Bytes, timeUnit));
    EXPECT_FALSE(
        RateType::getChecked(emitError, &context, timeUnit, Unit::Cycles));
  }
  for (Unit dataUnit : dataUnits) {
    EXPECT_FALSE(DurationType::getChecked(emitError, &context, dataUnit));
    EXPECT_TRUE(
        RateType::getChecked(emitError, &context, dataUnit, Unit::Cycles));
    EXPECT_FALSE(
        RateType::getChecked(emitError, &context, Unit::Bytes, dataUnit));
  }
}

TEST(ACIRTypesTest, TypesAreUniquedByTheirParameters) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();

  auto payload = mlir::IntegerType::get(&context, 32);
  EXPECT_EQ(OptionalType::get(&context, payload),
            OptionalType::get(&context, payload));
  EXPECT_NE(VectorType::get(&context, int64_t{4}, mlir::Type(payload)),
            VectorType::get(&context, int64_t{8}, mlir::Type(payload)));
}

TEST(ACIRTypesTest, NamedTypesPreserveTheirIdentity) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();

  auto lhs = mlir::SymbolRefAttr::get(
      &context, "types", {mlir::FlatSymbolRefAttr::get(&context, "Left")});
  auto rhs = mlir::SymbolRefAttr::get(
      &context, "types", {mlir::FlatSymbolRefAttr::get(&context, "Right")});
  EXPECT_NE(StructType::get(&context, lhs), StructType::get(&context, rhs));
}

TEST(ACIRTypesTest, CheckedBuildersRejectInvalidParameters) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  auto payload = mlir::IntegerType::get(&context, 8);
  mlir::ScopedDiagnosticHandler suppressExpectedDiagnostics(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });

  auto location = mlir::UnknownLoc::get(&context);
  auto emitError = [location] { return mlir::emitError(location); };
  EXPECT_FALSE(VectorType::getChecked(emitError, &context, int64_t{0},
                                      mlir::Type(payload)));
  EXPECT_FALSE(ValueArrayType::getChecked(emitError, &context, int64_t{0},
                                          mlir::Type(payload)));
  EXPECT_FALSE(DurationType::getChecked(emitError, &context, Unit::Bytes));
  auto functionType = mlir::FunctionType::get(&context, {payload}, {payload});
  EXPECT_FALSE(
      VarType::getChecked(emitError, &context, mlir::Type(functionType)));
  EXPECT_FALSE(
      QueueType::getChecked(emitError, &context, mlir::Type(functionType)));
  auto variable = VarType::get(&context, payload);
  auto queue = QueueType::get(&context, payload);
  auto dynamicList = ListType::get(&context, payload);
  auto valueArray = ValueArrayType::get(&context, int64_t{2}, payload);
  EXPECT_FALSE(
      QueueType::getChecked(emitError, &context, mlir::Type(variable)));
  EXPECT_FALSE(VarType::getChecked(emitError, &context, mlir::Type(queue)));
  EXPECT_FALSE(
      QueueType::getChecked(emitError, &context, mlir::Type(dynamicList)));
  EXPECT_FALSE(
      VarType::getChecked(emitError, &context, mlir::Type(dynamicList)));
  EXPECT_TRUE(
      VarType::getChecked(emitError, &context, mlir::Type(valueArray)));
  EXPECT_FALSE(ValueArrayType::getChecked(emitError, &context, int64_t{2},
                                          mlir::Type(queue)));
  auto queueCollection = QueueType::get(&context, payload);
  auto duplicateKeys =
      mlir::ArrayAttr::get(&context, {mlir::StringAttr::get(&context, "lane"),
                                      mlir::StringAttr::get(&context, "lane")});
  auto reversedKeys =
      mlir::ArrayAttr::get(&context, {mlir::StringAttr::get(&context, "right"),
                                      mlir::StringAttr::get(&context, "left")});
  EXPECT_FALSE(ArrayType::getChecked(emitError, &context, int64_t{0},
                                     mlir::Type(queueCollection)));
  EXPECT_FALSE(ArrayType::getChecked(emitError, &context, int64_t{2},
                                     mlir::Type(payload)));
  EXPECT_FALSE(MapType::getChecked(emitError, &context, duplicateKeys,
                                   mlir::Type(queueCollection)));
  EXPECT_FALSE(MapType::getChecked(emitError, &context, reversedKeys,
                                   mlir::Type(queueCollection)));
  EXPECT_FALSE(SetType::getChecked(emitError, &context, int64_t{0},
                                   mlir::Type(queueCollection)));
  EXPECT_FALSE(SetType::getChecked(emitError, &context, int64_t{2},
                                   mlir::Type(payload)));
  EXPECT_FALSE(
      RateType::getChecked(emitError, &context, Unit::Cycles, Unit::Cycles));
}

} // namespace
} // namespace acir::ac
