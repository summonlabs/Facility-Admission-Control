// Facility Admission Control - model types tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every value that can be persisted or digested is encoded, decoded and
// compared here, and every rule the model states about itself is exercised as a
// refusal with a named error code. The doctrine these tests exist for is that
// missing, unknown or unmeasured data never becomes zero: the "explicit
// unknown" cases below are as important as the round trips.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/limits.hpp"
#include "fac/model/decision.hpp"
#include "fac/model/reservation.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

using namespace fac;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char raw : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(raw)));
  }
  return out;
}

// Encodes a value, decodes it with the type's own decoder, requires that the
// decoder consumed exactly the canonical encoding and that the value survived.
template <class T>
void check_round_trip(const T& value) {
  codec::Writer writer;
  value.encode(writer);
  codec::Reader reader(writer.span());
  auto decoded = T::decode(reader);
  if (!decoded.ok()) {
    FAC_FAIL(std::string("decode failed: ") + decoded.error().to_string());
  }
  FAC_CHECK_OK(reader.expect_end());
  FAC_CHECK_EQ(decoded.value(), value);
}

EvidenceRef make_evidence(EvidenceKind kind, std::uint64_t generation, std::string_view seed,
                          Timestamp observed) {
  EvidenceRef reference;
  reference.kind = kind;
  reference.generation = generation;
  reference.digest = Digest256::of(bytes_of(seed));
  reference.observed_at = observed;
  reference.accepted = true;
  reference.rejection = ErrorCode::ok;
  return reference;
}

TargetScope make_scope() {
  return FAC_TAKE(TargetScope::make(fac_test::facility_a(), fac_test::rack_1(), fac_test::zone_a()));
}

MaintenanceWindow make_window(MaintenanceWindowId id, MaintenanceState state,
                              MaintenanceImpact impact) {
  MaintenanceWindow window;
  window.window = id;
  window.scope = FAC_TAKE(TargetScope::make(fac_test::facility_a(), std::nullopt, std::nullopt));
  window.start = fac_test::at_seconds(0);
  window.end = fac_test::at_seconds(3600);
  window.state = state;
  window.impact = impact;
  return window;
}

Grant make_grant() {
  Grant grant;
  grant.grant_id = FAC_TAKE(GrantId::from_hex("a1a1a1a1-b2b2-c3c3-d4d4-e5e5e5e5e5e5"));
  grant.revision = FAC_TAKE(GrantRevision::from_value(2));
  grant.request_id = fac_test::request_id(4);
  grant.request_digest = Digest256::of(bytes_of("request-content"));
  GrantBinding capacity;
  capacity.kind = EvidenceKind::capacity;
  capacity.generation = 7;
  capacity.evidence_digest = Digest256::of(bytes_of("capacity-evidence"));
  grant.bindings.push_back(capacity);
  GrantBinding policy;
  policy.kind = EvidenceKind::policy;
  policy.generation = 8;
  policy.evidence_digest = Digest256::of(bytes_of("policy-evidence"));
  grant.bindings.push_back(policy);
  grant.control_epoch = ControlEpoch(5);
  grant.sequence = FAC_TAKE(LedgerSequence::from_value(11));
  grant.issued_at = fac_test::fixture_now();
  grant.expires_at = fac_test::at_seconds(600);
  grant.scope = make_scope();
  grant.tenant = fac_test::tenant_alpha();
  grant.demand = fac_test::amounts(200, 100, 1, 1);
  grant.holds_capacity = true;
  grant.binding_digest = grant.compute_binding_digest();
  return grant;
}

ReservationIntent make_intent(CommitmentId commitment_id) {
  ReservationIntent intent;
  intent.intent_id = FAC_TAKE(IntentId::from_hex("12341234-5678-9abc-def0-1234567890ab"));
  intent.request_id = fac_test::request_id(4);
  intent.grant_id = FAC_TAKE(GrantId::from_hex("a1a1a1a1-b2b2-c3c3-d4d4-e5e5e5e5e5e5"));
  intent.commitment_id = commitment_id;
  intent.owner = fac_test::owner_reservation();
  intent.scope = make_scope();
  intent.tenant = fac_test::tenant_alpha();
  intent.service_class = fac_test::class_gold();
  intent.demand = fac_test::amounts(200, 100, 1, 1);
  intent.not_before = fac_test::at_seconds(0);
  intent.not_after = fac_test::at_seconds(600);
  intent.control_epoch = ControlEpoch(5);
  intent.sequence = FAC_TAKE(LedgerSequence::from_value(12));
  intent.binding_digest = Digest256::of(bytes_of("intent-binding"));
  FAC_CHECK_OK(intent.validate());
  return intent;
}

ReservationEvidence make_reservation_evidence(IntentId intent_id, ReservationOutcome outcome,
                                              AmountVector confirmed) {
  ReservationEvidence evidence;
  evidence.intent_id = intent_id;
  evidence.reservation = FAC_TAKE(ReservationId::from_hex("0f0f0f0f-1e1e-2d2d-3c3c-4b4b4b4b4b4b"));
  evidence.owner = fac_test::owner_reservation();
  evidence.outcome = outcome;
  evidence.confirmed = confirmed;
  evidence.owner_generation = 3;
  evidence.owner_digest = Digest256::of(bytes_of("owner-acknowledgement"));
  evidence.acknowledged_at = fac_test::at_seconds(30);
  FAC_CHECK_OK(evidence.validate());
  return evidence;
}

Decision make_decision(Verdict verdict, bool with_grant) {
  Decision decision;
  decision.request_id = fac_test::request_id(4);
  decision.request_digest = Digest256::of(bytes_of("request-content"));
  decision.verdict = verdict;
  decision.control_epoch = ControlEpoch(5);
  decision.sequence = FAC_TAKE(LedgerSequence::from_value(11));
  decision.decided_at = fac_test::fixture_now();

  FAC_CHECK_OK(decision.evidence.add(
      make_evidence(EvidenceKind::capacity, 7, "capacity-evidence", fac_test::fixture_now())));
  FAC_CHECK_OK(decision.evidence.add(
      make_evidence(EvidenceKind::policy, 8, "policy-evidence", fac_test::fixture_now())));

  DimensionAssessment power;
  power.dimension = Dimension::power;
  power.total = 1000;
  power.committed = 0;
  power.reserved = 0;
  power.protected_headroom = 100;
  power.demand = 200;
  power.available = 900;
  power.remaining = 700;
  power.overcommit = 0;
  FAC_CHECK_OK(decision.assessment.add(power));

  DimensionAssessment cooling;
  cooling.dimension = Dimension::cooling;
  cooling.total = 800;
  cooling.committed = 0;
  cooling.reserved = 0;
  cooling.demand = 100;
  cooling.available = 800;
  cooling.remaining = 700;
  FAC_CHECK_OK(decision.assessment.add(cooling));

  DimensionAssessment space;
  space.dimension = Dimension::space;
  space.total = 10;
  space.demand = 1;
  space.remaining = 9;
  FAC_CHECK_OK(decision.assessment.add(space));

  DimensionAssessment slots;
  slots.dimension = Dimension::slots;
  slots.total = 20;
  slots.demand = 1;
  slots.remaining = 19;
  FAC_CHECK_OK(decision.assessment.add(slots));

  decision.explanation.push_back("capacity evidence is current at generation 7");
  decision.explanation.push_back("the commitment fits under the stated policy");

  if (with_grant) {
    decision.grant = make_grant();
  }
  decision.digest = decision.compute_digest();
  return decision;
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

FAC_TEST(model, amount_vector_round_trip) {
  check_round_trip(AmountVector::unknown());
  check_round_trip(fac_test::amounts(0, 0, 0, 0));
  check_round_trip(fac_test::amounts(1000, 800, 10, 20));

  AmountVector partial;
  FAC_CHECK_OK(partial.set(Dimension::power, 200));
  FAC_CHECK_OK(partial.set(Dimension::space, 0));
  check_round_trip(partial);
}

FAC_TEST(model, target_scope_round_trip) {
  check_round_trip(FAC_TAKE(TargetScope::make(fac_test::facility_a(), std::nullopt, std::nullopt)));
  check_round_trip(FAC_TAKE(TargetScope::make(fac_test::facility_a(), fac_test::rack_1(), std::nullopt)));
  check_round_trip(
      FAC_TAKE(TargetScope::make(fac_test::facility_b(), fac_test::rack_2(), fac_test::zone_a())));
}

FAC_TEST(model, target_scope_narrowing_is_conservative) {
  const TargetScope facility = FAC_TAKE(TargetScope::make(fac_test::facility_a(), std::nullopt, std::nullopt));
  const TargetScope rack_one = FAC_TAKE(TargetScope::make(fac_test::facility_a(), fac_test::rack_1(), std::nullopt));
  const TargetScope rack_two = FAC_TAKE(TargetScope::make(fac_test::facility_a(), fac_test::rack_2(), std::nullopt));
  const TargetScope other_facility =
      FAC_TAKE(TargetScope::make(fac_test::facility_b(), fac_test::rack_1(), std::nullopt));

  // A facility-wide constraint affects a rack-narrowed request, because
  // proving it does not would need evidence this runtime does not own.
  FAC_CHECK(facility.affects(rack_one));
  FAC_CHECK(facility.affects(facility));
  FAC_CHECK(rack_one.affects(rack_one));
  FAC_CHECK(rack_one.affects(facility));
  FAC_CHECK(!rack_one.affects(rack_two));
  FAC_CHECK(!rack_one.affects(other_facility));
  FAC_CHECK(!facility.affects(other_facility));

  FAC_CHECK(facility.contains(rack_one));
  FAC_CHECK(rack_one.contains(rack_one));
  FAC_CHECK(!rack_one.contains(facility));
  FAC_CHECK(!facility.contains(other_facility));
}

FAC_TEST(model, evidence_ref_and_set_round_trip) {
  const EvidenceRef accepted =
      make_evidence(EvidenceKind::capacity, 9, "capacity-evidence", fac_test::fixture_now());
  check_round_trip(accepted);

  EvidenceRef rejected =
      make_evidence(EvidenceKind::policy, 3, "policy-evidence", fac_test::fixture_now());
  rejected.accepted = false;
  rejected.rejection = ErrorCode::digest_mismatch;
  check_round_trip(rejected);

  // An evidence set must survive its own canonical encoding. It does not:
  // EvidenceSet::decode bounds the declared count with
  //   sequence_count(kEvidenceKindCount, 60)
  // while one EvidenceRef encodes to exactly 52 bytes (1 + 8 + 32 + 8 + 1 + 2),
  // so the guard reports truncated_input for any non-empty set that is not
  // followed by unrelated bytes. Left failing deliberately.
  EvidenceSet set;
  FAC_CHECK_OK(set.add(accepted));
  FAC_CHECK_OK(set.add(rejected));
  check_round_trip(set);

  // The canonical digest is stable and changes when the set changes.
  FAC_CHECK_EQ(set.digest(), set.digest());
  EvidenceSet other;
  FAC_CHECK_OK(other.add(accepted));
  FAC_CHECK_NE(other.digest(), set.digest());
}

FAC_TEST(model, capacity_snapshot_round_trip) {
  CapacitySnapshot snapshot;
  snapshot.generation = CapacityGeneration(3);
  snapshot.observed_at = fac_test::fixture_now();
  snapshot.total = fac_test::amounts(1000, 800, 10, 20);
  snapshot.committed = fac_test::amounts(200, 100, 1, 1);
  snapshot.reserved = fac_test::amounts(0, 0, 0, 0);
  FAC_CHECK_OK(snapshot.validate());
  check_round_trip(snapshot);

  // Unmeasured dimensions survive the trip as unmeasured.
  CapacitySnapshot partial;
  partial.generation = CapacityGeneration(1);
  partial.observed_at = fac_test::fixture_now();
  FAC_CHECK_OK(partial.total.set(Dimension::power, 1000));
  FAC_CHECK_OK(partial.committed.set(Dimension::power, 10));
  partial.reserved = AmountVector::unknown();
  FAC_CHECK_OK(partial.validate());
  check_round_trip(partial);
}

FAC_TEST(model, redundancy_snapshot_round_trip) {
  RedundancySnapshot stated;
  stated.generation = RedundancyGeneration(2);
  stated.observed_at = fac_test::fixture_now();
  stated.level = RedundancyLevel::n_plus_two;
  stated.level_stated = true;
  stated.protected_headroom = fac_test::amounts(100, 0, 0, 0);
  FAC_CHECK_OK(stated.validate());
  check_round_trip(stated);

  // An unmeasured level is not "none", and it survives as unstated.
  RedundancySnapshot unstated;
  unstated.generation = RedundancyGeneration(1);
  unstated.observed_at = fac_test::fixture_now();
  unstated.level = RedundancyLevel::none;
  unstated.level_stated = false;
  FAC_CHECK_OK(unstated.validate());
  check_round_trip(unstated);
}

FAC_TEST(model, tenant_snapshot_round_trip) {
  for (const TenantStatus status : {TenantStatus::active, TenantStatus::suspended, TenantStatus::closed,
                                    TenantStatus::unknown}) {
    TenantSnapshot snapshot;
    snapshot.tenant = fac_test::tenant_alpha();
    snapshot.generation = TenantGeneration(4);
    snapshot.observed_at = fac_test::fixture_now();
    snapshot.status = status;
    FAC_CHECK_OK(snapshot.validate());
    check_round_trip(snapshot);
  }
}

FAC_TEST(model, envelope_snapshot_round_trip) {
  EnvelopeSnapshot envelope;
  envelope.envelope = fac_test::envelope_alpha();
  envelope.tenant = fac_test::tenant_alpha();
  envelope.generation = EnvelopeGeneration(6);
  envelope.observed_at = fac_test::fixture_now();
  envelope.limit = fac_test::amounts(5000, 4000, 40, 80);
  envelope.consumed = fac_test::amounts(500, 0, 1, 2);
  FAC_CHECK_OK(envelope.validate());
  check_round_trip(envelope);

  EnvelopeSnapshot partial;
  partial.envelope = fac_test::envelope_alpha();
  partial.tenant = fac_test::tenant_beta();
  partial.generation = EnvelopeGeneration(1);
  partial.observed_at = fac_test::fixture_now();
  FAC_CHECK_OK(partial.limit.set(Dimension::power, 100));
  partial.consumed = AmountVector::unknown();
  FAC_CHECK_OK(partial.validate());
  check_round_trip(partial);
}

FAC_TEST(model, service_class_snapshot_round_trip) {
  ServiceClassSnapshot snapshot;
  snapshot.service_class = fac_test::class_gold();
  snapshot.generation = ServiceClassGeneration(2);
  snapshot.observed_at = fac_test::fixture_now();
  snapshot.obligations.minimum_redundancy = RedundancyLevel::n_plus_one;
  snapshot.obligations.minimum_redundancy_stated = true;
  snapshot.obligations.required_protected_headroom = fac_test::amounts(50, 0, 0, 0);
  snapshot.obligations.requires_maintenance_clearance = true;
  snapshot.obligations.permits_overcommit = true;
  FAC_CHECK_OK(snapshot.validate());
  check_round_trip(snapshot);

  ServiceClassSnapshot bare;
  bare.service_class = fac_test::class_gold();
  bare.generation = ServiceClassGeneration(1);
  bare.observed_at = fac_test::fixture_now();
  bare.obligations.minimum_redundancy = RedundancyLevel::none;
  bare.obligations.minimum_redundancy_stated = false;
  FAC_CHECK_OK(bare.validate());
  check_round_trip(bare);
}

FAC_TEST(model, maintenance_snapshot_round_trip) {
  MaintenanceSnapshot snapshot;
  snapshot.generation = MaintenanceGeneration(3);
  snapshot.observed_at = fac_test::fixture_now();
  snapshot.windows.push_back(
      make_window(fac_test::window_one(), MaintenanceState::planned, MaintenanceImpact::capacity_reduction));
  snapshot.windows.push_back(make_window(
      FAC_TAKE(MaintenanceWindowId::from_hex("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbb2")),
      MaintenanceState::active, MaintenanceImpact::full_outage));
  FAC_CHECK(snapshot.windows[0].overlaps(fac_test::at_seconds(1), fac_test::at_seconds(2)));
  FAC_CHECK(!snapshot.windows[0].overlaps(fac_test::at_seconds(3600), fac_test::at_seconds(7200)));
  FAC_CHECK_OK(snapshot.validate());
  check_round_trip(snapshot);

  // The same defect as the evidence set: MaintenanceSnapshot::decode bounds the
  // declared count with sequence_count(kMaxMaintenanceWindows, 100) while one
  // MaintenanceWindow encodes to exactly 84 bytes (16 + 50 + 8 + 8 + 1 + 1), so
  // a snapshot carrying any window is refused as truncated. Left failing
  // deliberately.
  MaintenanceSnapshot empty;
  empty.generation = MaintenanceGeneration(1);
  empty.observed_at = fac_test::fixture_now();
  FAC_CHECK_OK(empty.validate());
  check_round_trip(empty);
}

FAC_TEST(model, incident_snapshot_round_trip) {
  IncidentSnapshot normal;
  normal.generation = IncidentGeneration(1);
  normal.observed_at = fac_test::fixture_now();
  normal.scope = FAC_TAKE(TargetScope::make(fac_test::facility_a(), std::nullopt, std::nullopt));
  normal.state = IncidentState::normal;
  normal.capacity_trust = CapacityTrust::trusted;
  FAC_CHECK_OK(normal.validate());
  check_round_trip(normal);

  IncidentSnapshot degraded;
  degraded.generation = IncidentGeneration(2);
  degraded.observed_at = fac_test::fixture_now();
  degraded.incident = fac_test::incident_one();
  degraded.scope = make_scope();
  degraded.state = IncidentState::degraded;
  degraded.capacity_trust = CapacityTrust::unreliable;
  FAC_CHECK_OK(degraded.validate());
  check_round_trip(degraded);
}

FAC_TEST(model, placement_policy_snapshot_round_trip) {
  PlacementPolicySnapshot placement;
  placement.generation = PlacementGeneration(1);
  placement.observed_at = fac_test::fixture_now();
  placement.allowed_racks.push_back(fac_test::rack_1());
  placement.forbidden_racks.push_back(fac_test::rack_2());
  placement.required_zone = fac_test::zone_a();
  placement.max_units_per_rack = 8;
  placement.max_units_per_facility = 40;
  FAC_CHECK_OK(placement.validate());
  check_round_trip(placement);

  PlacementPolicySnapshot bare;
  bare.generation = PlacementGeneration(1);
  bare.observed_at = fac_test::fixture_now();
  FAC_CHECK_OK(bare.validate());
  check_round_trip(bare);
}

FAC_TEST(model, policy_snapshot_round_trip) {
  PolicySnapshot permit;
  permit.policy = fac_test::policy_main();
  permit.generation = PolicyGeneration(9);
  permit.observed_at = fac_test::fixture_now();
  permit.verdict = PolicyVerdict::permit;
  permit.overcommit.mode = OvercommitMode::permitted_up_to;
  permit.overcommit.allowance = fac_test::amounts(100, 50, 1, 2);
  permit.policy_digest = Digest256::of(bytes_of("policy-content"));
  FAC_CHECK_OK(permit.validate());
  check_round_trip(permit);

  PolicySnapshot abstain;
  abstain.policy = fac_test::policy_main();
  abstain.generation = PolicyGeneration(1);
  abstain.observed_at = fac_test::fixture_now();
  abstain.verdict = PolicyVerdict::abstain;
  abstain.overcommit.mode = OvercommitMode::not_permitted;
  abstain.policy_digest = Digest256::of(bytes_of("policy-content-2"));
  FAC_CHECK_OK(abstain.validate());
  check_round_trip(abstain);
}

FAC_TEST(model, admission_request_round_trip) {
  AdmissionRequest request;
  request.request_id = fac_test::request_id(7);
  request.tenant = fac_test::tenant_alpha();
  request.service_class = fac_test::class_gold();
  request.envelope = fac_test::envelope_alpha();
  request.scope = make_scope();
  request.demand = fac_test::amounts(200, 100, 1, 1);
  request.requested_at = fac_test::fixture_now();
  request.commitment_start = fac_test::at_seconds(0);
  request.commitment_end = fac_test::at_seconds(3600);
  request.expected_epoch = ControlEpoch(4);

  GenerationPin capacity_pin;
  capacity_pin.kind = EvidenceKind::capacity;
  capacity_pin.generation = 11;
  request.pins.push_back(capacity_pin);

  GenerationPin tenant_pin;
  tenant_pin.kind = EvidenceKind::tenant;
  tenant_pin.generation = 12;
  request.pins.push_back(tenant_pin);

  FAC_CHECK_OK(request.validate());
  check_round_trip(request);

  // The content digest is the identity of the request and is stable.
  FAC_CHECK_EQ(request.digest(), request.digest());
  AdmissionRequest changed = request;
  changed.demand = fac_test::amounts(201, 100, 1, 1);
  FAC_CHECK_NE(changed.digest(), request.digest());
  FAC_CHECK_EQ(request.pinned_generation(EvidenceKind::capacity).value(), std::uint64_t(11));
  FAC_CHECK(!request.pinned_generation(EvidenceKind::envelope).has_value());
  FAC_CHECK_EQ(request.commitment_duration(), Duration::from_seconds(3600));

  // A request with no epoch assertion and no pins still round trips.
  AdmissionRequest unasserted = request;
  unasserted.request_id = fac_test::request_id(8);
  unasserted.expected_epoch.reset();
  unasserted.pins.clear();
  FAC_CHECK_OK(unasserted.validate());
  check_round_trip(unasserted);
}

FAC_TEST(model, decision_round_trip) {
  // A refusal: hard blockers, no grant.
  Decision refusal = make_decision(Verdict::refuse, false);
  FAC_CHECK_OK(refusal.blockers.add(BlockerCode::capacity_exhausted, "the rack has no free power",
                                    Dimension::power));
  refusal.digest = refusal.compute_digest();
  FAC_CHECK_OK(refusal.blockers.add(BlockerCode::tenant_not_active, "the tenant is suspended"));
  refusal.digest = refusal.compute_digest();
  check_round_trip(refusal);

  // A deferral: the runtime cannot conclude.
  Decision deferral = make_decision(Verdict::defer, false);
  FAC_CHECK_OK(deferral.blockers.add(BlockerCode::capacity_missing, "no capacity evidence was supplied"));
  deferral.digest = deferral.compute_digest();
  check_round_trip(deferral);

  // An allowance: exactly one grant, and no blockers.
  const Decision allowance = make_decision(Verdict::allow, true);
  FAC_CHECK(allowance.blockers.empty());
  check_round_trip(allowance);
}

FAC_TEST(model, decision_with_grant_round_trip) {
  Decision decision = make_decision(Verdict::allow, true);
  FAC_CHECK(decision.grant.has_value());
  FAC_CHECK_EQ(decision.grant.value(), make_grant());
  check_round_trip(decision);

  const Grant& grant = decision.grant.value();
  FAC_CHECK_EQ(grant.find_binding(EvidenceKind::capacity)->generation, std::uint64_t(7));
  FAC_CHECK_EQ(grant.find_binding(EvidenceKind::policy)->generation, std::uint64_t(8));
  FAC_CHECK(grant.find_binding(EvidenceKind::tenant) == nullptr);
  // The binding digest covers the request, the bindings, the epoch, the
  // sequence, the window, the scope, the tenant and the demand.
  FAC_CHECK_EQ(grant.binding_digest, grant.compute_binding_digest());
  Grant altered = grant;
  altered.control_epoch = ControlEpoch(6);
  FAC_CHECK_NE(altered.compute_binding_digest(), grant.binding_digest);
  Grant altered_binding = grant;
  altered_binding.bindings.front().generation = 99;
  FAC_CHECK_NE(altered_binding.compute_binding_digest(), grant.binding_digest);
}

FAC_TEST(model, reservation_intent_round_trip) {
  const CommitmentId commitment =
      FAC_TAKE(CommitmentId::from_hex("0a0b0c0d-1e1f-2a2b-3c3d-4e4f5a5b5c5d"));
  const ReservationIntent intent = make_intent(commitment);
  check_round_trip(intent);
}

FAC_TEST(model, reservation_evidence_round_trip) {
  const IntentId intent_id = FAC_TAKE(IntentId::from_hex("12341234-5678-9abc-def0-1234567890ab"));

  const ReservationEvidence full = make_reservation_evidence(intent_id, ReservationOutcome::reserved,
                                                             fac_test::amounts(200, 100, 1, 1));
  check_round_trip(full);

  AmountVector partial;
  FAC_CHECK_OK(partial.set(Dimension::power, 100));
  check_round_trip(make_reservation_evidence(intent_id, ReservationOutcome::partially_reserved, partial));

  check_round_trip(make_reservation_evidence(intent_id, ReservationOutcome::rejected,
                                             AmountVector::unknown()));
}

FAC_TEST(model, commitment_record_round_trip) {
  const CommitmentId commitment =
      FAC_TAKE(CommitmentId::from_hex("0a0b0c0d-1e1f-2a2b-3c3d-4e4f5a5b5c5d"));
  const ReservationIntent intent = make_intent(commitment);

  CommitmentRecord record;
  record.commitment_id = commitment;
  record.request_id = intent.request_id;
  record.grant_id = intent.grant_id;
  record.scope = intent.scope;
  record.tenant = intent.tenant;
  record.service_class = intent.service_class;
  record.demand = intent.demand;
  record.intent = intent;
  record.state = CommitmentState::provisional;
  record.recorded_at = fac_test::fixture_now();
  record.expires_at = fac_test::at_seconds(600);
  record.resolution_detail = "reservation intent emitted";
  FAC_CHECK(record.consumes());
  check_round_trip(record);

  // A confirmed commitment keeps the owner's evidence and still consumes.
  CommitmentRecord confirmed = record;
  confirmed.state = CommitmentState::confirmed;
  confirmed.resolved_at = fac_test::at_seconds(90);
  confirmed.evidence = make_reservation_evidence(intent.intent_id, ReservationOutcome::reserved,
                                                 fac_test::amounts(200, 100, 1, 1));
  FAC_CHECK(confirmed.consumes());
  check_round_trip(confirmed);

  // A released commitment no longer consumes and carries the resolution time.
  CommitmentRecord released = record;
  released.state = CommitmentState::released;
  released.resolved_at = fac_test::at_seconds(120);
  released.evidence = make_reservation_evidence(intent.intent_id, ReservationOutcome::rejected,
                                                AmountVector::unknown());
  released.resolution_detail = "the reservation owner rejected the intent";
  FAC_CHECK(!released.consumes());
  check_round_trip(released);

  // An expired commitment does not consume and carries no owner evidence.
  CommitmentRecord expired = record;
  expired.state = CommitmentState::expired;
  expired.resolved_at = fac_test::at_seconds(600);
  expired.resolution_detail = "reservation intent window passed without owner evidence";
  FAC_CHECK(!expired.consumes());
  check_round_trip(expired);
}

// ---------------------------------------------------------------------------
// Validation refusals
// ---------------------------------------------------------------------------

FAC_TEST(model, zero_generation_and_absent_observation_time_are_refused) {
  CapacitySnapshot capacity;
  capacity.observed_at = fac_test::fixture_now();
  capacity.total = fac_test::amounts(1, 1, 1, 1);
  FAC_CHECK_ERR(capacity.validate(), ErrorCode::invalid_argument);
  capacity.generation = CapacityGeneration(1);
  FAC_CHECK_OK(capacity.validate());
  capacity.observed_at = Timestamp(0);
  FAC_CHECK_ERR(capacity.validate(), ErrorCode::invalid_argument);

  RedundancySnapshot redundancy;
  redundancy.observed_at = fac_test::fixture_now();
  FAC_CHECK_ERR(redundancy.validate(), ErrorCode::invalid_argument);
  redundancy.generation = RedundancyGeneration(1);
  redundancy.observed_at = Timestamp(0);
  FAC_CHECK_ERR(redundancy.validate(), ErrorCode::invalid_argument);

  TenantSnapshot tenant;
  tenant.tenant = fac_test::tenant_alpha();
  tenant.observed_at = fac_test::fixture_now();
  FAC_CHECK_ERR(tenant.validate(), ErrorCode::invalid_argument);
  tenant.generation = TenantGeneration(1);
  tenant.observed_at = Timestamp(0);
  FAC_CHECK_ERR(tenant.validate(), ErrorCode::invalid_argument);

  EnvelopeSnapshot envelope;
  envelope.envelope = fac_test::envelope_alpha();
  envelope.tenant = fac_test::tenant_alpha();
  envelope.observed_at = fac_test::fixture_now();
  FAC_CHECK_ERR(envelope.validate(), ErrorCode::invalid_argument);
  envelope.generation = EnvelopeGeneration(1);
  envelope.observed_at = Timestamp(0);
  FAC_CHECK_ERR(envelope.validate(), ErrorCode::invalid_argument);

  ServiceClassSnapshot service_class;
  service_class.service_class = fac_test::class_gold();
  service_class.observed_at = fac_test::fixture_now();
  FAC_CHECK_ERR(service_class.validate(), ErrorCode::invalid_argument);
  service_class.generation = ServiceClassGeneration(1);
  service_class.observed_at = Timestamp(0);
  FAC_CHECK_ERR(service_class.validate(), ErrorCode::invalid_argument);

  MaintenanceSnapshot maintenance;
  maintenance.observed_at = fac_test::fixture_now();
  FAC_CHECK_ERR(maintenance.validate(), ErrorCode::invalid_argument);
  maintenance.generation = MaintenanceGeneration(1);
  maintenance.observed_at = Timestamp(0);
  FAC_CHECK_ERR(maintenance.validate(), ErrorCode::invalid_argument);

  IncidentSnapshot incident;
  incident.observed_at = fac_test::fixture_now();
  FAC_CHECK_ERR(incident.validate(), ErrorCode::invalid_argument);
  incident.generation = IncidentGeneration(1);
  incident.observed_at = Timestamp(0);
  FAC_CHECK_ERR(incident.validate(), ErrorCode::invalid_argument);

  PlacementPolicySnapshot placement;
  placement.observed_at = fac_test::fixture_now();
  FAC_CHECK_ERR(placement.validate(), ErrorCode::invalid_argument);
  placement.generation = PlacementGeneration(1);
  placement.observed_at = Timestamp(0);
  FAC_CHECK_ERR(placement.validate(), ErrorCode::invalid_argument);

  PolicySnapshot policy;
  policy.policy = fac_test::policy_main();
  policy.observed_at = fac_test::fixture_now();
  policy.policy_digest = Digest256::of(bytes_of("policy-content"));
  FAC_CHECK_ERR(policy.validate(), ErrorCode::invalid_argument);
  policy.generation = PolicyGeneration(1);
  policy.observed_at = Timestamp(0);
  FAC_CHECK_ERR(policy.validate(), ErrorCode::invalid_argument);
  policy.observed_at = fac_test::fixture_now();
  policy.policy_digest = Digest256{};
  FAC_CHECK_ERR(policy.validate(), ErrorCode::invalid_argument);
}

FAC_TEST(model, validation_refusals_at_the_persistence_boundary) {
  // An unstated redundancy level that carries a level can be constructed in
  // memory, so the decoder is what must refuse it.
  {
    RedundancySnapshot snapshot;
    snapshot.generation = RedundancyGeneration(1);
    snapshot.observed_at = fac_test::fixture_now();
    snapshot.level_stated = false;
    snapshot.level = RedundancyLevel::n_plus_one;
    codec::Writer writer;
    snapshot.encode(writer);
    codec::Reader reader(writer.span());
    FAC_CHECK_ERR(RedundancySnapshot::decode(reader), ErrorCode::invalid_argument);
  }

  // The same for an unstated service class obligation.
  {
    ServiceClassSnapshot snapshot;
    snapshot.service_class = fac_test::class_gold();
    snapshot.generation = ServiceClassGeneration(1);
    snapshot.observed_at = fac_test::fixture_now();
    snapshot.obligations.minimum_redundancy_stated = false;
    snapshot.obligations.minimum_redundancy = RedundancyLevel::two_n;
    codec::Writer writer;
    snapshot.encode(writer);
    codec::Reader reader(writer.span());
    FAC_CHECK_ERR(ServiceClassSnapshot::decode(reader), ErrorCode::invalid_argument);
  }

  // A normal incident state that names an incident.
  {
    IncidentSnapshot snapshot;
    snapshot.generation = IncidentGeneration(1);
    snapshot.observed_at = fac_test::fixture_now();
    snapshot.incident = fac_test::incident_one();
    snapshot.scope = FAC_TAKE(TargetScope::make(fac_test::facility_a(), std::nullopt, std::nullopt));
    snapshot.state = IncidentState::normal;
    snapshot.capacity_trust = CapacityTrust::trusted;
    FAC_CHECK_ERR(snapshot.validate(), ErrorCode::invalid_argument);
    codec::Writer writer;
    snapshot.encode(writer);
    codec::Reader reader(writer.span());
    FAC_CHECK_ERR(IncidentSnapshot::decode(reader), ErrorCode::invalid_argument);

    // The same snapshot without an incident is accepted.
    snapshot.incident.reset();
    FAC_CHECK_OK(snapshot.validate());
  }

  // A zero incident identity inside a present field is not an incident either.
  {
    IncidentSnapshot snapshot;
    snapshot.generation = IncidentGeneration(1);
    snapshot.observed_at = fac_test::fixture_now();
    snapshot.incident = IncidentId{};
    snapshot.scope = FAC_TAKE(TargetScope::make(fac_test::facility_a(), std::nullopt, std::nullopt));
    snapshot.state = IncidentState::degraded;
    FAC_CHECK_ERR(snapshot.validate(), ErrorCode::invalid_argument);
  }

  // Duplicate maintenance window identities.
  {
    MaintenanceSnapshot snapshot;
    snapshot.generation = MaintenanceGeneration(1);
    snapshot.observed_at = fac_test::fixture_now();
    snapshot.windows.push_back(make_window(fac_test::window_one(), MaintenanceState::planned,
                                           MaintenanceImpact::capacity_reduction));
    snapshot.windows.push_back(make_window(fac_test::window_one(), MaintenanceState::active,
                                           MaintenanceImpact::full_outage));
    FAC_CHECK_ERR(snapshot.validate(), ErrorCode::duplicate_field);
    codec::Writer writer;
    snapshot.encode(writer);
    codec::Reader reader(writer.span());
    FAC_CHECK_ERR(MaintenanceSnapshot::decode(reader), ErrorCode::duplicate_field);
  }

  // A placement policy that both allows and forbids one rack.
  {
    PlacementPolicySnapshot placement;
    placement.generation = PlacementGeneration(1);
    placement.observed_at = fac_test::fixture_now();
    placement.allowed_racks.push_back(fac_test::rack_1());
    placement.forbidden_racks.push_back(fac_test::rack_1());
    FAC_CHECK_ERR(placement.validate(), ErrorCode::conflict);
    codec::Writer writer;
    placement.encode(writer);
    codec::Reader reader(writer.span());
    FAC_CHECK_ERR(PlacementPolicySnapshot::decode(reader), ErrorCode::conflict);
  }

  // A zero rack in either placement list.
  {
    PlacementPolicySnapshot placement;
    placement.generation = PlacementGeneration(1);
    placement.observed_at = fac_test::fixture_now();
    placement.forbidden_racks.push_back(RackId{});
    FAC_CHECK_ERR(placement.validate(), ErrorCode::invalid_argument);
  }

  // A stated zero unit limit is not a limit of zero.
  {
    PlacementPolicySnapshot placement;
    placement.generation = PlacementGeneration(1);
    placement.observed_at = fac_test::fixture_now();
    placement.max_units_per_rack = 0;
    FAC_CHECK_ERR(placement.validate(), ErrorCode::invalid_argument);
    placement.max_units_per_rack.reset();
    placement.max_units_per_facility = 0;
    FAC_CHECK_ERR(placement.validate(), ErrorCode::invalid_argument);
  }
}

FAC_TEST(model, overcommit_allowance_must_match_its_mode) {
  // An allowance present when the mode does not use it.
  OvercommitAllowance not_permitted;
  not_permitted.mode = OvercommitMode::not_permitted;
  FAC_CHECK_OK(not_permitted.validate());
  FAC_CHECK_OK(not_permitted.allowance.set(Dimension::power, 1));
  FAC_CHECK_ERR(not_permitted.validate(), ErrorCode::invalid_argument);

  OvercommitAllowance unbounded;
  unbounded.mode = OvercommitMode::permitted_unbounded;
  FAC_CHECK_OK(unbounded.validate());
  FAC_CHECK_OK(unbounded.allowance.set(Dimension::slots, 0));
  FAC_CHECK_ERR(unbounded.validate(), ErrorCode::invalid_argument);

  // A bounded allowance missing a dimension is not a bounded allowance.
  OvercommitAllowance bounded;
  bounded.mode = OvercommitMode::permitted_up_to;
  FAC_CHECK_ERR(bounded.validate(), ErrorCode::invalid_argument);
  FAC_CHECK_OK(bounded.allowance.set(Dimension::power, 100));
  FAC_CHECK_ERR(bounded.validate(), ErrorCode::invalid_argument);
  FAC_CHECK_OK(bounded.allowance.set(Dimension::cooling, 0));
  FAC_CHECK_OK(bounded.allowance.set(Dimension::space, 1));
  FAC_CHECK_OK(bounded.allowance.set(Dimension::slots, 2));
  FAC_CHECK_OK(bounded.validate());

  // The mode is what a policy that did not permit the commitment may not carry.
  PolicySnapshot policy;
  policy.policy = fac_test::policy_main();
  policy.generation = PolicyGeneration(1);
  policy.observed_at = fac_test::fixture_now();
  policy.policy_digest = Digest256::of(bytes_of("policy-content"));
  policy.verdict = PolicyVerdict::permit;
  policy.overcommit.mode = OvercommitMode::permitted_unbounded;
  FAC_CHECK_OK(policy.validate());
  policy.verdict = PolicyVerdict::deny;
  FAC_CHECK_ERR(policy.validate(), ErrorCode::invalid_argument);
  policy.verdict = PolicyVerdict::abstain;
  FAC_CHECK_ERR(policy.validate(), ErrorCode::invalid_argument);
  policy.verdict = PolicyVerdict::permit;
  policy.overcommit.mode = OvercommitMode::permitted_up_to;
  FAC_CHECK_ERR(policy.validate(), ErrorCode::invalid_argument);
}

FAC_TEST(model, request_validation_refusals) {
  AdmissionRequest request;
  FAC_CHECK_ERR(request.validate(), ErrorCode::invalid_argument);

  request.request_id = fac_test::request_id(1);
  FAC_CHECK_ERR(request.validate(), ErrorCode::invalid_argument);  // no tenant
  request.tenant = fac_test::tenant_alpha();
  request.service_class = fac_test::class_gold();
  request.envelope = fac_test::envelope_alpha();
  request.scope = make_scope();
  request.requested_at = fac_test::fixture_now();
  request.commitment_start = fac_test::at_seconds(0);
  request.commitment_end = fac_test::at_seconds(3600);
  FAC_CHECK_OK(request.validate());

  // A zero identity is refused even when the field is present.
  AdmissionRequest zero_id = request;
  zero_id.request_id = RequestId{};
  FAC_CHECK_ERR(zero_id.validate(), ErrorCode::invalid_argument);

  // A commitment window must be a window.
  AdmissionRequest inverted = request;
  inverted.commitment_end = inverted.commitment_start;
  FAC_CHECK_ERR(inverted.validate(), ErrorCode::invalid_argument);

  // A pinned zero generation asserts nothing and is refused.
  AdmissionRequest zero_pin = request;
  GenerationPin pin;
  pin.kind = EvidenceKind::capacity;
  pin.generation = 0;
  zero_pin.pins.push_back(pin);
  FAC_CHECK_ERR(zero_pin.validate(), ErrorCode::invalid_argument);

  // Pins must be in ascending kind order and unique per kind.
  AdmissionRequest unordered = request;
  GenerationPin late;
  late.kind = EvidenceKind::policy;
  late.generation = 1;
  unordered.pins.push_back(late);
  GenerationPin early;
  early.kind = EvidenceKind::capacity;
  early.generation = 1;
  unordered.pins.push_back(early);
  FAC_CHECK_ERR(unordered.validate(), ErrorCode::invalid_argument);

  AdmissionRequest repeated = request;
  repeated.pins.push_back(late);
  repeated.pins.push_back(late);
  FAC_CHECK_ERR(repeated.validate(), ErrorCode::invalid_argument);

  // A zero expected epoch asserts nothing.
  AdmissionRequest zero_epoch = request;
  zero_epoch.expected_epoch = ControlEpoch(0);
  FAC_CHECK_ERR(zero_epoch.validate(), ErrorCode::invalid_argument);
}

// ---------------------------------------------------------------------------
// Explicit unknown
// ---------------------------------------------------------------------------

FAC_TEST(model, absent_dimensions_are_not_zero) {
  const AmountVector unknown = AmountVector::unknown();
  FAC_CHECK(!unknown.all_present());
  FAC_CHECK(!unknown.any_present());
  FAC_CHECK_EQ(unknown.present_count(), std::size_t(0));
  FAC_CHECK_EQ(unknown.missing().size(), kDimensionCount);
  FAC_CHECK(!unknown.get(Dimension::power).has_value());

  AmountVector partial;
  FAC_CHECK_OK(partial.set(Dimension::power, 0));
  FAC_CHECK(!partial.all_present());
  FAC_CHECK(partial.any_present());
  FAC_CHECK_EQ(partial.present_count(), std::size_t(1));
  FAC_CHECK(partial.get(Dimension::power).has_value());
  FAC_CHECK_EQ(partial.get(Dimension::power).value(), std::uint64_t(0));
  FAC_CHECK_EQ(partial.missing().size(), kDimensionCount - 1);
  FAC_CHECK(partial.missing().front() == Dimension::cooling);

  // A measured zero and an unmeasured dimension are different values.
  const AmountVector measured_zero = fac_test::amounts(0, 0, 0, 0);
  FAC_CHECK(measured_zero.all_present());
  FAC_CHECK_EQ(measured_zero.present_count(), kDimensionCount);
  FAC_CHECK(measured_zero != unknown);
  FAC_CHECK(measured_zero != partial);

  // Comparisons over an unmeasured vector refuse to conclude rather than
  // treating the missing side as zero.
  FAC_CHECK(!partial.all_less_or_equal(measured_zero));
  FAC_CHECK(!measured_zero.all_less_or_equal(partial));
  FAC_CHECK(!partial.all_greater_than(measured_zero));
  FAC_CHECK_ERR(partial.checked_add(measured_zero), ErrorCode::invalid_argument);
  FAC_CHECK_OK(measured_zero.checked_add(measured_zero));
  FAC_CHECK_EQ(FAC_TAKE(measured_zero.checked_add(fac_test::amounts(1, 1, 1, 1))),
               fac_test::amounts(1, 1, 1, 1));
}

FAC_TEST(model, envelope_remaining_omits_unmeasured_dimensions) {
  EnvelopeSnapshot envelope;
  envelope.envelope = fac_test::envelope_alpha();
  envelope.tenant = fac_test::tenant_alpha();
  envelope.generation = EnvelopeGeneration(1);
  envelope.observed_at = fac_test::fixture_now();
  envelope.limit = fac_test::amounts(5000, 4000, 40, 80);
  FAC_CHECK_OK(envelope.consumed.set(Dimension::power, 1000));

  const AmountVector remaining = envelope.remaining();
  FAC_CHECK(remaining.get(Dimension::power).has_value());
  FAC_CHECK_EQ(remaining.get(Dimension::power).value(), std::uint64_t(4000));
  FAC_CHECK(!remaining.get(Dimension::cooling).has_value());
  FAC_CHECK(!remaining.get(Dimension::space).has_value());
  FAC_CHECK(!remaining.get(Dimension::slots).has_value());
  FAC_CHECK_EQ(remaining.present_count(), std::size_t(1));
  FAC_CHECK(!remaining.all_present());

  // An unmeasured limit is not unlimited.
  EnvelopeSnapshot unmeasured = envelope;
  unmeasured.limit = AmountVector::unknown();
  FAC_CHECK_EQ(unmeasured.remaining().present_count(), std::size_t(0));
  FAC_CHECK(!unmeasured.remaining().any_present());

  // Consumption above the limit leaves no remaining room rather than a
  // negative amount.
  EnvelopeSnapshot overdrawn = envelope;
  overdrawn.consumed = fac_test::amounts(6000, 0, 0, 0);
  FAC_CHECK(!overdrawn.remaining().get(Dimension::power).has_value());

  // Exactly at the limit leaves a measured zero.
  EnvelopeSnapshot exact = envelope;
  exact.consumed = fac_test::amounts(5000, 0, 0, 0);
  FAC_CHECK(exact.remaining().get(Dimension::power).has_value());
  FAC_CHECK_EQ(exact.remaining().get(Dimension::power).value(), std::uint64_t(0));
}

FAC_TEST(model, grant_with_an_unmeasured_demand_is_refused_by_decode) {
  Grant grant = make_grant();
  AmountVector partial;
  FAC_CHECK_OK(partial.set(Dimension::power, 200));
  FAC_CHECK(!partial.all_present());
  grant.demand = partial;

  codec::Writer writer;
  grant.encode(writer);
  codec::Reader reader(writer.span());
  FAC_CHECK_ERR(Grant::decode(reader), ErrorCode::malformed_input);

  // The same grant with a fully measured demand decodes.
  grant.demand = fac_test::amounts(200, 100, 1, 1);
  codec::Writer good_writer;
  grant.encode(good_writer);
  codec::Reader good_reader(good_writer.span());
  FAC_CHECK_EQ(FAC_TAKE(Grant::decode(good_reader)), grant);
}

// ---------------------------------------------------------------------------
// Decision digest
// ---------------------------------------------------------------------------

FAC_TEST(model, decision_digest_is_stable_and_field_sensitive) {
  const Decision base = make_decision(Verdict::allow, true);
  FAC_CHECK(!base.digest.is_unset());
  FAC_CHECK_EQ(base.compute_digest(), base.digest);
  FAC_CHECK_EQ(base.compute_digest(), base.compute_digest());

  // The transient replay flag is deliberately outside the digest: replaying a
  // recorded decision must reproduce the recorded value.
  Decision replayed = base;
  replayed.replayed = true;
  FAC_CHECK_EQ(replayed.compute_digest(), base.compute_digest());

  const Digest256 original = base.compute_digest();

  Decision mutated = base;
  mutated.request_id = fac_test::request_id(99);
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.request_digest = Digest256::of(bytes_of("a different request"));
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.verdict = Verdict::refuse;
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  FAC_CHECK_OK(mutated.blockers.add(BlockerCode::policy_denied, "the policy refused the commitment"));
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.control_epoch = ControlEpoch(6);
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.sequence = FAC_TAKE(LedgerSequence::from_value(12));
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.decided_at = fac_test::at_seconds(1);
  FAC_CHECK_NE(mutated.compute_digest(), original);

  // The same evidence kinds with a different generation bound.
  mutated = base;
  mutated.evidence = EvidenceSet{};
  FAC_CHECK_OK(mutated.evidence.add(
      make_evidence(EvidenceKind::capacity, 8, "capacity-evidence", fac_test::fixture_now())));
  FAC_CHECK_OK(mutated.evidence.add(
      make_evidence(EvidenceKind::policy, 8, "policy-evidence", fac_test::fixture_now())));
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  DimensionAssessment extra;
  extra.dimension = Dimension::power;
  extra.remaining = 1;
  mutated.assessment = CommitmentAssessment{};
  FAC_CHECK_OK(mutated.assessment.add(extra));
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.explanation.push_back("one more statement");
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.grant.reset();
  FAC_CHECK_NE(mutated.compute_digest(), original);

  mutated = base;
  mutated.grant.value().holds_capacity = false;
  FAC_CHECK_NE(mutated.compute_digest(), original);
}

FAC_TEST(model, decision_blocker_set_is_ordered_and_idempotent) {
  Decision decision = make_decision(Verdict::refuse, false);
  FAC_CHECK_OK(decision.blockers.add(BlockerCode::capacity_exhausted, "no free power"));
  FAC_CHECK_OK(decision.blockers.add(BlockerCode::request_invalid, "the request names no tenant"));

  // Precedence order is the enum order, not the order the checks ran in.
  FAC_CHECK(decision.blockers.primary() != nullptr);
  FAC_CHECK_EQ(decision.blockers.primary()->code, BlockerCode::request_invalid);
  FAC_CHECK_EQ(decision.primary_blocker().value(), BlockerCode::request_invalid);
  FAC_CHECK_EQ(decision.blockers.implied_verdict(), Verdict::refuse);

  // Adding the same condition again does not duplicate it.
  FAC_CHECK_OK(decision.blockers.add(BlockerCode::capacity_exhausted, "no free power"));
  FAC_CHECK_EQ(decision.blockers.items().size(), std::size_t(2));

  // The same code with a different dimension is a different record.
  FAC_CHECK_OK(decision.blockers.add(BlockerCode::capacity_exhausted, "no free power", Dimension::power));
  FAC_CHECK_EQ(decision.blockers.items().size(), std::size_t(3));

  // A deferral set implies a deferral, and an empty set implies an allowance.
  Decision deferral = make_decision(Verdict::defer, false);
  FAC_CHECK_OK(deferral.blockers.add(BlockerCode::tenant_missing, "no tenant evidence"));
  FAC_CHECK_EQ(deferral.blockers.implied_verdict(), Verdict::defer);
  FAC_CHECK_EQ(blocker_verdict(BlockerCode::tenant_missing), Verdict::defer);
  FAC_CHECK_EQ(blocker_verdict(BlockerCode::tenant_not_active), Verdict::refuse);

  Decision empty;
  FAC_CHECK_EQ(empty.blockers.implied_verdict(), Verdict::allow);
  FAC_CHECK(empty.blockers.primary() == nullptr);
  FAC_CHECK(!empty.primary_blocker().has_value());
}
