// Facility Admission Control - tenancy, entitlement and service class evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/tenancy.hpp"

#include "detail/enum_codec.hpp"

namespace fac {
namespace {

constexpr std::pair<std::string_view, TenantStatus> kStatusNames[] = {
    {"active", TenantStatus::active},
    {"suspended", TenantStatus::suspended},
    {"closed", TenantStatus::closed},
    {"unknown", TenantStatus::unknown},
};

}  // namespace

const char* to_string(TenantStatus status) noexcept {
  return detail::enum_to_string(status, kStatusNames, "unknown_status");
}

Result<TenantStatus> tenant_status_from_string(std::string_view text) {
  return detail::enum_from_string(text, kStatusNames, "tenant status");
}

Status TenantSnapshot::validate() const {
  if (tenant.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "tenant snapshot carries no tenant identity");
  }
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "tenant snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "tenant snapshot carries no observation time");
  }
  return Status::success();
}

void TenantSnapshot::encode(codec::Writer& writer) const {
  writer.ident(tenant);
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  writer.u8(static_cast<std::uint8_t>(status));
}

Result<TenantSnapshot> TenantSnapshot::decode(codec::Reader& reader) {
  TenantSnapshot snapshot;
  auto tenant = reader.ident<struct TenantIdTag>();
  if (!tenant.ok()) return tenant.error();
  snapshot.tenant = tenant.value();
  auto generation = reader.counter<struct TenantGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "tenant observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto status = detail::read_enum<TenantStatus>(reader, static_cast<std::uint8_t>(TenantStatus::unknown),
                                                "tenant status");
  if (!status.ok()) return status.error();
  snapshot.status = status.value();
  const Status result = snapshot.validate();
  if (!result.ok()) {
    return result.error();
  }
  return snapshot;
}

Status EnvelopeSnapshot::validate() const {
  if (envelope.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "envelope snapshot carries no envelope identity");
  }
  if (tenant.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "envelope snapshot carries no tenant identity");
  }
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "envelope snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "envelope snapshot carries no observation time");
  }
  return Status::success();
}

AmountVector EnvelopeSnapshot::remaining() const {
  AmountVector result;
  for (const Dimension dimension : all_dimensions()) {
    const auto stated_limit = this->limit.get(dimension);
    const auto stated_consumed = this->consumed.get(dimension);
    if (!stated_limit.has_value() || !stated_consumed.has_value()) {
      continue;
    }
    if (stated_consumed.value() > stated_limit.value()) {
      continue;
    }
    const Status status = result.set(dimension, stated_limit.value() - stated_consumed.value());
    if (!status.ok()) {
      continue;
    }
  }
  return result;
}

void EnvelopeSnapshot::encode(codec::Writer& writer) const {
  writer.ident(envelope);
  writer.ident(tenant);
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  limit.encode(writer);
  consumed.encode(writer);
}

Result<EnvelopeSnapshot> EnvelopeSnapshot::decode(codec::Reader& reader) {
  EnvelopeSnapshot snapshot;
  auto envelope = reader.ident<struct EnvelopeIdTag>();
  if (!envelope.ok()) return envelope.error();
  snapshot.envelope = envelope.value();
  auto tenant = reader.ident<struct TenantIdTag>();
  if (!tenant.ok()) return tenant.error();
  snapshot.tenant = tenant.value();
  auto generation = reader.counter<struct EnvelopeGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "envelope observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto limit = AmountVector::decode(reader);
  if (!limit.ok()) return limit.error();
  snapshot.limit = limit.take();
  auto consumed = AmountVector::decode(reader);
  if (!consumed.ok()) return consumed.error();
  snapshot.consumed = consumed.take();
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

Status ServiceClassObligations::validate() const {
  if (!minimum_redundancy_stated && minimum_redundancy != RedundancyLevel::none) {
    return make_error(ErrorCode::invalid_argument, "unstated redundancy obligation carries a level");
  }
  return Status::success();
}

void ServiceClassObligations::encode(codec::Writer& writer) const {
  writer.boolean(minimum_redundancy_stated);
  writer.u8(static_cast<std::uint8_t>(minimum_redundancy));
  required_protected_headroom.encode(writer);
  writer.boolean(requires_maintenance_clearance);
  writer.boolean(permits_overcommit);
}

Result<ServiceClassObligations> ServiceClassObligations::decode(codec::Reader& reader) {
  ServiceClassObligations obligations;
  auto stated = reader.boolean();
  if (!stated.ok()) return stated.error();
  obligations.minimum_redundancy_stated = stated.value();
  auto level = detail::read_enum<RedundancyLevel>(reader, static_cast<std::uint8_t>(RedundancyLevel::two_n),
                                                  "redundancy level");
  if (!level.ok()) return level.error();
  obligations.minimum_redundancy = level.value();
  auto headroom = AmountVector::decode(reader);
  if (!headroom.ok()) return headroom.error();
  obligations.required_protected_headroom = headroom.take();
  auto clearance = reader.boolean();
  if (!clearance.ok()) return clearance.error();
  obligations.requires_maintenance_clearance = clearance.value();
  auto overcommit = reader.boolean();
  if (!overcommit.ok()) return overcommit.error();
  obligations.permits_overcommit = overcommit.value();
  const Status status = obligations.validate();
  if (!status.ok()) {
    return status.error();
  }
  return obligations;
}

Status ServiceClassSnapshot::validate() const {
  if (service_class.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "service class snapshot carries no identity");
  }
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "service class snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "service class snapshot carries no observation time");
  }
  return obligations.validate();
}

void ServiceClassSnapshot::encode(codec::Writer& writer) const {
  writer.ident(service_class);
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  obligations.encode(writer);
}

Result<ServiceClassSnapshot> ServiceClassSnapshot::decode(codec::Reader& reader) {
  ServiceClassSnapshot snapshot;
  auto identity = reader.ident<struct ServiceClassIdTag>();
  if (!identity.ok()) return identity.error();
  snapshot.service_class = identity.value();
  auto generation = reader.counter<struct ServiceClassGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "service class observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto obligations = ServiceClassObligations::decode(reader);
  if (!obligations.ok()) return obligations.error();
  snapshot.obligations = obligations.take();
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

}  // namespace fac
