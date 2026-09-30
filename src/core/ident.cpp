// Facility Admission Control - identity parsing and rendering.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/core/ident.hpp"

#include <cstdint>
#include <string>

namespace fac {
namespace {

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::size_t kHexDigitsCount = 32;

void append_hex16(std::string& out, std::uint64_t value) {
  char buffer[16];
  for (std::size_t i = 0; i < 16; ++i) {
    buffer[15 - i] = kHexDigits[(value >> (i * 4)) & 0x0Full];
  }
  out.append(buffer, 16);
}

}  // namespace

Result<std::pair<std::uint64_t, std::uint64_t>> ident_parse_hex(std::string_view text) {
  // Accepts the canonical dashed form (8-4-4-4-12) and the undashed 32-character
  // form. Every other separator, length or character is refused.
  char digits[kHexDigitsCount];
  std::size_t count = 0;
  for (const char c : text) {
    if (c == '-') {
      continue;
    }
    if (count >= kHexDigitsCount) {
      return make_error(ErrorCode::malformed_input, "identity has more than 32 hexadecimal digits");
    }
    if (hex_value(c) < 0) {
      return make_error(ErrorCode::malformed_input, "identity contains a non-hexadecimal character");
    }
    digits[count] = c;
    ++count;
  }
  if (count != kHexDigitsCount) {
    return make_error(ErrorCode::malformed_input, "identity must contain exactly 32 hexadecimal digits");
  }

  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    hi = (hi << 4) | static_cast<std::uint64_t>(hex_value(digits[i]));
    lo = (lo << 4) | static_cast<std::uint64_t>(hex_value(digits[i + 16]));
  }
  if (hi == 0 && lo == 0) {
    return make_error(ErrorCode::invalid_argument, "a zero identity is not a valid identity");
  }
  return std::make_pair(hi, lo);
}

std::string ident_format_hex(std::uint64_t hi, std::uint64_t lo) {
  std::string out;
  out.reserve(36);
  append_hex16(out, hi);
  append_hex16(out, lo);
  // 8-4-4-4-12 grouping for readability; parsing ignores the separators.
  out.insert(20, 1, '-');
  out.insert(16, 1, '-');
  out.insert(12, 1, '-');
  out.insert(8, 1, '-');
  return out;
}

}  // namespace fac
