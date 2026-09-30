// Facility Admission Control - boundary tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// "Exactly at the boundary" is the case where a comparison written with the
// wrong operator is invisible until a facility runs out of power. Every test
// here sits on the boundary itself and one unit either side of it.

#include "fixtures.hpp"
#include "test_support.hpp"

using namespace fac;
using namespace fac_test;

FAC_TEST(boundary, demand_equal_to_available_is_allowed) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 200));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  const DimensionAssessment* power = decision.assessment.find(Dimension::power);
  FAC_CHECK(power != nullptr);
  FAC_CHECK_EQ(power->available.value(), 200u);
  FAC_CHECK_EQ(power->remaining.value(), 0);
  FAC_CHECK_EQ(power->overcommit.value(), 0u);
}

FAC_TEST(boundary, one_unit_above_available_is_refused) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 199));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::capacity_exhausted);
  const DimensionAssessment* power = decision.assessment.find(Dimension::power);
  FAC_CHECK(power != nullptr);
  FAC_CHECK_EQ(power->remaining.value(), -1);
}

FAC_TEST(boundary, committed_and_reserved_are_both_subtracted) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 1000));
  FAC_CHECK_OK(fixture.evidence.capacity->committed.set(Dimension::power, 700));
  FAC_CHECK_OK(fixture.evidence.capacity->reserved.set(Dimension::power, 100));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK_EQ(decision.assessment.find(Dimension::power)->available.value(), 200u);
  FAC_CHECK_EQ(decision.assessment.find(Dimension::power)->remaining.value(), 0);
}

FAC_TEST(boundary, inconsistent_capacity_accounting_defers) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::cooling, 100));
  FAC_CHECK_OK(fixture.evidence.capacity->committed.set(Dimension::cooling, 80));
  FAC_CHECK_OK(fixture.evidence.capacity->reserved.set(Dimension::cooling, 40));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::capacity_unknown);
}

FAC_TEST(boundary, overcommit_is_refused_when_policy_forbids_it) {
  Fixture fixture = make_fixture();
  fixture.evidence.service_class->obligations.permits_overcommit = true;
  fixture.evidence.policy->overcommit.mode = OvercommitMode::not_permitted;
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::space, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::overcommit_not_permitted);
}

FAC_TEST(boundary, overcommit_is_refused_when_the_service_class_forbids_it) {
  Fixture fixture = make_fixture();
  fixture.evidence.service_class->obligations.permits_overcommit = false;
  fixture.evidence.policy->overcommit.mode = OvercommitMode::permitted_unbounded;
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::space, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::capacity_exhausted);
}

FAC_TEST(boundary, permitted_overcommit_is_allowed_and_recorded) {
  Fixture fixture = make_fixture();
  fixture.evidence.service_class->obligations.permits_overcommit = true;
  fixture.evidence.policy->overcommit.mode = OvercommitMode::permitted_up_to;
  fixture.evidence.policy->overcommit.allowance = amounts(50, 50, 5, 5);
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::space, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK(decision.assessment.overcommit_used());
  const DimensionAssessment* space = decision.assessment.find(Dimension::space);
  FAC_CHECK_EQ(space->overcommit.value(), 1u);
  FAC_CHECK(decision.grant.has_value());
}

FAC_TEST(boundary, overcommit_beyond_the_allowance_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.service_class->obligations.permits_overcommit = true;
  fixture.evidence.policy->overcommit.mode = OvercommitMode::permitted_up_to;
  fixture.evidence.policy->overcommit.allowance = amounts(50, 50, 5, 5);
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::space, 0));
  FAC_CHECK_OK(fixture.request.demand.set(Dimension::space, 6));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::overcommit_limit_exceeded);
}

FAC_TEST(boundary, overcommit_at_the_allowance_is_allowed) {
  Fixture fixture = make_fixture();
  fixture.evidence.service_class->obligations.permits_overcommit = true;
  fixture.evidence.policy->overcommit.mode = OvercommitMode::permitted_up_to;
  fixture.evidence.policy->overcommit.allowance = amounts(50, 50, 5, 5);
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::space, 0));
  FAC_CHECK_OK(fixture.request.demand.set(Dimension::space, 5));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK_EQ(decision.assessment.find(Dimension::space)->overcommit.value(), 5u);
}

FAC_TEST(boundary, protected_headroom_floor_is_enforced_exactly) {
  Fixture fixture = make_fixture();
  // 400 W total, 200 W demand, and 200 W that must remain protected.
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 200));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  // 400 W total, 200 W demanded, so exactly the 200 W floor remains.
  FAC_CHECK_EQ(decision.assessment.find(Dimension::power)->remaining.value(), 200);

  Fixture tighter = make_fixture();
  FAC_CHECK_OK(tighter.evidence.capacity->total.set(Dimension::power, 399));
  FAC_CHECK_OK(tighter.evidence.redundancy->protected_headroom.set(Dimension::power, 200));
  FAC_CHECK_OK(
      tighter.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness tighter_harness = make_harness(tighter);
  auto refused = FAC_TAKE(tighter_harness.engine->admit(tighter.request, tighter.evidence));
  FAC_CHECK_EQ(refused.verdict, Verdict::refuse);
  FAC_CHECK_EQ(refused.primary_blocker().value(), BlockerCode::protected_headroom_insufficient);
}

FAC_TEST(boundary, the_service_class_floor_can_exceed_the_reported_floor) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 250));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::protected_headroom_insufficient);
}

FAC_TEST(boundary, envelope_remaining_is_exact) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.envelope->limit.set(Dimension::slots, 5));
  FAC_CHECK_OK(fixture.evidence.envelope->consumed.set(Dimension::slots, 4));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);

  Fixture tighter = make_fixture();
  FAC_CHECK_OK(tighter.evidence.envelope->limit.set(Dimension::slots, 5));
  FAC_CHECK_OK(tighter.evidence.envelope->consumed.set(Dimension::slots, 4));
  FAC_CHECK_OK(tighter.request.demand.set(Dimension::slots, 2));
  Harness tighter_harness = make_harness(tighter);
  auto refused = FAC_TAKE(tighter_harness.engine->admit(tighter.request, tighter.evidence));
  FAC_CHECK_EQ(refused.verdict, Verdict::refuse);
  FAC_CHECK_EQ(refused.primary_blocker().value(), BlockerCode::envelope_exhausted);
}

FAC_TEST(boundary, a_granted_hold_consumes_capacity_for_the_next_request) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);

  Fixture second = make_fixture(2);
  FAC_CHECK_OK(second.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(second.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(second.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto competing = FAC_TAKE(harness.engine->admit(second.request, second.evidence));
  // The first grant holds 200 W, so only 200 W remain and the second fits
  // exactly. A third identical request must not.
  FAC_CHECK_EQ(competing.verdict, Verdict::allow);

  Fixture third = make_fixture(3);
  FAC_CHECK_OK(third.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(third.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(third.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto exhausted = FAC_TAKE(harness.engine->admit(third.request, third.evidence));
  FAC_CHECK_EQ(exhausted.verdict, Verdict::refuse);
  FAC_CHECK_EQ(exhausted.primary_blocker().value(), BlockerCode::capacity_exhausted);
  FAC_CHECK_EQ(exhausted.assessment.find(Dimension::power)->available.value(), 0u);
}

FAC_TEST(boundary, without_holds_two_grants_are_issued_for_the_same_capacity) {
  Fixture fixture = make_fixture();
  fixture.policy.grant_holds_capacity = false;
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(fixture.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(fixture.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);
  FAC_CHECK(!first.grant->holds_capacity);

  Fixture second = make_fixture(2);
  FAC_CHECK_OK(second.evidence.capacity->total.set(Dimension::power, 400));
  FAC_CHECK_OK(second.evidence.redundancy->protected_headroom.set(Dimension::power, 0));
  FAC_CHECK_OK(second.evidence.service_class->obligations.required_protected_headroom.set(Dimension::power, 0));
  auto competing = FAC_TAKE(harness.engine->admit(second.request, second.evidence));
  // A point-in-time decision is not a hold: both are allowed, and the conflict
  // is resolved when they are committed.
  FAC_CHECK_EQ(competing.verdict, Verdict::allow);
}

FAC_TEST(boundary, placement_unit_limit_is_enforced_per_rack) {
  Fixture fixture = make_fixture();
  fixture.evidence.placement->max_units_per_rack = 1;
  Harness harness = make_harness(fixture);
  auto first = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);

  Fixture second = make_fixture(2);
  second.evidence.placement->max_units_per_rack = 1;
  auto refused = FAC_TAKE(harness.engine->admit(second.request, second.evidence));
  FAC_CHECK_EQ(refused.verdict, Verdict::refuse);
  FAC_CHECK_EQ(refused.primary_blocker().value(), BlockerCode::placement_limit_exceeded);
}

FAC_TEST(boundary, placement_unit_limit_is_enforced_per_facility) {
  Fixture fixture = make_fixture();
  fixture.evidence.placement->max_units_per_facility = 2;
  Harness harness = make_harness(fixture);
  FAC_CHECK_EQ(FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence)).verdict, Verdict::allow);

  Fixture second = make_fixture(2);
  second.evidence.placement->max_units_per_facility = 2;
  FAC_CHECK_EQ(FAC_TAKE(harness.engine->admit(second.request, second.evidence)).verdict, Verdict::allow);

  Fixture third = make_fixture(3);
  third.evidence.placement->max_units_per_facility = 2;
  auto refused = FAC_TAKE(harness.engine->admit(third.request, third.evidence));
  FAC_CHECK_EQ(refused.verdict, Verdict::refuse);
  FAC_CHECK_EQ(refused.primary_blocker().value(), BlockerCode::placement_limit_exceeded);
}

