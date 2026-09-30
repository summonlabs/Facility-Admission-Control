// Facility Admission Control - content digests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A Digest256 is the SHA-256 of a canonical encoding. Where the runtime says
// "generation-bound" it means the decision names this value, so a later reader
// can prove which exact bytes authorized it. The all-zero digest is the
// explicit "unset" value and never compares equal to a computed digest in a
// binding check, because a binding check refuses an unset digest.

#ifndef FAC_CORE_DIGEST_HPP
#define FAC_CORE_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "fac/core/error.hpp"
#include "fac/core/sha256.hpp"

namespace fac {

class Digest256 {
 public:
  static constexpr std::size_t kBytes = 32;
  static constexpr std::size_t kHexLength = 64;

  Digest256() noexcept = default;

  [[nodiscard]] static Digest256 of(std::span<const std::byte> bytes) noexcept;

  // Accepts exactly 64 lowercase or uppercase hexadecimal characters. Any
  // other length, any non-hex character and any unset (all-zero) value are
  // refused: a textual digest that parses to zero would silently disable a
  // binding.
  [[nodiscard]] static Result<Digest256> from_hex(std::string_view hex);

  [[nodiscard]] const std::array<std::byte, kBytes>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::array<std::byte, kBytes>& bytes() noexcept { return bytes_; }

  [[nodiscard]] bool is_unset() const noexcept;
  [[nodiscard]] std::string to_hex() const;

  friend bool operator==(const Digest256&, const Digest256&) = default;
  friend auto operator<=>(const Digest256&, const Digest256&) = default;

 private:
  std::array<std::byte, kBytes> bytes_{};
};

struct Digest256Hash {
  [[nodiscard]] std::size_t operator()(const Digest256& digest) const noexcept;
};

// Incremental canonical digest: callers feed the exact canonical bytes.
class DigestBuilder {
 public:
  void update(std::span<const std::byte> bytes) noexcept;
  [[nodiscard]] Digest256 finish() noexcept;

 private:
  Sha256 hash_{};
};

}  // namespace fac

#endif  // FAC_CORE_DIGEST_HPP
