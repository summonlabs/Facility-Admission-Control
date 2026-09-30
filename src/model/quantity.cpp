// Facility Admission Control - facility quantities.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/quantity.hpp"

#include "fac/core/checked.hpp"
#include "fac/core/limits.hpp"
#include "detail/enum_codec.hpp"

namespace fac {
namespace {

constexpr std::pair<std::string_view, Dimension> kDimensionNames[] = {
    {"power", Dimension::power},
    {"cooling", Dimension::cooling},
    {"space", Dimension::space},
    {"slots", Dimension::slots},
};

}  // namespace

const char* to_string(Dimension dimension) noexcept {
  return detail::enum_to_string(dimension, kDimensionNames, "unknown_dimension");
}

const char* unit_label(Dimension dimension) noexcept {
  switch (dimension) {
    case Dimension::power:
    case Dimension::cooling:
      return "W";
    case Dimension::space:
      return "RU";
    case Dimension::slots:
      return "slots";
  }
  return "?";
}

Result<Dimension> dimension_from_string(std::string_view text) {
  return detail::enum_from_string(text, kDimensionNames, "dimension");
}

const std::array<Dimension, kDimensionCount>& all_dimensions() {
  static const std::array<Dimension, kDimensionCount> kAll = {
      Dimension::power, Dimension::cooling, Dimension::space, Dimension::slots};
  return kAll;
}

AmountVector AmountVector::unknown() { return AmountVector(); }

Status AmountVector::set(Dimension dimension, std::uint64_t amount) {
  if (amount > kMaxQuantity) {
    return make_error(ErrorCode::out_of_range, "quantity exceeds the supported maximum");
  }
  values_[static_cast<std::size_t>(dimension)] = amount;
  return Status::success();
}

bool AmountVector::has(Dimension dimension) const {
  return values_[static_cast<std::size_t>(dimension)].has_value();
}

std::optional<std::uint64_t> AmountVector::get(Dimension dimension) const {
  return values_[static_cast<std::size_t>(dimension)];
}

bool AmountVector::all_present() const {
  for (const auto& value : values_) {
    if (!value.has_value()) {
      return false;
    }
  }
  return true;
}

bool AmountVector::any_present() const {
  for (const auto& value : values_) {
    if (value.has_value()) {
      return true;
    }
  }
  return false;
}

std::size_t AmountVector::present_count() const {
  std::size_t count = 0;
  for (const auto& value : values_) {
    if (value.has_value()) {
      ++count;
    }
  }
  return count;
}

std::vector<Dimension> AmountVector::missing() const {
  std::vector<Dimension> result;
  for (const Dimension dimension : all_dimensions()) {
    if (!has(dimension)) {
      result.push_back(dimension);
    }
  }
  return result;
}

Result<AmountVector> AmountVector::checked_add(const AmountVector& other) const {
  AmountVector result;
  for (const Dimension dimension : all_dimensions()) {
    const auto left = get(dimension);
    const auto right = other.get(dimension);
    if (!left.has_value() || !right.has_value()) {
      return make_error(ErrorCode::invalid_argument, "cannot add a vector with an unmeasured dimension");
    }
    std::uint64_t sum = 0;
    if (add_overflow(left.value(), right.value(), sum) || sum > kMaxQuantity) {
      return make_error(ErrorCode::overflow, "quantity addition overflowed the supported range");
    }
    const Status status = result.set(dimension, sum);
    if (!status.ok()) {
      return status.error();
    }
  }
  return result;
}

bool AmountVector::all_less_or_equal(const AmountVector& other) const {
  for (const Dimension dimension : all_dimensions()) {
    const auto left = get(dimension);
    const auto right = other.get(dimension);
    if (!left.has_value() || !right.has_value()) {
      return false;
    }
    if (left.value() > right.value()) {
      return false;
    }
  }
  return true;
}

bool AmountVector::all_greater_than(const AmountVector& other) const {
  for (const Dimension dimension : all_dimensions()) {
    const auto left = get(dimension);
    const auto right = other.get(dimension);
    if (!left.has_value() || !right.has_value()) {
      return false;
    }
    if (left.value() <= right.value()) {
      return false;
    }
  }
  return true;
}

void AmountVector::encode(codec::Writer& writer) const {
  for (const auto& value : values_) {
    writer.boolean(value.has_value());
    writer.u64(value.value_or(0));
  }
}

Result<AmountVector> AmountVector::decode(codec::Reader& reader) {
  AmountVector result;
  for (const Dimension dimension : all_dimensions()) {
    auto present = reader.boolean();
    if (!present.ok()) {
      return present.error();
    }
    auto amount = reader.u64();
    if (!amount.ok()) {
      return amount.error();
    }
    if (present.value()) {
      if (amount.value() > kMaxQuantity) {
        return make_error(ErrorCode::out_of_range, "quantity in the input exceeds the supported maximum");
      }
      result.values_[static_cast<std::size_t>(dimension)] = amount.value();
    } else if (amount.value() != 0) {
      return make_error(ErrorCode::malformed_input, "absent quantity carries a non-zero value");
    }
  }
  return result;
}

}  // namespace fac
