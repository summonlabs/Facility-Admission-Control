// Facility Admission Control - shared test fixtures.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// One place builds a fully conforming admission scenario: every authority
// supplies fresh evidence, the tenant is active, the envelope has room, the
// facility policy permits the commitment and the capacity fits. A test that
// wants to prove a refusal changes exactly one thing and states why.

#ifndef FAC_TESTS_FIXTURES_HPP
#define FAC_TESTS_FIXTURES_HPP

#include <memory>
#include <optional>
#include <string>

#include "fac/engine/engine.hpp"
#include "test_support.hpp"

namespace fac_test {

using namespace fac;

inline constexpr Nanos kNanosPerSecond = 1'000'000'000LL;
inline constexpr Nanos kFixtureNowNanos = 1'700'000'000'000'000'000LL;

[[nodiscard]] inline Timestamp fixture_now() { return Timestamp(kFixtureNowNanos); }
[[nodiscard]] inline Timestamp at_seconds(std::int64_t seconds) {
  return Timestamp(kFixtureNowNanos + seconds * kNanosPerSecond);
}

[[nodiscard]] inline FacilityId facility_a() {
  return FAC_TAKE(FacilityId::from_hex("11111111-1111-1111-1111-111111111111"));
}
[[nodiscard]] inline FacilityId facility_b() {
  return FAC_TAKE(FacilityId::from_hex("22222222-2222-2222-2222-222222222222"));
}
[[nodiscard]] inline RackId rack_1() { return FAC_TAKE(RackId::from_hex("33333333-3333-3333-3333-333333333331")); }
[[nodiscard]] inline RackId rack_2() { return FAC_TAKE(RackId::from_hex("33333333-3333-3333-3333-333333333332")); }
[[nodiscard]] inline ZoneId zone_a() { return FAC_TAKE(ZoneId::from_hex("44444444-4444-4444-4444-444444444441")); }
[[nodiscard]] inline TenantId tenant_alpha() {
  return FAC_TAKE(TenantId::from_hex("55555555-5555-5555-5555-555555555551"));
}
[[nodiscard]] inline TenantId tenant_beta() {
  return FAC_TAKE(TenantId::from_hex("55555555-5555-5555-5555-555555555552"));
}
[[nodiscard]] inline ServiceClassId class_gold() {
  return FAC_TAKE(ServiceClassId::from_hex("66666666-6666-6666-6666-666666666661"));
}
[[nodiscard]] inline EnvelopeId envelope_alpha() {
  return FAC_TAKE(EnvelopeId::from_hex("77777777-7777-7777-7777-777777777771"));
}
[[nodiscard]] inline PolicyId policy_main() {
  return FAC_TAKE(PolicyId::from_hex("88888888-8888-8888-8888-888888888881"));
}
[[nodiscard]] inline OwnerId owner_reservation() {
  return FAC_TAKE(OwnerId::from_hex("99999999-9999-9999-9999-999999999991"));
}
[[nodiscard]] inline IncidentId incident_one() {
  return FAC_TAKE(IncidentId::from_hex("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaa1"));
}
[[nodiscard]] inline MaintenanceWindowId window_one() {
  return FAC_TAKE(MaintenanceWindowId::from_hex("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbb1"));
}
[[nodiscard]] inline RequestId request_id(std::uint64_t index) {
  return RequestId(0x1000000000000000ull + index, 0x2000000000000000ull + index);
}

// A canonical digest that does not depend on any test input.
[[nodiscard]] inline Digest256 fixture_digest(const std::string& text) {
  codec::Writer writer;
  writer.text(text);
  return Digest256::of(writer.span());
}

[[nodiscard]] inline AmountVector amounts(std::uint64_t power, std::uint64_t cooling,
                                          std::uint64_t space, std::uint64_t slots) {
  AmountVector vector;
  FAC_CHECK_OK(vector.set(Dimension::power, power));
  FAC_CHECK_OK(vector.set(Dimension::cooling, cooling));
  FAC_CHECK_OK(vector.set(Dimension::space, space));
  FAC_CHECK_OK(vector.set(Dimension::slots, slots));
  return vector;
}

struct Fixture {
  Timestamp now = fixture_now();
  AdmissionRequest request;
  EvidenceBundle evidence;
  AdmissionPolicy policy;
};

// Builds the conforming scenario: 1000 W / 800 W cooling / 10 RU / 20 slots of
// free capacity and a demand of 200 W / 100 W cooling / 1 RU / 1 slot.
[[nodiscard]] inline Fixture make_fixture(std::uint64_t request_index = 1) {
  Fixture fixture;
  fixture.policy.reservation_owner = owner_reservation();
  fixture.policy.grant_holds_capacity = true;

  AdmissionRequest& request = fixture.request;
  request.request_id = request_id(request_index);
  request.tenant = tenant_alpha();
  request.service_class = class_gold();
  request.envelope = envelope_alpha();
  request.scope = FAC_TAKE(TargetScope::make(facility_a(), rack_1(), zone_a()));
  request.demand = amounts(200, 100, 1, 1);
  request.requested_at = fixture.now;
  request.commitment_start = at_seconds(0);
  request.commitment_end = at_seconds(3600);

  CapacitySnapshot capacity;
  capacity.generation = CapacityGeneration(1);
  capacity.observed_at = fixture.now;
  capacity.total = amounts(1000, 800, 10, 20);
  capacity.committed = amounts(0, 0, 0, 0);
  capacity.reserved = amounts(0, 0, 0, 0);
  fixture.evidence.capacity = capacity;

  RedundancySnapshot redundancy;
  redundancy.generation = RedundancyGeneration(1);
  redundancy.observed_at = fixture.now;
  redundancy.level = RedundancyLevel::n_plus_one;
  redundancy.level_stated = true;
  redundancy.protected_headroom = amounts(100, 0, 0, 0);
  fixture.evidence.redundancy = redundancy;

  TenantSnapshot tenant;
  tenant.tenant = tenant_alpha();
  tenant.generation = TenantGeneration(1);
  tenant.observed_at = fixture.now;
  tenant.status = TenantStatus::active;
  fixture.evidence.tenant = tenant;

  EnvelopeSnapshot envelope;
  envelope.envelope = envelope_alpha();
  envelope.tenant = tenant_alpha();
  envelope.generation = EnvelopeGeneration(1);
  envelope.observed_at = fixture.now;
  envelope.limit = amounts(5000, 4000, 40, 80);
  envelope.consumed = amounts(0, 0, 0, 0);
  fixture.evidence.envelope = envelope;

  ServiceClassSnapshot service_class;
  service_class.service_class = class_gold();
  service_class.generation = ServiceClassGeneration(1);
  service_class.observed_at = fixture.now;
  service_class.obligations.minimum_redundancy = RedundancyLevel::n_plus_one;
  service_class.obligations.minimum_redundancy_stated = true;
  service_class.obligations.required_protected_headroom = amounts(50, 0, 0, 0);
  fixture.evidence.service_class = service_class;

  MaintenanceSnapshot maintenance;
  maintenance.generation = MaintenanceGeneration(1);
  maintenance.observed_at = fixture.now;
  fixture.evidence.maintenance = maintenance;

  IncidentSnapshot incident;
  incident.generation = IncidentGeneration(1);
  incident.observed_at = fixture.now;
  incident.scope = FAC_TAKE(TargetScope::make(facility_a(), std::nullopt, std::nullopt));
  incident.state = IncidentState::normal;
  incident.capacity_trust = CapacityTrust::trusted;
  fixture.evidence.incident = incident;

  PlacementPolicySnapshot placement;
  placement.generation = PlacementGeneration(1);
  placement.observed_at = fixture.now;
  placement.allowed_racks.push_back(rack_1());
  placement.allowed_racks.push_back(rack_2());
  fixture.evidence.placement = placement;

  PolicySnapshot policy;
  policy.policy = policy_main();
  policy.generation = PolicyGeneration(1);
  policy.observed_at = fixture.now;
  policy.verdict = PolicyVerdict::permit;
  policy.overcommit.mode = OvercommitMode::not_permitted;
  policy.policy_digest = fixture_digest("policy-main");
  fixture.evidence.policy = policy;

  return fixture;
}

// An engine whose clock is fixed at the fixture's evaluation time, so every
// decision in a test is reproducible byte for byte. The harness owns the clock,
// which is how a test can advance time between two calls.
struct Harness {
  std::shared_ptr<FixedClock> clock;
  std::unique_ptr<Engine> engine;
};

[[nodiscard]] inline Harness make_harness(const Fixture& fixture) {
  Harness harness;
  harness.clock = std::make_shared<FixedClock>(fixture.now);
  harness.engine = FAC_TAKE(Engine::open_in_memory(fixture.policy, harness.clock));
  return harness;
}

[[nodiscard]] inline Harness make_durable_harness(const Fixture& fixture, const std::string& directory,
                                                  bool create, bool read_only = false) {
  Harness harness;
  harness.clock = std::make_shared<FixedClock>(fixture.now);
  harness.engine = read_only ? FAC_TAKE(Engine::open_reader(directory, fixture.policy, harness.clock))
                             : FAC_TAKE(Engine::open_durable(directory, fixture.policy, harness.clock, create));
  return harness;
}

// Appends a maintenance window covering a scope.
inline void add_maintenance_window(EvidenceBundle& evidence, MaintenanceWindowId id, TargetScope scope,
                                   Timestamp start, Timestamp end, MaintenanceState state,
                                   MaintenanceImpact impact) {
  if (!evidence.maintenance.has_value()) {
    MaintenanceSnapshot snapshot;
    snapshot.generation = MaintenanceGeneration(1);
    snapshot.observed_at = fixture_now();
    evidence.maintenance = snapshot;
  }
  MaintenanceWindow window;
  window.window = id;
  window.scope = scope;
  window.start = start;
  window.end = end;
  window.state = state;
  window.impact = impact;
  evidence.maintenance->windows.push_back(window);
}

// Bumps the generation and observation time of one authority so a test can
// express "the same evidence, one generation later".
inline void touch_capacity(EvidenceBundle& evidence, std::uint64_t generation, Timestamp observed) {
  if (evidence.capacity.has_value()) {
    evidence.capacity->generation = CapacityGeneration(generation);
    evidence.capacity->observed_at = observed;
  }
}

// Overloads of the harness's describe() so a failing comparison names the
// verdict or blocker instead of printing a number.
[[nodiscard]] inline std::string describe(Verdict value) { return to_string(value); }
[[nodiscard]] inline std::string describe(BlockerCode value) { return to_string(value); }
[[nodiscard]] inline std::string describe(EvidenceKind value) { return to_string(value); }
[[nodiscard]] inline std::string describe(CommitmentState value) { return to_string(value); }
[[nodiscard]] inline std::string describe(GrantState value) { return to_string(value); }
[[nodiscard]] inline std::string describe(CommitState value) { return to_string(value); }
[[nodiscard]] inline std::string describe(TenantStatus value) { return to_string(value); }
[[nodiscard]] inline std::string describe(IncidentState value) { return to_string(value); }
[[nodiscard]] inline std::string describe(CapacityTrust value) { return to_string(value); }
[[nodiscard]] inline std::string describe(MaintenanceState value) { return to_string(value); }
[[nodiscard]] inline std::string describe(MaintenanceImpact value) { return to_string(value); }
[[nodiscard]] inline std::string describe(PolicyVerdict value) { return to_string(value); }
[[nodiscard]] inline std::string describe(RedundancyLevel value) { return to_string(value); }
[[nodiscard]] inline std::string describe(ReservationOutcome value) { return to_string(value); }
[[nodiscard]] inline std::string describe(Dimension value) { return to_string(value); }

[[nodiscard]] inline std::optional<BlockerCode> primary_blocker(const Decision& decision) {
  return decision.primary_blocker();
}

// A one-line account of a decision, used in assertions so a failure says what
// the evaluator actually concluded instead of only that it disagreed.
[[nodiscard]] inline std::string decision_summary(const Decision& decision) {
  std::string summary = std::string("verdict ") + to_string(decision.verdict);
  if (const Blocker* primary = decision.blockers.primary()) {
    summary += std::string(", primary ") + primary->render();
  }
  if (!decision.blockers.empty()) {
    summary += ", blockers [";
    for (std::size_t i = 0; i < decision.blockers.items().size(); ++i) {
      if (i != 0) {
        summary += "; ";
      }
      summary += decision.blockers.items()[i].render();
    }
    summary += "]";
  }
  return summary;
}

#define FAC_CHECK_VERDICT(decision, expected)                                              \
  do {                                                                                     \
    if ((decision).verdict != (expected)) {                                                \
      fac_test::fail(__FILE__, __LINE__,                                                   \
                     std::string("expected verdict ") + fac::to_string(expected) +         \
                         " but the decision was " + fac_test::decision_summary(decision)); \
    }                                                                                      \
  } while (false)

[[nodiscard]] inline bool has_blocker(const Decision& decision, BlockerCode code) {
  return decision.blockers.contains(code);
}

}  // namespace fac_test

#endif  // FAC_TESTS_FIXTURES_HPP
