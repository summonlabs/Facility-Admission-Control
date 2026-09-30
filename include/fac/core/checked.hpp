// Facility Admission Control - checked arithmetic.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Generations, epochs, sequences, quantities and sizes are never allowed to
// wrap silently. Every operation that can leave the representable range is
// expressed here and reports the failure instead of producing a wrapped value.

#ifndef FAC_CORE_CHECKED_HPP
#define FAC_CORE_CHECKED_HPP

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace fac {

// Returns true and writes the sum when a + b is representable in T.
template <class T>
[[nodiscard]] constexpr bool add_overflow(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "add_overflow requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return true;
    }
  } else {
    if ((b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) ||
        (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b))) {
      return true;
    }
  }
  out = static_cast<T>(a + b);
  return false;
}

// Returns true and writes the difference when a - b is representable in T.
template <class T>
[[nodiscard]] constexpr bool sub_underflow(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "sub_underflow requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a < b) {
      return true;
    }
  } else {
    if ((b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) ||
        (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b))) {
      return true;
    }
  }
  out = static_cast<T>(a - b);
  return false;
}

// Returns true and writes the product when a * b is representable in T.
template <class T>
[[nodiscard]] constexpr bool mul_overflow(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "mul_overflow requires an integral type");
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
      return true;
    }
    out = static_cast<T>(a * b);
    return false;
  } else {
    const T max = std::numeric_limits<T>::max();
    const T min = std::numeric_limits<T>::min();
    if (a > 0) {
      if (b > 0) {
        if (a > max / b) return true;
      } else {
        if (b < min / a) return true;
      }
    } else {
      if (b > 0) {
        if (a < min / b) return true;
      } else {
        if (a != 0 && b < max / a) return true;
      }
    }
    out = static_cast<T>(a * b);
    return false;
  }
}

// Narrowing helpers. A value that does not fit is reported, never truncated.
template <class To, class From>
[[nodiscard]] constexpr bool narrow(From value, To& out) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>, "narrow requires integral types");
  // Comparisons happen in the common type of the two, which is the unsigned
  // type when the widths are equal. The negative case is therefore handled
  // first, and the bounds are cast into that same type rather than into a type
  // chosen from the source alone.
  using Common = std::common_type_t<From, To>;
  if constexpr (std::is_signed_v<From> && std::is_unsigned_v<To>) {
    if (value < 0) {
      return false;
    }
  }
  if (static_cast<Common>(value) > static_cast<Common>(std::numeric_limits<To>::max())) {
    return false;
  }
  if constexpr (std::is_signed_v<To>) {
    if (static_cast<Common>(value) < static_cast<Common>(std::numeric_limits<To>::min())) {
      return false;
    }
  }
  out = static_cast<To>(value);
  return true;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_add_u64(std::uint64_t a,
                                                                    std::uint64_t b) noexcept {
  std::uint64_t out = 0;
  if (add_overflow(a, b, out)) {
    return std::nullopt;
  }
  return out;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> checked_sub_u64(std::uint64_t a,
                                                                    std::uint64_t b) noexcept {
  std::uint64_t out = 0;
  if (sub_underflow(a, b, out)) {
    return std::nullopt;
  }
  return out;
}

}  // namespace fac

#endif  // FAC_CORE_CHECKED_HPP
