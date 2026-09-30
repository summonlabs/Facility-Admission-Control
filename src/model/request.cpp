// Facility Admission Control - the admission request.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/request.hpp"

#include "detail/enum_codec.hpp"

namespace fac {

void GenerationPin::encode(codec::Writer& writer) const {
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u64(generation);
}

Result<GenerationPin> GenerationPin::decode(codec::Reader& reader) {
  auto kind = detail::read_enum<EvidenceKind>(reader, static_cast<std::uint8_t>(kEvidenceKindCount - 1),
                                              "evidence kind");
  if (!kind.ok()) {
    return kind.error();
  }
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  GenerationPin pin;
  pin.kind = kind.value();
  pin.generation = generation.value();
  return pin;
}

Status AdmissionRequest::validate() const {
  if (request_id.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "request carries no request identity");
  }
  if (tenant.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "request carries no tenant identity");
  }
  if (service_class.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "request carries no service class identity");
  }
  if (envelope.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "request carries no envelope identity");
  }
  if (scope.facility.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "request carries no facility identity");
  }
  if (requested_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "request carries no request time");
  }
  if (commitment_start.is_unset() || commitment_end.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "request carries no commitment window");
  }
  if (!(commitment_start < commitment_end)) {
    return make_error(ErrorCode::invalid_argument, "commitment window end is not after its start");
  }
  if (expected_epoch.has_value() && expected_epoch->is_zero()) {
    return make_error(ErrorCode::invalid_argument, "request pins a zero control epoch");
  }
  for (std::size_t i = 0; i < pins.size(); ++i) {
    if (pins[i].generation == 0) {
      return make_error(ErrorCode::invalid_argument, "request pins a zero generation");
    }
    if (i > 0 && static_cast<std::uint8_t>(pins[i - 1].kind) >= static_cast<std::uint8_t>(pins[i].kind)) {
      return make_error(ErrorCode::invalid_argument, "request pins are not in ascending kind order");
    }
  }
  return Status::success();
}

Duration AdmissionRequest::commitment_duration() const {
  const Nanos difference = commitment_end.unix_nanos() - commitment_start.unix_nanos();
  return Duration(difference > 0 ? difference : 0);
}

std::optional<std::uint64_t> AdmissionRequest::pinned_generation(EvidenceKind kind) const {
  for (const auto& pin : pins) {
    if (pin.kind == kind) {
      return pin.generation;
    }
  }
  return std::nullopt;
}

Digest256 AdmissionRequest::digest() const {
  codec::Writer writer;
  encode(writer);
  return Digest256::of(writer.span());
}

void AdmissionRequest::encode(codec::Writer& writer) const {
  writer.ident(request_id);
  writer.ident(tenant);
  writer.ident(service_class);
  writer.ident(envelope);
  scope.encode(writer);
  demand.encode(writer);
  writer.i64(requested_at.unix_nanos());
  writer.i64(commitment_start.unix_nanos());
  writer.i64(commitment_end.unix_nanos());
  writer.boolean(expected_epoch.has_value());
  writer.counter(expected_epoch.value_or(ControlEpoch{}));
  writer.u32(static_cast<std::uint32_t>(pins.size()));
  for (const auto& pin : pins) {
    pin.encode(writer);
  }
}

Result<AdmissionRequest> AdmissionRequest::decode(codec::Reader& reader) {
  AdmissionRequest request;
  auto request_id = reader.ident<struct RequestIdTag>();
  if (!request_id.ok()) return request_id.error();
  request.request_id = request_id.value();
  auto tenant = reader.ident<struct TenantIdTag>();
  if (!tenant.ok()) return tenant.error();
  request.tenant = tenant.value();
  auto service_class = reader.ident<struct ServiceClassIdTag>();
  if (!service_class.ok()) return service_class.error();
  request.service_class = service_class.value();
  auto envelope = reader.ident<struct EnvelopeIdTag>();
  if (!envelope.ok()) return envelope.error();
  request.envelope = envelope.value();
  auto scope = TargetScope::decode(reader);
  if (!scope.ok()) return scope.error();
  request.scope = scope.take();
  auto demand = AmountVector::decode(reader);
  if (!demand.ok()) return demand.error();
  request.demand = demand.take();
  auto requested_at = reader.i64();
  if (!requested_at.ok()) return requested_at.error();
  auto start = reader.i64();
  if (!start.ok()) return start.error();
  auto end = reader.i64();
  if (!end.ok()) return end.error();
  if (!is_valid_timestamp_nanos(requested_at.value()) || !is_valid_timestamp_nanos(start.value()) ||
      !is_valid_timestamp_nanos(end.value())) {
    return make_error(ErrorCode::out_of_range, "request carries a time outside the supported range");
  }
  request.requested_at = Timestamp(requested_at.value());
  request.commitment_start = Timestamp(start.value());
  request.commitment_end = Timestamp(end.value());
  auto has_epoch = reader.boolean();
  if (!has_epoch.ok()) return has_epoch.error();
  auto epoch = reader.counter<struct ControlEpochTag>();
  if (!epoch.ok()) return epoch.error();
  if (has_epoch.value()) {
    if (epoch.value().is_zero()) {
      return make_error(ErrorCode::invalid_argument, "request pins a zero control epoch");
    }
    request.expected_epoch = epoch.value();
  } else if (!epoch.value().is_zero()) {
    return make_error(ErrorCode::malformed_input, "absent control epoch carries a non-zero value");
  }
  auto count = reader.sequence_count(kMaxBindings, 9);
  if (!count.ok()) return count.error();
  request.pins.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto pin = GenerationPin::decode(reader);
    if (!pin.ok()) return pin.error();
    request.pins.push_back(pin.take());
  }
  const Status status = request.validate();
  if (!status.ok()) {
    return status.error();
  }
  return request;
}

}  // namespace fac
