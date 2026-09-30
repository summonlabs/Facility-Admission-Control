// Facility Admission Control - the admission ledger.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The ledger is the authoritative in-memory state of one admission control
// process, rebuilt by replaying the durable record stream. Live mutation and
// recovery replay run the same apply() path over the same encoded records, so
// the state after a restart cannot differ from the state before it.
//
// Two things live here that are easy to get wrong elsewhere:
//
//   * consumption. A commitment consumes capacity from the moment it exists,
//     and capacity snapshots come from another authority. Consumption recorded
//     after the snapshot's observation time is not reflected in it, so the
//     ledger adds exactly that and nothing else. No double counting and no
//     invisible consumption.
//
//   * watermarks. The highest generation the ledger has accepted per authority
//     and facility. A snapshot at a lower generation than one already accepted
//     is superseded evidence, and superseded evidence cannot re-open capacity
//     that a newer generation already showed as consumed.

#ifndef FAC_LEDGER_LEDGER_HPP
#define FAC_LEDGER_LEDGER_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/durable/format.hpp"
#include "fac/model/decision.hpp"
#include "fac/model/request.hpp"
#include "fac/model/reservation.hpp"

namespace fac {

namespace detail {
struct DecodedRecord;
}  // namespace detail

// Bumped whenever the ledger snapshot payload layout changes.
inline constexpr std::uint32_t kLedgerStateVersion = 1;

enum class GrantState : std::uint8_t {
  issued = 0,
  committed = 1,
  fenced = 2,
  expired = 3,
  released = 4,
};

[[nodiscard]] const char* to_string(GrantState state) noexcept;

struct DecisionEntry {
  AdmissionRequest request;
  Decision decision;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<DecisionEntry> decode(codec::Reader& reader);

  friend bool operator==(const DecisionEntry&, const DecisionEntry&) = default;
};

struct GrantEntry {
  Grant grant;
  GrantState state = GrantState::issued;
  Timestamp resolved_at;
  std::optional<CommitmentId> commitment_id;
  std::vector<Blocker> blockers;
  std::string detail;

  [[nodiscard]] bool usable() const noexcept { return state == GrantState::issued; }

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<GrantEntry> decode(codec::Reader& reader);

  friend bool operator==(const GrantEntry&, const GrantEntry&) = default;
};

// The key of a generation watermark: one authority's evidence about one
// facility.
struct WatermarkKey {
  EvidenceKind kind = EvidenceKind::capacity;
  FacilityId facility;

  friend bool operator<(const WatermarkKey& left, const WatermarkKey& right) {
    if (static_cast<std::uint8_t>(left.kind) != static_cast<std::uint8_t>(right.kind)) {
      return static_cast<std::uint8_t>(left.kind) < static_cast<std::uint8_t>(right.kind);
    }
    return left.facility < right.facility;
  }
  friend bool operator==(const WatermarkKey&, const WatermarkKey&) = default;
};

// The committed payload of a ledger record. Each variant is encoded on its own
// so the durable frame carries a self-describing payload.
struct DecisionRecorded {
  DecisionEntry entry;
};

struct GrantCommitted {
  GrantId grant_id;
  CommitmentId commitment_id;
  Timestamp committed_at;
  ReservationIntent intent;
};

struct GrantFenced {
  GrantId grant_id;
  Blocker blocker;
  Timestamp at;
};

struct GrantExpired {
  GrantId grant_id;
  Timestamp at;
};

struct GrantReleased {
  GrantId grant_id;
  Timestamp at;
  std::string reason;
};

struct CommitmentEvidenceRecorded {
  CommitmentId commitment_id;
  ReservationEvidence evidence;
  Timestamp at;
};

struct CommitmentReleased {
  CommitmentId commitment_id;
  Timestamp at;
  std::string reason;
};

struct CommitmentExpired {
  CommitmentId commitment_id;
  Timestamp at;
};

struct ConsumptionQuery {
  FacilityId facility;
  std::optional<RackId> rack;
  // Claims recorded at or after this instant are counted. A snapshot observed
  // at instant T describes the facility as it was at T, so a claim recorded at
  // T is not in it, and one recorded before T is. Counting a claim the snapshot
  // may already include is the safe direction: it can only refuse capacity, not
  // grant capacity that was never free.
  Timestamp observed_after;
  // A claim that must not count against itself, used when a grant re-checks the
  // headroom it was issued against.
  std::optional<GrantId> exclude_grant;
};

class Ledger {
 public:
  Ledger() = default;

  [[nodiscard]] LedgerSequence sequence() const noexcept { return sequence_; }

  [[nodiscard]] const std::map<RequestId, DecisionEntry>& decisions() const noexcept {
    return decisions_;
  }
  [[nodiscard]] const std::map<GrantId, GrantEntry>& grants() const noexcept { return grants_; }
  [[nodiscard]] const std::map<CommitmentId, CommitmentRecord>& commitments() const noexcept {
    return commitments_;
  }

  [[nodiscard]] const DecisionEntry* find_decision(RequestId id) const;
  [[nodiscard]] const GrantEntry* find_grant(GrantId id) const;
  [[nodiscard]] const CommitmentRecord* find_commitment(CommitmentId id) const;
  [[nodiscard]] const CommitmentRecord* find_commitment_by_grant(GrantId id) const;

  [[nodiscard]] std::uint64_t consumed(Dimension dimension, const ConsumptionQuery& query) const;
  [[nodiscard]] std::uint64_t consuming_commitments() const;

  [[nodiscard]] std::uint64_t watermark(EvidenceKind kind, FacilityId facility) const;
  void raise_watermark(EvidenceKind kind, FacilityId facility, std::uint64_t generation);

  // Decodes a record payload without touching state. The engine calls this
  // before the durable append so a payload that cannot be applied is never
  // committed in the first place.
  [[nodiscard]] Status validate_record(durable::RecordKind kind,
                                       std::span<const std::byte> payload);

  // Applies a record. Replay and live mutation both go through here.
  [[nodiscard]] Status apply(durable::RecordKind kind, LedgerSequence sequence,
                             std::span<const std::byte> payload);

  // Grants whose hold expired at or before now, in deterministic identity order.
  [[nodiscard]] std::vector<GrantId> expired_holds(Timestamp now) const;

  [[nodiscard]] Status encode_state(std::vector<std::byte>& out) const;
  [[nodiscard]] static Result<Ledger> decode_state(std::span<const std::byte> bytes);

  [[nodiscard]] Digest256 state_digest() const;

 private:
  [[nodiscard]] Status precheck(const detail::DecodedRecord& record) const;
  [[nodiscard]] Status commit_apply(const detail::DecodedRecord& record);

  LedgerSequence sequence_;
  std::map<RequestId, DecisionEntry> decisions_;
  std::map<GrantId, GrantEntry> grants_;
  std::map<CommitmentId, CommitmentRecord> commitments_;
  std::map<WatermarkKey, std::uint64_t> watermarks_;
};

}  // namespace fac

#endif  // FAC_LEDGER_LEDGER_HPP
