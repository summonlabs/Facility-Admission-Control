// Facility Admission Control - the admission engine.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/engine/engine.hpp"

#include <algorithm>

#include "fac/core/limits.hpp"
#include "engine/evaluate.hpp"
#include "ledger/records.hpp"

namespace fac {

bool EvidenceBundle::has(EvidenceKind kind) const {
  switch (kind) {
    case EvidenceKind::capacity: return capacity.has_value();
    case EvidenceKind::redundancy: return redundancy.has_value();
    case EvidenceKind::tenant: return tenant.has_value();
    case EvidenceKind::envelope: return envelope.has_value();
    case EvidenceKind::service_class: return service_class.has_value();
    case EvidenceKind::maintenance: return maintenance.has_value();
    case EvidenceKind::incident: return incident.has_value();
    case EvidenceKind::placement: return placement.has_value();
    case EvidenceKind::policy: return policy.has_value();
  }
  return false;
}

Engine::~Engine() = default;

Status Engine::initialize(const std::string& directory, const AdmissionPolicy& policy,
                          ClockHandle clock, bool read_only, bool create) {
  const Status valid = policy.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  if (!read_only && policy.reservation_owner.is_unset()) {
    return make_error(ErrorCode::invalid_argument,
                      "admission policy names no reservation owner for emitted intents");
  }
  if (clock == nullptr) {
    return make_error(ErrorCode::invalid_argument, "an engine needs a clock");
  }
  policy_ = policy;
  clock_ = std::move(clock);
  epoch_ = ControlEpoch(1);

  if (directory.empty()) {
    return Status::success();
  }

  durable::StoreOptions options;
  options.create = create;
  options.advance_epoch = !read_only;
  auto store = read_only ? durable::Store::open_reader(directory)
                         : durable::Store::open_writer(directory, options);
  if (!store.ok()) {
    return store.error();
  }
  store_ = store.take();
  recovery_ = store_->report();
  epoch_ = store_->epoch();

  if (store_->snapshot().has_value()) {
    auto decoded = Ledger::decode_state(store_->snapshot()->payload);
    if (!decoded.ok()) {
      return make_error(ErrorCode::state_unverified,
                        std::string("the snapshot could not be decoded: ") + decoded.error().to_string());
    }
    ledger_ = decoded.take();
    if (!(ledger_.sequence().value() == store_->snapshot()->sequence.value())) {
      return make_error(ErrorCode::state_unverified, "the snapshot sequence disagrees with its payload");
    }
  }

  for (const durable::Frame& frame : store_->frames()) {
    const Status applied = ledger_.apply(frame.kind, frame.sequence,
                                         std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
    if (!applied.ok()) {
      return make_error(ErrorCode::state_unverified,
                        std::string("record replay failed at sequence ") + frame.sequence.to_string() +
                            ": " + applied.error().to_string());
    }
  }
  if (!(ledger_.sequence() == store_->sequence())) {
    return make_error(ErrorCode::state_unverified,
                      "replayed state does not reach the sequence the manifest commits");
  }
  return Status::success();
}

Result<std::unique_ptr<Engine>> Engine::open_in_memory(const AdmissionPolicy& policy, ClockHandle clock) {
  auto engine = std::unique_ptr<Engine>(new Engine());
  const Status status = engine->initialize(std::string(), policy, clock, false, false);
  if (!status.ok()) {
    return status.error();
  }
  return engine;
}

Result<std::unique_ptr<Engine>> Engine::open_durable(const std::string& directory,
                                                     const AdmissionPolicy& policy, ClockHandle clock,
                                                     bool create) {
  auto engine = std::unique_ptr<Engine>(new Engine());
  const Status status = engine->initialize(directory, policy, clock, false, create);
  if (!status.ok()) {
    return status.error();
  }
  return engine;
}

Result<std::unique_ptr<Engine>> Engine::open_reader(const std::string& directory,
                                                    const AdmissionPolicy& policy, ClockHandle clock) {
  auto engine = std::unique_ptr<Engine>(new Engine());
  const Status status = engine->initialize(directory, policy, clock, true, false);
  if (!status.ok()) {
    return status.error();
  }
  return engine;
}

Status Engine::require_writable() const {
  if (read_only()) {
    return make_error(ErrorCode::read_only_store, "the engine is open for reading only");
  }
  return Status::success();
}

Status Engine::apply_record(durable::RecordKind kind, std::span<const std::byte> payload) {
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  auto next = ledger_.sequence().checked_next();
  if (!next.ok()) {
    return next.error();
  }
  // The payload is decoded and checked before anything durable happens, so a
  // record that could not be replayed is never committed in the first place.
  const Status checked = ledger_.validate_record(kind, payload);
  if (!checked.ok()) {
    // The record kind is named here rather than at the caller, so a durable
    // failure says which record could not be applied.
    return make_error(checked.error().code, std::string("record ") + durable::to_string(kind) +
                                                " could not be applied: " + checked.error().detail);
  }
  if (store_) {
    const Status appended = store_->append(kind, next.value(), payload);
    if (!appended.ok()) {
      return appended.error();
    }
  }
  return ledger_.apply(kind, next.value(), payload);
}

Status Engine::sweep_expired_holds(Timestamp now) {
  const std::vector<GrantId> expired = ledger_.expired_holds(now);
  for (const GrantId id : expired) {
    GrantExpired record;
    record.grant_id = id;
    record.at = now;
    const auto payload = detail::encode_grant_expired(record);
    const Status applied = apply_record(durable::RecordKind::grant_expired, payload);
    if (!applied.ok()) {
      return applied.error();
    }
  }

  std::vector<CommitmentId> unanswered;
  for (const auto& entry : ledger_.commitments()) {
    if (entry.second.state == CommitmentState::provisional && entry.second.intent.not_after <= now) {
      unanswered.push_back(entry.first);
    }
  }
  for (const CommitmentId id : unanswered) {
    CommitmentExpired record;
    record.commitment_id = id;
    record.at = now;
    const auto payload = detail::encode_commitment_expired(record);
    const Status applied = apply_record(durable::RecordKind::commitment_expired, payload);
    if (!applied.ok()) {
      return applied.error();
    }
  }
  return Status::success();
}

Result<Decision> Engine::admit(const AdmissionRequest& request, const EvidenceBundle& evidence) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  const Timestamp now = clock_->now();

  const auto refusal = [&now](const AdmissionRequest& target, BlockerCode code, std::string detail,
                              ControlEpoch epoch, LedgerSequence sequence) {
    Decision decision;
    decision.request_id = target.request_id;
    decision.request_digest = target.digest();
    decision.verdict = Verdict::refuse;
    (void)decision.blockers.add(code, std::move(detail));
    decision.control_epoch = epoch;
    decision.sequence = sequence;
    decision.decided_at = now;
    decision.digest = decision.compute_digest();
    return decision;
  };

  // Idempotent replay is resolved before ordinary staleness rejection. A
  // request that was already accepted returns exactly the decision that was
  // recorded, even though every generation in it is now stale.
  if (const DecisionEntry* existing = ledger_.find_decision(request.request_id)) {
    if (existing->decision.request_digest == request.digest()) {
      Decision replay = existing->decision;
      replay.replayed = true;
      return replay;
    }
    return refusal(request, BlockerCode::idempotency_conflict,
                   "this request identity was already decided with different content", epoch_,
                   ledger_.sequence());
  }

  if (request.request_id.is_unset()) {
    // A request without an identity cannot be recorded, because the ledger key
    // is the identity. It is still answered, deterministically, without a
    // durable record.
    return refusal(request, BlockerCode::request_invalid, "the request carries no request identity",
                   epoch_, ledger_.sequence());
  }

  const Status swept = sweep_expired_holds(now);
  if (!swept.ok()) {
    return swept.error();
  }

  auto next = ledger_.sequence().checked_next();
  if (!next.ok()) {
    return next.error();
  }
  detail::EvaluationContext context{ledger_, policy_, request, evidence, epoch_, next.value(), now};
  auto decision = detail::evaluate_request(context);
  if (!decision.ok()) {
    return decision.error();
  }

  DecisionEntry entry;
  entry.request = request;
  entry.decision = decision.value();
  const auto payload = detail::encode_decision_recorded(entry);
  const Status recorded = apply_record(durable::RecordKind::decision_recorded, payload);
  if (!recorded.ok()) {
    return recorded.error();
  }
  return decision.value();
}

Status Engine::compact() {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status writable = require_writable();
  if (!writable.ok()) {
    return writable.error();
  }
  if (!store_) {
    return make_error(ErrorCode::not_found, "a volatile engine has no durable history to compact");
  }
  durable::Snapshot snapshot;
  snapshot.sequence = ledger_.sequence();
  snapshot.epoch = epoch_;
  const Status encoded = ledger_.encode_state(snapshot.payload);
  if (!encoded.ok()) {
    return encoded.error();
  }
  return store_->publish_snapshot(snapshot);
}

Status Engine::verify() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!store_) {
    return make_error(ErrorCode::not_found, "a volatile engine has no durable store to verify");
  }
  return store_->verify();
}

ControlEpoch Engine::epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return epoch_;
}

LedgerSequence Engine::sequence() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return ledger_.sequence();
}

const Ledger& Engine::ledger() const { return ledger_; }

Ledger Engine::ledger_snapshot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return ledger_;
}

const AdmissionPolicy& Engine::policy() const { return policy_; }

bool Engine::read_only() const { return store_ != nullptr && store_->read_only(); }

bool Engine::durable() const { return store_ != nullptr; }

const durable::RecoveryReport* Engine::recovery() const {
  return recovery_.has_value() ? &recovery_.value() : nullptr;
}

}  // namespace fac