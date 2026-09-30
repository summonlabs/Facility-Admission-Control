// Facility Admission Control - grant fencing tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A grant is authority to commit, and authority that was issued against a
// moment that has passed must be fenced rather than inherited. Each test makes
// exactly one thing move between the decision and the commit, and then requires
// that the commit is refused for that reason, that the fence is recorded, and
// that no commitment was created.

#include "fixtures.hpp"
#include "test_support.hpp"

using namespace fac;
using namespace fac_test;

namespace {

// A second, independent request for the same facility.
[[nodiscard]] Fixture second_fixture(std::uint64_t index) {
  Fixture fixture = make_fixture(index);
  return fixture;
}

}  // namespace

FAC_TEST(fencing, a_new_control_epoch_fences_an_older_grant) {
  TempDir directory("fence-epoch");
  Fixture fixture = make_fixture();
  GrantId grant_id;
  {
    Harness harness = make_durable_harness(fixture, directory.path(), true);
    auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
    FAC_CHECK_EQ(decision.verdict, Verdict::allow);
    grant_id = decision.grant->grant_id;
    FAC_CHECK_EQ(decision.grant->control_epoch.value(), harness.engine->epoch().value());
  }
  Harness reopened = make_durable_harness(fixture, directory.path(), false);
  auto outcome = FAC_TAKE(reopened.engine->commit(grant_id));
  FAC_CHECK_EQ(outcome.state, CommitState::fenced);
  FAC_CHECK(!outcome.commitment_id.has_value());
  FAC_CHECK(outcome.blockers.contains(BlockerCode::grant_fenced));
  FAC_CHECK(outcome.blockers.contains(BlockerCode::control_epoch_superseded));
  const GrantEntry* entry = reopened.engine->ledger().find_grant(grant_id);
  FAC_CHECK(entry != nullptr);
  FAC_CHECK_EQ(entry->state, GrantState::fenced);
  FAC_CHECK(reopened.engine->ledger().commitments().empty());

  // Replaying the commit returns the recorded fence rather than committing.
  auto replay = FAC_TAKE(reopened.engine->commit(grant_id));
  FAC_CHECK(replay.replayed);
  FAC_CHECK_EQ(replay.state, CommitState::fenced);
  FAC_CHECK(reopened.engine->ledger().commitments().empty());
}

FAC_TEST(fencing, an_expired_grant_is_refused_and_releases_its_hold) {
  Fixture fixture = make_fixture();
  fixture.policy.grant_validity = Duration::from_seconds(60);
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);

  harness.clock->set(at_seconds(61));
  auto outcome = FAC_TAKE(harness.engine->commit(decision.grant->grant_id));
  FAC_CHECK_EQ(outcome.state, CommitState::expired);
  FAC_CHECK(!outcome.commitment_id.has_value());
  const GrantEntry* entry = harness.engine->ledger().find_grant(decision.grant->grant_id);
  FAC_CHECK(entry != nullptr);
  FAC_CHECK_EQ(entry->state, GrantState::expired);
}

FAC_TEST(fencing, a_swept_hold_returns_the_capacity_to_the_next_request) {
  Fixture fixture = make_fixture();
  fixture.policy.grant_validity = Duration::from_seconds(60);
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);

  Fixture competing = second_fixture(2);
  FAC_CHECK_OK(competing.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(competing.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(competing.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto refused = FAC_TAKE(harness.engine->admit(competing.request, competing.evidence));
  FAC_CHECK_EQ(refused.verdict, Verdict::refuse);
  FAC_CHECK_EQ(refused.primary_blocker().value(), BlockerCode::capacity_exhausted);

  // After the hold's validity window, evaluating another request sweeps it and
  // the capacity is available again. The new attempt is a new request: the
  // refused one keeps its recorded decision, which is exactly the behaviour an
  // operator relies on when a retry must not be re-decided.
  harness.clock->set(at_seconds(120));
  Fixture later = make_fixture(3);
  FAC_CHECK_OK(later.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(later.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(later.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  later.evidence.capacity->observed_at = at_seconds(120);
  later.evidence.redundancy->observed_at = at_seconds(120);
  later.evidence.tenant->observed_at = at_seconds(120);
  later.evidence.envelope->observed_at = at_seconds(120);
  later.evidence.service_class->observed_at = at_seconds(120);
  later.evidence.incident->observed_at = at_seconds(120);
  later.evidence.policy->observed_at = at_seconds(120);
  later.request.requested_at = at_seconds(120);
  later.request.commitment_start = at_seconds(120);
  later.request.commitment_end = at_seconds(4800);
  auto allowed = FAC_TAKE(harness.engine->admit(later.request, later.evidence));
  FAC_CHECK_VERDICT(allowed, Verdict::allow);
  // The earlier refusal keeps its recorded answer.
  auto replayed = FAC_TAKE(harness.engine->admit(competing.request, competing.evidence));
  FAC_CHECK(replayed.replayed);
  FAC_CHECK_EQ(replayed.verdict, Verdict::refuse);
  const GrantEntry* first_entry = harness.engine->ledger().find_grant(first.grant->grant_id);
  FAC_CHECK(first_entry != nullptr);
  FAC_CHECK_EQ(first_entry->state, GrantState::expired);
}

FAC_TEST(fencing, consumption_after_the_decision_fences_the_grant) {
  // Without holds two grants are issued against the same 300 W, and only one of
  // them can be committed.
  Fixture fixture = make_fixture();
  fixture.policy.grant_holds_capacity = false;
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 300));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_VERDICT(first, Verdict::allow);

  Fixture competing = second_fixture(2);
  FAC_CHECK_OK(competing.evidence.capacity->total.set(Dimension::power, 300));
  FAC_CHECK_OK(competing.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(competing.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto second = FAC_TAKE(harness.engine->admit(competing.request, competing.evidence));
  FAC_CHECK_VERDICT(second, Verdict::allow);

  auto committed = FAC_TAKE(harness.engine->commit(first.grant->grant_id));
  FAC_CHECK_EQ(committed.state, CommitState::committed);

  auto fenced = FAC_TAKE(harness.engine->commit(second.grant->grant_id));
  FAC_CHECK_EQ(fenced.state, CommitState::fenced);
  FAC_CHECK(fenced.blockers.contains(BlockerCode::capacity_exhausted));
  FAC_CHECK_EQ(harness.engine->ledger().commitments().size(), static_cast<std::size_t>(1));
}

FAC_TEST(fencing, a_generation_that_moved_fences_the_grant) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);

  Fixture moved = make_fixture();
  moved.evidence.capacity->generation = CapacityGeneration(2);
  moved.evidence.capacity->observed_at = at_seconds(1);
  auto outcome = FAC_TAKE(harness.engine->commit(decision.grant->grant_id, &moved.evidence));
  FAC_CHECK_EQ(outcome.state, CommitState::fenced);
  FAC_CHECK(outcome.blockers.contains(BlockerCode::evidence_superseded));
  FAC_CHECK(outcome.blockers.contains(BlockerCode::grant_fenced));
  FAC_CHECK(harness.engine->ledger().commitments().empty());
}

FAC_TEST(fencing, changed_content_at_the_same_generation_fences_the_grant) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);

  Fixture changed = make_fixture();
  FAC_CHECK_OK(changed.evidence.capacity->total.set(Dimension::power, 200000));
  auto outcome = FAC_TAKE(harness.engine->commit(decision.grant->grant_id, &changed.evidence));
  FAC_CHECK_EQ(outcome.state, CommitState::fenced);
  FAC_CHECK(outcome.blockers.contains(BlockerCode::evidence_superseded));
  FAC_CHECK(harness.engine->ledger().commitments().empty());
}

FAC_TEST(fencing, unchanged_evidence_commits) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  auto outcome = FAC_TAKE(harness.engine->commit(decision.grant->grant_id, &fixture.evidence));
  FAC_CHECK_EQ(outcome.state, CommitState::committed);
}

FAC_TEST(fencing, an_explicit_fence_records_the_cause_and_is_idempotent) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  auto fenced = FAC_TAKE(harness.engine->fence_grant(decision.grant->grant_id,
                                                     BlockerCode::incident_major,
                                                     "operator observed a major incident"));
  FAC_CHECK_EQ(fenced.state, GrantState::fenced);
  FAC_CHECK_EQ(fenced.blockers.size(), static_cast<std::size_t>(2));
  FAC_CHECK_EQ(fenced.blockers.front().code, BlockerCode::grant_fenced);

  auto again = FAC_TAKE(harness.engine->fence_grant(decision.grant->grant_id, BlockerCode::incident_major,
                                                    "operator observed a major incident"));
  FAC_CHECK_EQ(again.state, GrantState::fenced);
  FAC_CHECK_EQ(again.blockers.size(), static_cast<std::size_t>(4));

  auto commit = FAC_TAKE(harness.engine->commit(decision.grant->grant_id));
  FAC_CHECK_EQ(commit.state, CommitState::fenced);
  FAC_CHECK(harness.engine->ledger().commitments().empty());
}

FAC_TEST(fencing, a_released_grant_returns_its_hold) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);

  Fixture competing = second_fixture(2);
  FAC_CHECK_OK(competing.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(competing.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(competing.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_EQ(FAC_TAKE(harness.engine->admit(competing.request, competing.evidence)).verdict, Verdict::refuse);

  FAC_CHECK_EQ(FAC_TAKE(harness.engine->release_grant(first.grant->grant_id, "caller withdrew")).state,
               GrantState::released);
  // The refused attempt keeps its decision, so the retry after the release is a
  // new request.
  Fixture retry = second_fixture(3);
  FAC_CHECK_OK(retry.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(retry.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(retry.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto allowed = FAC_TAKE(harness.engine->admit(retry.request, retry.evidence));
  FAC_CHECK_VERDICT(allowed, Verdict::allow);
}

FAC_TEST(fencing, a_fenced_commit_is_durable_and_survives_a_restart) {
  TempDir directory("fence-durable");
  Fixture fixture = make_fixture();
  GrantId grant_id;
  {
    Harness harness = make_durable_harness(fixture, directory.path(), true);
    auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
    grant_id = decision.grant->grant_id;
    auto outcome = FAC_TAKE(harness.engine->fence_grant(grant_id, BlockerCode::redundancy_level_insufficient,
                                                        "redundancy was withdrawn"));
    FAC_CHECK_EQ(outcome.state, GrantState::fenced);
  }
  Harness reopened = make_durable_harness(fixture, directory.path(), false);
  const GrantEntry* entry = reopened.engine->ledger().find_grant(grant_id);
  FAC_CHECK(entry != nullptr);
  FAC_CHECK_EQ(entry->state, GrantState::fenced);
  FAC_CHECK(entry->blockers.size() >= static_cast<std::size_t>(2));
  auto outcome = FAC_TAKE(reopened.engine->commit(grant_id));
  FAC_CHECK_EQ(outcome.state, CommitState::fenced);
  FAC_CHECK(reopened.engine->ledger().commitments().empty());
}

FAC_TEST(fencing, a_partial_acknowledgement_keeps_the_whole_demand_counted) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  auto committed = FAC_TAKE(harness.engine->commit(decision.grant->grant_id));
  const CommitmentRecord* commitment =
      harness.engine->ledger().find_commitment(committed.commitment_id.value());
  FAC_CHECK(commitment != nullptr);

  ReservationEvidence evidence;
  evidence.intent_id = commitment->intent.intent_id;
  evidence.reservation = FAC_TAKE(ReservationId::from_hex("c0ffee00-0000-0000-0000-000000000002"));
  evidence.owner = commitment->intent.owner;
  evidence.outcome = ReservationOutcome::partially_reserved;
  FAC_CHECK_OK(evidence.confirmed.set(Dimension::power, 100));
  evidence.owner_generation = 3;
  evidence.owner_digest = fixture_digest("partial-answer");
  evidence.acknowledged_at = at_seconds(5);

  auto recorded = FAC_TAKE(harness.engine->record_evidence(evidence));
  FAC_CHECK_EQ(recorded.state, CommitmentState::partial);
  // The whole demand is still consumed: an owner that confirmed less than was
  // asked for has not returned the difference.
  const ConsumptionQuery query{facility_a(), rack_1(), Timestamp{}};
  FAC_CHECK_EQ(harness.engine->ledger().consumed(Dimension::power, query), 200u);

  Fixture competing = second_fixture(2);
  FAC_CHECK_OK(competing.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(competing.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(competing.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto refused = FAC_TAKE(harness.engine->admit(competing.request, competing.evidence));
  FAC_CHECK_EQ(refused.verdict, Verdict::refuse);
  FAC_CHECK_EQ(refused.primary_blocker().value(), BlockerCode::capacity_exhausted);
}

FAC_TEST(fencing, a_rejected_reservation_releases_the_capacity) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  auto committed = FAC_TAKE(harness.engine->commit(decision.grant->grant_id));
  const CommitmentRecord* commitment =
      harness.engine->ledger().find_commitment(committed.commitment_id.value());

  ReservationEvidence evidence;
  evidence.intent_id = commitment->intent.intent_id;
  evidence.reservation = FAC_TAKE(ReservationId::from_hex("c0ffee00-0000-0000-0000-000000000003"));
  evidence.owner = commitment->intent.owner;
  evidence.outcome = ReservationOutcome::rejected;
  evidence.owner_generation = 4;
  evidence.owner_digest = fixture_digest("rejected-answer");
  evidence.acknowledged_at = at_seconds(5);
  auto recorded = FAC_TAKE(harness.engine->record_evidence(evidence));
  FAC_CHECK_EQ(recorded.state, CommitmentState::released);

  Fixture competing = second_fixture(2);
  FAC_CHECK_OK(competing.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(competing.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(competing.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto allowed = FAC_TAKE(harness.engine->admit(competing.request, competing.evidence));
  FAC_CHECK_VERDICT(allowed, Verdict::allow);
}

FAC_TEST(fencing, an_intent_window_that_passes_expires_the_commitment) {
  Fixture fixture = make_fixture();
  fixture.policy.intent_validity = Duration::from_seconds(60);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  auto committed = FAC_TAKE(harness.engine->commit(decision.grant->grant_id));
  FAC_CHECK_EQ(harness.engine->ledger().find_commitment(committed.commitment_id.value())->state,
               CommitmentState::provisional);

  harness.clock->set(at_seconds(61));
  Fixture next = second_fixture(2);
  next.evidence.capacity->observed_at = at_seconds(61);
  next.evidence.redundancy->observed_at = at_seconds(61);
  next.evidence.tenant->observed_at = at_seconds(61);
  next.evidence.envelope->observed_at = at_seconds(61);
  next.evidence.service_class->observed_at = at_seconds(61);
  next.evidence.incident->observed_at = at_seconds(61);
  next.evidence.policy->observed_at = at_seconds(61);
  next.request.requested_at = at_seconds(61);
  next.request.commitment_start = at_seconds(61);
  next.request.commitment_end = at_seconds(3661);
  (void)FAC_TAKE(harness.engine->admit(next.request, next.evidence));

  const CommitmentRecord* expired =
      harness.engine->ledger().find_commitment(committed.commitment_id.value());
  FAC_CHECK(expired != nullptr);
  FAC_CHECK_EQ(expired->state, CommitmentState::expired);
}

