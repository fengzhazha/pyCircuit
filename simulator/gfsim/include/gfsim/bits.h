#ifndef GFSIM_BITS_H
#define GFSIM_BITS_H

#include "gfsim/packet.h"

#include <bit>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace gfsim {

/// Exact-width unsigned circuit value. Every producing operation truncates
/// modulo 2^Width, independent of C++ integer-promotion rules.
template <unsigned Width> class UInt {
  static_assert(Width > 0 && Width <= 64,
                "gfsim::UInt width must be in [1, 64]");

public:
  using storage_type = std::uint64_t;
  static constexpr unsigned width = Width;

  constexpr UInt() = default;

  template <typename T>
    requires(std::is_integral_v<T>)
  constexpr UInt(T value) : value_(static_cast<storage_type>(value) & mask()) {}

  constexpr storage_type value() const { return value_; }
  template <typename T>
    requires(std::is_integral_v<T>)
  constexpr explicit operator T() const {
    return static_cast<T>(value_);
  }
  constexpr explicit(Width != 1) operator bool() const { return value_ != 0; }

  friend constexpr UInt operator+(UInt lhs, UInt rhs) {
    return UInt(lhs.value_ + rhs.value_);
  }
  friend constexpr UInt operator-(UInt lhs, UInt rhs) {
    return UInt(lhs.value_ - rhs.value_);
  }
  friend constexpr UInt operator*(UInt lhs, UInt rhs) {
    return UInt(lhs.value_ * rhs.value_);
  }
  friend constexpr UInt operator/(UInt lhs, UInt rhs) {
    return rhs.value_ == 0 ? UInt{} : UInt(lhs.value_ / rhs.value_);
  }
  friend constexpr UInt operator&(UInt lhs, UInt rhs) {
    return UInt(lhs.value_ & rhs.value_);
  }
  friend constexpr UInt operator|(UInt lhs, UInt rhs) {
    return UInt(lhs.value_ | rhs.value_);
  }
  friend constexpr UInt operator^(UInt lhs, UInt rhs) {
    return UInt(lhs.value_ ^ rhs.value_);
  }
  friend constexpr UInt operator~(UInt value) { return UInt(~value.value_); }
  friend constexpr UInt operator<<(UInt lhs, UInt rhs) {
    return rhs.value_ >= Width ? UInt{} : UInt(lhs.value_ << rhs.value_);
  }
  friend constexpr UInt operator>>(UInt lhs, UInt rhs) {
    return rhs.value_ >= Width ? UInt{} : UInt(lhs.value_ >> rhs.value_);
  }

  constexpr std::int64_t signedValue() const {
    if constexpr (Width == 64)
      return std::bit_cast<std::int64_t>(value_);
    if ((value_ & (storage_type{1} << (Width - 1))) == 0)
      return static_cast<std::int64_t>(value_);
    const storage_type magnitude = ((~value_) & mask()) + 1;
    return -static_cast<std::int64_t>(magnitude);
  }

  constexpr UInt arithmeticShiftRight(UInt rhs) const {
    if (rhs.value_ == 0)
      return *this;
    const bool negative = (value_ & (storage_type{1} << (Width - 1))) != 0;
    if (rhs.value_ >= Width)
      return negative ? UInt(mask()) : UInt{};
    storage_type shifted = value_ >> rhs.value_;
    if (negative)
      shifted |= mask() ^ ((storage_type{1} << (Width - rhs.value_)) - 1);
    return UInt(shifted);
  }

  template <std::integral T> friend constexpr UInt operator+(UInt lhs, T rhs) {
    return lhs + UInt(rhs);
  }
  template <std::integral T> friend constexpr UInt operator-(UInt lhs, T rhs) {
    return lhs - UInt(rhs);
  }
  template <std::integral T> friend constexpr UInt operator*(UInt lhs, T rhs) {
    return lhs * UInt(rhs);
  }
  template <std::integral T> friend constexpr UInt operator&(UInt lhs, T rhs) {
    return lhs & UInt(rhs);
  }
  template <std::integral T> friend constexpr UInt operator|(UInt lhs, T rhs) {
    return lhs | UInt(rhs);
  }
  template <std::integral T> friend constexpr UInt operator^(UInt lhs, T rhs) {
    return lhs ^ UInt(rhs);
  }
  template <std::integral T> friend constexpr UInt operator<<(UInt lhs, T rhs) {
    if constexpr (std::signed_integral<T>)
      if (rhs < 0)
        return UInt{};
    const storage_type amount = static_cast<storage_type>(rhs);
    return amount >= Width ? UInt{} : UInt(lhs.value_ << amount);
  }
  template <std::integral T> friend constexpr UInt operator>>(UInt lhs, T rhs) {
    if constexpr (std::signed_integral<T>)
      if (rhs < 0)
        return UInt{};
    const storage_type amount = static_cast<storage_type>(rhs);
    return amount >= Width ? UInt{} : UInt(lhs.value_ >> amount);
  }

  friend constexpr bool operator==(UInt, UInt) = default;
  friend constexpr bool operator<(UInt lhs, UInt rhs) {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(UInt lhs, UInt rhs) { return rhs < lhs; }
  friend constexpr bool operator<=(UInt lhs, UInt rhs) { return !(rhs < lhs); }
  friend constexpr bool operator>=(UInt lhs, UInt rhs) { return !(lhs < rhs); }

  template <std::integral T> friend constexpr bool operator==(UInt lhs, T rhs) {
    return lhs == UInt(rhs);
  }
  template <std::integral T> friend constexpr bool operator<(UInt lhs, T rhs) {
    return lhs < UInt(rhs);
  }
  template <std::integral T> friend constexpr bool operator>(UInt lhs, T rhs) {
    return lhs > UInt(rhs);
  }
  template <std::integral T> friend constexpr bool operator<=(UInt lhs, T rhs) {
    return lhs <= UInt(rhs);
  }
  template <std::integral T> friend constexpr bool operator>=(UInt lhs, T rhs) {
    return lhs >= UInt(rhs);
  }

private:
  static constexpr storage_type mask() {
    if constexpr (Width == 64)
      return ~storage_type{0};
    else
      return (storage_type{1} << Width) - 1;
  }

  storage_type value_ = 0;
};

template <typename T> struct IsUInt : std::false_type {};
template <unsigned Width> struct IsUInt<UInt<Width>> : std::true_type {};

template <typename T>
concept IntegralLike = std::integral<T> || IsUInt<std::remove_cv_t<T>>::value;

template <typename T>
concept UnsignedIntegralLike =
    std::unsigned_integral<T> || IsUInt<std::remove_cv_t<T>>::value;

template <unsigned Width>
constexpr std::int64_t signedValue(UInt<Width> value) {
  return value.signedValue();
}

template <std::integral T> constexpr std::int64_t signedValue(T value) {
  return static_cast<std::int64_t>(value);
}

template <unsigned ResultWidth, unsigned InputWidth>
constexpr UInt<ResultWidth> bitExtract(UInt<InputWidth> value, size_t lsb) {
  static_assert(ResultWidth > 0 && ResultWidth <= InputWidth);
  return lsb + ResultWidth > InputWidth
             ? UInt<ResultWidth>{}
             : UInt<ResultWidth>{value.value() >> lsb};
}

template <unsigned... Widths>
  requires(sizeof...(Widths) > 0 && ((Widths + ...) <= 64))
constexpr UInt<(Widths + ...)> bitConcat(UInt<Widths>... values) {
  uint64_t result = 0;
  auto append = [&]<unsigned Width>(UInt<Width> value) {
    if constexpr (Width == 64)
      result = value.value();
    else
      result = (result << Width) | value.value();
  };
  (append(values), ...);
  return UInt<(Widths + ...)>{result};
}

template <unsigned BaseWidth, unsigned ValueWidth>
constexpr UInt<BaseWidth> bitInsert(UInt<BaseWidth> base,
                                    UInt<ValueWidth> value, size_t lsb) {
  static_assert(ValueWidth <= BaseWidth);
  if (lsb + ValueWidth > BaseWidth)
    return base;
  const uint64_t valueMask =
      ValueWidth == 64 ? ~uint64_t{0} : (uint64_t{1} << ValueWidth) - 1;
  const uint64_t mask = valueMask << lsb;
  return UInt<BaseWidth>{(base.value() & ~mask) |
                         ((value.value() & valueMask) << lsb)};
}

template <unsigned Width> struct PacketTraits<UInt<Width>> {
  static constexpr bool isPacket = false;
  static constexpr std::string_view schema = {};
  static constexpr size_t serializedSize = (Width + 7) / 8;
  static constexpr size_t maximumSerializedSize = serializedSize;
  static constexpr size_t alignment = 1;
  static constexpr PacketEndianness endianness = PacketEndianness::Little;
  static constexpr std::array<PacketField, 0> fields{};
  static constexpr std::optional<std::string_view> routingField = std::nullopt;
  static constexpr std::optional<std::string_view> correlationField =
      std::nullopt;
};

} // namespace gfsim

#endif // GFSIM_BITS_H
