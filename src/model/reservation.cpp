// Facility Admission Control - reservation intent and commitment lifecycle.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/reservation.hpp"

#include "fac/core/text.hpp"
#include "detail/enum_codec.hpp"

namespace fac {

Status ReservationIntent::validate() const {
  if (intent_id.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent carries no intent identity");
  }
  if (request_id.is_unset() || grant_id.is_unset() || commitment_id.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent is missing a bound identity");
  }
  if (owner.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent names no owner authority");
  }
  if (tenant.is_unset() || service_class.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent is missing tenancy identities");
  }
  if (scope.facility.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent names no facility");
  }
  if (!demand.all_present()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent carries an unmeasured demand");
  }
  if (!(not_before < not_after)) {
    return make_error(ErrorCode::invalid_argument, "reservation intent validity window is empty");
  }
  if (control_epoch.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent carries no control epoch");
  }
  if (binding_digest.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation intent carries no binding digest");
  }
  return Status::success();
}

void ReservationIntent::encode(codec::Writer& writer) const {
  writer.ident(intent_id);
  writer.ident(request_id);
  writer.ident(grant_id);
  writer.ident(commitment_id);
  writer.ident(owner);
  scope.encode(writer);
  writer.ident(tenant);
  writer.ident(service_class);
  demand.encode(writer);
  writer.i64(not_before.unix_nanos());
  writer.i64(not_after.unix_nanos());
  writer.counter(control_epoch);
  writer.counter(sequence);
  writer.digest(binding_digest);
}

Result<ReservationIntent> ReservationIntent::decode(codec::Reader& reader) {
  ReservationIntent intent;
  auto intent_id = reader.ident<struct IntentIdTag>();
  if (!intent_id.ok()) return intent_id.error();
  intent.intent_id = intent_id.value();
  auto request_id = reader.ident<struct RequestIdTag>();
  if (!request_id.ok()) return request_id.error();
  intent.request_id = request_id.value();
  auto grant_id = reader.ident<struct GrantIdTag>();
  if (!grant_id.ok()) return grant_id.error();
  intent.grant_id = grant_id.value();
  auto commitment_id = reader.ident<struct CommitmentIdTag>();
  if (!commitment_id.ok()) return commitment_id.error();
  intent.commitment_id = commitment_id.value();
  auto owner = reader.ident<struct OwnerIdTag>();
  if (!owner.ok()) return owner.error();
  intent.owner = owner.value();
  auto scope = TargetScope::decode(reader);
  if (!scope.ok()) return scope.error();
  intent.scope = scope.take();
  auto tenant = reader.ident<struct TenantIdTag>();
  if (!tenant.ok()) return tenant.error();
  intent.tenant = tenant.value();
  auto service_class = reader.ident<struct ServiceClassIdTag>();
  if (!service_class.ok()) return service_class.error();
  intent.service_class = service_class.value();
  auto demand = AmountVector::decode(reader);
  if (!demand.ok()) return demand.error();
  intent.demand = demand.take();
  auto not_before = reader.i64();
  if (!not_before.ok()) return not_before.error();
  auto not_after = reader.i64();
  if (!not_after.ok()) return not_after.error();
  if (!is_valid_timestamp_nanos(not_before.value()) || !is_valid_timestamp_nanos(not_after.value())) {
    return make_error(ErrorCode::out_of_range, "reservation intent window is outside the supported range");
  }
  intent.not_before = Timestamp(not_before.value());
  intent.not_after = Timestamp(not_after.value());
  auto epoch = reader.counter<struct ControlEpochTag>();
  if (!epoch.ok()) return epoch.error();
  intent.control_epoch = epoch.value();
  auto sequence = reader.counter<struct LedgerSequenceTag>();
  if (!sequence.ok()) return sequence.error();
  intent.sequence = sequence.value();
  auto digest = reader.digest();
  if (!digest.ok()) return digest.error();
  intent.binding_digest = digest.value();
  const Status status = intent.validate();
  if (!status.ok()) {
    return status.error();
  }
  return intent;
}

const char* to_string(ReservationOutcome outcome) noexcept {
  switch (outcome) {
    case ReservationOutcome::reserved: return "reserved";
    case ReservationOutcome::partially_reserved: return "partially_reserved";
    case ReservationOutcome::rejected: return "rejected";
  }
  return "unknown_outcome";
}

Result<ReservationOutcome> reservation_outcome_from_string(std::string_view text) {
  if (text == "reserved") return ReservationOutcome::reserved;
  if (text == "partially_reserved") return ReservationOutcome::partially_reserved;
  if (text == "rejected") return ReservationOutcome::rejected;
  return make_error(ErrorCode::invalid_argument, "unknown reservation outcome");
}

Status ReservationEvidence::validate() const {
  if (intent_id.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation evidence names no intent");
  }
  if (owner.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation evidence names no owner authority");
  }
  if (owner_digest.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation evidence carries no owner digest");
  }
  if (owner_generation == 0) {
    return make_error(ErrorCode::invalid_argument, "reservation evidence carries no owner generation");
  }
  if (acknowledged_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "reservation evidence carries no acknowledgement time");
  }
  switch (outcome) {
    case ReservationOutcome::reserved:
      if (!confirmed.all_present()) {
        return make_error(ErrorCode::invalid_argument, "a full reservation must confirm every dimension");
      }
      break;
    case ReservationOutcome::partially_reserved:
      if (!confirmed.any_present()) {
        return make_error(ErrorCode::invalid_argument, "a partial reservation must confirm a dimension");
      }
      break;
    case ReservationOutcome::rejected:
      if (confirmed.any_present()) {
        return make_error(ErrorCode::invalid_argument, "a rejected reservation confirms capacity");
      }
      break;
  }
  return Status::success();
}

void ReservationEvidence::encode(codec::Writer& writer) const {
  writer.ident(intent_id);
  writer.ident(reservation);
  writer.ident(owner);
  writer.u8(static_cast<std::uint8_t>(outcome));
  confirmed.encode(writer);
  writer.u64(owner_generation);
  writer.digest(owner_digest);
  writer.i64(acknowledged_at.unix_nanos());
}

Result<ReservationEvidence> ReservationEvidence::decode(codec::Reader& reader) {
  ReservationEvidence evidence;
  auto intent_id = reader.ident<struct IntentIdTag>();
  if (!intent_id.ok()) return intent_id.error();
  evidence.intent_id = intent_id.value();
  auto reservation = reader.ident<struct ReservationIdTag>();
  if (!reservation.ok()) return reservation.error();
  evidence.reservation = reservation.value();
  auto owner = reader.ident<struct OwnerIdTag>();
  if (!owner.ok()) return owner.error();
  evidence.owner = owner.value();
  auto outcome = detail::read_enum<ReservationOutcome>(reader,
                                                      static_cast<std::uint8_t>(ReservationOutcome::rejected),
                                                      "reservation outcome");
  if (!outcome.ok()) return outcome.error();
  evidence.outcome = outcome.value();
  auto confirmed = AmountVector::decode(reader);
  if (!confirmed.ok()) return confirmed.error();
  evidence.confirmed = confirmed.take();
  auto generation = reader.u64();
  if (!generation.ok()) return generation.error();
  evidence.owner_generation = generation.value();
  auto digest = reader.digest();
  if (!digest.ok()) return digest.error();
  evidence.owner_digest = digest.value();
  auto acknowledged = reader.i64();
  if (!acknowledged.ok()) return acknowledged.error();
  if (!is_valid_timestamp_nanos(acknowledged.value())) {
    return make_error(ErrorCode::out_of_range, "acknowledgement time is outside the supported range");
  }
  evidence.acknowledged_at = Timestamp(acknowledged.value());
  const Status status = evidence.validate();
  if (!status.ok()) {
    return status.error();
  }
  return evidence;
}

const char* to_string(CommitmentState state) noexcept {
  switch (state) {
    case CommitmentState::provisional: return "provisional";
    case CommitmentState::confirmed: return "confirmed";
    case CommitmentState::partial: return "partial";
    case CommitmentState::released: return "released";
    case CommitmentState::expired: return "expired";
  }
  return "unknown_commitment_state";
}

Result<CommitmentState> commitment_state_from_string(std::string_view text) {
  if (text == "provisional") return CommitmentState::provisional;
  if (text == "confirmed") return CommitmentState::confirmed;
  if (text == "partial") return CommitmentState::partial;
  if (text == "released") return CommitmentState::released;
  if (text == "expired") return CommitmentState::expired;
  return make_error(ErrorCode::invalid_argument, "unknown commitment state");
}

bool commitment_consumes(CommitmentState state) noexcept {
  return state == CommitmentState::provisional || state == CommitmentState::confirmed ||
         state == CommitmentState::partial;
}

void CommitmentRecord::encode(codec::Writer& writer) const {
  writer.ident(commitment_id);
  writer.ident(request_id);
  writer.ident(grant_id);
  scope.encode(writer);
  writer.ident(tenant);
  writer.ident(service_class);
  demand.encode(writer);
  intent.encode(writer);
  writer.u8(static_cast<std::uint8_t>(state));
  writer.i64(recorded_at.unix_nanos());
  writer.i64(expires_at.unix_nanos());
  writer.boolean(resolved_at.has_value());
  writer.i64(resolved_at.value_or(Timestamp{}).unix_nanos());
  writer.boolean(evidence.has_value());
  if (evidence.has_value()) {
    evidence->encode(writer);
  }
  writer.text(resolution_detail);
}

Result<CommitmentRecord> CommitmentRecord::decode(codec::Reader& reader) {
  CommitmentRecord record;
  auto commitment_id = reader.ident<struct CommitmentIdTag>();
  if (!commitment_id.ok()) return commitment_id.error();
  record.commitment_id = commitment_id.value();
  auto request_id = reader.ident<struct RequestIdTag>();
  if (!request_id.ok()) return request_id.error();
  record.request_id = request_id.value();
  auto grant_id = reader.ident<struct GrantIdTag>();
  if (!grant_id.ok()) return grant_id.error();
  record.grant_id = grant_id.value();
  auto scope = TargetScope::decode(reader);
  if (!scope.ok()) return scope.error();
  record.scope = scope.take();
  auto tenant = reader.ident<struct TenantIdTag>();
  if (!tenant.ok()) return tenant.error();
  record.tenant = tenant.value();
  auto service_class = reader.ident<struct ServiceClassIdTag>();
  if (!service_class.ok()) return service_class.error();
  record.service_class = service_class.value();
  auto demand = AmountVector::decode(reader);
  if (!demand.ok()) return demand.error();
  record.demand = demand.take();
  auto intent = ReservationIntent::decode(reader);
  if (!intent.ok()) return intent.error();
  record.intent = intent.take();
  if (!(record.intent.commitment_id == record.commitment_id)) {
    return make_error(ErrorCode::malformed_input, "commitment and its reservation intent disagree");
  }
  auto state = detail::read_enum<CommitmentState>(reader, static_cast<std::uint8_t>(CommitmentState::expired),
                                                  "commitment state");
  // The state set is dense and ordered; reordering it would silently reinterpret
  // durable records, so the last value is asserted here.
  static_assert(static_cast<std::uint8_t>(CommitmentState::expired) == 4,
                "commitment state encoding changed; the durable format revision must change with it");
  if (!state.ok()) return state.error();
  record.state = state.value();
  auto recorded_at = reader.i64();
  if (!recorded_at.ok()) return recorded_at.error();
  auto expires_at = reader.i64();
  if (!expires_at.ok()) return expires_at.error();
  if (!is_valid_timestamp_nanos(recorded_at.value()) || !is_valid_timestamp_nanos(expires_at.value())) {
    return make_error(ErrorCode::out_of_range, "commitment carries a time outside the supported range");
  }
  record.recorded_at = Timestamp(recorded_at.value());
  record.expires_at = Timestamp(expires_at.value());
  auto has_resolved = reader.boolean();
  if (!has_resolved.ok()) return has_resolved.error();
  auto resolved = reader.i64();
  if (!resolved.ok()) return resolved.error();
  if (!is_valid_timestamp_nanos(resolved.value())) {
    return make_error(ErrorCode::out_of_range, "commitment resolution time is outside the supported range");
  }
  if (has_resolved.value()) {
    record.resolved_at = Timestamp(resolved.value());
  } else if (resolved.value() != 0) {
    return make_error(ErrorCode::malformed_input, "unresolved commitment carries a resolution time");
  }
  auto has_evidence = reader.boolean();
  if (!has_evidence.ok()) return has_evidence.error();
  if (has_evidence.value()) {
    if (record.state == CommitmentState::provisional || record.state == CommitmentState::expired) {
      return make_error(ErrorCode::malformed_input, "an unanswered commitment carries reservation evidence");
    }
    auto evidence = ReservationEvidence::decode(reader);
    if (!evidence.ok()) return evidence.error();
    record.evidence = evidence.take();
  }
  auto detail = reader.text(kMaxReasonLength);
  if (!detail.ok()) return detail.error();
  record.resolution_detail = detail.take();
  if (!record.resolution_detail.empty() && !is_valid_reason(record.resolution_detail)) {
    return make_error(ErrorCode::malformed_input, "commitment resolution detail is not usable text");
  }

  // Lifecycle: provisional means "the intent is emitted and unanswered".
  // confirmed and partial mean "the owner answered", which resolves the
  // question while the capacity stays consumed. released and expired mean the
  // claim is gone.
  switch (record.state) {
    case CommitmentState::provisional:
      if (record.resolved_at.has_value()) {
        return make_error(ErrorCode::malformed_input, "an unanswered commitment carries a resolution time");
      }
      break;
    case CommitmentState::confirmed:
    case CommitmentState::partial:
      if (!record.resolved_at.has_value()) {
        return make_error(ErrorCode::malformed_input, "an answered commitment carries no resolution time");
      }
      break;
    case CommitmentState::released:
    case CommitmentState::expired:
      if (!record.resolved_at.has_value()) {
        return make_error(ErrorCode::malformed_input, "a resolved commitment carries no resolution time");
      }
      break;
  }
  if (record.state == CommitmentState::expired && record.evidence.has_value()) {
    return make_error(ErrorCode::malformed_input, "an expired commitment carries an owner answer");
  }
  return record;
}

}  // namespace fac
