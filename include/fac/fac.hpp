// Facility Admission Control - umbrella header.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Include this header to reach the whole public surface. Individual headers can
// be included on their own; none of them depends on this one.

#ifndef FAC_FAC_HPP
#define FAC_FAC_HPP

#include "fac/version.hpp"

#include "fac/core/checked.hpp"
#include "fac/core/crc32c.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/limits.hpp"
#include "fac/core/sha256.hpp"
#include "fac/core/text.hpp"
#include "fac/core/time.hpp"

#include "fac/codec/codec.hpp"

#include "fac/model/capacity.hpp"
#include "fac/model/decision.hpp"
#include "fac/model/evidence.hpp"
#include "fac/model/facility_state.hpp"
#include "fac/model/quantity.hpp"
#include "fac/model/request.hpp"
#include "fac/model/reservation.hpp"
#include "fac/model/scope.hpp"
#include "fac/model/tenancy.hpp"

#include "fac/engine/engine.hpp"
#include "fac/engine/policy.hpp"

#include "fac/durable/format.hpp"
#include "fac/durable/journal.hpp"
#include "fac/durable/lock.hpp"
#include "fac/durable/store.hpp"

#include "fac/ledger/ledger.hpp"

#include "fac/cli/cli.hpp"

#endif  // FAC_FAC_HPP
