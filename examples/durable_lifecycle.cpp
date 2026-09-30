// Facility Admission Control - example: the durable lifecycle.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A complete lifecycle against a real store directory: admit, commit, record
// the evidence the reservation owner returned, print the commitment state, and
// then reopen the same store read-only and print the state digest of what is
// actually on disk.
//
// The writer is closed before the read-only engine is opened, because the
// store allows exactly one writer incarnation at a time. That is also why the
// whole lifecycle runs before the reopen: a new writer process advances the
// control epoch and deliberately fences grants the previous process issued.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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
  std::cerr << "durable_lifecycle: " << message << "\n";
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

// A directory under the system temporary location that this run owns.
[[nodiscard]] std::string make_store_directory() {
  std::error_code code;
  std::filesystem::path base = std::filesystem::temp_directory_path(code);
  if (code) {
    fail("no temporary directory is available: " + code.message());
  }
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path candidate =
      base / ("fac-example-durable-" + std::to_string(static_cast<long long>(ticks)));
  std::filesystem::create_directories(candidate, code);
  if (code) {
    fail("the store directory could not be created: " + code.message());
  }
  const std::u8string text = candidate.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
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

}  // namespace

int main() {
  const std::string store = make_store_directory();
  const Scenario scenario = make_scenario();
  auto clock = std::make_shared<FixedClock>(Timestamp(kNowNanos));

  CommitmentId commitment_id;
  {
    std::cout << "store " << store << "\n";
    auto engine = unwrap(Engine::open_durable(store, scenario.policy, clock, true),
                         "open the durable store");
    std::cout << "opened-for-writing true\n";
    std::cout << "control-epoch " << engine->epoch().to_string() << "\n";

    auto decision = unwrap(engine->admit(scenario.request, scenario.evidence), "admit");
    std::cout << "verdict " << to_string(decision.verdict) << "\n";
    if (decision.verdict != Verdict::allow || !decision.grant.has_value()) {
      std::cerr << "durable_lifecycle: the conforming scenario was not allowed\n";
      return 1;
    }
    const GrantId grant_id = decision.grant.value().grant_id;
    std::cout << "grant " << grant_id.to_string() << "\n";

    auto committed = unwrap(engine->commit(grant_id), "commit");
    std::cout << "commit-state " << to_string(committed.state) << "\n";
    if (committed.state != CommitState::committed || !committed.commitment_id.has_value()) {
      std::cerr << "durable_lifecycle: the grant was not committed\n";
      return 1;
    }
    commitment_id = committed.commitment_id.value();

    const CommitmentRecord* provisional = engine->ledger().find_commitment(commitment_id);
    if (provisional == nullptr) {
      fail("the committed reservation is missing from the ledger");
    }
    std::cout << "commitment " << provisional->commitment_id.to_string() << "\n";
    std::cout << "commitment-state " << to_string(provisional->state) << "\n";
    std::cout << "intent " << provisional->intent.intent_id.to_string() << "\n";
    std::cout << "intent-owner " << provisional->intent.owner.to_string() << "\n";
    std::cout << "intent-valid-until " << provisional->intent.not_after.to_string() << "\n";

    // The defined evidence the reservation owner returns. This runtime never
    // performs the reservation itself; it records that the owner reported one.
    ReservationEvidence evidence;
    evidence.intent_id = provisional->intent.intent_id;
    evidence.reservation =
        unwrap(ReservationId::from_hex("00000000-0000-0000-0000-0000000000b2"), "reservation");
    evidence.owner = scenario.policy.reservation_owner;
    evidence.outcome = ReservationOutcome::reserved;
    evidence.confirmed = scenario.request.demand;
    evidence.owner_generation = 1;
    evidence.owner_digest = unwrap(
        Digest256::from_hex("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"),
        "owner digest");
    evidence.acknowledged_at = Timestamp(kNowNanos + 1 * kNanosPerSecond);

    auto answered = unwrap(engine->record_evidence(evidence), "record the reservation evidence");
    std::cout << "reservation " << answered.evidence.value().reservation.to_string() << "\n";
    std::cout << "reservation-outcome " << to_string(answered.evidence.value().outcome) << "\n";
    std::cout << "state " << to_string(answered.state) << "\n";
    std::cout << "sequence " << engine->sequence().to_string() << "\n";
    require(engine->verify(), "verify the store from disk");

    if (answered.state != CommitmentState::confirmed) {
      std::cerr << "durable_lifecycle: the acknowledged commitment is not confirmed\n";
      return 1;
    }
  }

  {
    auto reader = unwrap(Engine::open_reader(store, scenario.policy, clock),
                         "reopen the store read-only");
    const CommitmentRecord* committed = reader->ledger().find_commitment(commitment_id);
    if (committed == nullptr) {
      fail("the reopened store does not hold the commitment");
    }
    std::cout << "reopened-read-only true\n";
    std::cout << "recovered-commitment " << committed->commitment_id.to_string() << "\n";
    std::cout << "recovered-state " << to_string(committed->state) << "\n";
    std::cout << "recovered-sequence " << reader->ledger().sequence().to_string() << "\n";
    std::cout << "state-digest " << reader->ledger().state_digest().to_hex() << "\n";
    if (committed->state != CommitmentState::confirmed) {
      std::cerr << "durable_lifecycle: the reopened commitment is not confirmed\n";
      return 1;
    }
  }

  std::error_code code;
  std::filesystem::remove_all(std::filesystem::path(store), code);
  std::cout << "removed-temporary-store true\n";
  return 0;
}
