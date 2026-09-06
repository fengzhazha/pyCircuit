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
  const std::array<TypeCase, 11> cases = {{
      {"!ac.struct<@types::@Struct>", StructType::getTypeID()},
      {"!ac.packet<@types::@Packet>", PacketType::getTypeID()},
      {"!ac.transaction<@types::@Transaction>", TransactionType::getTypeID()},
      {"!ac.enum<@types::@Enum>", EnumType::getTypeID()},
      {"!ac.value_array<4 x i8>", ValueArrayType::getTypeID()},
      {"!ac.flow<i8, @protocol>", FlowType::getTypeID()},
      {"!ac.endpoint<@interface, @role>", EndpointType::getTypeID()},
      {"!ac.resource_ref<@resource_type, @role>", ResourceRefType::getTypeID()},
      {"!ac.channel<i8, @protocol>", ChannelType::getTypeID()},
      {"!ac.event<i8>", EventType::getTypeID()},
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
  const std::array<TypeCase, 1> cases = {{
      {"!ac.array<4 x !ac.queue<i32>>", ArrayType::getTypeID()},
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

TEST(ACIRTypesTest, TypesAreUniquedByTheirParameters) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();

  auto payload = mlir::IntegerType::get(&context, 32);
  EXPECT_EQ(ValueArrayType::get(&context, int64_t{4}, payload),
            ValueArrayType::get(&context, int64_t{4}, payload));
  EXPECT_NE(ValueArrayType::get(&context, int64_t{4}, payload),
            ValueArrayType::get(&context, int64_t{8}, payload));
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
  EXPECT_FALSE(ValueArrayType::getChecked(emitError, &context, int64_t{0},
                                          mlir::Type(payload)));
  auto functionType = mlir::FunctionType::get(&context, {payload}, {payload});
  EXPECT_FALSE(
      VarType::getChecked(emitError, &context, mlir::Type(functionType)));
  EXPECT_FALSE(
      QueueType::getChecked(emitError, &context, mlir::Type(functionType)));
  auto variable = VarType::get(&context, payload);
  auto queue = QueueType::get(&context, payload);
  auto valueArray = ValueArrayType::get(&context, int64_t{2}, payload);
  EXPECT_FALSE(
      QueueType::getChecked(emitError, &context, mlir::Type(variable)));
  EXPECT_FALSE(VarType::getChecked(emitError, &context, mlir::Type(queue)));
  EXPECT_TRUE(
      VarType::getChecked(emitError, &context, mlir::Type(valueArray)));
  EXPECT_FALSE(ValueArrayType::getChecked(emitError, &context, int64_t{2},
                                          mlir::Type(queue)));
  auto queueCollection = QueueType::get(&context, payload);
  EXPECT_FALSE(ArrayType::getChecked(emitError, &context, int64_t{0},
                                     mlir::Type(queueCollection)));
  EXPECT_FALSE(ArrayType::getChecked(emitError, &context, int64_t{2},
                                     mlir::Type(payload)));
}

} // namespace
} // namespace acir::ac
