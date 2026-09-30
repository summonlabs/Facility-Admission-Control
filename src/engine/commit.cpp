// Facility Admission Control - committing a grant.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Executing a grant is where a decision made in the past meets the present.
// Every material binding is re-verified: the control epoch that issued it, the
// content digest of each authority's evidence, and the headroom it was granted
// against, which another admission may have consumed in the meantime. Anything
// that moved fences the grant, and the fence is recorded rather than implied.
//
// The reservation itself is not performed here. A committed grant emits a
// bounded intent addressed to the authority that owns reservation, and the
// commitment stays provisional until that authority returns the defined
// evidence.

#include <algorithm>
#include <string>

#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"
#include "engine/evaluate.hpp"
#include "ledger/records.hpp"

namespace fac {
namespace {

// Identity derivation: a commitment and its reservation intent are named by
// the content that produced them, so replaying the same durable record always
// yields the same identities.
template <class Tag>
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> derive_identity(const char* tag,
                                                                     const Ident128<Tag>& seed_id,
                                                                     LedgerSequence sequence) {
  codec::Writer seed;
  seed.text(tag);
  seed.ident(seed_id);
  seed.counter(sequence);
  const Digest256 derived = Digest256::of(seed.span());
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (int i = 0; i < 8; ++i) {
    high = (high << 8) |
           static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(derived.bytes()[static_cast<std::size_t>(i)]));
    low = (low << 8) | static_cast<std::uint64_t>(
                           std::to_integer<std::uint8_t>(derived.bytes()[static_cast<std::size_t>(i) + 8]));
  }
  if (high == 0 && low == 0) {
    low = 1;
  }
  return {high, low};
}

[[nodiscard]] Digest256 digest_of_bundle_entry(const EvidenceBundle& evidence, EvidenceKind kind,
                                               bool& present) {
  codec::Writer writer;
  present = true;
  switch (kind) {
    case EvidenceKind::capacity:
      if (!evidence.capacity.has_value()) { present = false; break; }
      evidence.capacity->encode(writer);
      break;
    case EvidenceKind::redundancy:
      if (!evidence.redundancy.has_value()) { present = false; break; }
      evidence.redundancy->encode(writer);
      break;
    case EvidenceKind::tenant:
      if (!evidence.tenant.has_value()) { present = false; break; }
      evidence.tenant->encode(writer);
      break;
    case EvidenceKind::envelope:
      if (!evidence.envelope.has_value()) { present = false; break; }
      evidence.envelope->encode(writer);
      break;
    case EvidenceKind::service_class:
      if (!evidence.service_class.has_value()) { present = false; break; }
      evidence.service_class->encode(writer);
      break;
    case EvidenceKind::maintenance:
      if (!evidence.maintenance.has_value()) { present = false; break; }
      evidence.maintenance->encode(writer);
      break;
    case EvidenceKind::incident:
      if (!evidence.incident.has_value()) { present = false; break; }
      evidence.incident->encode(writer);
      break;
    case EvidenceKind::placement:
      if (!evidence.placement.has_value()) { present = false; break; }
      evidence.placement->encode(writer);
      break;
    case EvidenceKind::policy:
      if (!evidence.policy.has_value()) { present = false; break; }
      evidence.policy->encode(writer);
      break;
  }
  if (!present) {
    return Digest256{};
  }
  return Digest256::of(writer.span());
}

[[nodiscard]] CommitOutcome outcome_for(const GrantEntry& entry) {
  CommitOutcome outcome;
  outcome.grant_id = entry.grant.grant_id;
  switch (entry.state) {
    case GrantState::committed: outcome.state = CommitState::committed; break;
    case GrantState::fenced: outcome.state = CommitState::fenced; break;
    case GrantState::expired: outcome.state = CommitState::expired; break;
    case GrantState::released: outcome.state = CommitState::released; break;
    case GrantState::issued: outcome.state = CommitState::committed; break;
  }
  outcome.commitment_id = entry.commitment_id;
  for (const auto& blocker : entry.blockers) {
    (void)outcome.blockers.add(blocker);
  }
  if (outcome.state != CommitState::committed && outcome.blockers.empty()) {
    (void)outcome.blockers.add(BlockerCode::grant_fenced,
                               entry.detail.empty() ? std::string("the grant is no longer usable")
                                                    : entry.detail);
  }
  outcome.sequence = entry.grant.sequence;
  outcome.resolved_at = entry.resolved_at;
  outcome.replayed = true;
  return outcome;
}

}  // namespace

Result<CommitOutcome> Engine::commit(GrantId grant_id, const EvidenceBundle* evidence) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  const Timestamp now = clock_->now();

  const GrantEntry* found = ledger_.find_grant(grant_id);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no grant with that identity is recorded");
  }
  if (found->state != GrantState::issued) {
    // A retry after a lost response must return the recorded outcome rather
    // than committing a second time.
    return outcome_for(*found);
  }

  const Status swept = sweep_expired_holds(now);
  if (!swept.ok()) {
    return swept.error();
  }
  found = ledger_.find_grant(grant_id);
  if (found == nullptr) {
    return make_error(ErrorCode::internal, "the grant disappeared while its holds were swept");
  }
  if (found->state != GrantState::issued) {
    return outcome_for(*found);
  }
  const GrantEntry entry = *found;
  const Grant& grant = entry.grant;

  const auto fence = [&](BlockerCode code, std::string detail) -> Result<CommitOutcome> {
    // The fence itself is always recorded, followed by the specific condition
    // that produced it, so a restart reproduces the same diagnosis.
    const auto record_fence = [&](BlockerCode recorded) -> Status {
      GrantFenced record;
      record.grant_id = grant_id;
      record.blocker.code = recorded;
      record.blocker.detail = detail;
      record.at = now;
      const auto payload = detail::encode_grant_fenced(record);
      return apply_record(durable::RecordKind::grant_fenced, payload);
    };
    const Status applied = record_fence(BlockerCode::grant_fenced);
    if (!applied.ok()) {
      return applied.error();
    }
    if (code != BlockerCode::grant_fenced) {
      const Status specific = record_fence(code);
      if (!specific.ok()) {
        return specific.error();
      }
    }
    const GrantEntry* updated = ledger_.find_grant(grant_id);
    if (updated == nullptr) {
      return make_error(ErrorCode::internal, "the fenced grant is missing from the ledger");
    }
    CommitOutcome outcome;
    outcome.grant_id = grant_id;
    outcome.state = CommitState::fenced;
    (void)outcome.blockers.add(BlockerCode::grant_fenced, detail);
    (void)outcome.blockers.add(code, detail);
    outcome.sequence = updated->grant.sequence;
    outcome.resolved_at = now;
    return outcome;
  };

  if (!(grant.control_epoch == epoch_)) {
    return fence(BlockerCode::control_epoch_superseded,
                 "the grant was issued under control epoch " + grant.control_epoch.to_string() +
                     " and the current epoch is " + epoch_.to_string());
  }
  if (!(now < grant.expires_at)) {
    GrantExpired record;
    record.grant_id = grant_id;
    record.at = now;
    const auto payload = detail::encode_grant_expired(record);
    const Status applied = apply_record(durable::RecordKind::grant_expired, payload);
    if (!applied.ok()) {
      return applied.error();
    }
    CommitOutcome outcome;
    outcome.grant_id = grant_id;
    outcome.state = CommitState::expired;
    (void)outcome.blockers.add(BlockerCode::grant_expired, "the grant validity window has passed");
    outcome.sequence = grant.sequence;
    outcome.resolved_at = now;
    return outcome;
  }

  if (evidence != nullptr) {
    for (const GrantBinding& binding : grant.bindings) {
      bool present = false;
      const Digest256 digest = digest_of_bundle_entry(*evidence, binding.kind, present);
      if (!present) {
        continue;
      }
      std::uint64_t generation = 0;
      switch (binding.kind) {
        case EvidenceKind::capacity: generation = evidence->capacity->generation.value(); break;
        case EvidenceKind::redundancy: generation = evidence->redundancy->generation.value(); break;
        case EvidenceKind::tenant: generation = evidence->tenant->generation.value(); break;
        case EvidenceKind::envelope: generation = evidence->envelope->generation.value(); break;
        case EvidenceKind::service_class: generation = evidence->service_class->generation.value(); break;
        case EvidenceKind::maintenance: generation = evidence->maintenance->generation.value(); break;
        case EvidenceKind::incident: generation = evidence->incident->generation.value(); break;
        case EvidenceKind::placement: generation = evidence->placement->generation.value(); break;
        case EvidenceKind::policy: generation = evidence->policy->generation.value(); break;
      }
      if (generation != binding.generation) {
        return fence(BlockerCode::evidence_superseded,
                     std::string("the ") + to_string(binding.kind) + " generation moved from " +
                         std::to_string(binding.generation) + " to " + std::to_string(generation) +
                         " after the grant was issued");
      }
      if (!(digest == binding.evidence_digest)) {
        return fence(BlockerCode::evidence_superseded,
                     std::string("the ") + to_string(binding.kind) +
                         " evidence changed content at the same generation after the grant was issued");
      }
    }
  }

  // Headroom erosion: the grant was authorized against the capacity that was
  // free when it was issued. Consumption recorded after that moment by another
  // commitment reduces it.
  const DecisionEntry* decision = ledger_.find_decision(grant.request_id);
  if (decision == nullptr) {
    return make_error(ErrorCode::internal, "the grant names a request the ledger does not hold");
  }
  ConsumptionQuery query;
  query.facility = grant.scope.facility;
  query.rack = grant.scope.rack;
  query.observed_after = decision->decision.decided_at;
  // The grant's own hold is not erosion of its own headroom.
  query.exclude_grant = grant_id;
  for (const DimensionAssessment& dimension : decision->decision.assessment.dimensions()) {
    const std::uint64_t allowed =
        dimension.remaining.has_value() && dimension.remaining.value() > 0
            ? static_cast<std::uint64_t>(dimension.remaining.value())
            : 0;
    const std::uint64_t consumed_since = ledger_.consumed(dimension.dimension, query);
    if (consumed_since > allowed) {
      return fence(BlockerCode::capacity_exhausted,
                   std::string("headroom for ") + to_string(dimension.dimension) +
                       " was consumed after this grant was issued: " + std::to_string(consumed_since) +
                       " " + unit_label(dimension.dimension) + " against the " +
                       std::to_string(allowed) + " that was free");
    }
  }

  auto next = ledger_.sequence().checked_next();
  if (!next.ok()) {
    return next.error();
  }
  const auto commitment_seed =
      derive_identity("facility-admission-control/commitment/v1", grant_id, next.value());
  const CommitmentId commitment_id(commitment_seed.first, commitment_seed.second);
  const auto intent_seed =
      derive_identity("facility-admission-control/intent/v1", grant_id, next.value());

  ReservationIntent intent;
  intent.intent_id = IntentId(intent_seed.first, intent_seed.second);
  intent.request_id = grant.request_id;
  intent.grant_id = grant_id;
  intent.commitment_id = commitment_id;
  intent.owner = policy_.reservation_owner;
  intent.scope = grant.scope;
  intent.tenant = grant.tenant;
  intent.service_class = decision->request.service_class;
  intent.demand = grant.demand;
  intent.not_before = now;
  auto not_after = now.checked_add(policy_.intent_validity);
  if (!not_after.ok()) {
    return not_after.error();
  }
  intent.not_after = not_after.value();
  intent.control_epoch = epoch_;
  intent.sequence = next.value();
  intent.binding_digest = grant.binding_digest;
  const Status intent_valid = intent.validate();
  if (!intent_valid.ok()) {
    return intent_valid.error();
  }

  GrantCommitted record;
  record.grant_id = grant_id;
  record.commitment_id = commitment_id;
  record.committed_at = now;
  record.intent = intent;
  const auto payload = detail::encode_grant_committed(record);
  const Status applied = apply_record(durable::RecordKind::grant_committed, payload);
  if (!applied.ok()) {
    return applied.error();
  }

  CommitOutcome outcome;
  outcome.grant_id = grant_id;
  outcome.state = CommitState::committed;
  outcome.commitment_id = commitment_id;
  outcome.sequence = next.value();
  outcome.resolved_at = now;
  return outcome;
}

Result<CommitmentRecord> Engine::record_evidence(const ReservationEvidence& evidence) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  const Status valid = evidence.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  const CommitmentRecord* found = nullptr;
  for (const auto& entry : ledger_.commitments()) {
    if (entry.second.intent.intent_id == evidence.intent_id) {
      found = &entry.second;
      break;
    }
  }
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no commitment matches that reservation intent");
  }
  const CommitmentRecord commitment = *found;
  if (commitment.state != CommitmentState::provisional) {
    if (commitment.evidence.has_value() && commitment.evidence.value() == evidence) {
      return commitment;  // the same answer recorded twice is the same answer
    }
    return make_error(ErrorCode::conflict, "the commitment has already been answered");
  }

  const Timestamp now = clock_->now();
  CommitmentEvidenceRecorded record;
  record.commitment_id = commitment.commitment_id;
  record.evidence = evidence;
  record.at = now;
  const auto payload = detail::encode_commitment_evidence(record);
  const Status applied = apply_record(durable::RecordKind::commitment_evidence_recorded, payload);
  if (!applied.ok()) {
    return applied.error();
  }
  const CommitmentRecord* updated = ledger_.find_commitment(commitment.commitment_id);
  if (updated == nullptr) {
    return make_error(ErrorCode::internal, "the answered commitment is missing from the ledger");
  }
  return *updated;
}

Result<CommitmentRecord> Engine::release_commitment(CommitmentId commitment_id, std::string reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  if (!is_valid_reason(reason)) {
    return make_error(ErrorCode::invalid_argument, "a release must carry a bounded, printable reason");
  }
  const CommitmentRecord* found = ledger_.find_commitment(commitment_id);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no commitment with that identity is recorded");
  }
  if (!found->consumes()) {
    return *found;
  }
  CommitmentReleased record;
  record.commitment_id = commitment_id;
  record.at = clock_->now();
  record.reason = reason;
  const auto payload = detail::encode_commitment_released(record);
  const Status applied = apply_record(durable::RecordKind::commitment_released, payload);
  if (!applied.ok()) {
    return applied.error();
  }
  const CommitmentRecord* updated = ledger_.find_commitment(commitment_id);
  if (updated == nullptr) {
    return make_error(ErrorCode::internal, "the released commitment is missing from the ledger");
  }
  return *updated;
}

Result<GrantEntry> Engine::release_grant(GrantId grant_id, std::string reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  if (!is_valid_reason(reason)) {
    return make_error(ErrorCode::invalid_argument, "a release must carry a bounded, printable reason");
  }
  const GrantEntry* found = ledger_.find_grant(grant_id);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no grant with that identity is recorded");
  }
  if (found->state != GrantState::issued) {
    return *found;
  }
  GrantReleased record;
  record.grant_id = grant_id;
  record.at = clock_->now();
  record.reason = reason;
  const auto payload = detail::encode_grant_released(record);
  const Status applied = apply_record(durable::RecordKind::grant_released, payload);
  if (!applied.ok()) {
    return applied.error();
  }
  const GrantEntry* updated = ledger_.find_grant(grant_id);
  if (updated == nullptr) {
    return make_error(ErrorCode::internal, "the released grant is missing from the ledger");
  }
  return *updated;
}

Result<GrantEntry> Engine::fence_grant(GrantId grant_id, BlockerCode code, std::string reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  if (!is_valid_reason(reason)) {
    return make_error(ErrorCode::invalid_argument, "a fence must carry a bounded, printable reason");
  }
  const GrantEntry* found = ledger_.find_grant(grant_id);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no grant with that identity is recorded");
  }
  if (found->state != GrantState::issued && found->state != GrantState::fenced) {
    return *found;
  }
  const Timestamp now = clock_->now();
  const auto record_fence = [&](BlockerCode recorded, const std::string& detail) -> Status {
    GrantFenced record;
    record.grant_id = grant_id;
    record.blocker.code = recorded;
    record.blocker.detail = detail;
    record.at = now;
    const auto payload = detail::encode_grant_fenced(record);
    return apply_record(durable::RecordKind::grant_fenced, payload);
  };
  const Status primary = record_fence(BlockerCode::grant_fenced, reason);
  if (!primary.ok()) {
    return primary.error();
  }
  if (code != BlockerCode::grant_fenced) {
    const Status specific = record_fence(code, reason);
    if (!specific.ok()) {
      return specific.error();
    }
  }
  const GrantEntry* updated = ledger_.find_grant(grant_id);
  if (updated == nullptr) {
    return make_error(ErrorCode::internal, "the fenced grant is missing from the ledger");
  }
  return *updated;
}

}  // namespace fac