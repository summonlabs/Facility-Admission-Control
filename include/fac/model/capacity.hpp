// Facility Admission Control - capacity and redundancy evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These snapshots are produced by the systems that own the measurement:
// Facility Capacity, Rack Capacity, Power Capacity, Cooling Capacity, Space
// Capacity and the redundancy/failure-domain authorities. This repository
// consumes them and never derives one from another.
//
// CapacitySnapshot reports what the owner has measured. RedundancySnapshot
// reports the protection posture, which is what stops remaining capacity from
// being spent. There is no fallback between the two: missing redundancy
// evidence is a refusal to conclude, not an inference of "no protection" and
// certainly not an inference of "protected".

#ifndef FAC_MODEL_CAPACITY_HPP
#define FAC_MODEL_CAPACITY_HPP

#include <cstdint>
#include <string_view>

#include "fac/codec/codec.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/model/quantity.hpp"

namespace fac {

struct CapacitySnapshot {
  CapacityGeneration generation;
  Timestamp observed_at;
  AmountVector total;
  AmountVector committed;
  AmountVector reserved;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<CapacitySnapshot> decode(codec::Reader& reader);

  friend bool operator==(const CapacitySnapshot&, const CapacitySnapshot&) = default;
};

enum class RedundancyLevel : std::uint8_t {
  none = 0,
  n_plus_one = 1,
  n_plus_two = 2,
  two_n = 3,
};

[[nodiscard]] const char* to_string(RedundancyLevel level) noexcept;
[[nodiscard]] Result<RedundancyLevel> redundancy_level_from_string(std::string_view text);

// True when observed protection is at least as strong as the requirement.
[[nodiscard]] bool redundancy_at_least(RedundancyLevel observed, RedundancyLevel required) noexcept;

struct RedundancySnapshot {
  RedundancyGeneration generation;
  Timestamp observed_at;

  // An unmeasured level is not "none": level_stated distinguishes the two.
  RedundancyLevel level = RedundancyLevel::none;
  bool level_stated = false;

  // Capacity that must remain unconsumed for the protection posture to hold.
  // An absent dimension is unmeasured.
  AmountVector protected_headroom;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<RedundancySnapshot> decode(codec::Reader& reader);

  friend bool operator==(const RedundancySnapshot&, const RedundancySnapshot&) = default;
};

}  // namespace fac

#endif  // FAC_MODEL_CAPACITY_HPP
