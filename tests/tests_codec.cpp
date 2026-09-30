// Facility Admission Control - canonical codec tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The codec is the only way a value becomes bytes, and those bytes are what a
// digest binds and a durable store keeps. These tests pin the round trip of
// every primitive, the refusals that keep a non-canonical encoding from being
// read a second way, and the behaviour of the decoder on hostile bytes: every
// strict prefix and every single-bit mutation of a real decision.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/limits.hpp"
#include "fac/model/decision.hpp"
#include "fac/model/request.hpp"
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

bool same_bytes(std::span<const std::byte> left, std::span<const std::byte> right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

AdmissionRequest make_request() {
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
  request.expected_epoch = ControlEpoch(3);

  GenerationPin capacity_pin;
  capacity_pin.kind = EvidenceKind::capacity;
  capacity_pin.generation = 11;
  request.pins.push_back(capacity_pin);

  GenerationPin policy_pin;
  policy_pin.kind = EvidenceKind::policy;
  policy_pin.generation = 12;
  request.pins.push_back(policy_pin);
  FAC_CHECK_OK(request.validate());
  return request;
}

Decision make_allow_decision() {
  const AdmissionRequest request = make_request();

  Decision decision;
  decision.request_id = request.request_id;
  decision.request_digest = request.digest();
  decision.verdict = Verdict::allow;
  decision.control_epoch = ControlEpoch(3);
  decision.sequence = FAC_TAKE(LedgerSequence::from_value(7));
  decision.decided_at = fac_test::fixture_now();

  EvidenceRef reference;
  reference.kind = EvidenceKind::capacity;
  reference.generation = 11;
  reference.digest = Digest256::of(bytes_of("capacity-evidence"));
  reference.observed_at = fac_test::fixture_now();
  reference.accepted = true;
  reference.rejection = ErrorCode::ok;
  FAC_CHECK_OK(decision.evidence.add(reference));

  Grant grant;
  grant.grant_id = FAC_TAKE(GrantId::from_hex("aaaa1111-bbbb-2222-cccc-3333dddd4444"));
  grant.revision = FAC_TAKE(GrantRevision::from_value(1));
  grant.request_id = request.request_id;
  grant.request_digest = decision.request_digest;
  GrantBinding binding;
  binding.kind = EvidenceKind::capacity;
  binding.generation = 11;
  binding.evidence_digest = reference.digest;
  grant.bindings.push_back(binding);
  grant.control_epoch = decision.control_epoch;
  grant.sequence = decision.sequence;
  grant.issued_at = fac_test::fixture_now();
  grant.expires_at = fac_test::at_seconds(600);
  grant.scope = request.scope;
  grant.tenant = request.tenant;
  grant.demand = request.demand;
  grant.holds_capacity = true;
  grant.binding_digest = grant.compute_binding_digest();
  decision.grant = grant;

  decision.explanation.push_back("every required check passed");
  decision.explanation.push_back("capacity and policy evidence are current");
  decision.digest = decision.compute_digest();
  return decision;
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

FAC_TEST(codec, primitive_round_trip) {
  const RackId identity = FAC_TAKE(RackId::from_hex("33333333-3333-3333-3333-333333333331"));
  const Digest256 digest = Digest256::of(bytes_of("codec-primitive"));
  const LedgerSequence counter = FAC_TAKE(LedgerSequence::from_value(123456789));
  const std::string max_text(kMaxReasonLength, 'x');
  const std::vector<std::byte> blob = bytes_of("blob-bytes");

  codec::Writer writer;
  writer.u8(std::uint8_t{0x12});
  writer.u16(std::uint16_t{0x3456});
  writer.u32(std::uint32_t{0x789ABCDE});
  writer.u64(std::uint64_t{0x0123456789ABCDEF});
  writer.i64(std::int64_t{-42});
  writer.boolean(true);
  writer.boolean(false);
  writer.bytes(blob);
  writer.text("");
  writer.text(max_text);
  writer.digest(digest);
  writer.ident(identity);
  writer.counter(counter);
  writer.reserved(4);
  writer.raw(blob);
  FAC_CHECK_EQ(writer.size(), writer.data().size());

  codec::Reader reader(writer.span());
  FAC_CHECK_EQ(reader.offset(), std::size_t(0));
  FAC_CHECK_EQ(static_cast<unsigned>(FAC_TAKE(reader.u8())), 0x12u);
  FAC_CHECK_EQ(static_cast<unsigned>(FAC_TAKE(reader.u16())), 0x3456u);
  FAC_CHECK_EQ(FAC_TAKE(reader.u32()), std::uint32_t{0x789ABCDE});
  FAC_CHECK_EQ(FAC_TAKE(reader.u64()), std::uint64_t{0x0123456789ABCDEF});
  FAC_CHECK_EQ(FAC_TAKE(reader.i64()), std::int64_t{-42});
  FAC_CHECK_EQ(FAC_TAKE(reader.boolean()), true);
  FAC_CHECK_EQ(FAC_TAKE(reader.boolean()), false);
  FAC_CHECK(same_bytes(FAC_TAKE(reader.bytes(blob.size())), blob));
  FAC_CHECK_EQ(FAC_TAKE(reader.text(kMaxReasonLength)), std::string(""));
  FAC_CHECK_EQ(FAC_TAKE(reader.text(kMaxReasonLength)), max_text);
  FAC_CHECK_EQ(FAC_TAKE(reader.digest()), digest);
  FAC_CHECK_EQ(FAC_TAKE(reader.ident<RackIdTag>()), identity);
  FAC_CHECK_EQ(FAC_TAKE(reader.counter<LedgerSequenceTag>()), counter);
  FAC_CHECK_OK(reader.reserved(4));
  FAC_CHECK(same_bytes(FAC_TAKE(reader.raw(blob.size())), blob));
  FAC_CHECK(reader.empty());
  FAC_CHECK_EQ(reader.remaining(), std::size_t(0));
  FAC_CHECK_OK(reader.expect_end());
}

FAC_TEST(codec, integer_endianness_is_big_endian) {
  codec::Writer writer;
  writer.u16(std::uint16_t{0x0102});
  writer.u32(std::uint32_t{0x03040506});
  writer.u64(std::uint64_t{0x0708090A0B0C0D0E});
  writer.i64(std::int64_t{-1});
  const std::vector<std::byte>& data = writer.data();
  FAC_CHECK_EQ(data.size(), std::size_t(22));
  FAC_CHECK(data[0] == std::byte{0x01});
  FAC_CHECK(data[1] == std::byte{0x02});
  FAC_CHECK(data[2] == std::byte{0x03});
  FAC_CHECK(data[5] == std::byte{0x06});
  FAC_CHECK(data[6] == std::byte{0x07});
  FAC_CHECK(data[13] == std::byte{0x0E});
  for (std::size_t i = 14; i < 22; ++i) {
    FAC_CHECK(data[i] == std::byte{0xFF});
  }
}

FAC_TEST(codec, reserved_field_round_trip) {
  codec::Writer writer;
  writer.reserved(4);
  FAC_CHECK_EQ(writer.size(), std::size_t(4));
  for (const std::byte value : writer.data()) {
    FAC_CHECK(value == std::byte{0});
  }
  codec::Reader reader(writer.span());
  FAC_CHECK_OK(reader.reserved(4));
  FAC_CHECK(reader.empty());
  FAC_CHECK_OK(reader.expect_end());

  // A zero-length reserved field consumes nothing.
  codec::Reader empty(writer.span());
  FAC_CHECK_OK(empty.reserved(0));
  FAC_CHECK_EQ(empty.remaining(), std::size_t(4));
}

FAC_TEST(codec, text_length_bounds) {
  codec::Writer at_max;
  at_max.text(std::string(kMaxReasonLength, 'y'));
  codec::Reader at_max_reader(at_max.span());
  FAC_CHECK_EQ(FAC_TAKE(at_max_reader.text(kMaxReasonLength)).size(), kMaxReasonLength);
  FAC_CHECK_OK(at_max_reader.expect_end());

  codec::Writer empty;
  empty.text("");
  FAC_CHECK_EQ(empty.size(), std::size_t(4));
  codec::Reader empty_reader(empty.span());
  FAC_CHECK_EQ(FAC_TAKE(empty_reader.text(kMaxReasonLength)), std::string(""));
  FAC_CHECK_OK(empty_reader.expect_end());

  // One byte past the configured maximum is refused before anything is read.
  codec::Writer over;
  over.u32(static_cast<std::uint32_t>(kMaxReasonLength + 1));
  codec::Reader over_reader(over.span());
  FAC_CHECK_ERR(over_reader.text(kMaxReasonLength), ErrorCode::limit_exceeded);

  // A name-sized field refuses a name-sized overrun.
  codec::Writer name;
  name.text(std::string(kMaxNameLength + 1, 'n'));
  codec::Reader name_reader(name.span());
  FAC_CHECK_ERR(name_reader.text(kMaxNameLength), ErrorCode::limit_exceeded);
}

// ---------------------------------------------------------------------------
// Canonical refusals
// ---------------------------------------------------------------------------

FAC_TEST(codec, boolean_field_is_zero_or_one) {
  for (const unsigned value : {2u, 3u, 127u, 255u}) {
    codec::Writer writer;
    writer.u8(static_cast<std::uint8_t>(value));
    codec::Reader reader(writer.span());
    FAC_CHECK_ERR(reader.boolean(), ErrorCode::malformed_input);
  }

  codec::Writer present;
  present.u8(std::uint8_t{0});
  codec::Reader present_reader(present.span());
  FAC_CHECK_EQ(FAC_TAKE(present_reader.boolean()), false);

  codec::Writer at_end;
  codec::Reader at_end_reader(at_end.span());
  FAC_CHECK_ERR(at_end_reader.boolean(), ErrorCode::truncated_input);
}

FAC_TEST(codec, reserved_field_must_be_present_and_zero) {
  codec::Writer nonzero;
  nonzero.u8(std::uint8_t{0});
  nonzero.u8(std::uint8_t{1});
  codec::Reader nonzero_reader(nonzero.span());
  FAC_CHECK_ERR(nonzero_reader.reserved(2), ErrorCode::reserved_field_nonzero);

  codec::Writer high;
  high.u8(std::uint8_t{0xFF});
  codec::Reader high_reader(high.span());
  FAC_CHECK_ERR(high_reader.reserved(1), ErrorCode::reserved_field_nonzero);

  codec::Writer truncated;
  truncated.u8(std::uint8_t{0});
  codec::Reader truncated_reader(truncated.span());
  FAC_CHECK_ERR(truncated_reader.reserved(2), ErrorCode::truncated_input);
}

FAC_TEST(codec, declared_length_beyond_remaining_input) {
  // 100 bytes declared, 3 present.
  codec::Writer writer;
  writer.u32(std::uint32_t{100});
  writer.text("abc");
  codec::Reader reader(writer.span());
  FAC_CHECK_ERR(reader.bytes(4096), ErrorCode::truncated_input);

  codec::Reader text_reader(writer.span());
  FAC_CHECK_ERR(text_reader.text(4096), ErrorCode::truncated_input);

  // A length just past the end, and a length of 1 with nothing left.
  codec::Writer edge;
  edge.u32(std::uint32_t{4});
  edge.raw(bytes_of("abc"));
  codec::Reader edge_reader(edge.span());
  FAC_CHECK_ERR(edge_reader.bytes(64), ErrorCode::truncated_input);

  codec::Writer none;
  none.u32(std::uint32_t{1});
  codec::Reader none_reader(none.span());
  FAC_CHECK_ERR(none_reader.bytes(64), ErrorCode::truncated_input);

  // raw() has no length prefix of its own and must still refuse a count past
  // the end.
  const std::vector<std::byte> three = bytes_of("abc");
  codec::Reader raw_reader(three);
  FAC_CHECK_ERR(raw_reader.raw(4), ErrorCode::truncated_input);
  FAC_CHECK(same_bytes(FAC_TAKE(raw_reader.raw(3)), three));
  FAC_CHECK_ERR(raw_reader.raw(1), ErrorCode::truncated_input);
}

FAC_TEST(codec, trailing_bytes_after_expect_end) {
  codec::Writer writer;
  writer.u8(std::uint8_t{1});
  writer.u8(std::uint8_t{2});
  codec::Reader reader(writer.span());
  FAC_CHECK_EQ(static_cast<unsigned>(FAC_TAKE(reader.u8())), 1u);
  FAC_CHECK_ERR(reader.expect_end(), ErrorCode::trailing_bytes);
  FAC_CHECK_EQ(static_cast<unsigned>(FAC_TAKE(reader.u8())), 2u);
  FAC_CHECK_OK(reader.expect_end());

  codec::Writer empty;
  codec::Reader empty_reader(empty.span());
  FAC_CHECK_OK(empty_reader.expect_end());
}

FAC_TEST(codec, sequence_count_bounds) {
  // More elements than the caller allows.
  codec::Writer too_many;
  too_many.u32(std::uint32_t{100});
  codec::Reader too_many_reader(too_many.span());
  FAC_CHECK_ERR(too_many_reader.sequence_count(4, 6), ErrorCode::limit_exceeded);

  // Within the element bound but provably unable to fit: 4 elements of at least
  // 6 bytes cannot follow a header with no payload.
  codec::Writer cannot_fit;
  cannot_fit.u32(std::uint32_t{4});
  codec::Reader cannot_fit_reader(cannot_fit.span());
  FAC_CHECK_ERR(cannot_fit_reader.sequence_count(4, 6), ErrorCode::truncated_input);

  // A single remaining byte cannot carry two one-byte elements.
  codec::Writer tight;
  tight.u32(std::uint32_t{2});
  tight.u8(std::uint8_t{0});
  codec::Reader tight_reader(tight.span());
  FAC_CHECK_ERR(tight_reader.sequence_count(4, 1), ErrorCode::truncated_input);

  // Exactly enough room is accepted.
  codec::Writer exact;
  exact.u32(std::uint32_t{2});
  for (int i = 0; i < 12; ++i) {
    exact.u8(std::uint8_t{0});
  }
  codec::Reader exact_reader(exact.span());
  FAC_CHECK_EQ(FAC_TAKE(exact_reader.sequence_count(4, 6)), std::uint32_t{2});
  FAC_CHECK_EQ(exact_reader.remaining(), std::size_t(12));

  // Zero elements is always affordable.
  codec::Writer zero;
  zero.u32(std::uint32_t{0});
  codec::Reader zero_reader(zero.span());
  FAC_CHECK_EQ(FAC_TAKE(zero_reader.sequence_count(0, 6)), std::uint32_t{0});

  codec::Writer empty;
  codec::Reader empty_reader(empty.span());
  FAC_CHECK_ERR(empty_reader.sequence_count(4, 1), ErrorCode::truncated_input);
}

FAC_TEST(codec, digest_field_refusals) {
  codec::Writer zero;
  zero.reserved(Digest256::kBytes);
  codec::Reader zero_reader(zero.span());
  FAC_CHECK_ERR(zero_reader.digest(), ErrorCode::malformed_input);

  codec::Writer truncated;
  truncated.reserved(Digest256::kBytes - 1);
  codec::Reader truncated_reader(truncated.span());
  FAC_CHECK_ERR(truncated_reader.digest(), ErrorCode::truncated_input);

  codec::Writer last_bit;
  last_bit.reserved(Digest256::kBytes - 1);
  last_bit.u8(std::uint8_t{0x01});
  codec::Reader last_bit_reader(last_bit.span());
  const Digest256 parsed = FAC_TAKE(last_bit_reader.digest());
  FAC_CHECK(!parsed.is_unset());
  FAC_CHECK_EQ(parsed.to_hex(), std::string(63, '0') + "1");
}

FAC_TEST(codec, identity_and_counter_field_refusals) {
  codec::Writer zero_ident;
  zero_ident.u64(std::uint64_t{0});
  zero_ident.u64(std::uint64_t{0});
  codec::Reader zero_ident_reader(zero_ident.span());
  FAC_CHECK_ERR(zero_ident_reader.ident<FacilityIdTag>(), ErrorCode::invalid_argument);

  codec::Writer half_ident;
  half_ident.u64(std::uint64_t{1});
  codec::Reader half_ident_reader(half_ident.span());
  FAC_CHECK_ERR(half_ident_reader.ident<FacilityIdTag>(), ErrorCode::truncated_input);

  codec::Writer over_counter;
  over_counter.u64(std::numeric_limits<std::uint64_t>::max());
  codec::Reader over_counter_reader(over_counter.span());
  FAC_CHECK_ERR(over_counter_reader.counter<LedgerSequenceTag>(), ErrorCode::out_of_range);

  codec::Writer max_counter;
  max_counter.u64(kMaxCounter);
  codec::Reader max_counter_reader(max_counter.span());
  FAC_CHECK_EQ(FAC_TAKE(max_counter_reader.counter<LedgerSequenceTag>()).value(), kMaxCounter);
}

FAC_TEST(codec, absent_optional_carrying_a_value_is_refused) {
  // An absent dimension is a false presence byte followed by zero. Any other
  // value is a second encoding of "absent" and is refused.
  codec::Writer writer;
  for (int index = 0; index < 4; ++index) {
    writer.boolean(false);
    writer.u64(index == 0 ? std::uint64_t{7} : std::uint64_t{0});
  }
  codec::Reader reader(writer.span());
  FAC_CHECK_ERR(AmountVector::decode(reader), ErrorCode::malformed_input);

  // The same rule for an absent rack inside a scope.
  codec::Writer scope_writer;
  scope_writer.ident(fac_test::facility_a());
  scope_writer.boolean(false);
  scope_writer.u64(std::uint64_t{1});
  scope_writer.u64(std::uint64_t{0});
  scope_writer.boolean(false);
  scope_writer.u64(std::uint64_t{0});
  scope_writer.u64(std::uint64_t{0});
  codec::Reader scope_reader(scope_writer.span());
  FAC_CHECK_ERR(TargetScope::decode(scope_reader), ErrorCode::malformed_input);

  // A well-formed encoding of the same value is still accepted, so the refusal
  // above is about the contradiction and not about the shape.
  codec::Writer good_writer;
  AmountVector unknown = AmountVector::unknown();
  unknown.encode(good_writer);
  codec::Reader good_reader(good_writer.span());
  FAC_CHECK_EQ(FAC_TAKE(AmountVector::decode(good_reader)), unknown);
}

// ---------------------------------------------------------------------------
// Hostile and mutated bytes
// ---------------------------------------------------------------------------

FAC_TEST(codec, truncation_sweep_over_a_composite_value) {
  const AdmissionRequest request = make_request();
  codec::Writer writer;
  request.encode(writer);
  const std::vector<std::byte> full = writer.data();
  FAC_CHECK(full.size() > 64);

  // Decoding the whole encoding is the baseline the sweep is measured against.
  codec::Reader whole(full);
  FAC_CHECK_EQ(FAC_TAKE(AdmissionRequest::decode(whole)), request);
  FAC_CHECK_OK(whole.expect_end());

  for (std::size_t length = 0; length < full.size(); ++length) {
    codec::Reader reader(std::span<const std::byte>(full.data(), length));
    const auto decoded = AdmissionRequest::decode(reader);
    if (decoded.ok()) {
      FAC_FAIL("a strict prefix of " + std::to_string(full.size()) + " bytes decoded successfully at " +
               std::to_string(length) + " bytes");
    }
    if (decoded.error().code != ErrorCode::truncated_input) {
      FAC_FAIL("prefix of " + std::to_string(length) + " bytes failed with " +
               to_string(decoded.error().code) + " instead of truncated_input");
    }
    // The decoder never consumes past the bytes it was given.
    FAC_CHECK(reader.offset() <= length);
  }
}

FAC_TEST(codec, byte_flip_sweep_over_a_decision) {
  const Decision decision = make_allow_decision();
  codec::Writer writer;
  decision.encode(writer);
  const std::vector<std::byte> canonical = writer.data();
  FAC_CHECK(canonical.size() > 200);

  std::size_t accepted = 0;
  std::size_t refused = 0;
  for (std::size_t index = 0; index < canonical.size(); ++index) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::vector<std::byte> mutated = canonical;
      const unsigned original = std::to_integer<unsigned>(mutated[index]);
      mutated[index] = static_cast<std::byte>(original ^ (1u << bit));
      FAC_CHECK(mutated[index] != canonical[index]);

      codec::Reader reader(std::span<const std::byte>(mutated.data(), mutated.size()));
      const auto decoded = Decision::decode(reader);
      if (!decoded.ok()) {
        ++refused;
        continue;
      }
      ++accepted;
      // Accepting the bytes means accepting them as a canonical encoding: the
      // value must re-encode to exactly the bytes that were read.
      codec::Writer again;
      decoded.value().encode(again);
      FAC_CHECK(again.data() == mutated);
      FAC_CHECK_OK(reader.expect_end());
    }
  }
  FAC_CHECK_EQ(accepted + refused, canonical.size() * 8);
  FAC_CHECK(refused > 0);
}

FAC_TEST(codec, decision_digest_field_is_bound_to_the_content) {
  const Decision decision = make_allow_decision();
  codec::Writer writer;
  decision.encode(writer);
  const std::vector<std::byte> canonical = writer.data();

  // The decision digest is the last field of the encoding.
  const std::size_t digest_offset = canonical.size() - Digest256::kBytes;
  for (std::size_t index = digest_offset; index < canonical.size(); ++index) {
    std::vector<std::byte> mutated = canonical;
    const unsigned original = std::to_integer<unsigned>(mutated[index]);
    mutated[index] = static_cast<std::byte>(original ^ 0x01u);

    // A zero digest is refused as an unset binding rather than compared.
    bool all_zero = true;
    for (std::size_t i = digest_offset; i < canonical.size(); ++i) {
      if (mutated[i] != std::byte{0}) {
        all_zero = false;
        break;
      }
    }
    codec::Reader reader(std::span<const std::byte>(mutated.data(), mutated.size()));
    if (all_zero) {
      FAC_CHECK_ERR(Decision::decode(reader), ErrorCode::malformed_input);
    } else {
      FAC_CHECK_ERR(Decision::decode(reader), ErrorCode::digest_mismatch);
    }
  }

  // Unchanged bytes still decode, so the sweep is not passing by refusing
  // everything.
  codec::Reader untouched(canonical);
  FAC_CHECK_EQ(FAC_TAKE(Decision::decode(untouched)), decision);
}
