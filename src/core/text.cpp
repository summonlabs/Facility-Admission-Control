// Facility Admission Control - text validation and escaping.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/core/text.hpp"

#include <cstdint>

#include "fac/core/limits.hpp"

namespace fac {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] bool is_ascii_alphanumeric(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] bool is_identifier_tail(char c) noexcept {
  return is_ascii_alphanumeric(c) || c == '.' || c == '_' || c == '-';
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80u) {
      ++i;
      continue;
    } else if ((lead & 0xE0u) == 0xC0u) {
      continuation = 1;
      code_point = lead & 0x1Fu;
      if (code_point < 2) {
        // 0xC0 and 0xC1 can only begin an overlong encoding: the smallest
        // two-byte sequence is 0xC2 0x80 (U+0080).
        return false;
      }
    } else if ((lead & 0xF0u) == 0xE0u) {
      continuation = 2;
      code_point = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
      continuation = 3;
      code_point = lead & 0x07u;
    } else {
      return false;
    }
    if (i + continuation >= text.size()) {
      return false;
    }
    for (std::size_t k = 1; k <= continuation; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3Fu);
    }
    if (continuation == 2 && code_point < 0x800u) return false;      // overlong
    if (continuation == 3 && code_point < 0x10000u) return false;    // overlong
    if (code_point > 0x10FFFFu) return false;                        // out of range
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) return false;  // surrogate
    i += continuation + 1;
  }
  return true;
}

bool contains_control_characters(std::string_view text) noexcept {
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20u || byte == 0x7Fu) {
      return true;
    }
  }
  return false;
}

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxNameLength) {
    return false;
  }
  if (!is_ascii_alphanumeric(text.front())) {
    return false;
  }
  for (const char c : text) {
    if (!is_identifier_tail(c)) {
      return false;
    }
  }
  return true;
}

bool is_valid_reason(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxReasonLength) {
    return false;
  }
  return is_valid_utf8(text) && !contains_control_characters(text);
}

std::string escape_text(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x20u && byte < 0x7Fu) {
      out.push_back(c);
    } else {
      out.push_back('\\');
      out.push_back('x');
      out.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
      out.push_back(kHexDigits[byte & 0x0Fu]);
    }
  }
  return out;
}

std::string to_lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

}  // namespace fac
