// Facility Admission Control - ledger record payload encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ledger/records.hpp"

#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"

namespace fac::detail {
namespace {

[[nodiscard]] Status read_timestamp(codec::Reader& reader, const char* what, Timestamp& out) {
  auto value = reader.i64();
  if (!value.ok()) {
    return value.error();
  }
  if (!is_valid_timestamp_nanos(value.value()) || value.value() == 0) {
    return make_error(ErrorCode::out_of_range, std::string(what) + " is outside the supported range");
  }
  out = Timestamp(value.value());
  return Status::success();
}

[[nodiscard]] Result<std::string> read_reason(codec::Reader& reader) {
  auto text = reader.text(kMaxReasonLength);
  if (!text.ok()) {
    return text.error();
  }
  if (!is_valid_reason(text.value())) {
    return make_error(ErrorCode::malformed_input, "record reason is not usable text");
  }
  return text.take();
}

}  // namespace

std::vector<std::byte> encode_decision_recorded(const DecisionEntry& entry) {
  codec::Writer writer;
  entry.encode(writer);
  return writer.data();
}

std::vector<std::byte> encode_grant_committed(const GrantCommitted& record) {
  codec::Writer writer;
  writer.ident(record.grant_id);
  writer.ident(record.commitment_id);
  writer.i64(record.committed_at.unix_nanos());
  record.intent.encode(writer);
  return writer.data();
}

std::vector<std::byte> encode_grant_fenced(const GrantFenced& record) {
  codec::Writer writer;
  writer.ident(record.grant_id);
  record.blocker.encode(writer);
  writer.i64(record.at.unix_nanos());
  return writer.data();
}

std::vector<std::byte> encode_grant_expired(const GrantExpired& record) {
  codec::Writer writer;
  writer.ident(record.grant_id);
  writer.i64(record.at.unix_nanos());
  return writer.data();
}

std::vector<std::byte> encode_grant_released(const GrantReleased& record) {
  codec::Writer writer;
  writer.ident(record.grant_id);
  writer.i64(record.at.unix_nanos());
  writer.text(record.reason);
  return writer.data();
}

std::vector<std::byte> encode_commitment_evidence(const CommitmentEvidenceRecorded& record) {
  codec::Writer writer;
  writer.ident(record.commitment_id);
  record.evidence.encode(writer);
  writer.i64(record.at.unix_nanos());
  return writer.data();
}

std::vector<std::byte> encode_commitment_released(const CommitmentReleased& record) {
  codec::Writer writer;
  writer.ident(record.commitment_id);
  writer.i64(record.at.unix_nanos());
  writer.text(record.reason);
  return writer.data();
}

std::vector<std::byte> encode_commitment_expired(const CommitmentExpired& record) {
  codec::Writer writer;
  writer.ident(record.commitment_id);
  writer.i64(record.at.unix_nanos());
  return writer.data();
}

Result<DecodedRecord> decode_record(durable::RecordKind kind, std::span<const std::byte> payload) {
  DecodedRecord record;
  record.kind = kind;
  codec::Reader reader(payload);
  switch (kind) {
    case durable::RecordKind::decision_recorded: {
      auto entry = DecisionEntry::decode(reader);
      if (!entry.ok()) return entry.error();
      record.decision.entry = entry.take();
      break;
    }
    case durable::RecordKind::grant_committed: {
      auto grant_id = reader.ident<struct GrantIdTag>();
      if (!grant_id.ok()) return grant_id.error();
      record.committed.grant_id = grant_id.value();
      auto commitment_id = reader.ident<struct CommitmentIdTag>();
      if (!commitment_id.ok()) return commitment_id.error();
      record.committed.commitment_id = commitment_id.value();
      const Status at = read_timestamp(reader, "commit time", record.committed.committed_at);
      if (!at.ok()) return at.error();
      auto intent = ReservationIntent::decode(reader);
      if (!intent.ok()) return intent.error();
      record.committed.intent = intent.take();
      break;
    }
    case durable::RecordKind::grant_fenced: {
      auto grant_id = reader.ident<struct GrantIdTag>();
      if (!grant_id.ok()) return grant_id.error();
      record.fenced.grant_id = grant_id.value();
      auto blocker = Blocker::decode(reader);
      if (!blocker.ok()) return blocker.error();
      record.fenced.blocker = blocker.take();
      const Status at = read_timestamp(reader, "fence time", record.fenced.at);
      if (!at.ok()) return at.error();
      break;
    }
    case durable::RecordKind::grant_expired: {
      auto grant_id = reader.ident<struct GrantIdTag>();
      if (!grant_id.ok()) return grant_id.error();
      record.expired.grant_id = grant_id.value();
      const Status at = read_timestamp(reader, "expiry time", record.expired.at);
      if (!at.ok()) return at.error();
      break;
    }
    case durable::RecordKind::grant_released: {
      auto grant_id = reader.ident<struct GrantIdTag>();
      if (!grant_id.ok()) return grant_id.error();
      record.released.grant_id = grant_id.value();
      const Status at = read_timestamp(reader, "release time", record.released.at);
      if (!at.ok()) return at.error();
      auto reason = read_reason(reader);
      if (!reason.ok()) return reason.error();
      record.released.reason = reason.take();
      break;
    }
    case durable::RecordKind::commitment_evidence_recorded: {
      auto commitment_id = reader.ident<struct CommitmentIdTag>();
      if (!commitment_id.ok()) return commitment_id.error();
      record.evidence.commitment_id = commitment_id.value();
      auto evidence = ReservationEvidence::decode(reader);
      if (!evidence.ok()) return evidence.error();
      record.evidence.evidence = evidence.take();
      const Status at = read_timestamp(reader, "acknowledgement time", record.evidence.at);
      if (!at.ok()) return at.error();
      break;
    }
    case durable::RecordKind::commitment_released: {
      auto commitment_id = reader.ident<struct CommitmentIdTag>();
      if (!commitment_id.ok()) return commitment_id.error();
      record.released_commitment.commitment_id = commitment_id.value();
      const Status at = read_timestamp(reader, "release time", record.released_commitment.at);
      if (!at.ok()) return at.error();
      auto reason = read_reason(reader);
      if (!reason.ok()) return reason.error();
      record.released_commitment.reason = reason.take();
      break;
    }
    case durable::RecordKind::commitment_expired: {
      auto commitment_id = reader.ident<struct CommitmentIdTag>();
      if (!commitment_id.ok()) return commitment_id.error();
      record.expired_commitment.commitment_id = commitment_id.value();
      const Status at = read_timestamp(reader, "expiry time", record.expired_commitment.at);
      if (!at.ok()) return at.error();
      break;
    }
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end.error();
  }
  return record;
}

}  // namespace fac::detail
