// Facility Admission Control - the admission ledger.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/ledger/ledger.hpp"

#include <algorithm>

#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"
#include "ledger/records.hpp"

namespace fac {

const char* to_string(GrantState state) noexcept {
  switch (state) {
    case GrantState::issued: return "issued";
    case GrantState::committed: return "committed";
    case GrantState::fenced: return "fenced";
    case GrantState::expired: return "expired";
    case GrantState::released: return "released";
  }
  return "unknown_grant_state";
}

void DecisionEntry::encode(codec::Writer& writer) const {
  request.encode(writer);
  decision.encode(writer);
}

Result<DecisionEntry> DecisionEntry::decode(codec::Reader& reader) {
  DecisionEntry entry;
  auto request = AdmissionRequest::decode(reader);
  if (!request.ok()) return request.error();
  entry.request = request.take();
  auto decision = Decision::decode(reader);
  if (!decision.ok()) return decision.error();
  entry.decision = decision.take();
  if (!(entry.decision.request_id == entry.request.request_id)) {
    return make_error(ErrorCode::malformed_input, "decision and request identities disagree");
  }
  if (!(entry.decision.request_digest == entry.request.digest())) {
    return make_error(ErrorCode::digest_mismatch, "decision does not bind the request it records");
  }
  return entry;
}

void GrantEntry::encode(codec::Writer& writer) const {
  grant.encode(writer);
  writer.u8(static_cast<std::uint8_t>(state));
  writer.i64(resolved_at.unix_nanos());
  writer.boolean(commitment_id.has_value());
  writer.ident(commitment_id.value_or(CommitmentId{}));
  writer.u32(static_cast<std::uint32_t>(blockers.size()));
  for (const auto& blocker : blockers) {
    blocker.encode(writer);
  }
  writer.text(detail);
}

Result<GrantEntry> GrantEntry::decode(codec::Reader& reader) {
  GrantEntry entry;
  auto grant = Grant::decode(reader);
  if (!grant.ok()) return grant.error();
  entry.grant = grant.take();
  auto state = reader.u8();
  if (!state.ok()) return state.error();
  if (state.value() > static_cast<std::uint8_t>(GrantState::released)) {
    return make_error(ErrorCode::malformed_input, "impossible grant state");
  }
  entry.state = static_cast<GrantState>(state.value());
  auto resolved = reader.i64();
  if (!resolved.ok()) return resolved.error();
  if (!is_valid_timestamp_nanos(resolved.value())) {
    return make_error(ErrorCode::out_of_range, "grant resolution time is outside the supported range");
  }
  entry.resolved_at = Timestamp(resolved.value());
  auto has_commitment = reader.boolean();
  if (!has_commitment.ok()) return has_commitment.error();
  auto high = reader.u64();
  if (!high.ok()) return high.error();
  auto low = reader.u64();
  if (!low.ok()) return low.error();
  const CommitmentId commitment(high.value(), low.value());
  if (has_commitment.value()) {
    if (commitment.is_unset()) {
      return make_error(ErrorCode::malformed_input, "a committed grant names a zero commitment");
    }
    entry.commitment_id = commitment;
  } else if (!commitment.is_unset()) {
    return make_error(ErrorCode::malformed_input, "a grant without a commitment names one");
  }
  auto count = reader.sequence_count(kMaxBlockerRecords, 6);
  if (!count.ok()) return count.error();
  entry.blockers.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto blocker = Blocker::decode(reader);
    if (!blocker.ok()) return blocker.error();
    entry.blockers.push_back(blocker.take());
  }
  auto detail = reader.text(kMaxReasonLength);
  if (!detail.ok()) return detail.error();
  entry.detail = detail.take();
  // An issued or committed grant has no resolution detail yet, so empty text is
  // valid here; text that is present must still be usable.
  if (!entry.detail.empty() && !is_valid_reason(entry.detail)) {
    return make_error(ErrorCode::malformed_input, "grant detail is not usable text");
  }

  if (entry.state == GrantState::issued) {
    if (entry.commitment_id.has_value() || !entry.blockers.empty() || !entry.resolved_at.is_unset()) {
      return make_error(ErrorCode::malformed_input, "an issued grant carries a resolution");
    }
  } else if (entry.resolved_at.is_unset()) {
    return make_error(ErrorCode::malformed_input, "a resolved grant carries no resolution time");
  }
  if (entry.state == GrantState::committed && !entry.commitment_id.has_value()) {
    return make_error(ErrorCode::malformed_input, "a committed grant names no commitment");
  }
  if (entry.state != GrantState::committed && entry.commitment_id.has_value()) {
    return make_error(ErrorCode::malformed_input, "an uncommitted grant names a commitment");
  }
  return entry;
}

const DecisionEntry* Ledger::find_decision(RequestId id) const {
  const auto found = decisions_.find(id);
  return found == decisions_.end() ? nullptr : &found->second;
}

const GrantEntry* Ledger::find_grant(GrantId id) const {
  const auto found = grants_.find(id);
  return found == grants_.end() ? nullptr : &found->second;
}

const CommitmentRecord* Ledger::find_commitment(CommitmentId id) const {
  const auto found = commitments_.find(id);
  return found == commitments_.end() ? nullptr : &found->second;
}

const CommitmentRecord* Ledger::find_commitment_by_grant(GrantId id) const {
  for (const auto& entry : commitments_) {
    if (entry.second.grant_id == id) {
      return &entry.second;
    }
  }
  return nullptr;
}

namespace {

[[nodiscard]] bool same_rack(const std::optional<RackId>& left, const std::optional<RackId>& right) {
  return left.has_value() == right.has_value() && (!left.has_value() || left.value() == right.value());
}

}  // namespace

std::uint64_t Ledger::consumed(Dimension dimension, const ConsumptionQuery& query) const {
  std::uint64_t total = 0;
  const auto accumulate = [&total, dimension](const AmountVector& demand) {
    const auto amount = demand.get(dimension);
    if (!amount.has_value()) {
      // Every demand that reaches the ledger is validated fully present, so an
      // absent dimension here would mean a corrupted record rather than an
      // unmeasured value. Saturating is the safe direction: it consumes all
      // remaining capacity instead of silently freeing it.
      total = kMaxQuantity;
      return;
    }
    if (total >= kMaxQuantity - amount.value()) {
      total = kMaxQuantity;
      return;
    }
    total += amount.value();
  };

  for (const auto& entry : grants_) {
    const GrantEntry& grant = entry.second;
    if (grant.state != GrantState::issued || !grant.grant.holds_capacity) {
      continue;
    }
    if (query.exclude_grant.has_value() && entry.first == query.exclude_grant.value()) {
      continue;
    }
    if (!(grant.grant.scope.facility == query.facility)) {
      continue;
    }
    if (query.rack.has_value() && !same_rack(grant.grant.scope.rack, query.rack)) {
      continue;
    }
    if (grant.grant.issued_at < query.observed_after) {
      continue;
    }
    accumulate(grant.grant.demand);
  }

  for (const auto& entry : commitments_) {
    const CommitmentRecord& commitment = entry.second;
    if (!commitment.consumes()) {
      continue;
    }
    if (query.exclude_grant.has_value() && commitment.grant_id == query.exclude_grant.value()) {
      continue;
    }
    if (!(commitment.scope.facility == query.facility)) {
      continue;
    }
    if (query.rack.has_value() && !same_rack(commitment.scope.rack, query.rack)) {
      continue;
    }
    if (commitment.recorded_at < query.observed_after) {
      continue;
    }
    accumulate(commitment.demand);
  }
  return total;
}

std::uint64_t Ledger::consuming_commitments() const {
  std::uint64_t count = 0;
  for (const auto& entry : commitments_) {
    if (entry.second.consumes()) {
      ++count;
    }
  }
  return count;
}

std::uint64_t Ledger::watermark(EvidenceKind kind, FacilityId facility) const {
  const auto found = watermarks_.find(WatermarkKey{kind, facility});
  return found == watermarks_.end() ? 0 : found->second;
}

void Ledger::raise_watermark(EvidenceKind kind, FacilityId facility, std::uint64_t generation) {
  const WatermarkKey key{kind, facility};
  const auto found = watermarks_.find(key);
  if (found == watermarks_.end()) {
    watermarks_.emplace(key, generation);
    return;
  }
  found->second = std::max(found->second, generation);
}

std::vector<GrantId> Ledger::expired_holds(Timestamp now) const {
  std::vector<GrantId> result;
  for (const auto& entry : grants_) {
    const GrantEntry& grant = entry.second;
    if (grant.state != GrantState::issued) {
      continue;
    }
    if (grant.grant.expires_at <= now) {
      result.push_back(entry.first);
    }
  }
  return result;
}

Status Ledger::validate_record(durable::RecordKind kind, std::span<const std::byte> payload) {
  auto decoded = detail::decode_record(kind, payload);
  if (!decoded.ok()) {
    return decoded.error();
  }
  return precheck(decoded.value());
}

Status Ledger::apply(durable::RecordKind kind, LedgerSequence sequence,
                     std::span<const std::byte> payload) {
  auto expected = sequence_.checked_next();
  if (!expected.ok()) {
    return expected.error();
  }
  if (!(expected.value() == sequence)) {
    return make_error(ErrorCode::conflict, "record sequence is not the next ledger sequence");
  }
  auto decoded = detail::decode_record(kind, payload);
  if (!decoded.ok()) {
    return decoded.error();
  }
  const Status checked = precheck(decoded.value());
  if (!checked.ok()) {
    return checked.error();
  }
  const Status applied = commit_apply(decoded.value());
  if (!applied.ok()) {
    return applied.error();
  }
  sequence_ = sequence;
  return Status::success();
}

Status Ledger::precheck(const detail::DecodedRecord& record) const {
  switch (record.kind) {
    case durable::RecordKind::decision_recorded: {
      const DecisionEntry& entry = record.decision.entry;
      if (decisions_.find(entry.request.request_id) != decisions_.end()) {
        return make_error(ErrorCode::duplicate_field, "the ledger already holds this request");
      }
      if (entry.decision.grant.has_value()) {
        const Grant& grant = entry.decision.grant.value();
        if (grants_.find(grant.grant_id) != grants_.end()) {
          return make_error(ErrorCode::duplicate_field, "the ledger already holds this grant");
        }
        if (!(grant.request_id == entry.request.request_id)) {
          return make_error(ErrorCode::malformed_input, "grant and decision request identities disagree");
        }
        if (!(grant.control_epoch == entry.decision.control_epoch)) {
          return make_error(ErrorCode::malformed_input, "grant and decision control epochs disagree");
        }
        if (!(grant.sequence == entry.decision.sequence)) {
          return make_error(ErrorCode::malformed_input, "grant and decision sequences disagree");
        }
        if (!(grant.compute_binding_digest() == grant.binding_digest)) {
          return make_error(ErrorCode::digest_mismatch, "grant binding digest does not match its content");
        }
        if (!(grant.request_digest == entry.decision.request_digest)) {
          return make_error(ErrorCode::digest_mismatch, "grant does not bind the request content");
        }
      }
      return Status::success();
    }
    case durable::RecordKind::grant_committed: {
      const GrantEntry* grant = find_grant(record.committed.grant_id);
      if (grant == nullptr) {
        return make_error(ErrorCode::corrupt_store, "commit names a grant the ledger does not hold");
      }
      if (grant->state != GrantState::issued) {
        return make_error(ErrorCode::corrupt_store, "commit names a grant that is not issued");
      }
      if (commitments_.find(record.committed.commitment_id) != commitments_.end()) {
        return make_error(ErrorCode::duplicate_field, "the ledger already holds this commitment");
      }
      if (find_commitment_by_grant(record.committed.grant_id) != nullptr) {
        return make_error(ErrorCode::duplicate_field, "the grant already has a commitment");
      }
      const ReservationIntent& intent = record.committed.intent;
      if (!(intent.grant_id == record.committed.grant_id) ||
          !(intent.commitment_id == record.committed.commitment_id) ||
          !(intent.request_id == grant->grant.request_id)) {
        return make_error(ErrorCode::malformed_input, "the reservation intent does not match the grant");
      }
      if (!(intent.binding_digest == grant->grant.binding_digest)) {
        return make_error(ErrorCode::digest_mismatch, "the reservation intent does not bind the grant");
      }
      if (!(intent.control_epoch == grant->grant.control_epoch)) {
        return make_error(ErrorCode::malformed_input, "the reservation intent carries another epoch");
      }
      // The intent is emitted by the commit, which is a later ledger record
      // than the decision that issued the grant, so its sequence is greater
      // rather than equal.
      if (!(grant->grant.sequence < intent.sequence)) {
        return make_error(ErrorCode::malformed_input,
                          "the reservation intent predates the grant it answers");
      }
      if (!(record.committed.committed_at < intent.not_after)) {
        return make_error(ErrorCode::out_of_range, "the commit is outside the reservation window");
      }
      return Status::success();
    }
    case durable::RecordKind::grant_fenced: {
      const GrantEntry* grant = find_grant(record.fenced.grant_id);
      if (grant == nullptr) {
        return make_error(ErrorCode::corrupt_store, "fence names a grant the ledger does not hold");
      }
      // A fence may carry more than one cause: the fence itself and the
      // specific condition that produced it. Anything else is a conflict.
      if (grant->state != GrantState::issued && grant->state != GrantState::fenced) {
        return make_error(ErrorCode::corrupt_store, "fence names a grant that is already resolved");
      }
      if (grant->blockers.size() >= kMaxBlockerRecords) {
        return make_error(ErrorCode::limit_exceeded, "the grant already carries too many blockers");
      }
      return Status::success();
    }
    case durable::RecordKind::grant_expired:
    case durable::RecordKind::grant_released: {
      const GrantId id = record.kind == durable::RecordKind::grant_expired ? record.expired.grant_id
                                                                          : record.released.grant_id;
      const GrantEntry* grant = find_grant(id);
      if (grant == nullptr) {
        return make_error(ErrorCode::corrupt_store, "resolution names a grant the ledger does not hold");
      }
      if (grant->state != GrantState::issued) {
        return make_error(ErrorCode::corrupt_store, "resolution names a grant that is not issued");
      }
      return Status::success();
    }
    case durable::RecordKind::commitment_evidence_recorded: {
      const CommitmentRecord* commitment = find_commitment(record.evidence.commitment_id);
      if (commitment == nullptr) {
        return make_error(ErrorCode::corrupt_store, "evidence names a commitment the ledger does not hold");
      }
      if (commitment->state != CommitmentState::provisional) {
        return make_error(ErrorCode::corrupt_store, "evidence names a commitment that is already answered");
      }
      const ReservationEvidence& evidence = record.evidence.evidence;
      if (!(evidence.intent_id == commitment->intent.intent_id)) {
        return make_error(ErrorCode::malformed_input, "the acknowledgement answers a different intent");
      }
      if (!(evidence.owner == commitment->intent.owner)) {
        return make_error(ErrorCode::malformed_input,
                          "the acknowledgement names authority " + evidence.owner.to_string() +
                              " but the intent was addressed to " +
                              commitment->intent.owner.to_string());
      }
      if (!(commitment->intent.not_before <= evidence.acknowledged_at)) {
        return make_error(ErrorCode::out_of_range, "the acknowledgement predates the intent");
      }
      if (evidence.outcome != ReservationOutcome::rejected) {
        // A partial confirmation states the dimensions the owner confirmed;
        // only those are compared. A stated dimension may never exceed what was
        // asked for.
        for (const Dimension dimension : all_dimensions()) {
          const auto confirmed = evidence.confirmed.get(dimension);
          if (!confirmed.has_value()) {
            continue;
          }
          const auto demanded = commitment->demand.get(dimension);
          if (!demanded.has_value() || confirmed.value() > demanded.value()) {
            return make_error(ErrorCode::conflict,
                              std::string("the acknowledgement confirms more ") + to_string(dimension) +
                                  " than was asked for");
          }
        }
        if (evidence.outcome == ReservationOutcome::reserved &&
            !(evidence.confirmed == commitment->demand)) {
          return make_error(ErrorCode::conflict, "a full reservation must confirm exactly the demand");
        }
      }
      return Status::success();
    }
    case durable::RecordKind::commitment_released: {
      const CommitmentRecord* commitment = find_commitment(record.released_commitment.commitment_id);
      if (commitment == nullptr) {
        return make_error(ErrorCode::corrupt_store, "release names a commitment the ledger does not hold");
      }
      if (!commitment->consumes()) {
        return make_error(ErrorCode::corrupt_store, "release names a commitment that is already resolved");
      }
      if (record.released_commitment.reason.empty()) {
        return make_error(ErrorCode::invalid_argument, "a release must carry a reason");
      }
      return Status::success();
    }
    case durable::RecordKind::commitment_expired: {
      const CommitmentRecord* commitment = find_commitment(record.expired_commitment.commitment_id);
      if (commitment == nullptr) {
        return make_error(ErrorCode::corrupt_store, "expiry names a commitment the ledger does not hold");
      }
      if (commitment->state != CommitmentState::provisional) {
        return make_error(ErrorCode::corrupt_store, "expiry names a commitment that is already answered");
      }
      if (!(record.expired_commitment.at >= commitment->intent.not_after)) {
        return make_error(ErrorCode::out_of_range, "a commitment cannot expire before its intent window ends");
      }
      return Status::success();
    }
  }
  return make_error(ErrorCode::malformed_input, "impossible record kind");
}

Status Ledger::commit_apply(const detail::DecodedRecord& record) {
  switch (record.kind) {
    case durable::RecordKind::decision_recorded: {
      const DecisionEntry& entry = record.decision.entry;
      if (entry.decision.grant.has_value()) {
        GrantEntry grant_entry;
        grant_entry.grant = entry.decision.grant.value();
        grant_entry.state = GrantState::issued;
        grants_.emplace(grant_entry.grant.grant_id, std::move(grant_entry));
      }
      for (const auto& reference : entry.decision.evidence.refs()) {
        if (reference.accepted) {
          raise_watermark(reference.kind, entry.request.scope.facility, reference.generation);
        }
      }
      decisions_.emplace(entry.request.request_id, entry);
      return Status::success();
    }
    case durable::RecordKind::grant_committed: {
      GrantEntry& grant = grants_.at(record.committed.grant_id);
      CommitmentRecord commitment;
      commitment.commitment_id = record.committed.commitment_id;
      commitment.request_id = grant.grant.request_id;
      commitment.grant_id = grant.grant.grant_id;
      commitment.scope = grant.grant.scope;
      commitment.tenant = grant.grant.tenant;
      commitment.service_class = record.committed.intent.service_class;
      commitment.demand = grant.grant.demand;
      commitment.intent = record.committed.intent;
      commitment.state = CommitmentState::provisional;
      commitment.recorded_at = record.committed.committed_at;
      commitment.expires_at = record.committed.intent.not_after;
      commitment.resolution_detail = "reservation intent emitted";
      grant.state = GrantState::committed;
      grant.commitment_id = record.committed.commitment_id;
      grant.resolved_at = record.committed.committed_at;
      commitments_.emplace(commitment.commitment_id, std::move(commitment));
      return Status::success();
    }
    case durable::RecordKind::grant_fenced: {
      GrantEntry& grant = grants_.at(record.fenced.grant_id);
      if (grant.state == GrantState::issued) {
        grant.resolved_at = record.fenced.at;
      }
      grant.state = GrantState::fenced;
      if (grant.detail.empty()) {
        grant.detail = record.fenced.blocker.render();
      }
      grant.blockers.push_back(record.fenced.blocker);
      return Status::success();
    }
    case durable::RecordKind::grant_expired: {
      GrantEntry& grant = grants_.at(record.expired.grant_id);
      grant.state = GrantState::expired;
      grant.resolved_at = record.expired.at;
      grant.detail = "grant validity window passed before it was committed";
      return Status::success();
    }
    case durable::RecordKind::grant_released: {
      GrantEntry& grant = grants_.at(record.released.grant_id);
      grant.state = GrantState::released;
      grant.resolved_at = record.released.at;
      grant.detail = record.released.reason;
      return Status::success();
    }
    case durable::RecordKind::commitment_evidence_recorded: {
      CommitmentRecord& commitment = commitments_.at(record.evidence.commitment_id);
      const ReservationEvidence& evidence = record.evidence.evidence;
      switch (evidence.outcome) {
        case ReservationOutcome::reserved:
          commitment.state = CommitmentState::confirmed;
          commitment.resolution_detail = "reservation owner confirmed the full demand";
          break;
        case ReservationOutcome::partially_reserved:
          commitment.state = CommitmentState::partial;
          commitment.resolution_detail = "reservation owner confirmed part of the demand";
          break;
        case ReservationOutcome::rejected:
          commitment.state = CommitmentState::released;
          commitment.resolution_detail = "reservation owner rejected the intent";
          break;
      }
      commitment.evidence = evidence;
      commitment.resolved_at = record.evidence.at;
      return Status::success();
    }
    case durable::RecordKind::commitment_released: {
      CommitmentRecord& commitment = commitments_.at(record.released_commitment.commitment_id);
      commitment.state = CommitmentState::released;
      commitment.resolved_at = record.released_commitment.at;
      commitment.resolution_detail = record.released_commitment.reason;
      return Status::success();
    }
    case durable::RecordKind::commitment_expired: {
      CommitmentRecord& commitment = commitments_.at(record.expired_commitment.commitment_id);
      commitment.state = CommitmentState::expired;
      commitment.resolved_at = record.expired_commitment.at;
      commitment.resolution_detail = "reservation intent window passed without owner evidence";
      return Status::success();
    }
  }
  return make_error(ErrorCode::malformed_input, "impossible record kind");
}

Status Ledger::encode_state(std::vector<std::byte>& out) const {
  codec::Writer writer;
  writer.u32(kLedgerStateVersion);
  writer.counter(sequence_);
  writer.u32(static_cast<std::uint32_t>(decisions_.size()));
  for (const auto& entry : decisions_) {
    entry.second.encode(writer);
  }
  writer.u32(static_cast<std::uint32_t>(grants_.size()));
  for (const auto& entry : grants_) {
    entry.second.encode(writer);
  }
  writer.u32(static_cast<std::uint32_t>(commitments_.size()));
  for (const auto& entry : commitments_) {
    entry.second.encode(writer);
  }
  writer.u32(static_cast<std::uint32_t>(watermarks_.size()));
  for (const auto& entry : watermarks_) {
    writer.u8(static_cast<std::uint8_t>(entry.first.kind));
    writer.ident(entry.first.facility);
    writer.u64(entry.second);
  }
  out = writer.data();
  return Status::success();
}

Result<Ledger> Ledger::decode_state(std::span<const std::byte> bytes) {
  codec::Reader reader(bytes);
  auto version = reader.u32();
  if (!version.ok()) return version.error();
  if (version.value() != kLedgerStateVersion) {
    return make_error(ErrorCode::unsupported_version, "ledger snapshot version is not supported");
  }
  Ledger ledger;
  auto sequence = reader.counter<struct LedgerSequenceTag>();
  if (!sequence.ok()) return sequence.error();
  ledger.sequence_ = sequence.value();

  auto decision_count = reader.sequence_count(kMaxLedgerRecords, 8);
  if (!decision_count.ok()) return decision_count.error();
  for (std::uint32_t i = 0; i < decision_count.value(); ++i) {
    auto entry = DecisionEntry::decode(reader);
    if (!entry.ok()) return entry.error();
    const auto key = entry.value().request.request_id;
    if (ledger.decisions_.find(key) != ledger.decisions_.end()) {
      return make_error(ErrorCode::duplicate_field, "snapshot repeats a request identity");
    }
    ledger.decisions_.emplace(key, entry.take());
  }

  auto grant_count = reader.sequence_count(kMaxLedgerRecords, 8);
  if (!grant_count.ok()) return grant_count.error();
  for (std::uint32_t i = 0; i < grant_count.value(); ++i) {
    auto entry = GrantEntry::decode(reader);
    if (!entry.ok()) return entry.error();
    const auto key = entry.value().grant.grant_id;
    if (ledger.grants_.find(key) != ledger.grants_.end()) {
      return make_error(ErrorCode::duplicate_field, "snapshot repeats a grant identity");
    }
    ledger.grants_.emplace(key, entry.take());
  }

  auto commitment_count = reader.sequence_count(kMaxLedgerRecords, 8);
  if (!commitment_count.ok()) return commitment_count.error();
  for (std::uint32_t i = 0; i < commitment_count.value(); ++i) {
    auto record = CommitmentRecord::decode(reader);
    if (!record.ok()) return record.error();
    const auto key = record.value().commitment_id;
    if (ledger.commitments_.find(key) != ledger.commitments_.end()) {
      return make_error(ErrorCode::duplicate_field, "snapshot repeats a commitment identity");
    }
    ledger.commitments_.emplace(key, record.take());
  }

  auto watermark_count = reader.sequence_count(kMaxLedgerRecords, 20);
  if (!watermark_count.ok()) return watermark_count.error();
  for (std::uint32_t i = 0; i < watermark_count.value(); ++i) {
    auto kind = reader.u8();
    if (!kind.ok()) return kind.error();
    if (kind.value() > static_cast<std::uint8_t>(kEvidenceKindCount - 1)) {
      return make_error(ErrorCode::malformed_input, "snapshot carries an impossible evidence kind");
    }
    auto facility = reader.ident<struct FacilityIdTag>();
    if (!facility.ok()) return facility.error();
    auto generation = reader.u64();
    if (!generation.ok()) return generation.error();
    const WatermarkKey key{static_cast<EvidenceKind>(kind.value()), facility.value()};
    if (ledger.watermarks_.find(key) != ledger.watermarks_.end()) {
      return make_error(ErrorCode::duplicate_field, "snapshot repeats a generation watermark");
    }
    ledger.watermarks_.emplace(key, generation.value());
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end.error();
  }

  // Cross-checks: every grant that names a commitment must find it, and every
  // commitment must reference a grant that exists.
  for (const auto& entry : ledger.grants_) {
    if (entry.second.commitment_id.has_value() &&
        ledger.commitments_.find(entry.second.commitment_id.value()) == ledger.commitments_.end()) {
      return make_error(ErrorCode::state_unverified, "a grant names a commitment the snapshot lacks");
    }
  }
  for (const auto& entry : ledger.commitments_) {
    if (ledger.grants_.find(entry.second.grant_id) == ledger.grants_.end()) {
      return make_error(ErrorCode::state_unverified, "a commitment names a grant the snapshot lacks");
    }
    if (ledger.decisions_.find(entry.second.request_id) == ledger.decisions_.end()) {
      return make_error(ErrorCode::state_unverified, "a commitment names a request the snapshot lacks");
    }
  }
  return ledger;
}

Digest256 Ledger::state_digest() const {
  std::vector<std::byte> bytes;
  const Status status = encode_state(bytes);
  if (!status.ok()) {
    return Digest256{};
  }
  return Digest256::of(std::span<const std::byte>(bytes.data(), bytes.size()));
}

}  // namespace fac
