// Facility Admission Control - strong identities and counters.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A RequestId and a GrantId are both 128-bit values, and confusing them is a
// class of defect the type system can remove entirely. Ident128<Tag> gives each
// identity domain its own distinct type with one shared implementation, and
// Counter<Tag> does the same for generations, epochs, sequences and revisions.
//
// A zero identity is "unset", and no API here invents an identity: callers
// derive them from canonical request bytes or from a durable record.

#ifndef FAC_CORE_IDENT_HPP
#define FAC_CORE_IDENT_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fac/core/error.hpp"
#include "fac/core/limits.hpp"

namespace fac {

// Shared parsing helper. Defined out of line so every instantiation shares one
// implementation. Writes the two halves on success.
[[nodiscard]] Result<std::pair<std::uint64_t, std::uint64_t>> ident_parse_hex(std::string_view text);
[[nodiscard]] std::string ident_format_hex(std::uint64_t hi, std::uint64_t lo);

template <class Tag>
class Ident128 {
 public:
  constexpr Ident128() noexcept = default;
  constexpr Ident128(std::uint64_t hi, std::uint64_t lo) noexcept : hi_(hi), lo_(lo) {}

  [[nodiscard]] static Result<Ident128> from_hex(std::string_view text) {
    auto parsed = ident_parse_hex(text);
    if (!parsed.ok()) {
      return parsed.error();
    }
    const auto& parts = parsed.value();
    return Ident128(parts.first, parts.second);
  }

  [[nodiscard]] constexpr std::uint64_t hi() const noexcept { return hi_; }
  [[nodiscard]] constexpr std::uint64_t lo() const noexcept { return lo_; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return hi_ == 0 && lo_ == 0; }

  [[nodiscard]] std::string to_string() const { return ident_format_hex(hi_, lo_); }

  friend constexpr bool operator==(const Ident128&, const Ident128&) noexcept = default;
  friend constexpr auto operator<=>(const Ident128&, const Ident128&) noexcept = default;

 private:
  std::uint64_t hi_ = 0;
  std::uint64_t lo_ = 0;
};

template <class Tag>
struct Ident128Hash {
  [[nodiscard]] std::size_t operator()(const Ident128<Tag>& id) const noexcept {
    // FNV-1a style mixing of the two halves; stable across runs and platforms.
    std::uint64_t h = 1469598103934665603ull;
    const std::uint64_t parts[2] = {id.hi(), id.lo()};
    for (const std::uint64_t part : parts) {
      for (int byte = 0; byte < 8; ++byte) {
        h ^= (part >> (byte * 8)) & 0xFFull;
        h *= 1099511628211ull;
      }
    }
    return static_cast<std::size_t>(h);
  }
};

// A monotonic counter with a domain tag. Values are bounded by kMaxCounter so
// that a bounded sequence of increments can never approach the 64-bit ceiling.
template <class Tag>
class Counter {
 public:
  constexpr Counter() noexcept = default;
  constexpr explicit Counter(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static Result<Counter> from_value(std::uint64_t value) {
    if (value > kMaxCounter) {
      return make_error(ErrorCode::out_of_range, "counter value exceeds the supported maximum");
    }
    return Counter(value);
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  // Refuses to wrap. Callers that can reach the ceiling must handle the error.
  [[nodiscard]] Result<Counter> checked_next() const {
    if (value_ >= kMaxCounter) {
      return make_error(ErrorCode::overflow, "counter cannot be advanced without wrapping");
    }
    return Counter(value_ + 1);
  }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  friend constexpr bool operator==(const Counter&, const Counter&) noexcept = default;
  friend constexpr auto operator<=>(const Counter&, const Counter&) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

// ---------------------------------------------------------------------------
// Identity domains
// ---------------------------------------------------------------------------

using RequestId = Ident128<struct RequestIdTag>;
using GrantId = Ident128<struct GrantIdTag>;
using CommitmentId = Ident128<struct CommitmentIdTag>;
using IntentId = Ident128<struct IntentIdTag>;
using FacilityId = Ident128<struct FacilityIdTag>;
using RackId = Ident128<struct RackIdTag>;
using ZoneId = Ident128<struct ZoneIdTag>;
using TenantId = Ident128<struct TenantIdTag>;
using EnvelopeId = Ident128<struct EnvelopeIdTag>;
using ServiceClassId = Ident128<struct ServiceClassIdTag>;
using IncidentId = Ident128<struct IncidentIdTag>;
using MaintenanceWindowId = Ident128<struct MaintenanceWindowIdTag>;
using ReservationId = Ident128<struct ReservationIdTag>;
using OwnerId = Ident128<struct OwnerIdTag>;
using PolicyId = Ident128<struct PolicyIdTag>;

// ---------------------------------------------------------------------------
// Counter domains
// ---------------------------------------------------------------------------

using ControlEpoch = Counter<struct ControlEpochTag>;
using LedgerSequence = Counter<struct LedgerSequenceTag>;
using GrantRevision = Counter<struct GrantRevisionTag>;
using CapacityGeneration = Counter<struct CapacityGenerationTag>;
using RedundancyGeneration = Counter<struct RedundancyGenerationTag>;
using TenantGeneration = Counter<struct TenantGenerationTag>;
using EnvelopeGeneration = Counter<struct EnvelopeGenerationTag>;
using ServiceClassGeneration = Counter<struct ServiceClassGenerationTag>;
using MaintenanceGeneration = Counter<struct MaintenanceGenerationTag>;
using IncidentGeneration = Counter<struct IncidentGenerationTag>;
using PlacementGeneration = Counter<struct PlacementGenerationTag>;
using PolicyGeneration = Counter<struct PolicyGenerationTag>;

}  // namespace fac

#endif  // FAC_CORE_IDENT_HPP
