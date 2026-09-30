// Facility Admission Control - tenancy, entitlement and service class evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The Tenant Registry owns tenant identity and status, Resource Envelope owns
// what a tenant is entitled to, and Service Class Registry owns what a class of
// service obliges the facility to preserve. This repository consumes all three
// as generation-stamped snapshots and never invents a missing one.

#ifndef FAC_MODEL_TENANCY_HPP
#define FAC_MODEL_TENANCY_HPP

#include <cstdint>
#include <string_view>

#include "fac/codec/codec.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/model/capacity.hpp"
#include "fac/model/quantity.hpp"

namespace fac {

enum class TenantStatus : std::uint8_t {
  active = 0,
  suspended = 1,
  closed = 2,
  unknown = 3,
};

[[nodiscard]] const char* to_string(TenantStatus status) noexcept;
[[nodiscard]] Result<TenantStatus> tenant_status_from_string(std::string_view text);

struct TenantSnapshot {
  TenantId tenant;
  TenantGeneration generation;
  Timestamp observed_at;
  TenantStatus status = TenantStatus::unknown;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<TenantSnapshot> decode(codec::Reader& reader);

  friend bool operator==(const TenantSnapshot&, const TenantSnapshot&) = default;
};

// The Resource Envelope is an entitlement, not a measurement. limit is the
// absolute cap the tenant may hold; consumed is what the tenant already holds
// elsewhere. Either may be unmeasured per dimension, and an unmeasured value
// cannot be treated as unlimited.
struct EnvelopeSnapshot {
  EnvelopeId envelope;
  TenantId tenant;
  EnvelopeGeneration generation;
  Timestamp observed_at;
  AmountVector limit;
  AmountVector consumed;

  [[nodiscard]] Status validate() const;

  // Remaining entitlement per dimension, absent where either side is
  // unmeasured or where consumption already exceeds the limit.
  [[nodiscard]] AmountVector remaining() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<EnvelopeSnapshot> decode(codec::Reader& reader);

  friend bool operator==(const EnvelopeSnapshot&, const EnvelopeSnapshot&) = default;
};

struct ServiceClassObligations {
  // minimum_redundancy is stated explicitly: an unstated obligation is not the
  // same as an obligation of "none".
  RedundancyLevel minimum_redundancy = RedundancyLevel::none;
  bool minimum_redundancy_stated = false;

  // Protected headroom the facility must still hold after this commitment.
  AmountVector required_protected_headroom;

  // When true, a maintenance window that overlaps the commitment window in the
  // same scope blocks admission.
  bool requires_maintenance_clearance = false;

  // When true the service class tolerates overcommit, still subject to the
  // facility policy engine explicitly permitting it.
  bool permits_overcommit = false;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<ServiceClassObligations> decode(codec::Reader& reader);

  friend bool operator==(const ServiceClassObligations&, const ServiceClassObligations&) = default;
};

struct ServiceClassSnapshot {
  ServiceClassId service_class;
  ServiceClassGeneration generation;
  Timestamp observed_at;
  ServiceClassObligations obligations;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<ServiceClassSnapshot> decode(codec::Reader& reader);

  friend bool operator==(const ServiceClassSnapshot&, const ServiceClassSnapshot&) = default;
};

}  // namespace fac

#endif  // FAC_MODEL_TENANCY_HPP
