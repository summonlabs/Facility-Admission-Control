// Facility Admission Control - example: one conforming admission.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The smallest complete use of the public API: build the authoritative
// snapshots an evaluation is allowed to use, evaluate one request against
// them, and print the verdict together with the binding of the grant it
// issued. The engine is volatile and owns a fixed clock, so this program
// prints the same decision every time it runs.

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
  std::cerr << "admit_basic: " << message << "\n";
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

// The conforming scenario: every authority supplies fresh evidence, the tenant
// is active, the envelope has room, the facility policy permits the commitment
// and the capacity fits the demand.
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

void print_decision(const Decision& decision) {
  std::cout << "request " << decision.request_id.to_string() << "\n";
  std::cout << "verdict " << to_string(decision.verdict) << "\n";
  std::cout << "control-epoch " << decision.control_epoch.to_string() << "\n";
  std::cout << "sequence " << decision.sequence.to_string() << "\n";
  std::cout << "decided-at " << decision.decided_at.to_string() << "\n";
  for (const auto& reference : decision.evidence.refs()) {
    std::cout << "evidence " << to_string(reference.kind) << " generation " << reference.generation
              << (reference.accepted ? " accepted" : " rejected") << "\n";
  }
  std::cout << "decision-digest " << decision.digest.to_hex() << "\n";
}

}  // namespace

int main() {
  const Scenario scenario = make_scenario();
  auto clock = std::make_shared<FixedClock>(Timestamp(kNowNanos));
  auto engine = unwrap(Engine::open_in_memory(scenario.policy, clock), "open in-memory engine");

  auto decision = unwrap(engine->admit(scenario.request, scenario.evidence), "admit");
  print_decision(decision);

  if (decision.verdict != Verdict::allow || !decision.grant.has_value()) {
    std::cerr << "admit_basic: the conforming scenario was not allowed\n";
    return 1;
  }

  const Grant& grant = decision.grant.value();
  std::cout << "grant " << grant.grant_id.to_string() << "\n";
  std::cout << "grant-issued-at " << grant.issued_at.to_string() << "\n";
  std::cout << "grant-expires-at " << grant.expires_at.to_string() << "\n";
  std::cout << "grant-holds-capacity " << (grant.holds_capacity ? "true" : "false") << "\n";
  std::cout << "grant-binding-digest " << grant.binding_digest.to_hex() << "\n";
  for (const GrantBinding& binding : grant.bindings) {
    std::cout << "grant-binding " << to_string(binding.kind) << " generation " << binding.generation
              << " digest " << binding.evidence_digest.to_hex() << "\n";
  }
  std::cout << "admitted: the commitment may be executed against this grant\n";
  return 0;
}
