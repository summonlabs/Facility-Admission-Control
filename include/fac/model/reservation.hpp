// Facility Admission Control - reservation intent and commitment lifecycle.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This runtime does not reserve capacity. It decides, and then it emits a
// bounded, generation-bound request to the authority that does own the
// reservation. The effect is owned elsewhere and the record here says exactly
// that: an intent was emitted.
//
// A commitment therefore moves through explicit states, and capacity is
// consumed from the moment a claim exists, not from the moment someone claims
// success:
//
//   held         a grant holds the demand against this ledger; nothing has been
//                asked of the reservation owner yet
//   provisional  the intent was emitted; the owner has not answered
//   confirmed    the owner returned the defined evidence for a full reservation
//   released     an explicit release, or owner evidence that rejected the intent
//   expired      the hold's validity window passed before it was committed
//   fenced       a material generation moved, or the capacity it was granted
//                against no longer covers it, before it was committed
//
// Held, provisional and confirmed all count against availability. A partial
// acknowledgement keeps the whole demand counted: an owner that confirms less
// than was asked for has not returned capacity, and this runtime will not
// invent the difference.

#ifndef FAC_MODEL_RESERVATION_HPP
#define FAC_MODEL_RESERVATION_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/model/quantity.hpp"
#include "fac/model/scope.hpp"

namespace fac {

// A bounded request emitted to the authority that owns reservation. It is an
// output of this runtime and an input to another one; it is never executed here.
struct ReservationIntent {
  IntentId intent_id;
  RequestId request_id;
  GrantId grant_id;
  CommitmentId commitment_id;
  OwnerId owner;
  TargetScope scope;
  TenantId tenant;
  ServiceClassId service_class;
  AmountVector demand;
  Timestamp not_before;
  Timestamp not_after;
  ControlEpoch control_epoch;
  LedgerSequence sequence;
  Digest256 binding_digest;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<ReservationIntent> decode(codec::Reader& reader);

  friend bool operator==(const ReservationIntent&, const ReservationIntent&) = default;
};

enum class ReservationOutcome : std::uint8_t {
  reserved = 0,
  partially_reserved = 1,
  rejected = 2,
};

[[nodiscard]] const char* to_string(ReservationOutcome outcome) noexcept;
[[nodiscard]] Result<ReservationOutcome> reservation_outcome_from_string(std::string_view text);

// The defined evidence that turns "an intent was emitted" into "the owner
// reports a durable reservation". Anything less than this is not proof, and is
// refused rather than recorded.
struct ReservationEvidence {
  IntentId intent_id;
  ReservationId reservation;
  OwnerId owner;
  ReservationOutcome outcome = ReservationOutcome::rejected;
  AmountVector confirmed;
  std::uint64_t owner_generation = 0;
  Digest256 owner_digest;
  Timestamp acknowledged_at;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<ReservationEvidence> decode(codec::Reader& reader);

  friend bool operator==(const ReservationEvidence&, const ReservationEvidence&) = default;
};

// A commitment exists only once a grant has been executed, so there is no
// "held" state here: an uncommitted grant holds capacity through its own record
// and releases it by expiring, being fenced or being released.
enum class CommitmentState : std::uint8_t {
  provisional = 0,
  confirmed = 1,
  partial = 2,
  released = 3,
  expired = 4,
};

[[nodiscard]] const char* to_string(CommitmentState state) noexcept;
[[nodiscard]] Result<CommitmentState> commitment_state_from_string(std::string_view text);

// True for the states that still consume capacity.
[[nodiscard]] bool commitment_consumes(CommitmentState state) noexcept;

struct CommitmentRecord {
  CommitmentId commitment_id;
  RequestId request_id;
  GrantId grant_id;
  TargetScope scope;
  TenantId tenant;
  ServiceClassId service_class;
  AmountVector demand;

  // The bounded request that was emitted to the reservation owner. It is kept
  // with the commitment so an operator can see exactly what was asked for,
  // under which generations, without re-deriving it.
  ReservationIntent intent;
  CommitmentState state = CommitmentState::provisional;
  Timestamp recorded_at;
  Timestamp expires_at;
  std::optional<Timestamp> resolved_at;
  std::optional<ReservationEvidence> evidence;

  // Bounded, escaped explanation of the transition into a terminal state.
  std::string resolution_detail;

  [[nodiscard]] bool consumes() const noexcept { return commitment_consumes(state); }

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<CommitmentRecord> decode(codec::Reader& reader);

  friend bool operator==(const CommitmentRecord&, const CommitmentRecord&) = default;
};

}  // namespace fac

#endif  // FAC_MODEL_RESERVATION_HPP
