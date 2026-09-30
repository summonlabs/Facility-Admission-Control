// Facility Admission Control - verdict tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every test here starts from the conforming fixture and changes exactly one
// authoritative input. The expected verdict, the expected primary blocker and
// the expected secondary blockers are all asserted, because a refusal that
// reports the wrong reason is as unusable as no refusal at all.

#include "fixtures.hpp"
#include "test_support.hpp"

using namespace fac;
using namespace fac_test;

FAC_TEST(verdicts, conforming_request_is_allowed) {
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);
  auto decision = harness.engine->admit(fixture.request, fixture.evidence);
  FAC_CHECK_OK(decision);
  FAC_CHECK_EQ(decision.value().verdict, Verdict::allow);
  FAC_CHECK(decision.value().grant.has_value());
  FAC_CHECK(decision.value().blockers.empty());
  FAC_CHECK(!decision.value().explanation.empty());

  // Every authority the evaluator used is named in the grant, so a later change
  // to any of them is detectable at commit time.
  const Grant& grant = decision.value().grant.value();
  FAC_CHECK_EQ(grant.bindings.size(), static_cast<std::size_t>(9));
  for (const EvidenceKind kind : {EvidenceKind::capacity, EvidenceKind::redundancy,
                                  EvidenceKind::tenant, EvidenceKind::envelope,
                                  EvidenceKind::service_class, EvidenceKind::maintenance,
                                  EvidenceKind::incident, EvidenceKind::placement,
                                  EvidenceKind::policy}) {
    FAC_CHECK(grant.find_binding(kind) != nullptr);
  }
  FAC_CHECK(!grant.binding_digest.is_unset());
  FAC_CHECK_EQ(grant.compute_binding_digest(), grant.binding_digest);

  // The recorded decision reproduces its own digest.
  FAC_CHECK_EQ(decision.value().compute_digest(), decision.value().digest);
}

FAC_TEST(verdicts, suspended_tenant_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.tenant->status = TenantStatus::suspended;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::tenant_not_active);
  FAC_CHECK(!decision.grant.has_value());
}

FAC_TEST(verdicts, closed_tenant_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.tenant->status = TenantStatus::closed;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::tenant_not_active);
}

FAC_TEST(verdicts, unknown_tenant_status_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.tenant->status = TenantStatus::unknown;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::tenant_unknown);
}

FAC_TEST(verdicts, policy_denial_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.policy->verdict = PolicyVerdict::deny;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::policy_denied);
}

FAC_TEST(verdicts, policy_abstention_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.policy->verdict = PolicyVerdict::abstain;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::policy_unknown);
}

FAC_TEST(verdicts, envelope_limit_is_a_hard_refusal) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.envelope->limit.set(Dimension::power, 100));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::envelope_limit_exceeded);
  FAC_CHECK(has_blocker(decision, BlockerCode::capacity_exhausted) == false);
}

FAC_TEST(verdicts, exhausted_envelope_is_a_hard_refusal) {
  Fixture fixture = make_fixture();
  FAC_CHECK_OK(fixture.evidence.envelope->consumed.set(Dimension::cooling, 3950));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::envelope_exhausted);
}

FAC_TEST(verdicts, unmeasured_envelope_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.envelope->limit = AmountVector::unknown();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::envelope_unknown);
}

FAC_TEST(verdicts, forbidden_rack_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.placement->allowed_racks.clear();
  fixture.evidence.placement->forbidden_racks.push_back(rack_1());
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::placement_forbidden);
}

FAC_TEST(verdicts, rack_outside_the_allow_list_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.placement->allowed_racks.clear();
  fixture.evidence.placement->allowed_racks.push_back(rack_2());
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::placement_forbidden);
}

FAC_TEST(verdicts, rack_constraint_without_a_rack_target_defers) {
  Fixture fixture = make_fixture();
  fixture.request.scope = FAC_TAKE(TargetScope::make(facility_a(), std::nullopt, std::nullopt));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::placement_unknown);
}

FAC_TEST(verdicts, missing_required_zone_is_refused) {
  Fixture fixture = make_fixture();
  fixture.request.scope = FAC_TAKE(TargetScope::make(facility_a(), rack_1(), std::nullopt));
  fixture.evidence.placement->required_zone = zone_a();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::placement_forbidden);
}

FAC_TEST(verdicts, major_incident_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.incident->state = IncidentState::major;
  fixture.evidence.incident->incident = incident_one();
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::incident_major);
}

FAC_TEST(verdicts, degraded_incident_with_unreliable_capacity_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.incident->state = IncidentState::degraded;
  fixture.evidence.incident->incident = incident_one();
  fixture.evidence.incident->capacity_trust = CapacityTrust::unreliable;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::incident_degraded);
}

FAC_TEST(verdicts, degradation_never_becomes_normal) {
  Fixture fixture = make_fixture();
  fixture.evidence.incident->state = IncidentState::degraded;
  fixture.evidence.incident->capacity_trust = CapacityTrust::trusted;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  // An explicitly trusted capacity observation in a degraded scope is usable,
  // but the incident is still recorded in the evidence set.
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK(decision.evidence.contains(EvidenceKind::incident));
}

FAC_TEST(verdicts, unobserved_incident_state_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.incident->state = IncidentState::unknown;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::incident_state_unknown);
}

FAC_TEST(verdicts, active_full_outage_maintenance_is_refused) {
  Fixture fixture = make_fixture();
  add_maintenance_window(fixture.evidence, window_one(),
                         FAC_TAKE(TargetScope::make(facility_a(), rack_1(), std::nullopt)),
                         at_seconds(-600), at_seconds(600), MaintenanceState::active,
                         MaintenanceImpact::full_outage);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::maintenance_outage);
}

FAC_TEST(verdicts, active_capacity_reduction_defers) {
  Fixture fixture = make_fixture();
  add_maintenance_window(fixture.evidence, window_one(),
                         FAC_TAKE(TargetScope::make(facility_a(), std::nullopt, std::nullopt)),
                         at_seconds(-600), at_seconds(600), MaintenanceState::active,
                         MaintenanceImpact::capacity_reduction);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::maintenance_active);
}

FAC_TEST(verdicts, planned_maintenance_overlapping_the_window_defers) {
  Fixture fixture = make_fixture();
  add_maintenance_window(fixture.evidence, window_one(),
                         FAC_TAKE(TargetScope::make(facility_a(), rack_1(), std::nullopt)),
                         at_seconds(60), at_seconds(120), MaintenanceState::planned,
                         MaintenanceImpact::capacity_reduction);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::maintenance_exposure);
}

FAC_TEST(verdicts, maintenance_in_another_facility_does_not_apply) {
  Fixture fixture = make_fixture();
  add_maintenance_window(fixture.evidence, window_one(),
                         FAC_TAKE(TargetScope::make(facility_b(), std::nullopt, std::nullopt)),
                         at_seconds(-600), at_seconds(600), MaintenanceState::active,
                         MaintenanceImpact::full_outage);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
}

FAC_TEST(verdicts, maintenance_on_another_rack_does_not_apply) {
  Fixture fixture = make_fixture();
  add_maintenance_window(fixture.evidence, window_one(),
                         FAC_TAKE(TargetScope::make(facility_a(), rack_2(), std::nullopt)),
                         at_seconds(-600), at_seconds(600), MaintenanceState::active,
                         MaintenanceImpact::full_outage);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
}

FAC_TEST(verdicts, unknown_maintenance_impact_defers) {
  Fixture fixture = make_fixture();
  add_maintenance_window(fixture.evidence, window_one(),
                         FAC_TAKE(TargetScope::make(facility_a(), rack_1(), std::nullopt)),
                         at_seconds(-60), at_seconds(60), MaintenanceState::active,
                         MaintenanceImpact::unknown);
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::maintenance_unknown);
}

FAC_TEST(verdicts, insufficient_redundancy_level_is_refused) {
  Fixture fixture = make_fixture();
  fixture.evidence.redundancy->level = RedundancyLevel::none;
  fixture.evidence.service_class->obligations.minimum_redundancy = RedundancyLevel::n_plus_two;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::redundancy_level_insufficient);
}

FAC_TEST(verdicts, unmeasured_redundancy_level_defers) {
  Fixture fixture = make_fixture();
  fixture.evidence.redundancy->level = RedundancyLevel::none;
  fixture.evidence.redundancy->level_stated = false;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::defer);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::redundancy_unknown);
}

FAC_TEST(verdicts, hard_refusal_outranks_a_deferral_and_keeps_both) {
  Fixture fixture = make_fixture();
  // A deferral condition (unobserved incident state) and a hard refusal (the
  // tenant is closed) at the same time.
  fixture.evidence.incident->state = IncidentState::unknown;
  fixture.evidence.tenant->status = TenantStatus::closed;
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::tenant_not_active);
  FAC_CHECK(has_blocker(decision, BlockerCode::incident_state_unknown));
  FAC_CHECK_EQ(decision.blockers.items().size(), static_cast<std::size_t>(2));
}

FAC_TEST(verdicts, blocker_order_is_precedence_order_not_discovery_order) {
  Fixture fixture = make_fixture();
  // Discovered late (capacity) but more decisive than the deferral discovered
  // first (policy abstention).
  fixture.evidence.policy->verdict = PolicyVerdict::abstain;
  FAC_CHECK_OK(fixture.evidence.capacity->total.set(Dimension::power, 100));
  Harness harness = make_harness(fixture);
  auto decision = FAC_TAKE(harness.engine->admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(decision.verdict, Verdict::refuse);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::capacity_exhausted);
  FAC_CHECK(has_blocker(decision, BlockerCode::policy_unknown));
  // The reported order is the precedence order.
  const auto& items = decision.blockers.items();
  for (std::size_t i = 1; i < items.size(); ++i) {
    FAC_CHECK(blocker_precedence(items[i - 1].code) < blocker_precedence(items[i].code));
  }
}

FAC_TEST(verdicts, equal_inputs_produce_byte_identical_decisions) {
  Fixture first = make_fixture(7);
  Fixture second = make_fixture(7);
  Harness harness_a = make_harness(first);
  Harness harness_b = make_harness(second);
  auto left = FAC_TAKE(harness_a.engine->admit(first.request, first.evidence));
  auto right = FAC_TAKE(harness_b.engine->admit(second.request, second.evidence));
  FAC_CHECK_EQ(left.digest, right.digest);
  FAC_CHECK_EQ(left, right);
  FAC_CHECK_EQ(left.grant->grant_id, right.grant->grant_id);
}

