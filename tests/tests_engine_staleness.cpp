// Facility Admission Control - staleness and supersession tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Stale, absent, superseded and unmeasured evidence must never become available
// capacity. Each test presents one degraded authority and asserts the exact
// blocker, so an operator reading the refusal knows which authority to refresh.

#include "fixtures.hpp"
#include "test_support.hpp"

using namespace fac;
using namespace fac_test;

FAC_TEST(staleness, stale_capacity_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity->observed_at = at_seconds(-400);  // budget is 300 s
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::capacity_stale);
  FAC_CHECK(!decision.grant.has_value());
  // The rejected evidence is still recorded, with the reason.
  const EvidenceRef* reference = decision.evidence.find(EvidenceKind::capacity);
  FAC_CHECK(reference != nullptr);
  FAC_CHECK(!reference->accepted);
  FAC_CHECK_NE(reference->rejection, ErrorCode::ok);
}

FAC_TEST(staleness, capacity_inside_its_budget_is_usable) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity->observed_at = at_seconds(-299);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
}

FAC_TEST(staleness, each_authority_has_its_own_budget) {
  Fixture fixture = make_fixture();
  fixture.evidence.service_class->observed_at = at_seconds(-7200);  // budget is 86400 s
  fixture.evidence.incident->observed_at = at_seconds(-200);        // budget is 120 s
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::incident_stale);
  FAC_CHECK(decision.evidence.find(EvidenceKind::service_class)->accepted);
  FAC_CHECK(!decision.evidence.find(EvidenceKind::incident)->accepted);
}

FAC_TEST(staleness, future_dated_evidence_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity->observed_at = at_seconds(600);  // skew allowance is 60 s
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::evidence_future_dated);
}

FAC_TEST(staleness, small_clock_skew_is_tolerated) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity->observed_at = at_seconds(30);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
}

FAC_TEST(staleness, a_pinned_generation_that_moved_defers) {
  Fixture fixture = make_fixture();
  GenerationPin pin;
  pin.kind = EvidenceKind::capacity;
  pin.generation = 9;
  fixture.request.pins.push_back(pin);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::generation_mismatch);
}

FAC_TEST(staleness, a_pinned_generation_that_matches_is_accepted) {
  Fixture fixture = make_fixture();
  GenerationPin pin;
  pin.kind = EvidenceKind::capacity;
  pin.generation = 1;
  fixture.request.pins.push_back(pin);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
}

FAC_TEST(staleness, missing_capacity_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity.reset();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::capacity_missing);
}

FAC_TEST(staleness, missing_redundancy_evidence_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.redundancy.reset();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::redundancy_missing);
}

FAC_TEST(staleness, every_required_authority_can_be_named) {
  struct Case {
    EvidenceKind kind;
    BlockerCode expected;
  };
  const Case cases[] = {
      {EvidenceKind::capacity, BlockerCode::capacity_missing},
      {EvidenceKind::redundancy, BlockerCode::redundancy_missing},
      {EvidenceKind::tenant, BlockerCode::tenant_missing},
      {EvidenceKind::envelope, BlockerCode::envelope_missing},
      {EvidenceKind::service_class, BlockerCode::service_class_missing},
      {EvidenceKind::maintenance, BlockerCode::maintenance_missing},
      {EvidenceKind::incident, BlockerCode::incident_missing},
      {EvidenceKind::placement, BlockerCode::placement_missing},
      {EvidenceKind::policy, BlockerCode::policy_missing},
  };
  for (const Case& item : cases) {
    Fixture fixture = make_fixture();
    switch (item.kind) {
      case EvidenceKind::capacity: fixture.evidence.capacity.reset(); break;
      case EvidenceKind::redundancy: fixture.evidence.redundancy.reset(); break;
      case EvidenceKind::tenant: fixture.evidence.tenant.reset(); break;
      case EvidenceKind::envelope: fixture.evidence.envelope.reset(); break;
      case EvidenceKind::service_class: fixture.evidence.service_class.reset(); break;
      case EvidenceKind::maintenance: fixture.evidence.maintenance.reset(); break;
      case EvidenceKind::incident: fixture.evidence.incident.reset(); break;
      case EvidenceKind::placement: fixture.evidence.placement.reset(); break;
      case EvidenceKind::policy: fixture.evidence.policy.reset(); break;
    }
    Harness harness = make_harness(fixture);
    auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
    FAC_CHECK_EQ(decision.verdict, Verdict::defer);
    FAC_CHECK_EQ(decision.primary_blocker().value(), item.expected);
  }
}

FAC_TEST(staleness, an_authority_that_is_not_required_may_be_absent) {
  Fixture fixture = make_fixture();
  fixture.policy.required.placement = false;
  fixture.policy.required.capacity = false;
  fixture.policy.required.redundancy = false;
  fixture.policy.required.maintenance = false;
  fixture.policy.required.incident = false;
  fixture.evidence.placement.reset();
  fixture.evidence.capacity.reset();
  fixture.evidence.redundancy.reset();
  fixture.evidence.maintenance.reset();
  fixture.evidence.incident.reset();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK(!decision.evidence.contains(EvidenceKind::capacity));
  // A grant binds what it used, and it used less.
  FAC_CHECK_EQ(decision.grant->bindings.size(), static_cast<std::size_t>(4));
}

FAC_TEST(staleness, superseded_generations_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity->generation = CapacityGeneration(5);
  Harness harness = make_harness(fixture);
  FAC_CHECK_EQ(FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence)).verdict, Verdict::allow);

  // The same facility, one generation older: newer capacity was already
  // accepted, so this snapshot cannot re-open capacity that may already be
  // consumed.
  Fixture older = make_fixture(2);
  older.evidence.capacity->generation = CapacityGeneration(4);
  older.evidence.capacity->total = amounts(100000, 100000, 1000, 1000);
  auto decision = FAC_TAKE(harness.engine->admit(older.request, older.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::evidence_superseded);
}

FAC_TEST(staleness, a_newer_generation_is_accepted) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity->generation = CapacityGeneration(3);
  Harness harness = make_harness(fixture);
  FAC_CHECK_EQ(FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence)).verdict, Verdict::allow);

  Fixture newer = make_fixture(2);
  newer.evidence.capacity->generation = CapacityGeneration(4);
  auto decision = FAC_TAKE(harness.engine->admit(newer.request, newer.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
}

FAC_TEST(staleness, unmeasured_dimensions_cannot_be_assumed_to_fit) {
  Fixture fixture = make_fixture();
  fixture.evidence.capacity->total = AmountVector::unknown();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::capacity_unknown);
  FAC_CHECK(decision.blockers.items().size() >= static_cast<std::size_t>(4));
}

FAC_TEST(staleness, an_unmeasured_protected_headroom_cannot_become_zero) {
  Fixture fixture = make_fixture();
  fixture.evidence.redundancy->protected_headroom = AmountVector::unknown();
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 100));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::redundancy_unknown);
  FAC_CHECK(has_blocker(decision, BlockerCode::redundancy_unknown));
}

FAC_TEST(staleness, an_expired_request_is_refused) {
  Fixture fixture = make_fixture();
  fixture.request.requested_at = at_seconds(-600);  // maximum age is 300 s
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::request_expired);
}

FAC_TEST(staleness, a_commitment_window_that_has_passed_is_refused) {
  Fixture fixture = make_fixture();
  fixture.request.commitment_start = at_seconds(-7200);
  fixture.request.commitment_end = at_seconds(-3600);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::request_expired);
}

FAC_TEST(staleness, a_horizon_beyond_the_policy_is_refused) {
  Fixture fixture = make_fixture();
  fixture.policy.max_commitment_duration = Duration::from_seconds(60);
  fixture.request.commitment_end = at_seconds(3600);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::horizon_exceeded);
}

FAC_TEST(staleness, a_superseded_control_epoch_is_refused) {
  Fixture fixture = make_fixture();
  fixture.request.expected_epoch = ControlEpoch(7);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::control_epoch_superseded);
}

FAC_TEST(staleness, an_unstated_demand_defers) {
  Fixture fixture = make_fixture();
  fixture.request.demand = AmountVector::unknown();
  FAC_CHECK_OK(fixture.request.demand.set(Dimension::power, 200));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK(has_blocker(decision, BlockerCode::demand_unspecified));
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::demand_unspecified);
}

FAC_TEST(staleness, evidence_about_another_tenant_is_refused_as_unusable) {
  Fixture fixture = make_fixture();
  fixture.evidence.tenant->tenant = tenant_beta();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::tenant_unknown);
  FAC_CHECK(!decision.evidence.find(EvidenceKind::tenant)->accepted);
}

FAC_TEST(staleness, evidence_about_another_envelope_is_refused_as_unusable) {
  Fixture fixture = make_fixture();
  fixture.evidence.envelope->envelope = FAC_TAKE(EnvelopeId::from_hex("77777777-7777-7777-7777-777777777772"));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::envelope_unknown);
}

FAC_TEST(staleness, evidence_about_another_service_class_is_refused_as_unusable) {
  Fixture fixture = make_fixture();
  fixture.evidence.service_class->service_class =
      FAC_TAKE(ServiceClassId::from_hex("66666666-6666-6666-6666-666666666662"));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::service_class_unknown);
}

