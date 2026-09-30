// Facility Admission Control - CRC-32C translation unit.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The table and the incremental class are defined in the header, so this
// translation unit exists only to give the header a home in the library and to
// keep the build layout uniform.

#include "fac/core/crc32c.hpp"

namespace fac {

// The known-answer vector for this implementation is asserted by the test suite
// (tests/tests_core.cpp): CRC-32C("123456789") == 0xE3069283.

}  // namespace fac
