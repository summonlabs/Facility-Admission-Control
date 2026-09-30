// Facility Admission Control - CRC-32C (Castagnoli) frame integrity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// CRC-32C detects the accidental corruption a durable frame can suffer:
// torn writes, truncation, bit rot, interference from another writer. It is an
// integrity check, not a security primitive, and it is never the only binding
// on a decision: content bindings use SHA-256.

#ifndef FAC_CORE_CRC32C_HPP
#define FAC_CORE_CRC32C_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fac {

namespace detail {

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  constexpr std::uint32_t kPolynomial = 0x82F63B78u;  // reflected Castagnoli
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? static_cast<std::uint32_t>((crc >> 1) ^ kPolynomial)
                             : static_cast<std::uint32_t>(crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

}  // namespace detail

// Incremental CRC-32C. The running value is the final value: update() applies
// the usual pre/post inversion once, so crc32c(data) == Crc32c{}.update(data)
// and finish() is a pure accessor.
class Crc32c {
 public:
  Crc32c() noexcept = default;

  void update(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = state_;
    for (const std::byte b : bytes) {
      const auto index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)));
      crc = static_cast<std::uint32_t>(detail::kCrc32cTable[index] ^ (crc >> 8));
    }
    state_ = crc;
  }

  [[nodiscard]] std::uint32_t finish() const noexcept {
    return static_cast<std::uint32_t>(state_ ^ 0xFFFFFFFFu);
  }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

[[nodiscard]] inline std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
  Crc32c crc;
  crc.update(bytes);
  return crc.finish();
}

}  // namespace fac

#endif  // FAC_CORE_CRC32C_HPP
