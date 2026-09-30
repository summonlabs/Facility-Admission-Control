// Facility Admission Control - library version.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/version.hpp"

namespace fac {

Version version() noexcept { return Version{kVersionMajor, kVersionMinor, kVersionPatch}; }

std::string version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
         std::to_string(kVersionPatch);
}

}  // namespace fac
