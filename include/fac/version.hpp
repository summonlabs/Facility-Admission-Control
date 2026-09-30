// Facility Admission Control - library version.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FAC_VERSION_HPP
#define FAC_VERSION_HPP

#include <cstdint>
#include <string>

namespace fac {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// The durable format revision this build writes. It is independent of the
// library version: a build that keeps the on-disk layout identical keeps this
// number identical, and a reader refuses a revision it does not implement.
inline constexpr std::uint32_t kDurableFormatRevision = 1;

struct Version {
  std::uint32_t major = kVersionMajor;
  std::uint32_t minor = kVersionMinor;
  std::uint32_t patch = kVersionPatch;

  friend bool operator==(const Version&, const Version&) = default;
};

[[nodiscard]] Version version() noexcept;
[[nodiscard]] std::string version_string();

}  // namespace fac

#endif  // FAC_VERSION_HPP
