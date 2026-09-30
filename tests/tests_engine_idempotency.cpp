// Facility Admission Control - idempotency and replay tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A lost response is the normal case for a caller that did not hear back. The
// retry must return what was already decided, and it must do so before any
// staleness rule can reject the request, because by then every generation in it
// is genuinely stale.

#include "fixtures.hpp"
#include "test_support.hpp"

using namespace fac;
using namespace fac_test;

FAC_TEST(idempotency, a_replayed_request_returns_the_recorded_decision) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);
  FAC_CHECK(!first.replayed);
  const LedgerSequence after_first = harness.engine->sequence();

  auto replay = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK(replay.replayed);
  FAC_CHECK_EQ(replay.digest, first.digest);
  FAC_CHECK_EQ(replay.verdict, first.verdict);
  FAC_CHECK_EQ(replay.grant->grant_id, first.grant->grant_id);
  // A replay is not a second decision.
  FAC_CHECK_EQ(harness.engine->sequence(), after_first);
  FAC_CHECK_EQ(harness.engine->ledger().decisions().size(), static_cast<std::size_t>(1));
  FAC_CHECK_EQ(harness.engine->ledger().grants().size(), static_cast<std::size_t>(1));
}

FAC_TEST(idempotency, a_replay_is_resolved_before_staleness) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);

  // Everything the original decision used is now stale and the generations have
  // moved on. An evaluation would defer; a replay must not.
  harness.clock->set(at_seconds(10000));
  // The retry carries the same request bytes and evidence that is now far
  // outside every freshness budget. An evaluation would defer; the replay must
  // not, because the answer to that request already exists.
  Fixture stale = make_fixture();
  stale.evidence.capacity->observed_at = at_seconds(10000);
  stale.evidence.capacity->generation = CapacityGeneration(99);
  stale.evidence.tenant->observed_at = at_seconds(10000);
  stale.evidence.envelope->observed_at = at_seconds(10000);
  stale.evidence.service_class->observed_at = at_seconds(10000);
  stale.evidence.incident->observed_at = at_seconds(10000);
  stale.evidence.policy->observed_at = at_seconds(10000);
  auto replay = FAC_TAKE(harness.engine->admit(fixture.request, stale.evidence));
  FAC_CHECK(replay.replayed);
  FAC_CHECK_EQ(replay.verdict, Verdict::allow);
  FAC_CHECK_EQ(replay.digest, first.digest);
  FAC_CHECK_EQ(harness.engine->ledger().decisions().size(), static_cast<std::size_t>(1));
}

FAC_TEST(idempotency, the_same_identity_with_different_content_is_a_conflict) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  FAC_CHECK_EQ(FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence)).verdict, Verdict::allow);

  AdmissionRequest conflicting = fixture.request;
  FAC_CHECK_OK(conflicting.demand.set(Dimension::power, 999));
  auto decision = FAC_TAKE(harness.engine->admit(conflicting, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::idempotency_conflict);
  FAC_CHECK(!decision.replayed);
  // The refusal is not recorded: the identity already belongs to the first
  // decision, and the ledger is not rewritten.
  FAC_CHECK_EQ(harness.engine->ledger().decisions().size(), static_cast<std::size_t>(1));
  FAC_CHECK_EQ(FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence)).verdict, Verdict::allow);
}

FAC_TEST(idempotency, a_request_without_an_identity_is_answered_but_not_recorded) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  AdmissionRequest anonymous = fixture.request;
  anonymous.request_id = RequestId{};
  auto decision = FAC_TAKE(harness.engine->admit(anonymous, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::request_invalid);
  FAC_CHECK(harness.engine->ledger().decisions().empty());
  FAC_CHECK_EQ(harness.engine->sequence().value(), 0u);
}

FAC_TEST(idempotency, replay_survives_a_new_control_epoch) {
  TempDir directory("replay-epoch");
  Fixture fixture = make_fixture();
  GrantId original_grant;
  {
    Harness harness = make_durable_harness(fixture, directory.path(), true);
    auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
    FAC_CHECK_EQ(decision.verdict, Verdict::allow);
    original_grant = decision.grant->grant_id;
  }
  // A new process incarnation advances the control epoch. The decision it
  // recorded is still the answer to the same request with the same content.
  Harness reopened = make_durable_harness(fixture, directory.path(), false);
  FAC_CHECK(reopened.engine->epoch().value() > 1u);
  auto replay = FAC_TAKE(reopened.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK(replay.replayed);
  FAC_CHECK_EQ(replay.grant->grant_id, original_grant);
  FAC_CHECK_EQ(replay.verdict, Verdict::allow);
}

FAC_TEST(idempotency, committing_twice_returns_the_same_commitment) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  const GrantId grant_id = decision.grant->grant_id;

  auto first = FAC_TAKE(harness.engine->commit(grant_id));
  FAC_CHECK_EQ(first.state, CommitState::committed);
  FAC_CHECK(first.commitment_id.has_value());
  FAC_CHECK(!first.replayed);
  const LedgerSequence after_first = harness.engine->sequence();

  auto second = FAC_TAKE(harness.engine->commit(grant_id));
  FAC_CHECK_EQ(second.state, CommitState::committed);
  FAC_CHECK_EQ(second.commitment_id.value(), first.commitment_id.value());
  FAC_CHECK(second.replayed);
  FAC_CHECK_EQ(harness.engine->sequence(), after_first);
  FAC_CHECK_EQ(harness.engine->ledger().commitments().size(), static_cast<std::size_t>(1));
}

FAC_TEST(idempotency, recording_the_same_evidence_twice_is_the_same_answer) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  auto committed = FAC_TAKE(harness.engine->commit(decision.grant->grant_id));
  const CommitmentRecord* commitment =
      harness.engine->ledger().find_commitment(committed.commitment_id.value());
  FAC_CHECK(commitment != nullptr);

  ReservationEvidence evidence;
  evidence.intent_id = commitment->intent.intent_id;
  evidence.reservation = FAC_TAKE(ReservationId::from_hex("c0ffee00-0000-0000-0000-000000000001"));
  evidence.owner = commitment->intent.owner;
  evidence.outcome = ReservationOutcome::reserved;
  evidence.confirmed = fixture.request.demand;
  evidence.owner_generation = 1;
  evidence.owner_digest = fixture_digest("owner-answer");
  evidence.acknowledged_at = at_seconds(10);

  auto first = FAC_TAKE(harness.engine->record_evidence(evidence));
  FAC_CHECK_EQ(first.state, CommitmentState::confirmed);
  const LedgerSequence after_first = harness.engine->sequence();

  auto second = FAC_TAKE(harness.engine->record_evidence(evidence));
  FAC_CHECK_EQ(second.state, CommitmentState::confirmed);
  FAC_CHECK_EQ(harness.engine->sequence(), after_first);

  ReservationEvidence conflicting = evidence;
  conflicting.outcome = ReservationOutcome::rejected;
  conflicting.confirmed = AmountVector::unknown();
  auto refused = harness.engine->record_evidence(conflicting);
  FAC_CHECK_ERR(refused, ErrorCode::conflict);
}

FAC_TEST(idempotency, releasing_twice_is_the_same_answer) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  auto committed = FAC_TAKE(harness.engine->commit(decision.grant->grant_id));
  auto first = FAC_TAKE(harness.engine->release_commitment(committed.commitment_id.value(), "operator release"));
  FAC_CHECK_EQ(first.state, CommitmentState::released);
  const LedgerSequence after_first = harness.engine->sequence();
  auto second = FAC_TAKE(harness.engine->release_commitment(committed.commitment_id.value(), "operator release"));
  FAC_CHECK_EQ(second.state, CommitmentState::released);
  FAC_CHECK_EQ(harness.engine->sequence(), after_first);
}

FAC_TEST(idempotency, releasing_an_unknown_commitment_is_an_error) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  const CommitmentId unknown =
      FAC_TAKE(CommitmentId::from_hex("deadbeef-0000-0000-0000-000000000001"));
  FAC_CHECK_ERR(harness.engine->release_commitment(unknown, "nothing to release"), ErrorCode::not_found);
  FAC_CHECK_ERR(harness.engine->commit(FAC_TAKE(GrantId::from_hex("deadbeef-0000-0000-0000-000000000002"))),
                ErrorCode::not_found);
}

FAC_TEST(idempotency, a_released_grant_is_not_reissued) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  auto released = FAC_TAKE(harness.engine->release_grant(decision.grant->grant_id, "caller withdrew"));
  FAC_CHECK_EQ(released.state, GrantState::released);
  // The request identity is still taken, so the retry replays the decision
  // rather than issuing a new grant.
  auto replay = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK(replay.replayed);
  FAC_CHECK_EQ(replay.grant->grant_id, decision.grant->grant_id);
  FAC_CHECK_EQ(harness.engine->ledger().grants().size(), static_cast<std::size_t>(1));
}

