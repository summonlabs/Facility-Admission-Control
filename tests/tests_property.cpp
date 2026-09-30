// Facility Admission Control - property tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Seeded randomized state machines over a real durable engine. Every action is
// a public engine call; after every action the whole authoritative state is
// re-checked against the invariants below. Both tests print their seed and the
// index of the action that first broke an invariant, so a failure is
// reproducible from the printed line alone.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fac/core/error.hpp"
#include "fac/engine/engine.hpp"
#include "fac/ledger/ledger.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

namespace {

using namespace fac;
using namespace fac_test;

enum class ActionKind {
  admit,
  commit,
  acknowledge,
  release_commitment,
  release_grant,
  fence_grant,
  tick,
  compact,
};

const char* to_string(ActionKind kind) {
  switch (kind) {
    case ActionKind::admit: return "admit";
    case ActionKind::commit: return "commit";
    case ActionKind::acknowledge: return "acknowledge";
    case ActionKind::release_commitment: return "release_commitment";
    case ActionKind::release_grant: return "release_grant";
    case ActionKind::fence_grant: return "fence_grant";
    case ActionKind::tick: return "tick";
    case ActionKind::compact: return "compact";
  }
  return "unknown";
}

class Machine {
 public:
  Machine(std::uint64_t seed, std::string directory)
      : seed_(seed), directory_(std::move(directory)), rng_(seed), fixture_(make_fixture()) {
    harness_.clock = std::make_shared<FixedClock>(fixture_.now);
    harness_.engine = FAC_TAKE(Engine::open_durable(directory_, fixture_.policy, harness_.clock, true));
    now_ = fixture_.now;
    // The capacity snapshot is observed here for the whole run, one nanosecond
    // before every evaluation. Every grant and every commitment this run
    // records is therefore later than the observation, so the ledger counts all
    // of its own consumption, which is what makes the capacity invariant exact.
    capacity_observed_ = Timestamp(fixture_.now.unix_nanos() - 1);
  }

  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;
  ~Machine() = default;

  void run(std::uint64_t actions) {
    std::cout << "[property] seed=" << seed_ << " actions=" << actions << " store=" << directory_
              << std::endl;
    for (std::uint64_t index = 0; index < actions; ++index) {
      action_index_ = index;
      dispatch(pick_action());
      check_invariants();
    }
    if (violated_) {
      FAC_FAIL("invariant violated at action " + std::to_string(violation_action_) + " of " +
               std::to_string(actions) + " (seed " + std::to_string(seed_) + ", action type " +
               to_string(violation_action_kind_) + "): " + violation_);
    }
  }

  void close() { harness_.engine.reset(); }

  [[nodiscard]] const Fixture& fixture() const { return fixture_; }
  [[nodiscard]] Harness& harness() { return harness_; }
  [[nodiscard]] std::uint64_t seed() const { return seed_; }

  [[nodiscard]] Digest256 state_digest() const { return harness_.engine->ledger().state_digest(); }
  [[nodiscard]] std::uint64_t sequence() const { return harness_.engine->sequence().value(); }
  [[nodiscard]] const std::map<RequestId, DecisionEntry>& decisions() const {
    return harness_.engine->ledger().decisions();
  }
  [[nodiscard]] const std::map<GrantId, GrantEntry>& grants() const {
    return harness_.engine->ledger().grants();
  }
  [[nodiscard]] const std::map<CommitmentId, CommitmentRecord>& commitments() const {
    return harness_.engine->ledger().commitments();
  }

 private:
  // -------------------------------------------------------------------------
  // Failure reporting
  // -------------------------------------------------------------------------

  // A violation does not abort the run: the first one is kept with the action
  // that produced it, and the loop keeps going so the remaining invariants and
  // actions are still exercised. The run reports the first violation at the end.
  void violate(std::string message) {
    if (!violated_) {
      violated_ = true;
      violation_ = std::move(message);
      violation_action_ = action_index_;
      violation_action_kind_ = action_kind_;
    }
  }

  // An engine error the state machine does not expect is a hard failure.
  [[noreturn]] void fail(std::string message) {
    FAC_FAIL(message + " (seed " + std::to_string(seed_) + ", action " +
             std::to_string(action_index_) + " " + to_string(action_kind_) + ")");
  }

  // -------------------------------------------------------------------------
  // Scenario
  // -------------------------------------------------------------------------

  AmountVector capacity_total() const {
    return amounts(600, 400, 16, 32);
  }

  AdmissionRequest make_request() {
    AdmissionRequest request;
    request.request_id = request_id(next_request_++);
    request.tenant = tenant_alpha();
    request.service_class = class_gold();
    request.envelope = envelope_alpha();
    request.scope = FAC_TAKE(TargetScope::make(facility_a(), rack_1(), zone_a()));
    request.demand = amounts(rng_.in_range(20, 200), rng_.in_range(10, 100), rng_.in_range(1, 2),
                             rng_.in_range(1, 2));
    request.requested_at = now_;
    request.commitment_start = now_;
    request.commitment_end = Timestamp(now_.unix_nanos() + 3600LL * 1000000000LL);
    return request;
  }

  [[nodiscard]] Timestamp observed(std::uint64_t budget_seconds) {
    const std::uint64_t age_seconds = rng_.in_range(0, budget_seconds);
    return Timestamp(now_.unix_nanos() - static_cast<Nanos>(age_seconds * 1000000000ull));
  }

  // A conforming bundle whose capacity arithmetic is fixed and whose every other
  // authority is randomized: a generation that never runs backwards (so the
  // watermark accepts it), an observation time that is sometimes stale and
  // sometimes future-dated, and sometimes one authority missing altogether.
  EvidenceBundle build_evidence(const AmountVector& demand) {
    EvidenceBundle evidence = fixture_.evidence;
    generation_ += rng_.below(3);

    evidence.capacity->generation = CapacityGeneration(generation_);
    evidence.capacity->observed_at = capacity_observed_;
    evidence.capacity->total = capacity_total();
    evidence.capacity->committed = amounts(0, 0, 0, 0);
    evidence.capacity->reserved = amounts(0, 0, 0, 0);

    evidence.tenant->generation = TenantGeneration(generation_);
    evidence.tenant->observed_at = observed(7200);
    evidence.envelope->generation = EnvelopeGeneration(generation_);
    evidence.envelope->observed_at = observed(7200);
    evidence.service_class->generation = ServiceClassGeneration(generation_);
    evidence.service_class->observed_at = observed(1000);
    evidence.maintenance->generation = MaintenanceGeneration(generation_);
    evidence.maintenance->observed_at = observed(600);
    evidence.incident->generation = IncidentGeneration(generation_);
    evidence.incident->observed_at = observed(240);
    evidence.placement->generation = PlacementGeneration(generation_);
    evidence.placement->observed_at = observed(1000);
    evidence.policy->generation = PolicyGeneration(generation_);
    evidence.policy->observed_at = observed(600);

    // Sometimes the tenant is not active, the protection is gone, the facility
    // is under maintenance or an incident, a rack is forbidden, the policy
    // denies, or the envelope cannot cover the demand. Each of these is a
    // different refusal path, and none of them may leak a grant.
    if (rng_.below(8) == 0) {
      evidence.tenant->status = TenantStatus::suspended;
    }
    if (rng_.below(8) == 0) {
      evidence.redundancy->level = RedundancyLevel::none;
    }
    if (rng_.below(8) == 0) {
      MaintenanceWindow window;
      window.window = window_one();
      window.scope = FAC_TAKE(TargetScope::make(facility_a(), std::nullopt, std::nullopt));
      window.start = Timestamp(now_.unix_nanos() - 60LL * 1000000000LL);
      window.end = Timestamp(now_.unix_nanos() + 600LL * 1000000000LL);
      window.state = rng_.below(2) == 0 ? MaintenanceState::active : MaintenanceState::planned;
      window.impact = MaintenanceImpact::full_outage;
      evidence.maintenance->windows.push_back(window);
    }
    if (rng_.below(10) == 0) {
      evidence.incident->state = IncidentState::major;
    } else if (rng_.below(10) == 0) {
      evidence.incident->state = IncidentState::degraded;
      evidence.incident->capacity_trust = CapacityTrust::unreliable;
    }
    if (rng_.below(8) == 0) {
      evidence.placement->forbidden_racks.push_back(rack_1());
    }
    if (rng_.below(8) == 0) {
      evidence.policy->verdict = PolicyVerdict::deny;
    } else if (rng_.below(8) == 0) {
      evidence.policy->verdict = PolicyVerdict::abstain;
    }
    if (rng_.below(8) == 0) {
      evidence.envelope->limit = amounts(demand.get(Dimension::power).value_or(0) / 2 + 1,
                                         demand.get(Dimension::cooling).value_or(0) / 2 + 1,
                                         demand.get(Dimension::space).value_or(0),
                                         demand.get(Dimension::slots).value_or(0));
    }

    // One authority at a time may be missing, stale beyond its budget, future
    // dated, or one generation behind the watermark it already raised.
    const std::uint64_t twist = rng_.below(6);
    if (twist == 0) {
      switch (rng_.below(3)) {
        case 0: evidence.redundancy.reset(); break;
        case 1: evidence.incident.reset(); break;
        default: evidence.placement.reset(); break;
      }
    } else if (twist == 1) {
      evidence.incident->observed_at = Timestamp(now_.unix_nanos() + 120LL * 1000000000LL);
    } else if (twist == 2) {
      switch (rng_.below(3)) {
        case 0: evidence.tenant->observed_at = observed(7200); break;
        case 1: evidence.policy->observed_at = observed(600); break;
        default: evidence.maintenance->observed_at = observed(600); break;
      }
    } else if (twist == 3 && generation_ > 1) {
      switch (rng_.below(3)) {
        case 0: evidence.envelope->generation = EnvelopeGeneration(generation_ - 1); break;
        case 1: evidence.service_class->generation = ServiceClassGeneration(generation_ - 1); break;
        default: evidence.policy->generation = PolicyGeneration(generation_ - 1); break;
      }
    }
    return evidence;
  }

  // -------------------------------------------------------------------------
  // Actions
  // -------------------------------------------------------------------------

  ActionKind pick_action() {
    const std::uint64_t roll = rng_.below(100);
    if (roll < 38) return ActionKind::admit;
    if (roll < 58) return ActionKind::commit;
    if (roll < 72) return ActionKind::acknowledge;
    if (roll < 80) return ActionKind::release_commitment;
    if (roll < 86) return ActionKind::release_grant;
    if (roll < 92) return ActionKind::fence_grant;
    if (roll < 98) return ActionKind::tick;
    return ActionKind::compact;
  }

  void dispatch(ActionKind kind) {
    action_kind_ = kind;
    switch (kind) {
      case ActionKind::admit: act_admit(); break;
      case ActionKind::commit: act_commit(); break;
      case ActionKind::acknowledge: act_acknowledge(); break;
      case ActionKind::release_commitment: act_release_commitment(); break;
      case ActionKind::release_grant: act_release_grant(); break;
      case ActionKind::fence_grant: act_fence_grant(); break;
      case ActionKind::tick: act_tick(); break;
      case ActionKind::compact: act_compact(); break;
    }
  }

  // The records the engine will apply on its own before it even looks at the
  // caller's request: every issued grant whose window has passed, and every
  // provisional commitment whose intent window has passed.
  [[nodiscard]] std::uint64_t sweep_estimate(Timestamp at) const {
    const Ledger& ledger = harness_.engine->ledger();
    std::uint64_t count = ledger.expired_holds(at).size();
    for (const auto& entry : ledger.commitments()) {
      if (entry.second.state == CommitmentState::provisional && entry.second.intent.not_after <= at) {
        ++count;
      }
    }
    return count;
  }

  void act_admit() {
    const AdmissionRequest request = make_request();
    const EvidenceBundle evidence = build_evidence(request.demand);
    const std::uint64_t sweep = sweep_estimate(now_);
    const std::uint64_t sequence_before = expected_sequence_;
    auto result = harness_.engine->admit(request, evidence);
    if (!result.ok()) {
      fail("admit returned " + result.error().to_string());
    }
    const Decision& decision = result.value();
    ++admitted_;
    if (!(decision.request_id == request.request_id)) {
      violate("the returned decision names another request identity");
    }
    if (!(decision.request_digest == request.digest())) {
      violate("the returned decision does not bind the request content");
    }
    if (!(decision.digest == decision.compute_digest())) {
      violate("the returned decision digest does not cover its own body");
    }
    if (!(decision.verdict == decision.blockers.implied_verdict())) {
      violate("the returned verdict disagrees with its blocker set");
    }
    if (decision.grant.has_value() != (decision.verdict == Verdict::allow)) {
      violate("the decision and its grant disagree about the verdict");
    }
    if (decision.sequence.value() != sequence_before + sweep + 1) {
      violate("the decision was recorded at sequence " + decision.sequence.to_string() +
              " but " + std::to_string(sequence_before) + " records were applied and " +
              std::to_string(sweep) + " holds were swept");
    }
    expected_sequence_ = sequence_before + sweep + 1;
  }

  void act_commit() {
    const GrantId id = pick_grant(true);
    if (id.is_unset()) {
      return;
    }
    const GrantEntry* before = harness_.engine->ledger().find_grant(id);
    if (before == nullptr) {
      return;
    }
    const bool issued_before = before->state == GrantState::issued;
    const bool swept_here = issued_before && before->grant.expires_at <= now_;
    const std::uint64_t sweep = issued_before ? sweep_estimate(now_) : 0;
    const AmountVector demand = before->grant.demand;

    const EvidenceBundle evidence = build_evidence(demand);
    const bool with_evidence = rng_.below(2) == 0;
    auto result = harness_.engine->commit(id, with_evidence ? &evidence : nullptr);
    if (!result.ok()) {
      fail("commit returned " + result.error().to_string());
    }
    const CommitOutcome& outcome = result.value();
    if (outcome.grant_id != id) {
      violate("the commit outcome names another grant");
    }
    if (!issued_before) {
      // The recorded outcome is returned unchanged and nothing is appended.
      if (outcome.state == CommitState::committed) {
        violate("a grant that was already resolved was reported as newly committed");
      }
      return;
    }
    if (swept_here) {
      // The engine's own sweep resolved this grant before the commit looked at
      // it, so the only records appended are the swept ones.
      expected_sequence_ += sweep;
      if (outcome.state == CommitState::committed) {
        violate("a grant past its validity window was committed");
      }
      return;
    }
    const std::uint64_t applied = outcome.state == CommitState::committed
                                      ? 1
                                      : (outcome.state == CommitState::fenced ? 2 : 1);
    expected_sequence_ += sweep + applied;
    if (outcome.state != CommitState::committed) {
      return;
    }
    ++committed_;
    if (!outcome.commitment_id.has_value()) {
      violate("a committed grant names no commitment");
      return;
    }
    const CommitmentRecord* commitment =
        harness_.engine->ledger().find_commitment(outcome.commitment_id.value());
    if (commitment == nullptr) {
      violate("the commitment of a committed grant is missing from the ledger");
      return;
    }
    if (!commitment->consumes()) {
      violate("a freshly committed commitment does not consume capacity");
    }
    if (!(commitment->demand == demand)) {
      violate("the commitment carries a different demand than the grant");
    }
    if (!(commitment->grant_id == id)) {
      violate("the commitment names another grant");
    }
    // A commitment's expiry is the reservation intent's window, not the
    // grant's validity window.
    if (!(commitment->expires_at == commitment->intent.not_after)) {
      violate("the commitment carries a different expiry than its own intent window");
    }
    if (!(commitment->intent.not_before < commitment->expires_at)) {
      violate("the commitment's reservation window is empty");
    }
  }

  void act_acknowledge() {
    const CommitmentId id = pick_commitment(true);
    if (id.is_unset()) {
      return;
    }
    const CommitmentRecord* commitment = harness_.engine->ledger().find_commitment(id);
    if (commitment == nullptr || commitment->state != CommitmentState::provisional) {
      return;
    }
    ReservationEvidence evidence;
    evidence.intent_id = commitment->intent.intent_id;
    evidence.reservation =
        ReservationId(0x9000000000000000ull + acknowledgements_, 0xA000000000000000ull + acknowledgements_);
    evidence.owner = fixture_.policy.reservation_owner;
    evidence.owner_generation = 1 + rng_.below(1000);
    evidence.owner_digest = fixture_digest("owner-acknowledgement");
    evidence.acknowledged_at = now_;

    CommitmentState expected = CommitmentState::released;
    switch (rng_.below(3)) {
      case 0:
        evidence.outcome = ReservationOutcome::reserved;
        evidence.confirmed = commitment->demand;
        expected = CommitmentState::confirmed;
        break;
      case 1: {
        evidence.outcome = ReservationOutcome::partially_reserved;
        AmountVector part;
        for (const Dimension dimension : all_dimensions()) {
          const std::uint64_t amount = commitment->demand.get(dimension).value_or(0);
          (void)part.set(dimension, std::max<std::uint64_t>(1, amount / 2));
        }
        evidence.confirmed = part;
        expected = CommitmentState::partial;
        break;
      }
      default:
        evidence.outcome = ReservationOutcome::rejected;
        expected = CommitmentState::released;
        break;
    }

    auto result = harness_.engine->record_evidence(evidence);
    if (!result.ok()) {
      fail("record_evidence returned " + result.error().to_string());
    }
    ++acknowledgements_;
    expected_sequence_ += 1;
    if (result.value().state != expected) {
      violate(std::string("the acknowledgement left the commitment in state ") +
              to_string(result.value().state) + " instead of " + to_string(expected));
    }
  }

  void act_release_commitment() {
    const CommitmentId id = pick_commitment(false);
    if (id.is_unset()) {
      return;
    }
    const CommitmentRecord* before = harness_.engine->ledger().find_commitment(id);
    if (before == nullptr) {
      return;
    }
    const bool consuming = before->consumes();
    auto result = harness_.engine->release_commitment(id, "property release");
    if (!result.ok()) {
      fail("release_commitment returned " + result.error().to_string());
    }
    if (consuming) {
      expected_sequence_ += 1;
    }
    if (result.value().consumes()) {
      violate("a released commitment still consumes capacity");
    }
  }

  void act_release_grant() {
    const GrantId id = pick_grant(true);
    if (id.is_unset()) {
      return;
    }
    const GrantEntry* before = harness_.engine->ledger().find_grant(id);
    if (before == nullptr) {
      return;
    }
    const bool issued = before->state == GrantState::issued;
    auto result = harness_.engine->release_grant(id, "property release");
    if (!result.ok()) {
      fail("release_grant returned " + result.error().to_string());
    }
    if (issued) {
      expected_sequence_ += 1;
    }
    if (result.value().state != GrantState::released) {
      violate("a released grant is in state " + std::string(to_string(result.value().state)));
    }
  }

  void act_fence_grant() {
    const GrantId id = pick_grant(false);
    if (id.is_unset()) {
      return;
    }
    auto result =
        harness_.engine->fence_grant(id, BlockerCode::evidence_superseded, "property fence");
    if (!result.ok()) {
      fail("fence_grant returned " + result.error().to_string());
    }
    expected_sequence_ += 2;  // the fence itself and the condition that caused it
    if (result.value().state != GrantState::fenced) {
      violate("a fenced grant is in state " + std::string(to_string(result.value().state)));
    }
  }

  void act_tick() {
    const std::uint64_t seconds = rng_.in_range(0, 5);
    now_ = Timestamp(now_.unix_nanos() + static_cast<Nanos>(seconds * 1000000000ull));
    harness_.clock->set(now_);
  }

  void act_compact() {
    const Digest256 before = harness_.engine->ledger().state_digest();
    const Status status = harness_.engine->compact();
    if (!status.ok()) {
      fail("compact returned " + status.error().to_string());
    }
    if (!(harness_.engine->ledger().state_digest() == before)) {
      violate("compaction changed the authoritative state");
    }
  }

  // -------------------------------------------------------------------------
  // Selection
  // -------------------------------------------------------------------------

  [[nodiscard]] GrantId pick_grant(bool issued_only) {
    std::vector<GrantId> candidates;
    for (const auto& entry : harness_.engine->ledger().grants()) {
      if (issued_only && entry.second.state != GrantState::issued) {
        continue;
      }
      if (!issued_only && entry.second.state != GrantState::issued &&
          entry.second.state != GrantState::fenced) {
        continue;
      }
      candidates.push_back(entry.first);
    }
    if (candidates.empty()) {
      return GrantId{};
    }
    return candidates[rng_.below(candidates.size())];
  }

  [[nodiscard]] CommitmentId pick_commitment(bool provisional_only) {
    std::vector<CommitmentId> candidates;
    for (const auto& entry : harness_.engine->ledger().commitments()) {
      if (provisional_only && entry.second.state != CommitmentState::provisional) {
        continue;
      }
      if (!provisional_only && !entry.second.consumes()) {
        continue;
      }
      candidates.push_back(entry.first);
    }
    if (candidates.empty()) {
      return CommitmentId{};
    }
    return candidates[rng_.below(candidates.size())];
  }

  // -------------------------------------------------------------------------
  // Invariants, checked after every action
  // -------------------------------------------------------------------------

  void check_invariants() {
    const Ledger& ledger = harness_.engine->ledger();
    const std::uint64_t sequence = ledger.sequence().value();
    if (sequence != expected_sequence_) {
      violate("the ledger sequence is " + std::to_string(sequence) + " but " +
              std::to_string(expected_sequence_) + " records were applied");
    }
    if (harness_.engine->sequence().value() != sequence) {
      violate("the engine and its ledger disagree about the committed sequence");
    }
    if (ledger.decisions().size() != admitted_) {
      violate("the ledger holds " + std::to_string(ledger.decisions().size()) +
              " decisions but " + std::to_string(admitted_) + " were admitted");
    }

    for (const auto& entry : ledger.decisions()) {
      const DecisionEntry& recorded = entry.second;
      if (!(entry.first == recorded.request.request_id)) {
        violate("a decision is filed under another identity");
      }
      if (!(recorded.decision.request_digest == recorded.request.digest())) {
        violate("a recorded decision does not bind its request content");
      }
      if (!(recorded.decision.digest == recorded.decision.compute_digest())) {
        violate("a recorded decision digest does not cover its own body");
      }
      if (!(recorded.decision.verdict == recorded.decision.blockers.implied_verdict())) {
        violate("a recorded verdict disagrees with its blocker set");
      }
      if (recorded.decision.grant.has_value() != (recorded.decision.verdict == Verdict::allow)) {
        violate("a recorded decision and its grant disagree about the verdict");
      }
    }

    for (const auto& entry : ledger.grants()) {
      const GrantEntry& grant = entry.second;
      if (!(entry.first == grant.grant.grant_id)) {
        violate("a grant is filed under another identity");
      }
      if (!(grant.grant.binding_digest == grant.grant.compute_binding_digest())) {
        violate("a grant binding digest does not cover its content");
      }
      if (grant.commitment_id.has_value() &&
          ledger.find_commitment(grant.commitment_id.value()) == nullptr) {
        violate("the grant " + grant.grant.grant_id.to_string() +
                " names a commitment the ledger does not hold");
      }
      const DecisionEntry* decision = ledger.find_decision(grant.grant.request_id);
      if (decision == nullptr || !decision->decision.grant.has_value()) {
        violate("the grant " + grant.grant.grant_id.to_string() +
                " has no decision that issued it");
      }
    }

    for (const auto& entry : ledger.commitments()) {
      const CommitmentRecord& commitment = entry.second;
      if (ledger.find_grant(commitment.grant_id) == nullptr) {
        violate("the commitment " + commitment.commitment_id.to_string() +
                " names a grant the ledger does not hold");
      }
      if (ledger.find_decision(commitment.request_id) == nullptr) {
        violate("the commitment " + commitment.commitment_id.to_string() +
                " names a request the ledger does not hold");
      }
      // The commitment lifecycle: provisional is "intent emitted, unanswered";
      // confirmed and partial are "the owner answered", which resolves the
      // question while the capacity stays consumed; released and expired mean
      // the claim is gone. Expired is the only resolved state with no answer.
      const std::string id = commitment.commitment_id.to_string();
      switch (commitment.state) {
        case CommitmentState::provisional:
          if (!commitment.consumes()) {
            violate("the provisional commitment " + id + " does not consume capacity");
          }
          if (commitment.resolved_at.has_value()) {
            violate("the unanswered commitment " + id + " carries a resolution time");
          }
          if (commitment.evidence.has_value()) {
            violate("the unanswered commitment " + id + " carries an owner answer");
          }
          break;
        case CommitmentState::confirmed:
        case CommitmentState::partial:
          if (!commitment.consumes()) {
            violate("the answered commitment " + id + " does not consume capacity");
          }
          if (!commitment.resolved_at.has_value()) {
            violate("the answered commitment " + id + " carries no resolution time");
          }
          if (!commitment.evidence.has_value()) {
            violate("the answered commitment " + id + " carries no owner answer");
          }
          break;
        case CommitmentState::released:
        case CommitmentState::expired:
          if (commitment.consumes()) {
            violate("the resolved commitment " + id + " still consumes capacity");
          }
          if (!commitment.resolved_at.has_value()) {
            violate("the resolved commitment " + id + " carries no resolution time");
          }
          if (commitment.state == CommitmentState::expired && commitment.evidence.has_value()) {
            violate("the expired commitment " + id + " carries an owner answer");
          }
          break;
      }
    }

    for (const Dimension dimension : all_dimensions()) {
      ConsumptionQuery query;
      query.facility = facility_a();
      query.observed_after = capacity_observed_;
      const std::uint64_t consumed = ledger.consumed(dimension, query);
      const std::uint64_t capacity = capacity_total().get(dimension).value();
      if (consumed > capacity) {
        violate("consumption of " + std::string(to_string(dimension)) + " reached " +
                std::to_string(consumed) + " against a configured capacity of " +
                std::to_string(capacity));
      }
    }
  }

  std::uint64_t seed_ = 0;
  std::string directory_;
  Rng rng_;
  Fixture fixture_;
  Harness harness_;
  Timestamp now_;
  Timestamp capacity_observed_;
  std::uint64_t generation_ = 1;
  std::uint64_t next_request_ = 1;
  std::uint64_t expected_sequence_ = 0;
  std::uint64_t admitted_ = 0;
  std::uint64_t committed_ = 0;
  std::uint64_t acknowledgements_ = 0;
  std::uint64_t action_index_ = 0;
  ActionKind action_kind_ = ActionKind::admit;

  bool violated_ = false;
  std::string violation_;
  std::uint64_t violation_action_ = 0;
  ActionKind violation_action_kind_ = ActionKind::admit;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// A deterministic action loop over a real durable engine. After every action the
// authoritative state must satisfy: the ledger sequence equals the number of
// applied records, the commitment lifecycle holds (provisional is unanswered
// and carries no resolution time; an answered commitment carries both its
// resolution time and the owner's answer while still consuming capacity; a
// released or expired commitment carries a resolution time, and only expired
// carries no answer), every grant that names a commitment finds it, consumption
// never exceeds the configured capacity, and every verdict agrees with its
// blocker set.
FAC_TEST(property, randomized_action_machine) {
  TempDir dir("property-machine");
  const std::uint64_t seed = 0x5EED0001ull;
  Machine machine(seed, dir.path());
  machine.run(240);

  // The run is a real durable history: it must verify and replay to the same
  // state from the bytes on disk.
  FAC_CHECK_OK(machine.harness().engine->verify());
  const Digest256 digest = machine.state_digest();
  const std::uint64_t sequence = machine.sequence();
  const auto decisions = machine.decisions();
  const auto grants = machine.grants();
  machine.close();

  Harness reopened = make_durable_harness(machine.fixture(), dir.path(), false);
  FAC_CHECK_EQ(reopened.engine->sequence().value(), sequence);
  FAC_CHECK(reopened.engine->ledger().state_digest() == digest);
  FAC_CHECK(reopened.engine->ledger().decisions() == decisions);
  FAC_CHECK(reopened.engine->ledger().grants() == grants);
}

// After a randomized run the same durable store reopens to the same state as
// many times as it is opened, and compacting and reopening yields that same
// state again.
FAC_TEST(property, replay_determinism_after_a_randomized_run) {
  TempDir dir("property-replay");
  const std::uint64_t seed = 0x5EED0002ull;
  Machine machine(seed, dir.path());
  machine.run(160);

  const Digest256 digest = machine.state_digest();
  const std::uint64_t sequence = machine.sequence();
  const auto decisions = machine.decisions();
  const auto grants = machine.grants();
  const auto commitments = machine.commitments();
  machine.close();

  Harness first = make_durable_harness(machine.fixture(), dir.path(), false);
  FAC_CHECK(first.engine->ledger().state_digest() == digest);
  FAC_CHECK_EQ(first.engine->sequence().value(), sequence);
  FAC_CHECK(first.engine->ledger().decisions() == decisions);
  FAC_CHECK(first.engine->ledger().grants() == grants);
  FAC_CHECK(first.engine->ledger().commitments() == commitments);
  FAC_CHECK_OK(first.engine->verify());
  first.engine.reset();

  Harness second = make_durable_harness(machine.fixture(), dir.path(), false);
  FAC_CHECK(second.engine->ledger().state_digest() == digest);
  FAC_CHECK_EQ(second.engine->sequence().value(), sequence);
  FAC_CHECK_OK(second.engine->verify());
  FAC_CHECK_OK(second.engine->compact());
  second.engine.reset();

  Harness third = make_durable_harness(machine.fixture(), dir.path(), false);
  FAC_CHECK(third.engine->ledger().state_digest() == digest);
  FAC_CHECK_EQ(third.engine->sequence().value(), sequence);
  FAC_CHECK(third.engine->recovery() != nullptr);
  FAC_CHECK(third.engine->recovery()->snapshot_loaded);
  FAC_CHECK(third.engine->ledger().decisions() == decisions);
  FAC_CHECK(third.engine->ledger().grants() == grants);
  FAC_CHECK(third.engine->ledger().commitments() == commitments);
}

}  // namespace
