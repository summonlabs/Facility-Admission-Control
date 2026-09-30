// Facility Admission Control - SHA-256 content binding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SHA-256 is used to bind a decision, a grant, an evidence set and a durable
// record to the exact bytes they were produced from. It is implemented here so
// the runtime has no third-party dependency, and it is validated against the
// published NIST test vectors by the test suite.

#ifndef FAC_CORE_SHA256_HPP
#define FAC_CORE_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fac {

class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kBlockBytes = 64;

  Sha256() noexcept = default;

  void update(std::span<const std::byte> bytes) noexcept;

  // Finishes the digest and resets the object to its initial state.
  [[nodiscard]] std::array<std::byte, kDigestBytes> finish() noexcept;

  [[nodiscard]] static std::array<std::byte, kDigestBytes> hash(
      std::span<const std::byte> bytes) noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                      0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
  std::array<std::byte, kBlockBytes> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

// Renders a digest as lowercase hexadecimal (64 characters).
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> bytes);

}  // namespace fac

#endif  // FAC_CORE_SHA256_HPP
