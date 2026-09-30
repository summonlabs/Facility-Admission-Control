// Facility Admission Control - verdicts, blockers, decisions and grants.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A decision is one of exactly three verdicts:
//
//   Allow   every required check passed against fresh, generation-identified
//           evidence and the commitment fits under the stated policy.
//   Refuse  a hard condition failed. Retrying the same request against the same
//           authoritative state cannot change the answer.
//   Defer   the runtime cannot conclude: evidence is missing, stale, superseded
//           by newer generations, conflicts with a pinned generation, or a
//           temporal condition (active maintenance, an open incident) makes the
//           answer depend on state that is still moving.
//
// "Defer" is how this runtime expresses "unknown". There is deliberately no
// fourth verdict, because a caller that cannot distinguish "refused" from
// "cannot tell" will retry the wrong class of request.
//
// Blocker precedence is fixed by the order of the enum below: lower values are
// more decisive. Every blocker that applies is recorded; the primary blocker is
// the first one in precedence order, which makes a refusal reproducible and
// attributable regardless of the order the checks happened to run in.

#ifndef FAC_MODEL_DECISION_HPP
#define FAC_MODEL_DECISION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/model/evidence.hpp"
#include "fac/model/quantity.hpp"
#include "fac/model/request.hpp"
#include "fac/model/scope.hpp"

namespace fac {

enum class Verdict : std::uint8_t {
  allow = 0,
  refuse = 1,
  defer = 2,
};

[[nodiscard]] const char* to_string(Verdict verdict) noexcept;
[[nodiscard]] Result<Verdict> verdict_from_string(std::string_view text);

// Precedence order is the declaration order. Do not reorder without also
// updating the documented precedence table in README.md.
enum class BlockerCode : std::uint8_t {
  // Hard refusals, most decisive first.
  request_invalid = 0,
  idempotency_conflict = 1,
  control_epoch_superseded = 2,
  evidence_scope_mismatch = 3,
  tenant_not_active = 4,
  envelope_limit_exceeded = 5,
  envelope_exhausted = 6,
  policy_denied = 7,
  placement_forbidden = 8,
  placement_limit_exceeded = 9,
  redundancy_level_insufficient = 10,
  maintenance_outage = 11,
  incident_major = 12,
  capacity_exhausted = 13,
  protected_headroom_insufficient = 14,
  overcommit_not_permitted = 15,
  overcommit_limit_exceeded = 16,
  horizon_exceeded = 17,
  request_expired = 18,
  grant_not_found = 19,
  grant_expired = 20,
  grant_fenced = 21,
  grant_state_conflict = 22,

  // Deferrals: missing, stale, superseded or temporally unsettled evidence.
  commitment_window_unspecified = 23,
  demand_unspecified = 24,
  capacity_missing = 25,
  capacity_stale = 26,
  capacity_unknown = 27,
  redundancy_missing = 28,
  redundancy_stale = 29,
  redundancy_unknown = 30,
  tenant_missing = 31,
  tenant_stale = 32,
  tenant_unknown = 33,
  envelope_missing = 34,
  envelope_stale = 35,
  envelope_unknown = 36,
  service_class_missing = 37,
  service_class_stale = 38,
  service_class_unknown = 39,
  maintenance_missing = 40,
  maintenance_stale = 41,
  maintenance_unknown = 42,
  maintenance_state_unknown = 43,
  maintenance_active = 44,
  maintenance_exposure = 45,
  incident_missing = 46,
  incident_stale = 47,
  incident_unknown = 48,
  incident_state_unknown = 49,
  incident_degraded = 50,
  placement_missing = 51,
  placement_stale = 52,
  placement_unknown = 53,
  policy_missing = 54,
  policy_stale = 55,
  policy_unknown = 56,
  evidence_future_dated = 57,
  evidence_superseded = 58,
  generation_mismatch = 59,
};

inline constexpr std::size_t kBlockerCodeCount = 60;

[[nodiscard]] const char* to_string(BlockerCode code) noexcept;
[[nodiscard]] Result<BlockerCode> blocker_code_from_string(std::string_view text);

// The verdict a blocker on its own implies.
[[nodiscard]] Verdict blocker_verdict(BlockerCode code) noexcept;
[[nodiscard]] std::size_t blocker_precedence(BlockerCode code) noexcept;

struct Blocker {
  BlockerCode code = BlockerCode::request_invalid;
  std::optional<Dimension> dimension;
  std::string detail;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<Blocker> decode(codec::Reader& reader);

  [[nodiscard]] std::string render() const;

  friend bool operator==(const Blocker&, const Blocker&) = default;
};

// A duplicate-free blocker set kept in precedence order. Two independent checks
// that find the same condition produce one entry, so the rendering and the
// digest of a decision do not depend on how many code paths noticed.
class BlockerSet {
 public:
  [[nodiscard]] Status add(Blocker blocker);
  [[nodiscard]] Status add(BlockerCode code, std::string detail,
                           std::optional<Dimension> dimension = std::nullopt);

  [[nodiscard]] const std::vector<Blocker>& items() const noexcept { return items_; }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] bool contains(BlockerCode code) const;
  [[nodiscard]] const Blocker* primary() const;

  [[nodiscard]] Verdict implied_verdict() const;

  friend bool operator==(const BlockerSet&, const BlockerSet&) = default;

 private:
  std::vector<Blocker> items_;
};

// The per-dimension arithmetic behind a decision, kept so that a grant or a
// refusal can be explained without re-running the evaluation.
struct DimensionAssessment {
  Dimension dimension = Dimension::power;
  std::optional<std::uint64_t> total;
  std::optional<std::uint64_t> committed;
  std::optional<std::uint64_t> reserved;
  std::optional<std::uint64_t> protected_headroom;
  std::optional<std::uint64_t> demand;
  std::optional<std::uint64_t> available;
  std::optional<std::int64_t> remaining;
  std::optional<std::uint64_t> overcommit;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<DimensionAssessment> decode(codec::Reader& reader);

  friend bool operator==(const DimensionAssessment&, const DimensionAssessment&) = default;
};

class CommitmentAssessment {
 public:
  [[nodiscard]] Status add(DimensionAssessment assessment);

  [[nodiscard]] const std::vector<DimensionAssessment>& dimensions() const noexcept {
    return dimensions_;
  }
  [[nodiscard]] const DimensionAssessment* find(Dimension dimension) const;

  [[nodiscard]] bool overcommit_used() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<CommitmentAssessment> decode(codec::Reader& reader);

  friend bool operator==(const CommitmentAssessment&, const CommitmentAssessment&) = default;

 private:
  std::vector<DimensionAssessment> dimensions_;
};

// One material generation a grant is bound to, together with the digest of the
// exact evidence bytes that carried it.
struct GrantBinding {
  EvidenceKind kind = EvidenceKind::capacity;
  std::uint64_t generation = 0;
  Digest256 evidence_digest;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<GrantBinding> decode(codec::Reader& reader);

  friend bool operator==(const GrantBinding&, const GrantBinding&) = default;
};

struct Grant {
  GrantId grant_id;
  GrantRevision revision;
  RequestId request_id;
  Digest256 request_digest;

  // Digest over the request content, the evidence bindings, the control epoch,
  // the ledger sequence, the validity window, the scope, the tenant and the
  // demand. Everything the grant depends on is inside it, so a grant whose
  // content was altered no longer matches its own binding digest.
  //
  // The grant deliberately does not carry the digest of the decision that
  // issued it: that digest covers the grant, so the two would have to be
  // computed from each other. The link is structural instead - a grant exists
  // only inside the decision record that issued it, and the ledger refuses a
  // grant whose request, epoch or sequence disagrees with that record.
  Digest256 binding_digest;

  std::vector<GrantBinding> bindings;
  ControlEpoch control_epoch;
  LedgerSequence sequence;

  Timestamp issued_at;
  Timestamp expires_at;

  TargetScope scope;
  TenantId tenant;
  AmountVector demand;

  // True when this grant holds its demand against the ledger while it waits to
  // be committed. The hold is released by committing, expiring, being fenced or
  // being released, and it is recorded rather than assumed from configuration.
  bool holds_capacity = false;

  [[nodiscard]] const GrantBinding* find_binding(EvidenceKind kind) const;

  // Digest over the request digest, the bindings, the control epoch and the
  // ledger sequence: everything that must still hold for the grant to be usable.
  [[nodiscard]] Digest256 compute_binding_digest() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<Grant> decode(codec::Reader& reader);

  friend bool operator==(const Grant&, const Grant&) = default;
};

struct Decision {
  RequestId request_id;
  Digest256 request_digest;
  Verdict verdict = Verdict::defer;
  BlockerSet blockers;

  ControlEpoch control_epoch;
  LedgerSequence sequence;
  Timestamp decided_at;

  EvidenceSet evidence;
  CommitmentAssessment assessment;

  // Deterministic, ordered human-readable statements. Generated from the
  // assessment and the blockers so the explanation cannot drift from the math.
  std::vector<std::string> explanation;

  std::optional<Grant> grant;

  // True when the decision was resolved from a durable record because the same
  // request had already been decided, rather than evaluated again.
  bool replayed = false;

  // Digest of the canonical encoding of everything above except this field.
  Digest256 digest;

  [[nodiscard]] std::optional<BlockerCode> primary_blocker() const;

  // Digest of the canonical decision body, excluding this field and the
  // transient replayed flag. Replaying a recorded decision reproduces exactly
  // this value.
  [[nodiscard]] Digest256 compute_digest() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<Decision> decode(codec::Reader& reader);

  friend bool operator==(const Decision&, const Decision&) = default;
};

// The result of executing (committing) a grant.
enum class CommitState : std::uint8_t {
  committed = 0,
  fenced = 1,
  expired = 2,
  released = 3,
};

[[nodiscard]] const char* to_string(CommitState state) noexcept;

struct CommitOutcome {
  GrantId grant_id;
  CommitState state = CommitState::fenced;
  BlockerSet blockers;
  std::optional<CommitmentId> commitment_id;
  LedgerSequence sequence;
  Timestamp resolved_at;
  bool replayed = false;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<CommitOutcome> decode(codec::Reader& reader);

  friend bool operator==(const CommitOutcome&, const CommitOutcome&) = default;
};

}  // namespace fac

#endif  // FAC_MODEL_DECISION_HPP
