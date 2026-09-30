// Facility Admission Control - facility quantities.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Four dimensions are modelled, each in one exact integer base unit so that no
// decision depends on floating point rounding:
//
//   power    watts
//   cooling  watts of heat rejection
//   space    rack units
//   slots    countable installation slots
//
// An amount that was never measured is absent, never zero. AmountVector keeps
// that distinction all the way through evaluation, the decision record and the
// durable format, because turning an unmeasured value into zero is exactly how
// a capacity system admits a commitment it cannot prove fits.

#ifndef FAC_MODEL_QUANTITY_HPP
#define FAC_MODEL_QUANTITY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/error.hpp"

namespace fac {

enum class Dimension : std::uint8_t {
  power = 0,
  cooling = 1,
  space = 2,
  slots = 3,
};

inline constexpr std::size_t kDimensionCount = 4;

[[nodiscard]] const char* to_string(Dimension dimension) noexcept;
[[nodiscard]] const char* unit_label(Dimension dimension) noexcept;
[[nodiscard]] Result<Dimension> dimension_from_string(std::string_view text);
[[nodiscard]] const std::array<Dimension, kDimensionCount>& all_dimensions();

class AmountVector {
 public:
  AmountVector() = default;

  [[nodiscard]] static AmountVector unknown();

  // Refuses a value above kMaxQuantity, so a bounded summation of bounded
  // amounts cannot approach the 64-bit ceiling.
  [[nodiscard]] Status set(Dimension dimension, std::uint64_t amount);

  [[nodiscard]] bool has(Dimension dimension) const;
  [[nodiscard]] std::optional<std::uint64_t> get(Dimension dimension) const;
  [[nodiscard]] bool all_present() const;
  [[nodiscard]] bool any_present() const;
  [[nodiscard]] std::size_t present_count() const;

  // Dimensions with no measured amount, in canonical dimension order.
  [[nodiscard]] std::vector<Dimension> missing() const;

  // Element-wise addition of two vectors that are both fully present.
  [[nodiscard]] Result<AmountVector> checked_add(const AmountVector& other) const;

  // Element-wise comparison of two fully present vectors.
  [[nodiscard]] bool all_less_or_equal(const AmountVector& other) const;
  [[nodiscard]] bool all_greater_than(const AmountVector& other) const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<AmountVector> decode(codec::Reader& reader);

  friend bool operator==(const AmountVector&, const AmountVector&) = default;

 private:
  std::array<std::optional<std::uint64_t>, kDimensionCount> values_{};
};

}  // namespace fac

#endif  // FAC_MODEL_QUANTITY_HPP
