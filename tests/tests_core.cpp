// Facility Admission Control - core primitives tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Everything here is a known-answer or a boundary case: the primitives under
// test are the ones every digest, identity, timestamp and text check in the
// runtime is built from, so a defect in one of them is not a local problem.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "fac/core/checked.hpp"
#include "fac/core/crc32c.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/limits.hpp"
#include "fac/core/sha256.hpp"
#include "fac/core/text.hpp"
#include "fac/core/time.hpp"
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

// Builds a string from explicit byte values so an escape sequence can never be
// read as a longer hexadecimal escape than intended.
std::string raw_bytes(std::initializer_list<unsigned> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned value : values) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  return out;
}

// Renders a digest as lowercase hexadecimal.
//
// The public header declares fac::sha256_hex for exactly this purpose, but the
// repository contains no definition of it, so calling it fails to link:
//
//   error LNK2019: unresolved external symbol
//     "class std::basic_string<...> __cdecl fac::sha256_hex(class std::span<enum std::byte const,-1>)"
//
// The tests below therefore render the bytes themselves until the definition
// exists. Nothing here depends on how the rendering is spelled: the expected
// values are the published NIST digests.
std::string hex_of(std::span<const std::byte> bytes) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::byte value : bytes) {
    const auto byte = std::to_integer<std::uint8_t>(value);
    out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
    out.push_back(kDigits[byte & 0x0Fu]);
  }
  return out;
}

// The digest of a message, rendered as lowercase hexadecimal.
std::string digest_text(std::span<const std::byte> bytes) { return hex_of(Sha256::hash(bytes)); }

std::string upper_ascii(std::string text) {
  for (char& c : text) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

FAC_TEST(core, sha256_known_answers) {
  // Published NIST/FIPS-180-4 vectors, compared as exact hex text.
  const std::string empty_hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  const std::string abc_hex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  const std::string block56_hex = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
  const std::string block64_hex = "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb";
  const std::string block112_hex = "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1";
  const std::string million_hex = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

  // Empty input: the padding-only block.
  FAC_CHECK_EQ(digest_text(std::span<const std::byte>()), empty_hex);
  FAC_CHECK_EQ(digest_text(bytes_of("")), empty_hex);

  FAC_CHECK_EQ(digest_text(bytes_of("abc")), abc_hex);

  // 56 bytes: the last message that fits without an extra padding block.
  const std::string block56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  FAC_CHECK_EQ(block56.size(), std::size_t(56));
  FAC_CHECK_EQ(digest_text(bytes_of(block56)), block56_hex);

  // 64 bytes: exactly one full block, so the length lives in a second block.
  const std::string block64(64, 'a');
  FAC_CHECK_EQ(block64.size(), std::size_t(64));
  FAC_CHECK_EQ(digest_text(bytes_of(block64)), block64_hex);

  // 112 bytes: a multi-block message.
  const std::string block112 =
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  FAC_CHECK_EQ(block112.size(), std::size_t(112));
  FAC_CHECK_EQ(digest_text(bytes_of(block112)), block112_hex);

  // One million 'a' characters.
  const std::string million(1000000, 'a');
  FAC_CHECK_EQ(digest_text(bytes_of(million)), million_hex);

  // Sha256::hash and Digest256::of must agree bit for bit with the same input.
  const auto direct = Sha256::hash(bytes_of(block112));
  FAC_CHECK_EQ(hex_of(direct), block112_hex);
  FAC_CHECK_EQ(Digest256::of(bytes_of(block112)).to_hex(), block112_hex);
}

FAC_TEST(core, sha256_incremental_matches_one_shot) {
  const std::string million(1000000, 'a');
  const std::vector<std::byte> data = bytes_of(million);
  const std::string expected = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

  // Irregular chunk boundaries, including chunks that straddle the 64-byte
  // block size and the 56-byte padding boundary.
  Sha256 hasher;
  std::size_t offset = 0;
  std::size_t step = 1;
  while (offset < data.size()) {
    const std::size_t count = step < data.size() - offset ? step : data.size() - offset;
    hasher.update(std::span<const std::byte>(data.data() + offset, count));
    offset += count;
    step = (step % 127) + 1;
  }
  FAC_CHECK_EQ(hex_of(hasher.finish()), expected);

  // finish() resets the object, so a second run computes the digest of only the
  // bytes fed after the reset.
  Sha256 reused;
  reused.update(data);
  FAC_CHECK_EQ(hex_of(reused.finish()), expected);
  reused.update(bytes_of("abc"));
  FAC_CHECK_EQ(hex_of(reused.finish()),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------

FAC_TEST(core, crc32c_known_answers) {
  // The canonical Castagnoli check value.
  FAC_CHECK_EQ(crc32c(bytes_of("123456789")), 0xE3069283u);
  FAC_CHECK_EQ(crc32c(std::span<const std::byte>()), 0x00000000u);
  FAC_CHECK_EQ(crc32c(bytes_of("")), 0x00000000u);

  // A frame that is mostly zero bytes must not collapse to the empty checksum.
  const std::vector<std::byte> zeros(64, std::byte{0});
  FAC_CHECK_NE(crc32c(zeros), 0x00000000u);
}

FAC_TEST(core, crc32c_incremental_matches_one_shot) {
  const std::vector<std::byte> check = bytes_of("123456789");
  const std::uint32_t one_shot = crc32c(check);
  for (std::size_t split = 0; split <= check.size(); ++split) {
    Crc32c crc;
    crc.update(std::span<const std::byte>(check.data(), split));
    crc.update(std::span<const std::byte>(check.data() + split, check.size() - split));
    FAC_CHECK_EQ(crc.finish(), one_shot);
  }

  // A longer body split at several boundaries, including block edges and empty
  // halves on both sides.
  std::vector<std::byte> body;
  body.reserve(1000);
  for (std::size_t i = 0; i < 1000; ++i) {
    body.push_back(static_cast<std::byte>(i % 251));
  }
  const std::uint32_t body_one_shot = crc32c(body);
  const std::size_t boundaries[] = {0,  1,  7,   63,  64,   65,  127,
                                    128, 255, 256, 999, 1000};
  for (const std::size_t split : boundaries) {
    Crc32c crc;
    crc.update(std::span<const std::byte>(body.data(), split));
    crc.update(std::span<const std::byte>(body.data() + split, body.size() - split));
    FAC_CHECK_EQ(crc.finish(), body_one_shot);
  }

  // finish() is an accessor: reading it twice does not advance the state.
  Crc32c accessor;
  accessor.update(check);
  FAC_CHECK_EQ(accessor.finish(), accessor.finish());
}

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

FAC_TEST(core, checked_add_unsigned_limits) {
  using U = std::uint64_t;
  const U max = std::numeric_limits<U>::max();
  U out = 0;

  FAC_CHECK(!add_overflow<U>(0, 0, out));
  FAC_CHECK_EQ(out, U(0));
  FAC_CHECK(!add_overflow<U>(0, max, out));
  FAC_CHECK_EQ(out, max);
  FAC_CHECK(!add_overflow<U>(max, 0, out));
  FAC_CHECK_EQ(out, max);
  FAC_CHECK(!add_overflow<U>(max - 1, 1, out));
  FAC_CHECK_EQ(out, max);
  FAC_CHECK(!add_overflow<U>(1, max - 1, out));
  FAC_CHECK_EQ(out, max);

  // A refused operation must not leave a partial result behind.
  out = 12345;
  FAC_CHECK(add_overflow<U>(max, 1, out));
  FAC_CHECK_EQ(out, U(12345));
  FAC_CHECK(add_overflow<U>(max - 1, 2, out));
  FAC_CHECK_EQ(out, U(12345));
  FAC_CHECK(add_overflow<U>(max / 2 + 1, max / 2 + 1, out));
  FAC_CHECK_EQ(out, U(12345));
}

FAC_TEST(core, checked_add_signed_limits) {
  using S = std::int64_t;
  const S max = std::numeric_limits<S>::max();
  const S min = std::numeric_limits<S>::min();
  S out = 0;

  FAC_CHECK(!add_overflow<S>(0, 0, out));
  FAC_CHECK_EQ(out, S(0));
  FAC_CHECK(!add_overflow<S>(max, 0, out));
  FAC_CHECK_EQ(out, max);
  FAC_CHECK(!add_overflow<S>(min, 0, out));
  FAC_CHECK_EQ(out, min);
  FAC_CHECK(!add_overflow<S>(max - 1, 1, out));
  FAC_CHECK_EQ(out, max);
  FAC_CHECK(!add_overflow<S>(min + 1, -1, out));
  FAC_CHECK_EQ(out, min);
  FAC_CHECK(!add_overflow<S>(max, -1, out));
  FAC_CHECK_EQ(out, max - 1);

  out = -7;
  FAC_CHECK(add_overflow<S>(max, 1, out));
  FAC_CHECK_EQ(out, S(-7));
  FAC_CHECK(add_overflow<S>(min, -1, out));
  FAC_CHECK_EQ(out, S(-7));
  FAC_CHECK(add_overflow<S>(max, max, out));
  FAC_CHECK_EQ(out, S(-7));
}

FAC_TEST(core, checked_sub_underflow_limits) {
  using U = std::uint64_t;
  using S = std::int64_t;
  const U umax = std::numeric_limits<U>::max();
  const S smax = std::numeric_limits<S>::max();
  const S smin = std::numeric_limits<S>::min();

  U uout = 99;
  FAC_CHECK(!sub_underflow<U>(0, 0, uout));
  FAC_CHECK_EQ(uout, U(0));
  FAC_CHECK(!sub_underflow<U>(umax, umax, uout));
  FAC_CHECK_EQ(uout, U(0));
  FAC_CHECK(!sub_underflow<U>(umax, umax - 1, uout));
  FAC_CHECK_EQ(uout, U(1));
  FAC_CHECK(sub_underflow<U>(0, 1, uout));
  FAC_CHECK_EQ(uout, U(1));
  FAC_CHECK(sub_underflow<U>(5, 6, uout));
  FAC_CHECK_EQ(uout, U(1));

  S sout = -99;
  FAC_CHECK(!sub_underflow<S>(smin, 0, sout));
  FAC_CHECK_EQ(sout, smin);
  FAC_CHECK(!sub_underflow<S>(smin, -1, sout));
  FAC_CHECK_EQ(sout, smin + 1);
  FAC_CHECK(!sub_underflow<S>(smax, 1, sout));
  FAC_CHECK_EQ(sout, smax - 1);

  // A refused operation must not leave a partial result behind.
  sout = -99;
  FAC_CHECK(sub_underflow<S>(smin, 1, sout));
  FAC_CHECK_EQ(sout, S(-99));
  FAC_CHECK(sub_underflow<S>(smax, -1, sout));
  FAC_CHECK_EQ(sout, S(-99));
}

FAC_TEST(core, checked_mul_overflow_limits) {
  using U = std::uint64_t;
  using S = std::int64_t;
  const U umax = std::numeric_limits<U>::max();
  const S smax = std::numeric_limits<S>::max();
  const S smin = std::numeric_limits<S>::min();

  U uout = 7;
  FAC_CHECK(!mul_overflow<U>(0, umax, uout));
  FAC_CHECK_EQ(uout, U(0));
  FAC_CHECK(!mul_overflow<U>(umax, 0, uout));
  FAC_CHECK_EQ(uout, U(0));
  FAC_CHECK(!mul_overflow<U>(1, umax, uout));
  FAC_CHECK_EQ(uout, umax);
  FAC_CHECK(!mul_overflow<U>(umax / 2, 2, uout));
  FAC_CHECK_EQ(uout, umax - 1);

  // A refused operation must not leave a partial result behind.
  uout = 7;
  FAC_CHECK(mul_overflow<U>(umax, 2, uout));
  FAC_CHECK_EQ(uout, U(7));
  FAC_CHECK(mul_overflow<U>(umax / 2 + 1, 2, uout));
  FAC_CHECK_EQ(uout, U(7));

  S sout = 7;
  FAC_CHECK(!mul_overflow<S>(smin, 1, sout));
  FAC_CHECK_EQ(sout, smin);
  FAC_CHECK(!mul_overflow<S>(smax, 1, sout));
  FAC_CHECK_EQ(sout, smax);
  FAC_CHECK(!mul_overflow<S>(-1, smax, sout));
  FAC_CHECK_EQ(sout, -smax);
  FAC_CHECK(!mul_overflow<S>(-2, -3, sout));
  FAC_CHECK_EQ(sout, S(6));
  FAC_CHECK(!mul_overflow<S>(3, -2, sout));
  FAC_CHECK_EQ(sout, S(-6));
  sout = 7;
  FAC_CHECK(mul_overflow<S>(smin, -1, sout));
  FAC_CHECK_EQ(sout, S(7));
  FAC_CHECK(mul_overflow<S>(smax, 2, sout));
  FAC_CHECK_EQ(sout, S(7));
  FAC_CHECK(mul_overflow<S>(smin, 2, sout));
  FAC_CHECK_EQ(sout, S(7));
  FAC_CHECK(mul_overflow<S>(smax, -2, sout));
  FAC_CHECK_EQ(sout, S(7));
}

FAC_TEST(core, narrow_refuses_values_that_do_not_fit) {
  std::uint8_t u8 = 0;
  std::int8_t i8 = 0;
  std::uint16_t u16 = 0;
  std::uint64_t u64 = 0;

  FAC_CHECK(narrow<std::uint8_t>(0, u8));
  FAC_CHECK_EQ(u8, std::uint8_t(0));
  FAC_CHECK(narrow<std::uint8_t>(255, u8));
  FAC_CHECK_EQ(u8, std::uint8_t(255));
  u8 = 42;
  FAC_CHECK(!narrow<std::uint8_t>(256, u8));
  FAC_CHECK_EQ(u8, std::uint8_t(42));
  FAC_CHECK(!narrow<std::uint8_t>(-1, u8));
  FAC_CHECK_EQ(u8, std::uint8_t(42));
  FAC_CHECK(!narrow<std::uint8_t>(65535, u8));

  FAC_CHECK(narrow<std::int8_t>(-128, i8));
  FAC_CHECK_EQ(i8, std::int8_t(-128));
  FAC_CHECK(narrow<std::int8_t>(127, i8));
  FAC_CHECK_EQ(i8, std::int8_t(127));
  FAC_CHECK(!narrow<std::int8_t>(128, i8));
  FAC_CHECK(!narrow<std::int8_t>(-129, i8));

  FAC_CHECK(narrow<std::uint16_t>(65535, u16));
  FAC_CHECK_EQ(u16, std::uint16_t(65535));
  FAC_CHECK(!narrow<std::uint16_t>(65536, u16));

  FAC_CHECK(narrow<std::uint64_t>(std::numeric_limits<std::uint64_t>::max(), u64));
  FAC_CHECK_EQ(u64, std::numeric_limits<std::uint64_t>::max());
}

// The header states that narrow() "reports a value that does not fit" and is
// "never truncated". A non-negative signed value always fits an unsigned target
// that is at least as wide, so this must succeed. It does not.
//
// The implementation compares against
//   static_cast<std::intmax_t>(std::numeric_limits<std::uint64_t>::max())
// which is implementation-defined and, on this toolchain, -1. Every signed
// source is therefore refused for a std::uint64_t target, including zero:
//
//   std::uint64_t out = 0;
//   fac::narrow<std::uint64_t>(std::int64_t{0}, out)   // returns false
//
// This test is deliberately left failing: the defect is in the library, not in
// the expectation, and weakening it would hide a narrowing helper that refuses
// values which fit.
FAC_TEST(core, narrow_accepts_a_signed_value_that_fits_an_unsigned_target) {
  std::uint64_t wide = 0;
  FAC_CHECK(narrow<std::uint64_t>(std::int64_t{0}, wide));
  FAC_CHECK_EQ(wide, std::uint64_t{0});
  FAC_CHECK(narrow<std::uint64_t>(std::int64_t{42}, wide));
  FAC_CHECK_EQ(wide, std::uint64_t{42});
  FAC_CHECK(!narrow<std::uint64_t>(std::int64_t{-1}, wide));

  // The same shape with a narrower unsigned target behaves correctly, which is
  // what makes the 64-bit case a defect rather than a design choice.
  std::uint32_t narrow_target = 0;
  FAC_CHECK(narrow<std::uint32_t>(std::int64_t{7}, narrow_target));
  FAC_CHECK_EQ(narrow_target, std::uint32_t{7});
}

FAC_TEST(core, checked_u64_helpers) {
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();

  const std::optional<std::uint64_t> sum = checked_add_u64(0, 0);
  FAC_CHECK(sum.has_value());
  FAC_CHECK_EQ(sum.value(), std::uint64_t(0));
  FAC_CHECK_EQ(checked_add_u64(max - 1, 1).value(), max);
  FAC_CHECK_EQ(checked_add_u64(max, 0).value(), max);
  FAC_CHECK(!checked_add_u64(max, 1).has_value());
  FAC_CHECK(!checked_add_u64(max, max).has_value());

  FAC_CHECK_EQ(checked_sub_u64(0, 0).value(), std::uint64_t(0));
  FAC_CHECK_EQ(checked_sub_u64(5, 3).value(), std::uint64_t(2));
  FAC_CHECK_EQ(checked_sub_u64(max, max).value(), std::uint64_t(0));
  FAC_CHECK(!checked_sub_u64(0, 1).has_value());
  FAC_CHECK(!checked_sub_u64(3, 5).has_value());
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

FAC_TEST(core, utf8_accepts_well_formed_text) {
  FAC_CHECK(is_valid_utf8(""));
  FAC_CHECK(is_valid_utf8("plain ascii"));
  FAC_CHECK(is_valid_utf8(raw_bytes({0xC3, 0xA9})));              // U+00E9
  FAC_CHECK(is_valid_utf8(raw_bytes({0xE2, 0x82, 0xAC})));        // U+20AC
  FAC_CHECK(is_valid_utf8(raw_bytes({0xF0, 0x9F, 0x98, 0x80})));  // U+1F600
  FAC_CHECK(is_valid_utf8(raw_bytes({0xED, 0x9F, 0xBF})));        // U+D7FF, below the surrogates
  FAC_CHECK(is_valid_utf8(raw_bytes({0xEE, 0x80, 0x80})));        // U+E000, above the surrogates
  FAC_CHECK(is_valid_utf8(raw_bytes({0xF4, 0x8F, 0xBF, 0xBF})));  // U+10FFFF, the last code point
  FAC_CHECK(is_valid_utf8(raw_bytes({0x7F})));                    // DEL is valid UTF-8, just not a reason
}

FAC_TEST(core, utf8_refuses_malformed_text) {
  // Overlong encodings: a shorter form of the same code point is not canonical
  // UTF-8 and is how a filter that scans bytes can be walked past. The two-byte
  // family has its own test below, because that is where the defect is.
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xE0, 0x80, 0x80})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xE0, 0x9F, 0xBF})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xF0, 0x80, 0x80, 0x80})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xF0, 0x8F, 0xBF, 0xBF})));

  // Truncated sequences.
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xC3})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xE2, 0x82})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xF0, 0x9F, 0x98})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0x80})));       // a bare continuation byte
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xE2, 0x41}))); // a continuation byte that is not one
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xFE, 0xFF})));

  // Surrogates are not Unicode scalar values and must never be encoded.
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xED, 0xA0, 0x80})));  // U+D800
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xED, 0xBF, 0xBF})));  // U+DFFF

  // Beyond U+10FFFF.
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xF4, 0x90, 0x80, 0x80})));  // U+110000
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xF5, 0x80, 0x80, 0x80})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xF7, 0xBF, 0xBF, 0xBF})));
}

// Every two-byte sequence whose code point is below U+0080 is an overlong
// encoding and is not UTF-8. The implementation rejects a two-byte lead whose
// extracted high bits are zero, which catches only a 0xC0 lead byte, so the
// entire 0xC1 range is accepted:
//
//   fac::is_valid_utf8("\xC1\xBF")   // true
//
// but 0xC1 0xBF encodes U+007F in two bytes instead of one.
//
// Left failing deliberately: an overlong encoding is the classic way past a
// byte-oriented filter, and this runtime refuses it in every other position.
FAC_TEST(core, utf8_refuses_overlong_two_byte_sequences) {
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xC0, 0x80})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xC0, 0xBF})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xC1, 0x80})));
  FAC_CHECK(!is_valid_utf8(raw_bytes({0xC1, 0xBF})));
}

FAC_TEST(core, identifier_bounds_and_alphabet) {
  FAC_CHECK(is_valid_identifier("a"));
  FAC_CHECK(is_valid_identifier("Rack-01.prod_2"));
  FAC_CHECK(is_valid_identifier("9lives"));
  FAC_CHECK(is_valid_identifier(std::string(kMaxNameLength, 'a')));
  FAC_CHECK(!is_valid_identifier(std::string(kMaxNameLength + 1, 'a')));
  FAC_CHECK(!is_valid_identifier(""));
  FAC_CHECK(!is_valid_identifier("-leading"));
  FAC_CHECK(!is_valid_identifier(".leading"));
  FAC_CHECK(!is_valid_identifier("_leading"));
  FAC_CHECK(!is_valid_identifier("has space"));
  FAC_CHECK(!is_valid_identifier("has/slash"));
  FAC_CHECK(!is_valid_identifier("has:colon"));
  FAC_CHECK(!is_valid_identifier(raw_bytes({'a', 0x00, 'b'})));
  FAC_CHECK(!is_valid_identifier(raw_bytes({0xC3, 0xA9})));
}

FAC_TEST(core, reason_bounds_and_control_characters) {
  FAC_CHECK(is_valid_reason("ok"));
  FAC_CHECK(is_valid_reason(std::string(kMaxReasonLength, 'r')));
  FAC_CHECK(!is_valid_reason(std::string(kMaxReasonLength + 1, 'r')));
  FAC_CHECK(!is_valid_reason(""));
  FAC_CHECK(!is_valid_reason("two\nlines"));
  FAC_CHECK(!is_valid_reason("carriage\rreturn"));
  FAC_CHECK(!is_valid_reason("tab\tseparated"));
  FAC_CHECK(!is_valid_reason(raw_bytes({0x7F})));
  FAC_CHECK(!is_valid_reason(raw_bytes({'a', 0x00, 'b'})));
  FAC_CHECK(!is_valid_reason(raw_bytes({0xC3, 0x28})));
  FAC_CHECK(is_valid_reason(raw_bytes({0xE2, 0x82, 0xAC})));
  FAC_CHECK(contains_control_characters(raw_bytes({'a', 0x1F})));
  FAC_CHECK(!contains_control_characters("printable text"));
}

FAC_TEST(core, escape_text_makes_control_bytes_printable) {
  const std::string hostile = raw_bytes({0x01, 0x1F, 0x7F, 0x80, 0xFF, 'o', 'k'});
  const std::string escaped = escape_text(hostile);
  FAC_CHECK_EQ(escaped, std::string("\\x01\\x1f\\x7f\\x80\\xffok"));

  // The result never contains a raw control byte, which is what makes it safe
  // to place directly in a log line.
  for (const char c : escaped) {
    const auto byte = static_cast<unsigned char>(c);
    FAC_CHECK(byte >= 0x20u && byte < 0x7Fu);
  }

  FAC_CHECK_EQ(escape_text("plain"), std::string("plain"));
  FAC_CHECK_EQ(escape_text(""), std::string(""));
  FAC_CHECK_EQ(escape_text(raw_bytes({0x00})), std::string("\\x00"));
}

FAC_TEST(core, ascii_case_helpers) {
  FAC_CHECK_EQ(to_lower_ascii("MiXeD-01"), std::string("mixed-01"));
  FAC_CHECK_EQ(to_lower_ascii(raw_bytes({0xC3, 0x89})), raw_bytes({0xC3, 0x89}));
  FAC_CHECK(iequals_ascii("Permit", "permit"));
  FAC_CHECK(iequals_ascii("", ""));
  FAC_CHECK(!iequals_ascii("permit", "permitted"));
  FAC_CHECK(!iequals_ascii("permit", "deny"));
}

// ---------------------------------------------------------------------------
// Identities and digests
// ---------------------------------------------------------------------------

FAC_TEST(core, ident_parse_format_round_trip) {
  const std::string dashed = "11111111-1111-1111-1111-111111111111";
  const std::string undashed = "11111111111111111111111111111111";

  const FacilityId from_dashed = FAC_TAKE(FacilityId::from_hex(dashed));
  const FacilityId from_undashed = FAC_TAKE(FacilityId::from_hex(undashed));
  FAC_CHECK_EQ(from_dashed, from_undashed);
  FAC_CHECK_EQ(from_dashed.hi(), 0x1111111111111111ull);
  FAC_CHECK_EQ(from_dashed.lo(), 0x1111111111111111ull);
  FAC_CHECK_EQ(from_dashed.to_string(), dashed);
  FAC_CHECK_EQ(from_undashed.to_string(), dashed);
  FAC_CHECK(!from_dashed.is_unset());

  // Uppercase input parses and renders back in the canonical lowercase form.
  const std::string upper = "ABCDEF01-2345-6789-ABCD-EF0123456780";
  const RackId from_upper = FAC_TAKE(RackId::from_hex(upper));
  FAC_CHECK_EQ(from_upper.to_string(), std::string("abcdef01-2345-6789-abcd-ef0123456780"));
  FAC_CHECK_EQ(FAC_TAKE(RackId::from_hex("abcdef0123456789abcdef0123456780")), from_upper);
  // The two halves are independent: a differing low half is a different value.
  FAC_CHECK_NE(from_upper.hi(), from_upper.lo());
  FAC_CHECK_EQ(from_upper.hi(), 0xabcdef0123456789ull);
  FAC_CHECK_EQ(from_upper.lo(), 0xabcdef0123456780ull);
}

FAC_TEST(core, ident_refusals) {
  FAC_CHECK_ERR(FacilityId::from_hex(""), ErrorCode::malformed_input);
  FAC_CHECK_ERR(FacilityId::from_hex("1111"), ErrorCode::malformed_input);
  FAC_CHECK_ERR(FacilityId::from_hex("1111111111111111111111111111111"), ErrorCode::malformed_input);
  FAC_CHECK_ERR(FacilityId::from_hex("111111111111111111111111111111111"), ErrorCode::malformed_input);
  FAC_CHECK_ERR(FacilityId::from_hex("g1111111-1111-1111-1111-111111111111"), ErrorCode::malformed_input);
  FAC_CHECK_ERR(FacilityId::from_hex("11111111-1111-1111-1111-11111111111z"), ErrorCode::malformed_input);
  FAC_CHECK_ERR(FacilityId::from_hex("00000000-0000-0000-0000-000000000000"),
                ErrorCode::invalid_argument);
  FAC_CHECK_ERR(FacilityId::from_hex("00000000000000000000000000000000"),
                ErrorCode::invalid_argument);
  FAC_CHECK_ERR(TenantId::from_hex("00000000000000000000000000000000"),
                ErrorCode::invalid_argument);
}

FAC_TEST(core, counter_bounds) {
  FAC_CHECK(Counter<struct ProbeTag>{}.is_zero());
  FAC_CHECK_EQ(Counter<struct ProbeTag>(0).value(), std::uint64_t(0));

  const auto at_max = Counter<struct ProbeTag>::from_value(kMaxCounter);
  FAC_CHECK(at_max.ok());
  FAC_CHECK_EQ(at_max.value().value(), kMaxCounter);
  FAC_CHECK_ERR(Counter<struct ProbeTag>::from_value(kMaxCounter + 1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(Counter<struct ProbeTag>::from_value(std::numeric_limits<std::uint64_t>::max()),
                ErrorCode::out_of_range);

  FAC_CHECK_OK(Counter<struct ProbeTag>(kMaxCounter - 1).checked_next());
  FAC_CHECK_EQ(Counter<struct ProbeTag>(kMaxCounter - 1).checked_next().value().value(), kMaxCounter);
  FAC_CHECK_ERR(Counter<struct ProbeTag>(kMaxCounter).checked_next(), ErrorCode::overflow);

  FAC_CHECK_EQ(Counter<struct ProbeTag>(kMaxCounter).to_string(), std::to_string(kMaxCounter));
}

FAC_TEST(core, digest_hex_round_trip_and_refusals) {
  const std::vector<std::byte> data = bytes_of("facility-admission-control");
  const Digest256 digest = Digest256::of(data);
  FAC_CHECK(!digest.is_unset());

  const std::string hex = digest.to_hex();
  FAC_CHECK_EQ(hex.size(), Digest256::kHexLength);
  for (const char c : hex) {
    FAC_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
  FAC_CHECK_EQ(FAC_TAKE(Digest256::from_hex(hex)), digest);
  FAC_CHECK_EQ(FAC_TAKE(Digest256::from_hex(upper_ascii(hex))), digest);

  FAC_CHECK(Digest256{}.is_unset());
  FAC_CHECK_ERR(Digest256::from_hex(std::string(64, '0')), ErrorCode::malformed_input);
  FAC_CHECK_ERR(Digest256::from_hex(std::string(63, 'a')), ErrorCode::malformed_input);
  FAC_CHECK_ERR(Digest256::from_hex(std::string(65, 'a')), ErrorCode::malformed_input);
  FAC_CHECK_ERR(Digest256::from_hex(std::string(64, 'z')), ErrorCode::malformed_input);
  FAC_CHECK_ERR(Digest256::from_hex(""), ErrorCode::malformed_input);

  // DigestBuilder over the same bytes must produce the same value.
  DigestBuilder builder;
  builder.update(std::span<const std::byte>(data.data(), 10));
  builder.update(std::span<const std::byte>(data.data() + 10, data.size() - 10));
  FAC_CHECK_EQ(builder.finish(), digest);
  FAC_CHECK_EQ(digest_text(data), hex);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

FAC_TEST(core, timestamp_bounds) {
  FAC_CHECK(is_valid_timestamp_nanos(0));
  FAC_CHECK(is_valid_timestamp_nanos(1));
  FAC_CHECK(is_valid_timestamp_nanos(kMaxTimestampNanos));
  FAC_CHECK(!is_valid_timestamp_nanos(-1));
  FAC_CHECK(!is_valid_timestamp_nanos(std::numeric_limits<Nanos>::min()));
  FAC_CHECK(!is_valid_timestamp_nanos(kMaxTimestampNanos + 1));
  FAC_CHECK(!is_valid_timestamp_nanos(std::numeric_limits<Nanos>::max()));

  FAC_CHECK_OK(Timestamp::from_unix_nanos(0));
  FAC_CHECK(Timestamp(0).is_unset());
  FAC_CHECK(!Timestamp(1).is_unset());
  FAC_CHECK_OK(Timestamp::from_unix_nanos(kMaxTimestampNanos));
  FAC_CHECK_ERR(Timestamp::from_unix_nanos(-1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(Timestamp::from_unix_nanos(kMaxTimestampNanos + 1), ErrorCode::out_of_range);

  FAC_CHECK_OK(Duration::from_nanos_checked(0));
  FAC_CHECK_OK(Duration::from_nanos_checked(kMaxTimestampNanos));
  FAC_CHECK_ERR(Duration::from_nanos_checked(-1), ErrorCode::out_of_range);
  FAC_CHECK_ERR(Duration::from_nanos_checked(kMaxTimestampNanos + 1), ErrorCode::out_of_range);
  FAC_CHECK(Duration().is_zero());
}

FAC_TEST(core, age_of_refuses_a_future_timestamp) {
  const Duration age = FAC_TAKE(age_of(Timestamp(1000), Timestamp(4000)));
  FAC_CHECK_EQ(age.nanos(), Nanos(3000));
  FAC_CHECK_EQ(FAC_TAKE(age_of(Timestamp(5000), Timestamp(5000))).nanos(), Nanos(0));
  FAC_CHECK_ERR(age_of(Timestamp(6000), Timestamp(5000)), ErrorCode::underflow);
  FAC_CHECK_ERR(age_of(Timestamp(1), Timestamp(0)), ErrorCode::underflow);
  FAC_CHECK_EQ(age.to_string(), std::string("3000ns"));
}

FAC_TEST(core, checked_add_overflow_on_time) {
  FAC_CHECK_EQ(FAC_TAKE(Timestamp(1000).checked_add(Duration(500))), Timestamp(1500));
  FAC_CHECK_EQ(FAC_TAKE(Timestamp(0).checked_add(Duration(0))), Timestamp(0));
  FAC_CHECK_ERR(Timestamp(kMaxTimestampNanos).checked_add(Duration(1)), ErrorCode::overflow);
  FAC_CHECK_ERR(Timestamp(kMaxTimestampNanos - 1).checked_add(Duration(2)), ErrorCode::overflow);
  FAC_CHECK_ERR(Timestamp(1000).checked_add(Duration(-1)), ErrorCode::underflow);
  FAC_CHECK_OK(Timestamp(kMaxTimestampNanos - 1).checked_add(Duration(1)));

  FAC_CHECK_EQ(FAC_TAKE(Duration(2).checked_add(Duration(3))), Duration(5));
  FAC_CHECK_EQ(FAC_TAKE(Duration(0).checked_add(Duration(0))), Duration(0));
  FAC_CHECK_ERR(Duration(kMaxTimestampNanos).checked_add(Duration(1)), ErrorCode::overflow);
  FAC_CHECK_ERR(Duration(kMaxTimestampNanos - 1).checked_add(Duration(2)), ErrorCode::overflow);
}

FAC_TEST(core, timestamp_to_string_of_known_instants) {
  FAC_CHECK_EQ(Timestamp(0).to_string(), std::string("1970-01-01T00:00:00.000000000Z"));
  FAC_CHECK_EQ(Timestamp(1).to_string(), std::string("1970-01-01T00:00:00.000000001Z"));
  FAC_CHECK_EQ(Timestamp(1'500'000'001LL).to_string(), std::string("1970-01-01T00:00:01.500000001Z"));
  FAC_CHECK_EQ(Timestamp(1'700'000'000'000'000'000LL).to_string(),
               std::string("2023-11-14T22:13:20.000000000Z"));
  FAC_CHECK_EQ(Timestamp(kMaxTimestampNanos).to_string(),
               std::string("2261-07-15T11:33:20.000000000Z"));
  FAC_CHECK_EQ(Timestamp(86'400'000'000'000LL).to_string(), std::string("1970-01-02T00:00:00.000000000Z"));
}
