// Facility Admission Control - hard input bounds.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every bound here is a refusal threshold, not a suggestion. The encoders,
// decoders and persistence layer validate against these values before they
// allocate or mutate anything, so hostile input cannot turn a declared length
// into an absurd allocation.

#ifndef FAC_CORE_LIMITS_HPP
#define FAC_CORE_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace fac {

// Text.
inline constexpr std::size_t kMaxNameLength = 64;         // identifiers, keys, enum names
inline constexpr std::size_t kMaxReasonLength = 512;      // human-readable refusal reasons
inline constexpr std::size_t kMaxPathLength = 4096;       // filesystem paths

// Collections.
inline constexpr std::size_t kMaxDimensions = 4;          // power, cooling, space, slots
inline constexpr std::size_t kMaxMaintenanceWindows = 64;
inline constexpr std::size_t kMaxBlockerKinds = 64;
inline constexpr std::size_t kMaxBlockerRecords = 64;
inline constexpr std::size_t kMaxBindings = 32;
inline constexpr std::size_t kMaxLedgerRecords = 1'000'000;
inline constexpr std::size_t kMaxPendingRecords = 100'000;

// Durable framing.
inline constexpr std::size_t kMaxFramePayload = 1u << 20;        // 1 MiB
inline constexpr std::size_t kMaxJournalBytes = 1ull << 32;      // 4 GiB
inline constexpr std::size_t kMaxSnapshotBytes = 1ull << 30;     // 1 GiB
inline constexpr std::size_t kMaxSequenceSteps = 1'000'000;      // frames applied by one recovery

// Integer bounds for domain quantities, chosen far below the 64-bit ceiling so
// that a checked addition over a bounded vector cannot wrap.
inline constexpr std::uint64_t kMaxQuantity = 1ull << 48;        // base units per dimension
inline constexpr std::uint64_t kMaxCounter = (1ull << 48) - 1;   // generations, epochs, sequences

}  // namespace fac

#endif  // FAC_CORE_LIMITS_HPP
