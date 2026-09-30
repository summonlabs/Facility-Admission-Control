// Facility Admission Control - adversarial tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Hostile, absurd and boundary input at the two places it can enter: the
// canonical codec and the durable store's path handling. Every refusal here is
// asserted with its exact error code. A test in this file that only proved
// "something failed" would be worthless, because the interesting question is
// always which refusal happened: a truncated buffer, an impossible enum, an
// unset digest and a path that is a file all have to be different answers.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"
#include "fac/durable/store.hpp"
#include "fac/model/decision.hpp"
#include "fac/model/request.hpp"
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

// A scope prefix as TargetScope::encode writes it, so a decoder can be driven
// to the field under test.
void write_open_scope(codec::Writer& writer) {
  writer.ident(fac_test::facility_a());
  writer.boolean(false);
  writer.reserved(16);
  writer.boolean(false);
  writer.reserved(16);
}

AdmissionRequest make_valid_request() {
  AdmissionRequest request;
  request.request_id = fac_test::request_id(1);
  request.tenant = fac_test::tenant_alpha();
  request.service_class = fac_test::class_gold();
  request.envelope = fac_test::envelope_alpha();
  request.scope =
      FAC_TAKE(TargetScope::make(fac_test::facility_a(), fac_test::rack_1(), fac_test::zone_a()));
  request.demand = fac_test::amounts(200, 100, 1, 1);
  request.requested_at = fac_test::fixture_now();
  request.commitment_start = fac_test::at_seconds(0);
  request.commitment_end = fac_test::at_seconds(3600);
  FAC_CHECK_OK(request.validate());
  return request;
}

std::string control_characters(std::size_t count) {
  std::string out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(static_cast<char>(i % 32));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Structural hostility at the codec boundary
// ---------------------------------------------------------------------------

FAC_TEST(adversarial, declared_lengths_far_beyond_the_buffer) {
  // A length of 4 GiB - 1 behind four bytes: refused by the caller's bound
  // before any allocation is attempted.
  codec::Writer huge;
  huge.u32(std::numeric_limits<std::uint32_t>::max());
  huge.raw(bytes_of("abcd"));
  codec::Reader huge_bytes(huge.span());
  FAC_CHECK_ERR(huge_bytes.bytes(kMaxFramePayload), ErrorCode::limit_exceeded);
  codec::Reader huge_text(huge.span());
  FAC_CHECK_ERR(huge_text.text(kMaxReasonLength), ErrorCode::limit_exceeded);

  // A length inside the caller's bound but far past the buffer.
  codec::Writer far;
  far.u32(std::uint32_t{1u << 20});
  far.raw(bytes_of("abcd"));
  codec::Reader far_bytes(far.span());
  FAC_CHECK_ERR(far_bytes.bytes(kMaxFramePayload), ErrorCode::truncated_input);
  codec::Reader far_text(far.span());
  FAC_CHECK_ERR(far_text.text(kMaxFramePayload), ErrorCode::truncated_input);
  codec::Reader far_raw(far.span());
  FAC_CHECK_ERR(far_raw.raw(1u << 20), ErrorCode::truncated_input);

  // A sequence claiming more elements than any bound allows.
  codec::Writer sequence;
  sequence.u32(std::numeric_limits<std::uint32_t>::max());
  codec::Reader sequence_reader(sequence.span());
  FAC_CHECK_ERR(sequence_reader.sequence_count(kMaxMaintenanceWindows, 100),
                ErrorCode::limit_exceeded);
}

FAC_TEST(adversarial, huge_sequence_counts) {
  // Far above the configured element bound.
  codec::Writer over;
  over.u32(std::numeric_limits<std::uint32_t>::max());
  codec::Reader over_reader(over.span());
  FAC_CHECK_ERR(over_reader.sequence_count(kDimensionCount, 40), ErrorCode::limit_exceeded);

  // Inside the element bound, but provably unable to fit in what remains.
  codec::Writer tight;
  tight.u32(std::uint32_t{1000});
  tight.raw(bytes_of("ab"));
  codec::Reader tight_reader(tight.span());
  FAC_CHECK_ERR(tight_reader.sequence_count(4096, 16), ErrorCode::truncated_input);

  // The same claim inside real model decoders.
  codec::Writer maintenance;
  maintenance.counter(MaintenanceGeneration(1));
  maintenance.i64(fac_test::fixture_now().unix_nanos());
  maintenance.u32(std::numeric_limits<std::uint32_t>::max());
  codec::Reader maintenance_reader(maintenance.span());
  FAC_CHECK_ERR(MaintenanceSnapshot::decode(maintenance_reader), ErrorCode::limit_exceeded);

  codec::Writer evidence;
  evidence.u32(std::uint32_t{1000});
  codec::Reader evidence_reader(evidence.span());
  FAC_CHECK_ERR(EvidenceSet::decode(evidence_reader), ErrorCode::limit_exceeded);

  codec::Writer evidence_tight;
  evidence_tight.u32(std::uint32_t{9});
  codec::Reader evidence_tight_reader(evidence_tight.span());
  FAC_CHECK_ERR(EvidenceSet::decode(evidence_tight_reader), ErrorCode::truncated_input);

  codec::Writer placement;
  placement.counter(PlacementGeneration(1));
  placement.i64(fac_test::fixture_now().unix_nanos());
  placement.u32(std::uint32_t{100000});
  codec::Reader placement_reader(placement.span());
  FAC_CHECK_ERR(PlacementPolicySnapshot::decode(placement_reader), ErrorCode::limit_exceeded);
}

FAC_TEST(adversarial, impossible_enum_bytes) {
  // An evidence kind one past the last defined value.
  codec::Writer kind;
  kind.u8(static_cast<std::uint8_t>(kEvidenceKindCount));
  codec::Reader kind_reader(kind.span());
  FAC_CHECK_ERR(EvidenceRef::decode(kind_reader), ErrorCode::malformed_input);

  // A verdict that is not allow, refuse or defer.
  codec::Writer verdict;
  verdict.ident(fac_test::request_id(1));
  verdict.digest(Digest256::of(bytes_of("request-content")));
  verdict.u8(std::uint8_t{3});
  codec::Reader verdict_reader(verdict.span());
  FAC_CHECK_ERR(Decision::decode(verdict_reader), ErrorCode::malformed_input);

  // A dimension that does not exist.
  codec::Writer dimension;
  dimension.u8(static_cast<std::uint8_t>(kDimensionCount));
  codec::Reader dimension_reader(dimension.span());
  FAC_CHECK_ERR(DimensionAssessment::decode(dimension_reader), ErrorCode::malformed_input);

  // A blocker code one past the last defined value.
  codec::Writer blocker;
  blocker.u8(static_cast<std::uint8_t>(kBlockerCodeCount));
  codec::Reader blocker_reader(blocker.span());
  FAC_CHECK_ERR(Blocker::decode(blocker_reader), ErrorCode::malformed_input);

  // A tenant status that does not exist.
  codec::Writer tenant;
  tenant.ident(fac_test::tenant_alpha());
  tenant.counter(TenantGeneration(1));
  tenant.i64(fac_test::fixture_now().unix_nanos());
  tenant.u8(std::uint8_t{4});
  codec::Reader tenant_reader(tenant.span());
  FAC_CHECK_ERR(TenantSnapshot::decode(tenant_reader), ErrorCode::malformed_input);

  // An overcommit mode that does not exist.
  codec::Writer overcommit;
  overcommit.u8(std::uint8_t{3});
  codec::Reader overcommit_reader(overcommit.span());
  FAC_CHECK_ERR(OvercommitAllowance::decode(overcommit_reader), ErrorCode::malformed_input);

  // A redundancy level that does not exist, behind its presence byte.
  codec::Writer redundancy;
  redundancy.counter(RedundancyGeneration(1));
  redundancy.i64(fac_test::fixture_now().unix_nanos());
  redundancy.boolean(true);
  redundancy.u8(std::uint8_t{4});
  codec::Reader redundancy_reader(redundancy.span());
  FAC_CHECK_ERR(RedundancySnapshot::decode(redundancy_reader), ErrorCode::malformed_input);

  // A commit state that does not exist.
  codec::Writer outcome;
  outcome.ident(FAC_TAKE(GrantId::from_hex("a1a1a1a1-b2b2-c3c3-d4d4-e5e5e5e5e5e5")));
  outcome.u8(std::uint8_t{5});
  codec::Reader outcome_reader(outcome.span());
  FAC_CHECK_ERR(CommitOutcome::decode(outcome_reader), ErrorCode::malformed_input);

  // An impossible dimension carried by a blocker that says it has one.
  codec::Writer blocker_dimension;
  blocker_dimension.u8(static_cast<std::uint8_t>(BlockerCode::capacity_exhausted));
  blocker_dimension.boolean(true);
  blocker_dimension.u8(static_cast<std::uint8_t>(kDimensionCount));
  blocker_dimension.text("out of room");
  codec::Reader blocker_dimension_reader(blocker_dimension.span());
  FAC_CHECK_ERR(Blocker::decode(blocker_dimension_reader), ErrorCode::malformed_input);

  // A blocker that says it has no dimension but carries one anyway.
  codec::Writer blocker_contradiction;
  blocker_contradiction.u8(static_cast<std::uint8_t>(BlockerCode::capacity_exhausted));
  blocker_contradiction.boolean(false);
  blocker_contradiction.u8(std::uint8_t{1});
  blocker_contradiction.text("out of room");
  codec::Reader blocker_contradiction_reader(blocker_contradiction.span());
  FAC_CHECK_ERR(Blocker::decode(blocker_contradiction_reader), ErrorCode::malformed_input);

  // An evidence rejection code outside the error model.
  codec::Writer rejection;
  rejection.u8(static_cast<std::uint8_t>(EvidenceKind::capacity));
  rejection.u64(std::uint64_t{1});
  rejection.digest(Digest256::of(bytes_of("capacity-evidence")));
  rejection.i64(fac_test::fixture_now().unix_nanos());
  rejection.boolean(false);
  rejection.u16(static_cast<std::uint16_t>(ErrorCode::internal) + 1);
  codec::Reader rejection_reader(rejection.span());
  FAC_CHECK_ERR(EvidenceRef::decode(rejection_reader), ErrorCode::malformed_input);
}

FAC_TEST(adversarial, reserved_field_that_is_not_zero) {
  codec::Writer tail;
  tail.reserved(3);
  tail.u8(std::uint8_t{0x80});
  codec::Reader tail_reader(tail.span());
  FAC_CHECK_ERR(tail_reader.reserved(4), ErrorCode::reserved_field_nonzero);

  codec::Writer negative_one;
  negative_one.reserved(7);
  negative_one.u8(std::uint8_t{0xFF});
  codec::Reader negative_one_reader(negative_one.span());
  FAC_CHECK_ERR(negative_one_reader.reserved(8), ErrorCode::reserved_field_nonzero);

  codec::Writer truncated;
  truncated.reserved(3);
  codec::Reader truncated_reader(truncated.span());
  FAC_CHECK_ERR(truncated_reader.reserved(4), ErrorCode::truncated_input);

  // A reserved field that is not present at all.
  codec::Writer empty;
  codec::Reader empty_reader(empty.span());
  FAC_CHECK_ERR(empty_reader.reserved(1), ErrorCode::truncated_input);
}

FAC_TEST(adversarial, digest_of_sixty_four_zeros) {
  // The all-zero digest is the explicit "unset" value and is never a binding.
  codec::Writer zeros;
  zeros.reserved(Digest256::kBytes);
  codec::Reader zeros_reader(zeros.span());
  FAC_CHECK_ERR(zeros_reader.digest(), ErrorCode::malformed_input);

  FAC_CHECK_ERR(Digest256::from_hex(std::string(64, '0')), ErrorCode::malformed_input);

  // Inside a decision: an unset request digest.
  codec::Writer decision;
  decision.ident(fac_test::request_id(1));
  decision.reserved(Digest256::kBytes);
  codec::Reader decision_reader(decision.span());
  FAC_CHECK_ERR(Decision::decode(decision_reader), ErrorCode::malformed_input);

  // Inside a grant binding: an unset evidence digest.
  codec::Writer binding;
  binding.u8(static_cast<std::uint8_t>(EvidenceKind::capacity));
  binding.u64(std::uint64_t{1});
  binding.reserved(Digest256::kBytes);
  codec::Reader binding_reader(binding.span());
  FAC_CHECK_ERR(GrantBinding::decode(binding_reader), ErrorCode::malformed_input);

  // Inside a policy snapshot: an unset policy digest.
  codec::Writer policy;
  policy.ident(fac_test::policy_main());
  policy.counter(PolicyGeneration(1));
  policy.i64(fac_test::fixture_now().unix_nanos());
  policy.u8(static_cast<std::uint8_t>(PolicyVerdict::permit));
  policy.u8(static_cast<std::uint8_t>(OvercommitMode::not_permitted));
  for (int i = 0; i < 4; ++i) {
    policy.boolean(false);
    policy.u64(std::uint64_t{0});
  }
  policy.reserved(Digest256::kBytes);
  codec::Reader policy_reader(policy.span());
  FAC_CHECK_ERR(PolicySnapshot::decode(policy_reader), ErrorCode::malformed_input);
}

// ---------------------------------------------------------------------------
// Identity, scope, time and quantity hostility
// ---------------------------------------------------------------------------

FAC_TEST(adversarial, request_with_a_zero_identity) {
  AdmissionRequest request = make_valid_request();

  request.request_id = RequestId{};
  FAC_CHECK_ERR(request.validate(), ErrorCode::invalid_argument);
  codec::Writer zero_request;
  request.encode(zero_request);
  codec::Reader zero_request_reader(zero_request.span());
  FAC_CHECK_ERR(AdmissionRequest::decode(zero_request_reader), ErrorCode::invalid_argument);

  request = make_valid_request();
  request.tenant = TenantId{};
  FAC_CHECK_ERR(request.validate(), ErrorCode::invalid_argument);
  codec::Writer zero_tenant;
  request.encode(zero_tenant);
  codec::Reader zero_tenant_reader(zero_tenant.span());
  FAC_CHECK_ERR(AdmissionRequest::decode(zero_tenant_reader), ErrorCode::invalid_argument);

  request = make_valid_request();
  request.service_class = ServiceClassId{};
  FAC_CHECK_ERR(request.validate(), ErrorCode::invalid_argument);
  codec::Writer zero_class;
  request.encode(zero_class);
  codec::Reader zero_class_reader(zero_class.span());
  FAC_CHECK_ERR(AdmissionRequest::decode(zero_class_reader), ErrorCode::invalid_argument);

  request = make_valid_request();
  request.envelope = EnvelopeId{};
  FAC_CHECK_ERR(request.validate(), ErrorCode::invalid_argument);
  codec::Writer zero_envelope;
  request.encode(zero_envelope);
  codec::Reader zero_envelope_reader(zero_envelope.span());
  FAC_CHECK_ERR(AdmissionRequest::decode(zero_envelope_reader), ErrorCode::invalid_argument);

  // A zero facility inside the scope.
  request = make_valid_request();
  request.scope.facility = FacilityId{};
  FAC_CHECK_ERR(request.validate(), ErrorCode::invalid_argument);
  codec::Writer zero_facility;
  request.encode(zero_facility);
  codec::Reader zero_facility_reader(zero_facility.span());
  FAC_CHECK_ERR(AdmissionRequest::decode(zero_facility_reader), ErrorCode::invalid_argument);

  // Only the all-zero identity is unset: a half-zero identity is a value.
  const RequestId half_zero = FAC_TAKE(RequestId::from_hex("00000000-0000-0000-0000-000000000001"));
  FAC_CHECK(!half_zero.is_unset());
  FAC_CHECK_EQ(half_zero.hi(), std::uint64_t{0});
  FAC_CHECK_EQ(half_zero.lo(), std::uint64_t{1});
  request = make_valid_request();
  request.request_id = half_zero;
  FAC_CHECK_OK(request.validate());
}

FAC_TEST(adversarial, scope_with_a_zero_rack) {
  FAC_CHECK_ERR(TargetScope::make(fac_test::facility_a(), RackId{}, std::nullopt),
                ErrorCode::invalid_argument);
  FAC_CHECK_ERR(TargetScope::make(fac_test::facility_a(), std::nullopt, ZoneId{}),
                ErrorCode::invalid_argument);
  FAC_CHECK_ERR(TargetScope::make(FacilityId{}, fac_test::rack_1(), std::nullopt),
                ErrorCode::invalid_argument);

  // The same at the persistence boundary: the presence byte says a rack is
  // present and sixteen zero bytes follow.
  codec::Writer zero_rack;
  zero_rack.ident(fac_test::facility_a());
  zero_rack.boolean(true);
  zero_rack.reserved(16);
  zero_rack.boolean(false);
  zero_rack.reserved(16);
  codec::Reader zero_rack_reader(zero_rack.span());
  FAC_CHECK_ERR(TargetScope::decode(zero_rack_reader), ErrorCode::invalid_argument);

  codec::Writer zero_zone;
  zero_zone.ident(fac_test::facility_a());
  zero_zone.boolean(false);
  zero_zone.reserved(16);
  zero_zone.boolean(true);
  zero_zone.reserved(16);
  codec::Reader zero_zone_reader(zero_zone.span());
  FAC_CHECK_ERR(TargetScope::decode(zero_zone_reader), ErrorCode::invalid_argument);

  codec::Writer zero_facility;
  zero_facility.reserved(16);
  codec::Reader zero_facility_reader(zero_facility.span());
  FAC_CHECK_ERR(TargetScope::decode(zero_facility_reader), ErrorCode::invalid_argument);

  // An absent rack that carries a non-zero identity is a different encoding of
  // "absent", not a value this reader may ignore.
  codec::Writer contradiction;
  contradiction.ident(fac_test::facility_a());
  contradiction.boolean(false);
  contradiction.u64(std::uint64_t{1});
  contradiction.u64(std::uint64_t{0});
  contradiction.boolean(false);
  contradiction.reserved(16);
  codec::Reader contradiction_reader(contradiction.span());
  FAC_CHECK_ERR(TargetScope::decode(contradiction_reader), ErrorCode::malformed_input);

  codec::Writer absent_zone_contradiction;
  absent_zone_contradiction.ident(fac_test::facility_a());
  absent_zone_contradiction.boolean(false);
  absent_zone_contradiction.reserved(16);
  absent_zone_contradiction.boolean(false);
  absent_zone_contradiction.u64(std::uint64_t{0});
  absent_zone_contradiction.u64(std::uint64_t{9});
  codec::Reader absent_zone_contradiction_reader(absent_zone_contradiction.span());
  FAC_CHECK_ERR(TargetScope::decode(absent_zone_contradiction_reader), ErrorCode::malformed_input);
}

FAC_TEST(adversarial, negative_timestamps) {
  FAC_CHECK_ERR(Timestamp::from_unix_nanos(-1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(Timestamp::from_unix_nanos(std::numeric_limits<Nanos>::min()),
                ErrorCode::out_of_range);
  FAC_CHECK_ERR(Timestamp::from_unix_nanos(kMaxTimestampNanos + 1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(age_of(Timestamp(10), Timestamp(0)), ErrorCode::underflow);

  // A capacity snapshot observed before the epoch.
  codec::Writer capacity;
  capacity.counter(CapacityGeneration(1));
  capacity.i64(std::int64_t{-1});
  codec::Reader capacity_reader(capacity.span());
  FAC_CHECK_ERR(CapacitySnapshot::decode(capacity_reader), ErrorCode::out_of_range);

  // An evidence reference observed before the epoch.
  codec::Writer evidence;
  evidence.u8(static_cast<std::uint8_t>(EvidenceKind::capacity));
  evidence.u64(std::uint64_t{1});
  evidence.digest(Digest256::of(bytes_of("capacity-evidence")));
  evidence.i64(std::numeric_limits<std::int64_t>::min());
  codec::Reader evidence_reader(evidence.span());
  FAC_CHECK_ERR(EvidenceRef::decode(evidence_reader), ErrorCode::out_of_range);

  // A maintenance window whose bounds precede the epoch.
  codec::Writer window;
  window.ident(fac_test::window_one());
  write_open_scope(window);
  window.i64(std::int64_t{-10});
  window.i64(std::int64_t{10});
  window.u8(static_cast<std::uint8_t>(MaintenanceState::planned));
  window.u8(static_cast<std::uint8_t>(MaintenanceImpact::none));
  codec::Reader window_reader(window.span());
  FAC_CHECK_ERR(MaintenanceWindow::decode(window_reader), ErrorCode::out_of_range);

  // A request whose commitment window is negative. The range rule is applied
  // where the bytes are read: Timestamp's unchecked constructor can hold a
  // negative value, and the decoder is what refuses to carry it onwards.
  AdmissionRequest request = make_valid_request();
  request.commitment_start = Timestamp(-1);
  request.commitment_end = Timestamp(10);
  codec::Writer request_writer;
  request.encode(request_writer);
  codec::Reader request_reader(request_writer.span());
  FAC_CHECK_ERR(AdmissionRequest::decode(request_reader), ErrorCode::out_of_range);

  // A request whose requested_at precedes the epoch.
  AdmissionRequest stale = make_valid_request();
  stale.requested_at = Timestamp(-1);
  codec::Writer stale_writer;
  stale.encode(stale_writer);
  codec::Reader stale_reader(stale_writer.span());
  FAC_CHECK_ERR(AdmissionRequest::decode(stale_reader), ErrorCode::out_of_range);
}

FAC_TEST(adversarial, demand_above_the_supported_maximum) {
  AmountVector vector;
  FAC_CHECK_ERR(vector.set(Dimension::power, kMaxQuantity + 1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(vector.set(Dimension::slots, std::numeric_limits<std::uint64_t>::max()),
                ErrorCode::out_of_range);
  FAC_CHECK_OK(vector.set(Dimension::power, kMaxQuantity));
  FAC_CHECK_EQ(vector.get(Dimension::power).value(), kMaxQuantity);

  // A present amount one past the maximum, encoded by hand.
  codec::Writer over;
  over.boolean(true);
  over.u64(kMaxQuantity + 1);
  for (int index = 1; index < 4; ++index) {
    over.boolean(false);
    over.u64(std::uint64_t{0});
  }
  codec::Reader over_reader(over.span());
  FAC_CHECK_ERR(AmountVector::decode(over_reader), ErrorCode::out_of_range);

  // The 64-bit ceiling in a quantity field.
  codec::Writer extreme;
  extreme.boolean(true);
  extreme.u64(std::numeric_limits<std::uint64_t>::max());
  for (int index = 1; index < 4; ++index) {
    extreme.boolean(false);
    extreme.u64(std::uint64_t{0});
  }
  codec::Reader extreme_reader(extreme.span());
  FAC_CHECK_ERR(AmountVector::decode(extreme_reader), ErrorCode::out_of_range);

  // Exactly the maximum is a legal demand and survives the round trip.
  const AmountVector at_max = fac_test::amounts(kMaxQuantity, kMaxQuantity, kMaxQuantity, kMaxQuantity);
  codec::Writer at_max_writer;
  at_max.encode(at_max_writer);
  codec::Reader at_max_reader(at_max_writer.span());
  FAC_CHECK_EQ(FAC_TAKE(AmountVector::decode(at_max_reader)), at_max);

  // Adding two maximal demands exceeds the supported range rather than wrapping.
  FAC_CHECK_ERR(at_max.checked_add(at_max), ErrorCode::overflow);

  // A request may carry an unmeasured demand: the engine defers on that rather
  // than reading the missing dimension as zero. What it may not carry is a value
  // above the supported maximum, and set() refuses it before the request exists.
  AdmissionRequest request = make_valid_request();
  FAC_CHECK_ERR(request.demand.set(Dimension::cooling, kMaxQuantity + 1), ErrorCode::out_of_range);
  FAC_CHECK_OK(request.demand.set(Dimension::cooling, kMaxQuantity));
  codec::Writer writer;
  request.encode(writer);
  codec::Reader reader(writer.span());
  FAC_CHECK_EQ(FAC_TAKE(AdmissionRequest::decode(reader)), request);
}

FAC_TEST(adversarial, boundary_integers) {
  // Quantities at and past the bound.
  AmountVector vector;
  FAC_CHECK_OK(vector.set(Dimension::power, kMaxQuantity));
  FAC_CHECK_OK(vector.set(Dimension::power, 0));
  FAC_CHECK_OK(vector.set(Dimension::power, kMaxQuantity - 1));
  FAC_CHECK_ERR(vector.set(Dimension::power, kMaxQuantity + 1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(vector.set(Dimension::power, std::numeric_limits<std::uint64_t>::max()),
                ErrorCode::out_of_range);

  // Counters at and past the bound, in memory and in the codec.
  FAC_CHECK_OK(LedgerSequence::from_value(kMaxCounter));
  FAC_CHECK_OK(LedgerSequence::from_value(0));
  FAC_CHECK_ERR(LedgerSequence::from_value(kMaxCounter + 1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(LedgerSequence::from_value(std::numeric_limits<std::uint64_t>::max()),
                ErrorCode::out_of_range);
  FAC_CHECK_ERR(LedgerSequence(kMaxCounter).checked_next(), ErrorCode::overflow);
  FAC_CHECK_OK(LedgerSequence(kMaxCounter - 1).checked_next());
  FAC_CHECK_EQ(LedgerSequence(kMaxCounter - 1).checked_next().value().value(), kMaxCounter);

  codec::Writer max_counter;
  max_counter.u64(kMaxCounter);
  codec::Reader max_counter_reader(max_counter.span());
  FAC_CHECK_EQ(FAC_TAKE(max_counter_reader.counter<LedgerSequenceTag>()).value(), kMaxCounter);

  codec::Writer over_counter;
  over_counter.u64(kMaxCounter + 1);
  codec::Reader over_counter_reader(over_counter.span());
  FAC_CHECK_ERR(over_counter_reader.counter<LedgerSequenceTag>(), ErrorCode::out_of_range);

  codec::Writer ceiling;
  ceiling.u64(std::numeric_limits<std::uint64_t>::max());
  codec::Reader ceiling_reader(ceiling.span());
  FAC_CHECK_ERR(ceiling_reader.counter<ControlEpochTag>(), ErrorCode::out_of_range);

  // A quantity of exactly the maximum inside a decoded request's demand.
  AdmissionRequest request = make_valid_request();
  request.demand = fac_test::amounts(kMaxQuantity, 0, kMaxQuantity, 0);
  codec::Writer writer;
  request.encode(writer);
  codec::Reader reader(writer.span());
  FAC_CHECK_EQ(FAC_TAKE(AdmissionRequest::decode(reader)), request);
}

// ---------------------------------------------------------------------------
// Hostile text
// ---------------------------------------------------------------------------

FAC_TEST(adversarial, hundred_kilobytes_of_text) {
  const std::string hundred_kb(100 * 1024, 'x');
  FAC_CHECK_EQ(hundred_kb.size(), std::size_t(102400));

  codec::Writer writer;
  writer.text(hundred_kb);

  // Bounded reasons and names refuse it before reading it.
  codec::Reader reason_reader(writer.span());
  FAC_CHECK_ERR(reason_reader.text(kMaxReasonLength), ErrorCode::limit_exceeded);
  codec::Reader name_reader(writer.span());
  FAC_CHECK_ERR(name_reader.text(kMaxNameLength), ErrorCode::limit_exceeded);

  // A caller that allows the bytes still gets them back exactly.
  codec::Reader allowed_reader(writer.span());
  FAC_CHECK_EQ(FAC_TAKE(allowed_reader.text(hundred_kb.size())), hundred_kb);

  // The same body in a blocker's detail is refused by its own bound.
  codec::Writer blocker;
  blocker.u8(static_cast<std::uint8_t>(BlockerCode::request_invalid));
  blocker.boolean(false);
  blocker.u8(std::uint8_t{0});
  blocker.text(hundred_kb);
  codec::Reader blocker_reader(blocker.span());
  FAC_CHECK_ERR(Blocker::decode(blocker_reader), ErrorCode::limit_exceeded);

  // One byte over the reason bound is refused as well.
  codec::Writer just_over;
  just_over.u8(static_cast<std::uint8_t>(BlockerCode::request_invalid));
  just_over.boolean(false);
  just_over.u8(std::uint8_t{0});
  just_over.text(std::string(kMaxReasonLength + 1, 'r'));
  codec::Reader just_over_reader(just_over.span());
  FAC_CHECK_ERR(Blocker::decode(just_over_reader), ErrorCode::limit_exceeded);

  // ... and a reason of exactly the bound is accepted.
  codec::Writer at_bound;
  at_bound.u8(static_cast<std::uint8_t>(BlockerCode::request_invalid));
  at_bound.boolean(false);
  at_bound.u8(std::uint8_t{0});
  at_bound.text(std::string(kMaxReasonLength, 'r'));
  codec::Reader at_bound_reader(at_bound.span());
  const Blocker accepted = FAC_TAKE(Blocker::decode(at_bound_reader));
  FAC_CHECK_EQ(accepted.detail.size(), kMaxReasonLength);
}

FAC_TEST(adversarial, invalid_utf8_in_a_text_field) {
  // A truncated two-byte sequence.
  codec::Writer truncated;
  truncated.text(std::string("\xC3", 1));
  codec::Reader truncated_reader(truncated.span());
  FAC_CHECK_ERR(truncated_reader.text(kMaxReasonLength), ErrorCode::invalid_utf8);

  // A continuation byte that is not one.
  codec::Writer stray;
  stray.text(std::string("\xC3\x28", 2));
  codec::Reader stray_reader(stray.span());
  FAC_CHECK_ERR(stray_reader.text(kMaxReasonLength), ErrorCode::invalid_utf8);

  // An overlong encoding of NUL.
  codec::Writer overlong;
  overlong.text(std::string("\xC0\x80", 2));
  codec::Reader overlong_reader(overlong.span());
  FAC_CHECK_ERR(overlong_reader.text(kMaxReasonLength), ErrorCode::invalid_utf8);

  // A surrogate.
  codec::Writer surrogate;
  surrogate.text(std::string("\xED\xA0\x80", 3));
  codec::Reader surrogate_reader(surrogate.span());
  FAC_CHECK_ERR(surrogate_reader.text(kMaxReasonLength), ErrorCode::invalid_utf8);

  // The same bytes inside a blocker's detail.
  codec::Writer blocker;
  blocker.u8(static_cast<std::uint8_t>(BlockerCode::evidence_scope_mismatch));
  blocker.boolean(false);
  blocker.u8(std::uint8_t{0});
  blocker.text(std::string("\xFF\xFE", 2));
  codec::Reader blocker_reader(blocker.span());
  FAC_CHECK_ERR(Blocker::decode(blocker_reader), ErrorCode::invalid_utf8);

  // Well-formed non-ASCII text is still accepted, so the refusal is about the
  // encoding and not about the alphabet.
  codec::Writer well_formed;
  well_formed.text(std::string("\xE2\x82\xAC", 3));
  codec::Reader well_formed_reader(well_formed.span());
  FAC_CHECK_EQ(FAC_TAKE(well_formed_reader.text(kMaxReasonLength)).size(), std::size_t(3));
}

FAC_TEST(adversarial, embedded_nul_in_a_text_field) {
  const std::string with_nul("a\0b", 3);
  FAC_CHECK_EQ(with_nul.size(), std::size_t(3));

  codec::Writer writer;
  writer.text(with_nul);
  codec::Reader reader(writer.span());
  const std::string decoded = FAC_TAKE(reader.text(kMaxReasonLength));

  // The byte survives the codec: a NUL is valid UTF-8. It is the reason rule
  // that refuses it, which is the whole point of keeping the two apart.
  FAC_CHECK_EQ(decoded.size(), std::size_t(3));
  FAC_CHECK_EQ(decoded[1], '\0');
  FAC_CHECK(is_valid_utf8(decoded));
  FAC_CHECK(contains_control_characters(decoded));
  FAC_CHECK(!is_valid_reason(decoded));
  FAC_CHECK_EQ(escape_text(decoded), std::string("a\\x00b"));

  // A blocker whose detail embeds a NUL is refused at the persistence boundary.
  codec::Writer blocker;
  blocker.u8(static_cast<std::uint8_t>(BlockerCode::request_invalid));
  blocker.boolean(false);
  blocker.u8(std::uint8_t{0});
  blocker.text(with_nul);
  codec::Reader blocker_reader(blocker.span());
  FAC_CHECK_ERR(Blocker::decode(blocker_reader), ErrorCode::malformed_input);

  // The same for a commitment's resolution detail.
  codec::Writer detail;
  detail.text(with_nul);
  codec::Reader detail_reader(detail.span());
  FAC_CHECK_EQ(FAC_TAKE(detail_reader.text(kMaxReasonLength)), with_nul);
}

FAC_TEST(adversarial, reason_full_of_control_characters) {
  const std::string controls = control_characters(32);
  FAC_CHECK_EQ(controls.size(), std::size_t(32));
  FAC_CHECK(contains_control_characters(controls));
  FAC_CHECK(!is_valid_reason(controls));

  // A blocker detail made of control characters.
  codec::Writer blocker;
  blocker.u8(static_cast<std::uint8_t>(BlockerCode::request_invalid));
  blocker.boolean(false);
  blocker.u8(std::uint8_t{0});
  blocker.text(controls);
  codec::Reader blocker_reader(blocker.span());
  FAC_CHECK_ERR(Blocker::decode(blocker_reader), ErrorCode::malformed_input);

  // A decision explanation line made of control characters.
  codec::Writer decision;
  decision.ident(fac_test::request_id(1));
  decision.digest(Digest256::of(bytes_of("request-content")));
  decision.u8(static_cast<std::uint8_t>(Verdict::allow));
  decision.u32(std::uint32_t{0});
  decision.counter(ControlEpoch(1));
  decision.counter(LedgerSequence(0));
  decision.i64(fac_test::fixture_now().unix_nanos());
  decision.u32(std::uint32_t{0});
  decision.u32(std::uint32_t{0});
  decision.u32(std::uint32_t{1});
  decision.text(controls);
  codec::Reader decision_reader(decision.span());
  FAC_CHECK_ERR(Decision::decode(decision_reader), ErrorCode::malformed_input);

  // A commitment resolution detail made of control characters.
  codec::Writer record;
  record.text(controls);
  codec::Reader record_reader(record.span());
  FAC_CHECK(!is_valid_reason(FAC_TAKE(record_reader.text(kMaxReasonLength))));

  // The escape is what makes such a detail safe to print.
  const std::string escaped = escape_text(controls);
  for (const char c : escaped) {
    const auto byte = static_cast<unsigned char>(c);
    FAC_CHECK(byte >= 0x20u && byte < 0x7Fu);
  }
  FAC_CHECK_EQ(escaped.find('\n'), std::string::npos);
}

// ---------------------------------------------------------------------------
// Durable store path handling
// ---------------------------------------------------------------------------

FAC_TEST(adversarial, durable_store_path_handling) {
  namespace durable = fac::durable;

  durable::StoreOptions create_options;
  create_options.create = true;

  durable::StoreOptions inspect_options;
  inspect_options.create = false;
  inspect_options.advance_epoch = false;

  // An empty directory string is not a directory.
  FAC_CHECK_ERR(durable::Store::open_writer("", create_options), ErrorCode::path_invalid);
  FAC_CHECK_ERR(durable::Store::open_writer("", inspect_options), ErrorCode::path_invalid);
  FAC_CHECK_ERR(durable::Store::open_reader(""), ErrorCode::path_invalid);

  // A directory string that is not valid UTF-8.
  const std::string invalid_utf8 = std::string("\xC3\x28", 2);
  FAC_CHECK_ERR(durable::Store::open_writer(invalid_utf8, create_options), ErrorCode::path_invalid);
  FAC_CHECK_ERR(durable::Store::open_reader(invalid_utf8), ErrorCode::path_invalid);

  // A path longer than the supported bound.
  const std::string too_long(kMaxPathLength + 1, 'p');
  FAC_CHECK_ERR(durable::Store::open_writer(too_long, create_options), ErrorCode::path_too_long);
  FAC_CHECK_ERR(durable::Store::open_writer(too_long, inspect_options), ErrorCode::path_too_long);
  FAC_CHECK_ERR(durable::Store::open_reader(too_long), ErrorCode::path_too_long);

  // A store directory that does not exist, when nothing may be created.
  fac_test::TempDir temporary("adversarial-store");
  const std::string missing = temporary.child("never-created");
  FAC_CHECK_ERR(durable::Store::open_writer(missing, inspect_options), ErrorCode::file_not_found);
  FAC_CHECK_ERR(durable::Store::open_reader(missing), ErrorCode::file_not_found);

  // A path that is a file rather than a directory.
  const std::string file_path = temporary.child("not-a-directory");
  {
    std::ofstream out(file_path, std::ios::binary);
    out << "this is a file, not a store directory";
  }
  FAC_CHECK_ERR(durable::Store::open_writer(file_path, create_options), ErrorCode::path_invalid);

  // A directory below a file cannot be created. The refusal is an I/O failure
  // rather than a path-shape refusal, because the shape itself is legal: the
  // parent simply is not a directory.
  const std::string nested_file = temporary.child("not-a-directory") + "/child";
  FAC_CHECK_ERR(durable::Store::open_writer(nested_file, create_options), ErrorCode::io_error);
  FAC_CHECK_ERR(durable::Store::open_writer(nested_file, inspect_options), ErrorCode::file_not_found);
}
