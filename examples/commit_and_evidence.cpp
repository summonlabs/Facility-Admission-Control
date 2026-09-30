// Facility Admission Control - example: commit and owner evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two commitments against one engine:
//
//   * the first is acknowledged with the defined evidence for a full
//     reservation, and moves from provisional to confirmed;
//   * the second is acknowledged with a partial reservation, and stays partial.
//
// Both keep the whole demand counted. An owner that confirms less than was
// asked for has not returned capacity, and this runtime will not invent the
// difference. The commit itself performs no reservation: it emits a bounded
// intent addressed to the authority that owns reservation.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "fac/engine/engine.hpp"

namespace {

using namespace fac;

constexpr Nanos kNowNanos = 1'700'000'000'000'000'000LL;
constexpr Nanos kNanosPerSecond = 1'000'000'000LL;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "commit_and_evidence: " << message << "\n";
  std::exit(2);
}

template <class T>
[[nodiscard]] T unwrap(Result<T> result, const char* what) {
  if (!result.ok()) {
    fail(std::string(what) + ": " + result.error().to_string());
  }
  return result.take();
}

void require(Status status, const char* what) {
  if (!status.ok()) {
    fail(std::string(what) + ": " + status.error().to_string());
  }
}

[[nodiscard]] AmountVector amounts(std::uint64_t power, std::uint64_t cooling, std::uint64_t space,
                                   std::uint64_t slots) {
  AmountVector vector;
  require(vector.set(Dimension::power, power), "power amount");
  require(vector.set(Dimension::cooling, cooling), "cooling amount");
  require(vector.set(Dimension::space, space), "space amount");
  require(vector.set(Dimension::slots, slots), "slots amount");
  return vector;
}

struct Scenario {
  AdmissionRequest request;
  EvidenceBundle evidence;
  AdmissionPolicy policy;
};

[[nodiscard]] Scenario make_scenario() {
  const Timestamp now(kNowNanos);
  Scenario scenario;

  scenario.policy.reservation_owner =
      unwrap(OwnerId::from_hex("99999999-9999-9999-9999-999999999991"), "owner identity");
  scenario.policy.grant_holds_capacity = true;

  AdmissionRequest& request = scenario.request;
  request.request_id =
      unwrap(RequestId::from_hex("00000000-0000-0000-0000-0000000000a1"), "request identity");
  request.tenant =
      unwrap(TenantId::from_hex("55555555-5555-5555-5555-555555555551"), "tenant identity");
  request.service_class =
      unwrap(ServiceClassId::from_hex("66666666-6666-6666-6666-666666666661"), "service class");
  request.envelope =
      unwrap(EnvelopeId::from_hex("77777777-7777-7777-7777-777777777771"), "envelope identity");
  request.scope = unwrap(
      TargetScope::make(unwrap(FacilityId::from_hex("11111111-1111-1111-1111-111111111111"), "facility"),
                        unwrap(RackId::from_hex("33333333-3333-3333-3333-333333333331"), "rack"),
                        unwrap(ZoneId::from_hex("44444444-4444-4444-4444-444444444441"), "zone")),
      "scope");
  request.demand = amounts(200, 100, 1, 1);
  request.requested_at = now;
  request.commitment_start = now;
  request.commitment_end = Timestamp(kNowNanos + 3600 * kNanosPerSecond);

  CapacitySnapshot capacity;
  capacity.generation = CapacityGeneration(1);
  capacity.observed_at = now;
  capacity.total = amounts(1000, 800, 10, 20);
  capacity.committed = amounts(0, 0, 0, 0);
  capacity.reserved = amounts(0, 0, 0, 0);
  scenario.evidence.capacity = capacity;

  RedundancySnapshot redundancy;
  redundancy.generation = RedundancyGeneration(1);
  redundancy.observed_at = now;
  redundancy.level = RedundancyLevel::n_plus_one;
  redundancy.level_stated = true;
  redundancy.protected_headroom = amounts(100, 0, 0, 0);
  scenario.evidence.redundancy = redundancy;

  TenantSnapshot tenant;
  tenant.tenant = request.tenant;
  tenant.generation = TenantGeneration(1);
  tenant.observed_at = now;
  tenant.status = TenantStatus::active;
  scenario.evidence.tenant = tenant;

  EnvelopeSnapshot envelope;
  envelope.envelope = request.envelope;
  envelope.tenant = request.tenant;
  envelope.generation = EnvelopeGeneration(1);
  envelope.observed_at = now;
  envelope.limit = amounts(5000, 4000, 40, 80);
  envelope.consumed = amounts(0, 0, 0, 0);
  scenario.evidence.envelope = envelope;

  ServiceClassSnapshot service_class;
  service_class.service_class = request.service_class;
  service_class.generation = ServiceClassGeneration(1);
  service_class.observed_at = now;
  service_class.obligations.minimum_redundancy = RedundancyLevel::n_plus_one;
  service_class.obligations.minimum_redundancy_stated = true;
  service_class.obligations.required_protected_headroom = amounts(50, 0, 0, 0);
  scenario.evidence.service_class = service_class;

  MaintenanceSnapshot maintenance;
  maintenance.generation = MaintenanceGeneration(1);
  maintenance.observed_at = now;
  scenario.evidence.maintenance = maintenance;

  IncidentSnapshot incident;
  incident.generation = IncidentGeneration(1);
  incident.observed_at = now;
  incident.scope = unwrap(
      TargetScope::make(request.scope.facility, std::nullopt, std::nullopt), "incident scope");
  incident.state = IncidentState::normal;
  incident.capacity_trust = CapacityTrust::trusted;
  scenario.evidence.incident = incident;

  PlacementPolicySnapshot placement;
  placement.generation = PlacementGeneration(1);
  placement.observed_at = now;
  placement.allowed_racks.push_back(request.scope.rack.value());
  scenario.evidence.placement = placement;

  PolicySnapshot policy;
  policy.policy = unwrap(PolicyId::from_hex("88888888-8888-8888-8888-888888888881"), "policy");
  policy.generation = PolicyGeneration(1);
  policy.observed_at = now;
  policy.verdict = PolicyVerdict::permit;
  policy.overcommit.mode = OvercommitMode::not_permitted;
  policy.policy_digest = unwrap(
      Digest256::from_hex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
      "policy digest");
  scenario.evidence.policy = policy;

  return scenario;
}

// What this ledger counts against the facility, across everything it holds.
[[nodiscard]] std::uint64_t counted_power(const Engine& engine, const TargetScope& scope) {
  ConsumptionQuery query;
  query.facility = scope.facility;
  query.rack = scope.rack;
  query.observed_after = Timestamp{};
  return engine.ledger().consumed(Dimension::power, query);
}

}  // namespace

int main() {
  Scenario scenario = make_scenario();
  auto clock = std::make_shared<FixedClock>(Timestamp(kNowNanos));
  auto engine = unwrap(Engine::open_in_memory(scenario.policy, clock), "open in-memory engine");
  const TargetScope scope = scenario.request.scope;

  // ---- The first commitment: a full reservation. ----
  auto first = unwrap(engine->admit(scenario.request, scenario.evidence), "admit (first)");
  if (first.verdict != Verdict::allow || !first.grant.has_value()) {
    std::cerr << "commit_and_evidence: the first request was not allowed\n";
    return 1;
  }
  std::cout << "first-verdict " << to_string(first.verdict) << "\n";
  std::cout << "first-grant " << first.grant.value().grant_id.to_string() << "\n";

  auto first_commit = unwrap(engine->commit(first.grant.value().grant_id), "commit (first)");
  std::cout << "first-commit-state " << to_string(first_commit.state) << "\n";
  const CommitmentRecord* first_record =
      engine->ledger().find_commitment(first_commit.commitment_id.value());
  if (first_record == nullptr) {
    fail("the first commitment is missing from the ledger");
  }

  // The intent is the bounded request this runtime emitted to the authority
  // that owns reservation. Nothing here performed the reservation.
  const ReservationIntent& intent = first_record->intent;
  std::cout << "first-intent " << intent.intent_id.to_string() << "\n";
  std::cout << "first-intent-owner " << intent.owner.to_string() << "\n";
  std::cout << "first-intent-epoch " << intent.control_epoch.to_string() << "\n";
  std::cout << "first-intent-sequence " << intent.sequence.to_string() << "\n";
  std::cout << "first-intent-window " << intent.not_before.to_string() << " .. "
            << intent.not_after.to_string() << "\n";
  std::cout << "first-intent-window-seconds "
            << (intent.not_after.unix_nanos() - intent.not_before.unix_nanos()) / kNanosPerSecond
            << "\n";
  std::cout << "first-intent-demand power=" << intent.demand.get(Dimension::power).value_or(0)
            << " cooling=" << intent.demand.get(Dimension::cooling).value_or(0)
            << " space=" << intent.demand.get(Dimension::space).value_or(0)
            << " slots=" << intent.demand.get(Dimension::slots).value_or(0) << "\n";
  std::cout << "first-intent-binding-digest " << intent.binding_digest.to_hex() << "\n";
  std::cout << "first-state " << to_string(first_record->state) << "\n";
  std::cout << "first-counted-power " << counted_power(*engine, scope) << "\n";

  ReservationEvidence full;
  full.intent_id = intent.intent_id;
  full.reservation =
      unwrap(ReservationId::from_hex("00000000-0000-0000-0000-0000000000b2"), "reservation");
  full.owner = scenario.policy.reservation_owner;
  full.outcome = ReservationOutcome::reserved;
  full.confirmed = scenario.request.demand;
  full.owner_generation = 1;
  full.owner_digest = unwrap(
      Digest256::from_hex("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"),
      "owner digest");
  full.acknowledged_at = Timestamp(kNowNanos + 1 * kNanosPerSecond);

  auto confirmed = unwrap(engine->record_evidence(full), "record full reservation evidence");
  std::cout << "first-acknowledged-state " << to_string(confirmed.state) << "\n";
  std::cout << "first-counted-power-after-ack " << counted_power(*engine, scope) << "\n";

  // ---- The second commitment: a partial reservation. ----
  scenario.request.request_id =
      unwrap(RequestId::from_hex("00000000-0000-0000-0000-0000000000a2"), "request identity");
  auto second = unwrap(engine->admit(scenario.request, scenario.evidence), "admit (second)");
  if (second.verdict != Verdict::allow || !second.grant.has_value()) {
    std::cerr << "commit_and_evidence: the second request was not allowed\n";
    return 1;
  }
  auto second_commit = unwrap(engine->commit(second.grant.value().grant_id), "commit (second)");
  const CommitmentRecord* second_record =
      engine->ledger().find_commitment(second_commit.commitment_id.value());
  if (second_record == nullptr) {
    fail("the second commitment is missing from the ledger");
  }
  std::cout << "second-commit-state " << to_string(second_commit.state) << "\n";
  std::cout << "second-state " << to_string(second_record->state) << "\n";
  std::cout << "second-counted-power " << counted_power(*engine, scope) << "\n";

  ReservationEvidence partial;
  partial.intent_id = second_record->intent.intent_id;
  partial.reservation =
      unwrap(ReservationId::from_hex("00000000-0000-0000-0000-0000000000b3"), "reservation");
  partial.owner = scenario.policy.reservation_owner;
  partial.outcome = ReservationOutcome::partially_reserved;
  partial.confirmed = amounts(100, 0, 0, 0);  // half the power, and nothing else
  partial.owner_generation = 1;
  partial.owner_digest = unwrap(
      Digest256::from_hex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
      "owner digest");
  partial.acknowledged_at = Timestamp(kNowNanos + 2 * kNanosPerSecond);

  auto answered_partial = unwrap(engine->record_evidence(partial), "record partial evidence");
  std::cout << "second-acknowledged-state " << to_string(answered_partial.state) << "\n";
  std::cout << "second-confirmed-power "
            << answered_partial.evidence.value().confirmed.get(Dimension::power).value_or(0) << "\n";
  std::cout << "second-counted-power-after-partial-ack " << counted_power(*engine, scope) << "\n";
  std::cout << "consuming-commitments " << engine->ledger().consuming_commitments() << "\n";

  const bool confirmed_as_expected = confirmed.state == CommitmentState::confirmed;
  const bool partial_as_expected = answered_partial.state == CommitmentState::partial &&
                                   answered_partial.consumes() &&
                                   counted_power(*engine, scope) == 400;
  if (!confirmed_as_expected || !partial_as_expected) {
    std::cerr << "commit_and_evidence: the commitments did not reach the expected states\n";
    return 1;
  }
  std::cout << "a partial acknowledgement keeps the whole demand counted\n";
  return 0;
}
