#include "gfsim/bits.h"
#include "gfsim/count_zeros.h"
#include "gfsim/popcount.h"
#include "gfsim/priority_encode.h"

#include "gtest/gtest.h"

#include <cstdint>

namespace gfsim {
namespace {

TEST(UIntTest, OperationsTruncateToDeclaredWidth) {
  UInt<3> seven = 7;
  UInt<3> one = 1;

  EXPECT_EQ(0u, (seven + one).value());
  EXPECT_EQ(6u, (seven - one).value());
  EXPECT_EQ(1u, (seven * seven).value());
  EXPECT_EQ(7u, (seven / one).value());
  EXPECT_EQ(0u, (seven / UInt<3>{0}).value());
  EXPECT_EQ(1u, (seven & one).value());
  EXPECT_EQ(7u, (seven | one).value());
  EXPECT_EQ(6u, (seven ^ one).value());
  EXPECT_EQ(0u, (~seven).value());
}

TEST(UIntTest, OneBitArithmeticUsesModuloTwo) {
  UInt<1> one = 1;

  EXPECT_EQ(0u, (one + one).value());
  EXPECT_EQ(0u, (one + 1).value());
  EXPECT_EQ(0u, (~one).value());
  EXPECT_EQ(0u, (one << 1).value());
  EXPECT_TRUE(one);
}

TEST(UIntTest, ShiftsAndComparisonsAreUnsignedAndWidthBounded) {
  UInt<7> high = 0x40;
  UInt<7> low = 0x03;

  EXPECT_TRUE(high > low);
  EXPECT_EQ(0x0cu, (low << UInt<7>{2}).value());
  EXPECT_EQ(0x10u, (high >> UInt<7>{2}).value());
  EXPECT_EQ(0u, (high << UInt<7>{7}).value());
  EXPECT_EQ(0u, (high >> UInt<7>{7}).value());
  EXPECT_EQ(0u, (high << 8).value());
  EXPECT_EQ(0u, (high >> 8).value());
  EXPECT_EQ(0x40u, static_cast<std::uint64_t>(high));
}

TEST(UIntTest, SignedViewAndArithmeticShiftRespectDeclaredWidth) {
  UInt<3> negativeOne = 7;
  UInt<3> negativeFour = 4;
  UInt<3> positiveThree = 3;

  EXPECT_EQ(-1, negativeOne.signedValue());
  EXPECT_EQ(-4, negativeFour.signedValue());
  EXPECT_EQ(3, positiveThree.signedValue());
  EXPECT_EQ(7u, negativeOne.arithmeticShiftRight(UInt<3>{3}).value());
  EXPECT_EQ(6u, negativeFour.arithmeticShiftRight(UInt<3>{1}).value());
  EXPECT_EQ(0u, positiveThree.arithmeticShiftRight(UInt<3>{3}).value());
  EXPECT_EQ(1u, PacketTraits<UInt<3>>::serializedSize);
  EXPECT_EQ(2u, PacketTraits<UInt<13>>::serializedSize);
}

TEST(UIntTest, SixtyFourBitArithmeticAlsoWrapsExactly) {
  UInt<64> maximum = ~std::uint64_t{0};
  UInt<64> one = 1;

  EXPECT_EQ(0u, (maximum + one).value());
  EXPECT_EQ(~std::uint64_t{0}, (one - UInt<64>{2}).value());
}

TEST(UIntTest, ExtractConcatAndInsertPreserveStaticWidths) {
  constexpr UInt<17> value{0x1a35b};
  constexpr UInt<5> low = bitExtract<5>(value, 0);
  constexpr UInt<3> middle = bitExtract<3>(value, 5);
  constexpr UInt<9> joined = bitConcat(middle, UInt<1>{1}, low);
  constexpr UInt<17> updated = bitInsert(value, UInt<3>{2}, 5);

  static_assert(decltype(low)::width == 5);
  static_assert(decltype(middle)::width == 3);
  static_assert(decltype(joined)::width == 9);
  EXPECT_EQ(low.value(), value.value() & 0x1f);
  EXPECT_EQ(middle.value(), (value.value() >> 5) & 0x7);
  EXPECT_EQ(joined.value(),
            (middle.value() << 6) | (uint64_t{1} << 5) | low.value());
  EXPECT_EQ(bitExtract<3>(updated, 5).value(), 2u);
  EXPECT_EQ(bitExtract<5>(updated, 0), low);
}

TEST(UIntTest, UnaryFullWidthConcatDoesNotShiftByStorageWidth) {
  constexpr UInt<64> value{0x8000000000000001ULL};
  constexpr UInt<64> joined = bitConcat(value);

  static_assert(joined.value() == value.value());
  EXPECT_EQ(value, joined);
}

TEST(PriorityEncodeTest, SimQueueBlockPreservesLowAndHighOrder) {
  SimQueue<UInt<13>> lowInput("low_input", 1, nullptr, 1);
  SimQueue<PriorityEncodeResult<13>> lowOutput("low_output", 2, nullptr, 1);
  PriorityEncode<13, true> low("low", 3, nullptr, lowInput, lowOutput);
  SimQueue<UInt<13>> highInput("high_input", 4, nullptr, 1);
  SimQueue<PriorityEncodeResult<13>> highOutput("high_output", 5, nullptr, 1);
  PriorityEncode<13, false> high("high", 6, nullptr, highInput, highOutput);
  const UInt<13> mask = (std::uint64_t{1} << 11) | (std::uint64_t{1} << 3);

  ASSERT_TRUE(lowInput.proposePush(mask));
  ASSERT_TRUE(highInput.proposePush(mask));
  lowInput.doXfer({0, 0});
  highInput.doXfer({0, 0});
  low.doWork({1, 0});
  high.doWork({1, 0});
  lowOutput.doXfer({1, 0});
  highOutput.doXfer({1, 0});
  lowInput.doXfer({1, 0});
  highInput.doXfer({1, 0});

  ASSERT_EQ(lowOutput.committedSize(), 1u);
  ASSERT_EQ(highOutput.committedSize(), 1u);
  EXPECT_EQ(lowOutput.peek()->index.value(), 3u);
  EXPECT_TRUE(static_cast<bool>(lowOutput.peek()->valid));
  EXPECT_EQ(highOutput.peek()->index.value(), 11u);
  EXPECT_TRUE(static_cast<bool>(highOutput.peek()->valid));
}

TEST(PopcountTest, ExactWidthsAndSimQueueBlockAgree) {
  static_assert(PopcountWidth<1> == 1);
  static_assert(PopcountWidth<13> == 4);
  static_assert(PopcountWidth<64> == 7);
  EXPECT_EQ(populationCount(UInt<1>{1}).value(), 1u);
  EXPECT_EQ(populationCount(UInt<13>{0x1123}).value(), 5u);
  EXPECT_EQ(populationCount(UInt<64>{~std::uint64_t{0}}).value(), 64u);

  SimQueue<UInt<13>> input("input", 1, nullptr, 1);
  SimQueue<UInt<4>> output("output", 2, nullptr, 1);
  Popcount<13> block("popcount", 3, nullptr, input, output);
  ASSERT_TRUE(input.proposePush(UInt<13>{0x1123}));
  input.doXfer({0, 0});
  block.doWork({1, 0});
  input.doXfer({1, 0});
  output.doXfer({1, 0});
  ASSERT_NE(output.peek(), nullptr);
  EXPECT_EQ(output.peek()->value(), 5u);
}

TEST(CountLeadingZerosTest, ExactWidthsAndSimQueueBlockAgree) {
  static_assert(CountZerosWidth<1> == 1);
  static_assert(CountZerosWidth<13> == 4);
  static_assert(CountZerosWidth<64> == 7);
  EXPECT_EQ(countLeadingZeros(UInt<1>{0}).value(), 1u);
  EXPECT_EQ(countLeadingZeros(UInt<1>{1}).value(), 0u);
  EXPECT_EQ(countLeadingZeros(UInt<13>{0}).value(), 13u);
  EXPECT_EQ(countLeadingZeros(UInt<13>{0x0123}).value(), 4u);
  EXPECT_EQ(countLeadingZeros(UInt<64>{1}).value(), 63u);
  EXPECT_EQ(countTrailingZeros(UInt<1>{0}).value(), 1u);
  EXPECT_EQ(countTrailingZeros(UInt<13>{0}).value(), 13u);
  EXPECT_EQ(countTrailingZeros(UInt<13>{0x0120}).value(), 5u);
  EXPECT_EQ(countTrailingZeros(UInt<64>{std::uint64_t{1} << 63}).value(), 63u);

  SimQueue<UInt<13>> input("clz_input", 4, nullptr, 1);
  SimQueue<UInt<4>> output("clz_output", 5, nullptr, 1);
  CountLeadingZeros<13> block("count_leading_zeros", 6, nullptr, input, output);
  ASSERT_TRUE(input.proposePush(UInt<13>{0x0123}));
  input.doXfer({0, 0});
  block.doWork({1, 0});
  input.doXfer({1, 0});
  output.doXfer({1, 0});
  ASSERT_NE(output.peek(), nullptr);
  EXPECT_EQ(output.peek()->value(), 4u);

  SimQueue<UInt<13>> trailingInput("ctz_input", 7, nullptr, 1);
  SimQueue<UInt<4>> trailingOutput("ctz_output", 8, nullptr, 1);
  CountTrailingZeros<13> trailingBlock("count_trailing_zeros", 9, nullptr,
                                       trailingInput, trailingOutput);
  ASSERT_TRUE(trailingInput.proposePush(UInt<13>{0x0120}));
  trailingInput.doXfer({0, 0});
  trailingBlock.doWork({1, 0});
  trailingInput.doXfer({1, 0});
  trailingOutput.doXfer({1, 0});
  ASSERT_NE(trailingOutput.peek(), nullptr);
  EXPECT_EQ(trailingOutput.peek()->value(), 5u);
}

} // namespace
} // namespace gfsim
