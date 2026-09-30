// Facility Admission Control - downstream consumer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A standalone program that uses only the installed package: it links the
// exported target, includes the installed headers, evaluates a real admission
// request, commits the grant it receives and records the reservation evidence
// that comes back. Linking is not enough; this has to produce the same
// behaviour the library promises.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <fac/fac.hpp>

namespace {

using namespace fac;

[[nodiscard]] int report(const std::string& message) {
  std::cerr << "consumer: " << message << "\n";
  return 1;
}

[[nodiscard]] AmountVector amounts(std::uint64_t power, std::uint64_t cooling, std::uint64_t space,
                                   std::uint64_t slots) {
  AmountVector vector;
  (void)vector.set(Dimension::power, power);
  (void)vector.set(Dimension::cooling, cooling);
  (void)vector.set(Dimension::space, space);
  (void)vector.set(Dimension::slots, slots);
  return vector;
}

// A consumer is not a test: a failed conversion is reported and the program
// exits non-zero rather than throwing.
template <class T>
[[nodiscard]] T consumer_take(Result<T> result, const char* what) {
  if (!result.ok()) {
    std::cerr << "consumer: " << what << " failed: " << result.error().to_string() << "\n";
    std::exit(1);
  }
  return result.take();
}

#define FAC_CONSUMER_TAKE(expression) consumer_take((expression), #expression)

}  // namespace

int main() {
  if (std::string(FAC_CONSUMER_EXPECTED_VERSION) != version_string()) {
    return report("installed library version does not match the installed headers");
  }

  const Timestamp now(1'700'000'000'000'000'000LL);
  const FacilityId facility = FAC_CONSUMER_TAKE(FacilityId::from_hex("11111111-1111-1111-1111-111111111111"));
  const TenantId tenant = FAC_CONSUMER_TAKE(TenantId::from_hex("55555555-5555-5555-5555-555555555551"));
  const ServiceClassId service_class =
      FAC_CONSUMER_TAKE(ServiceClassId::from_hex("66666666-6666-6666-6666-666666666661"));
  const EnvelopeId envelope = FAC_CONSUMER_TAKE(EnvelopeId::from_hex("77777777-7777-7777-7777-777777777771"));
  const OwnerId owner = FAC_CONSUMER_TAKE(OwnerId::from_hex("99999999-9999-9999-9999-999999999991"));

  AdmissionPolicy policy;
  policy.reservation_owner = owner;
  auto clock = std::make_shared<FixedClock>(now);
  auto engine = Engine::open_in_memory(policy, clock);
  if (!engine.ok()) {
    return report("engine could not be opened: " + engine.error().to_string());
  }

  AdmissionRequest request;
  request.request_id = FAC_CONSUMER_TAKE(RequestId::from_hex("10000000-0000-0000-0000-000000000001"));
  request.tenant = tenant;
  request.service_class = service_class;
  request.envelope = envelope;
  request.scope = FAC_CONSUMER_TAKE(TargetScope::make(facility, std::nullopt, std::nullopt));
  request.demand = amounts(200, 100, 1, 1);
  request.requested_at = now;
  request.commitment_start = now;
  request.commitment_end = Timestamp(now.unix_nanos() + 3600LL * 1000000000LL);

  EvidenceBundle evidence;
  CapacitySnapshot capacity;
  capacity.generation = CapacityGeneration(1);
  capacity.observed_at = now;
  capacity.total = amounts(1000, 800, 10, 20);
  capacity.committed = amounts(0, 0, 0, 0);
  capacity.reserved = amounts(0, 0, 0, 0);
  evidence.capacity = capacity;

  TenantSnapshot tenant_snapshot;
  tenant_snapshot.tenant = tenant;
  tenant_snapshot.generation = TenantGeneration(1);
  tenant_snapshot.observed_at = now;
  tenant_snapshot.status = TenantStatus::active;
  evidence.tenant = tenant_snapshot;

  EnvelopeSnapshot envelope_snapshot;
  envelope_snapshot.envelope = envelope;
  envelope_snapshot.tenant = tenant;
  envelope_snapshot.generation = EnvelopeGeneration(1);
  envelope_snapshot.observed_at = now;
  envelope_snapshot.limit = amounts(5000, 4000, 40, 80);
  envelope_snapshot.consumed = amounts(0, 0, 0, 0);
  evidence.envelope = envelope_snapshot;

  ServiceClassSnapshot service_class_snapshot;
  service_class_snapshot.service_class = service_class;
  service_class_snapshot.generation = ServiceClassGeneration(1);
  service_class_snapshot.observed_at = now;
  evidence.service_class = service_class_snapshot;

  // The default policy requires every authority. A consumer that omits one gets
  // a deferral, which is the point: missing evidence is never assumed away.
  RedundancySnapshot redundancy;
  redundancy.generation = RedundancyGeneration(1);
  redundancy.observed_at = now;
  redundancy.level = RedundancyLevel::n_plus_one;
  redundancy.level_stated = true;
  redundancy.protected_headroom = amounts(0, 0, 0, 0);
  evidence.redundancy = redundancy;

  MaintenanceSnapshot maintenance;
  maintenance.generation = MaintenanceGeneration(1);
  maintenance.observed_at = now;
  evidence.maintenance = maintenance;

  IncidentSnapshot incident;
  incident.generation = IncidentGeneration(1);
  incident.observed_at = now;
  incident.scope = FAC_CONSUMER_TAKE(TargetScope::make(facility, std::nullopt, std::nullopt));
  incident.state = IncidentState::normal;
  incident.capacity_trust = CapacityTrust::trusted;
  evidence.incident = incident;

  PlacementPolicySnapshot placement;
  placement.generation = PlacementGeneration(1);
  placement.observed_at = now;
  evidence.placement = placement;

  PolicySnapshot policy_snapshot;
  policy_snapshot.policy = FAC_CONSUMER_TAKE(PolicyId::from_hex("88888888-8888-8888-8888-888888888881"));
  policy_snapshot.generation = PolicyGeneration(1);
  policy_snapshot.observed_at = now;
  policy_snapshot.verdict = PolicyVerdict::permit;
  codec::Writer seed;
  seed.text("consumer-policy");
  policy_snapshot.policy_digest = Digest256::of(seed.span());
  evidence.policy = policy_snapshot;

  auto decision = engine.value()->admit(request, evidence);
  if (!decision.ok()) {
    return report("admission failed: " + decision.error().to_string());
  }
  std::cout << "verdict " << to_string(decision.value().verdict) << "\n";
  std::cout << "decision-digest " << decision.value().digest.to_hex() << "\n";
  if (decision.value().verdict != Verdict::allow || !decision.value().grant.has_value()) {
    return report("the consumer expected an allowing decision");
  }

  auto outcome = engine.value()->commit(decision.value().grant->grant_id);
  if (!outcome.ok()) {
    return report("commit failed: " + outcome.error().to_string());
  }
  if (outcome.value().state != CommitState::committed || !outcome.value().commitment_id.has_value()) {
    return report("the consumer expected a committed grant");
  }
  std::cout << "commit-state " << to_string(outcome.value().state) << "\n";
  std::cout << "commitment " << outcome.value().commitment_id.value().to_string() << "\n";

  const CommitmentRecord* commitment =
      engine.value()->ledger().find_commitment(outcome.value().commitment_id.value());
  if (commitment == nullptr) {
    return report("the committed record is missing from the ledger");
  }
  std::cout << "intent " << commitment->intent.intent_id.to_string() << "\n";
  std::cout << "state-digest " << engine.value()->ledger().state_digest().to_hex() << "\n";
  std::cout << "consumer-ok\n";
  return 0;
}