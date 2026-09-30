// Facility Admission Control - content digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/core/digest.hpp"

#include <cstdint>

namespace fac {
namespace {

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

Digest256 Digest256::of(std::span<const std::byte> bytes) noexcept {
  const auto raw = Sha256::hash(bytes);
  Digest256 digest;
  digest.bytes_ = raw;
  return digest;
}

Result<Digest256> Digest256::from_hex(std::string_view hex) {
  if (hex.size() != kHexLength) {
    return make_error(ErrorCode::malformed_input, "digest text must be exactly 64 hexadecimal characters");
  }
  Digest256 digest;
  for (std::size_t i = 0; i < kBytes; ++i) {
    const int high = hex_value(hex[i * 2]);
    const int low = hex_value(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return make_error(ErrorCode::malformed_input, "digest text contains a non-hexadecimal character");
    }
    digest.bytes_[i] = static_cast<std::byte>((high << 4) | low);
  }
  if (digest.is_unset()) {
    return make_error(ErrorCode::malformed_input, "a zero digest is not a valid binding");
  }
  return digest;
}

bool Digest256::is_unset() const noexcept {
  for (const std::byte b : bytes_) {
    if (b != std::byte{0}) {
      return false;
    }
  }
  return true;
}

std::string Digest256::to_hex() const {
  std::string out;
  out.resize(kHexLength);
  for (std::size_t i = 0; i < kBytes; ++i) {
    const auto value = std::to_integer<std::uint8_t>(bytes_[i]);
    out[i * 2] = kHexDigits[(value >> 4) & 0x0Fu];
    out[i * 2 + 1] = kHexDigits[value & 0x0Fu];
  }
  return out;
}

std::size_t Digest256Hash::operator()(const Digest256& digest) const noexcept {
  std::uint64_t h = 1469598103934665603ull;
  for (const std::byte b : digest.bytes()) {
    h ^= std::to_integer<std::uint8_t>(b);
    h *= 1099511628211ull;
  }
  return static_cast<std::size_t>(h);
}

void DigestBuilder::update(std::span<const std::byte> bytes) noexcept { hash_.update(bytes); }

Digest256 DigestBuilder::finish() noexcept {
  Digest256 digest;
  digest.bytes() = hash_.finish();
  return digest;
}

}  // namespace fac
