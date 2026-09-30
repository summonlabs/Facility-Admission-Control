// Facility Admission Control - verdicts, blockers, decisions and grants.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/decision.hpp"

#include <algorithm>

#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"
#include "detail/enum_codec.hpp"

namespace fac {
namespace {

struct NamedBlocker {
  std::string_view name;
  BlockerCode code;
};

// Declaration order is precedence order and must stay in step with the enum.
constexpr NamedBlocker kBlockerNames[] = {
    {"request_invalid", BlockerCode::request_invalid},
    {"idempotency_conflict", BlockerCode::idempotency_conflict},
    {"control_epoch_superseded", BlockerCode::control_epoch_superseded},
    {"evidence_scope_mismatch", BlockerCode::evidence_scope_mismatch},
    {"tenant_not_active", BlockerCode::tenant_not_active},
    {"envelope_limit_exceeded", BlockerCode::envelope_limit_exceeded},
    {"envelope_exhausted", BlockerCode::envelope_exhausted},
    {"policy_denied", BlockerCode::policy_denied},
    {"placement_forbidden", BlockerCode::placement_forbidden},
    {"placement_limit_exceeded", BlockerCode::placement_limit_exceeded},
    {"redundancy_level_insufficient", BlockerCode::redundancy_level_insufficient},
    {"maintenance_outage", BlockerCode::maintenance_outage},
    {"incident_major", BlockerCode::incident_major},
    {"capacity_exhausted", BlockerCode::capacity_exhausted},
    {"protected_headroom_insufficient", BlockerCode::protected_headroom_insufficient},
    {"overcommit_not_permitted", BlockerCode::overcommit_not_permitted},
    {"overcommit_limit_exceeded", BlockerCode::overcommit_limit_exceeded},
    {"horizon_exceeded", BlockerCode::horizon_exceeded},
    {"request_expired", BlockerCode::request_expired},
    {"grant_not_found", BlockerCode::grant_not_found},
    {"grant_expired", BlockerCode::grant_expired},
    {"grant_fenced", BlockerCode::grant_fenced},
    {"grant_state_conflict", BlockerCode::grant_state_conflict},
    {"commitment_window_unspecified", BlockerCode::commitment_window_unspecified},
    {"demand_unspecified", BlockerCode::demand_unspecified},
    {"capacity_missing", BlockerCode::capacity_missing},
    {"capacity_stale", BlockerCode::capacity_stale},
    {"capacity_unknown", BlockerCode::capacity_unknown},
    {"redundancy_missing", BlockerCode::redundancy_missing},
    {"redundancy_stale", BlockerCode::redundancy_stale},
    {"redundancy_unknown", BlockerCode::redundancy_unknown},
    {"tenant_missing", BlockerCode::tenant_missing},
    {"tenant_stale", BlockerCode::tenant_stale},
    {"tenant_unknown", BlockerCode::tenant_unknown},
    {"envelope_missing", BlockerCode::envelope_missing},
    {"envelope_stale", BlockerCode::envelope_stale},
    {"envelope_unknown", BlockerCode::envelope_unknown},
    {"service_class_missing", BlockerCode::service_class_missing},
    {"service_class_stale", BlockerCode::service_class_stale},
    {"service_class_unknown", BlockerCode::service_class_unknown},
    {"maintenance_missing", BlockerCode::maintenance_missing},
    {"maintenance_stale", BlockerCode::maintenance_stale},
    {"maintenance_unknown", BlockerCode::maintenance_unknown},
    {"maintenance_state_unknown", BlockerCode::maintenance_state_unknown},
    {"maintenance_active", BlockerCode::maintenance_active},
    {"maintenance_exposure", BlockerCode::maintenance_exposure},
    {"incident_missing", BlockerCode::incident_missing},
    {"incident_stale", BlockerCode::incident_stale},
    {"incident_unknown", BlockerCode::incident_unknown},
    {"incident_state_unknown", BlockerCode::incident_state_unknown},
    {"incident_degraded", BlockerCode::incident_degraded},
    {"placement_missing", BlockerCode::placement_missing},
    {"placement_stale", BlockerCode::placement_stale},
    {"placement_unknown", BlockerCode::placement_unknown},
    {"policy_missing", BlockerCode::policy_missing},
    {"policy_stale", BlockerCode::policy_stale},
    {"policy_unknown", BlockerCode::policy_unknown},
    {"evidence_future_dated", BlockerCode::evidence_future_dated},
    {"evidence_superseded", BlockerCode::evidence_superseded},
    {"generation_mismatch", BlockerCode::generation_mismatch},
};

static_assert(std::size(kBlockerNames) == kBlockerCodeCount, "blocker name table is out of step");

// The last code whose own verdict is Refuse. Every code after it defers.
constexpr BlockerCode kLastRefuseCode = BlockerCode::grant_state_conflict;

[[nodiscard]] std::uint8_t dimension_key(const std::optional<Dimension>& dimension) noexcept {
  return dimension.has_value() ? static_cast<std::uint8_t>(static_cast<std::uint8_t>(dimension.value()) + 1) : 0;
}

}  // namespace

const char* to_string(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::allow: return "allow";
    case Verdict::refuse: return "refuse";
    case Verdict::defer: return "defer";
  }
  return "unknown_verdict";
}

Result<Verdict> verdict_from_string(std::string_view text) {
  if (text == "allow") return Verdict::allow;
  if (text == "refuse") return Verdict::refuse;
  if (text == "defer") return Verdict::defer;
  return make_error(ErrorCode::invalid_argument, "unknown verdict");
}

const char* to_string(BlockerCode code) noexcept {
  for (const auto& entry : kBlockerNames) {
    if (entry.code == code) {
      return entry.name.data();
    }
  }
  return "unknown_blocker";
}

Result<BlockerCode> blocker_code_from_string(std::string_view text) {
  for (const auto& entry : kBlockerNames) {
    if (entry.name == text) {
      return entry.code;
    }
  }
  return make_error(ErrorCode::invalid_argument, "unknown blocker code");
}

std::size_t blocker_precedence(BlockerCode code) noexcept { return static_cast<std::size_t>(code); }

Verdict blocker_verdict(BlockerCode code) noexcept {
  return static_cast<std::uint8_t>(code) <= static_cast<std::uint8_t>(kLastRefuseCode) ? Verdict::refuse
                                                                                      : Verdict::defer;
}

void Blocker::encode(codec::Writer& writer) const {
  writer.u8(static_cast<std::uint8_t>(code));
  writer.boolean(dimension.has_value());
  writer.u8(dimension.has_value() ? static_cast<std::uint8_t>(dimension.value()) : 0);
  writer.text(detail);
}

Result<Blocker> Blocker::decode(codec::Reader& reader) {
  auto code = detail::read_enum<BlockerCode>(reader, static_cast<std::uint8_t>(kBlockerCodeCount - 1),
                                             "blocker code");
  if (!code.ok()) return code.error();
  auto has_dimension = reader.boolean();
  if (!has_dimension.ok()) return has_dimension.error();
  auto dimension = reader.u8();
  if (!dimension.ok()) return dimension.error();
  Blocker blocker;
  blocker.code = code.value();
  if (has_dimension.value()) {
    if (dimension.value() >= kDimensionCount) {
      return make_error(ErrorCode::malformed_input, "blocker carries an impossible dimension");
    }
    blocker.dimension = static_cast<Dimension>(dimension.value());
  } else if (dimension.value() != 0) {
    return make_error(ErrorCode::malformed_input, "blocker without a dimension carries a dimension value");
  }
  auto detail = reader.text(kMaxReasonLength);
  if (!detail.ok()) return detail.error();
  blocker.detail = detail.take();
  if (!is_valid_reason(blocker.detail)) {
    return make_error(ErrorCode::malformed_input, "blocker detail is not usable text");
  }
  return blocker;
}

std::string Blocker::render() const {
  std::string out = to_string(code);
  if (dimension.has_value()) {
    out += "[";
    out += to_string(dimension.value());
    out += "]";
  }
  if (!detail.empty()) {
    out += ": ";
    out += detail;
  }
  return out;
}

Status BlockerSet::add(Blocker blocker) {
  const std::size_t key = blocker_precedence(blocker.code) * 8 + dimension_key(blocker.dimension);
  const auto position = std::lower_bound(
      items_.begin(), items_.end(), key, [](const Blocker& existing, std::size_t wanted) {
        return blocker_precedence(existing.code) * 8 + dimension_key(existing.dimension) < wanted;
      });
  if (position != items_.end() &&
      blocker_precedence(position->code) * 8 + dimension_key(position->dimension) == key) {
    return Status::success();  // already recorded; the set is idempotent by design
  }
  if (items_.size() >= kMaxBlockerRecords) {
    return make_error(ErrorCode::limit_exceeded, "blocker set is full");
  }
  items_.insert(position, std::move(blocker));
  return Status::success();
}

Status BlockerSet::add(BlockerCode code, std::string detail, std::optional<Dimension> dimension) {
  Blocker blocker;
  blocker.code = code;
  blocker.dimension = dimension;
  blocker.detail = std::move(detail);
  return add(std::move(blocker));
}

bool BlockerSet::contains(BlockerCode code) const {
  return std::any_of(items_.begin(), items_.end(), [code](const Blocker& blocker) {
    return blocker.code == code;
  });
}

const Blocker* BlockerSet::primary() const { return items_.empty() ? nullptr : &items_.front(); }

Verdict BlockerSet::implied_verdict() const {
  for (const auto& blocker : items_) {
    if (blocker_verdict(blocker.code) == Verdict::refuse) {
      return Verdict::refuse;
    }
  }
  return items_.empty() ? Verdict::allow : Verdict::defer;
}

void DimensionAssessment::encode(codec::Writer& writer) const {
  const auto option = [&writer](const std::optional<std::uint64_t>& value) {
    writer.boolean(value.has_value());
    writer.u64(value.value_or(0));
  };
  writer.u8(static_cast<std::uint8_t>(dimension));
  option(total);
  option(committed);
  option(reserved);
  option(protected_headroom);
  option(demand);
  option(available);
  writer.boolean(remaining.has_value());
  writer.i64(remaining.value_or(0));
  option(overcommit);
}

Result<DimensionAssessment> DimensionAssessment::decode(codec::Reader& reader) {
  auto dimension = detail::read_enum<Dimension>(reader, static_cast<std::uint8_t>(kDimensionCount - 1),
                                                "dimension");
  if (!dimension.ok()) return dimension.error();
  DimensionAssessment assessment;
  assessment.dimension = dimension.value();
  const auto option = [&reader](const char* what, std::optional<std::uint64_t>& target) -> Status {
    auto present = reader.boolean();
    if (!present.ok()) return present.error();
    auto value = reader.u64();
    if (!value.ok()) return value.error();
    if (present.value()) {
      target = value.value();
    } else if (value.value() != 0) {
      return make_error(ErrorCode::malformed_input, std::string("absent ") + what + " carries a value");
    }
    return Status::success();
  };
  Status status = option("total", assessment.total);
  if (!status.ok()) return status.error();
  status = option("committed", assessment.committed);
  if (!status.ok()) return status.error();
  status = option("reserved", assessment.reserved);
  if (!status.ok()) return status.error();
  status = option("protected headroom", assessment.protected_headroom);
  if (!status.ok()) return status.error();
  status = option("demand", assessment.demand);
  if (!status.ok()) return status.error();
  status = option("available", assessment.available);
  if (!status.ok()) return status.error();
  auto has_remaining = reader.boolean();
  if (!has_remaining.ok()) return has_remaining.error();
  auto remaining = reader.i64();
  if (!remaining.ok()) return remaining.error();
  if (has_remaining.value()) {
    assessment.remaining = remaining.value();
  } else if (remaining.value() != 0) {
    return make_error(ErrorCode::malformed_input, "absent remaining value carries a value");
  }
  status = option("overcommit", assessment.overcommit);
  if (!status.ok()) return status.error();
  return assessment;
}

Status CommitmentAssessment::add(DimensionAssessment assessment) {
  if (!dimensions_.empty()) {
    const auto previous = static_cast<std::uint8_t>(dimensions_.back().dimension);
    const auto current = static_cast<std::uint8_t>(assessment.dimension);
    if (current <= previous) {
      return make_error(ErrorCode::duplicate_field, "assessment dimension is out of order or repeated");
    }
  }
  if (dimensions_.size() >= kDimensionCount) {
    return make_error(ErrorCode::limit_exceeded, "assessment carries more dimensions than exist");
  }
  dimensions_.push_back(std::move(assessment));
  return Status::success();
}

const DimensionAssessment* CommitmentAssessment::find(Dimension dimension) const {
  for (const auto& assessment : dimensions_) {
    if (assessment.dimension == dimension) {
      return &assessment;
    }
  }
  return nullptr;
}

bool CommitmentAssessment::overcommit_used() const {
  return std::any_of(dimensions_.begin(), dimensions_.end(), [](const DimensionAssessment& assessment) {
    return assessment.overcommit.has_value() && assessment.overcommit.value() > 0;
  });
}

void CommitmentAssessment::encode(codec::Writer& writer) const {
  writer.u32(static_cast<std::uint32_t>(dimensions_.size()));
  for (const auto& assessment : dimensions_) {
    assessment.encode(writer);
  }
}

Result<CommitmentAssessment> CommitmentAssessment::decode(codec::Reader& reader) {
  auto count = reader.sequence_count(kDimensionCount, 40);
  if (!count.ok()) return count.error();
  CommitmentAssessment assessment;
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto dimension = DimensionAssessment::decode(reader);
    if (!dimension.ok()) return dimension.error();
    const Status status = assessment.add(dimension.take());
    if (!status.ok()) return status.error();
  }
  return assessment;
}

void GrantBinding::encode(codec::Writer& writer) const {
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u64(generation);
  writer.digest(evidence_digest);
}

Result<GrantBinding> GrantBinding::decode(codec::Reader& reader) {
  auto kind = detail::read_enum<EvidenceKind>(reader, static_cast<std::uint8_t>(kEvidenceKindCount - 1),
                                              "evidence kind");
  if (!kind.ok()) return kind.error();
  auto generation = reader.u64();
  if (!generation.ok()) return generation.error();
  auto digest = reader.digest();
  if (!digest.ok()) return digest.error();
  GrantBinding binding;
  binding.kind = kind.value();
  binding.generation = generation.value();
  binding.evidence_digest = digest.value();
  return binding;
}

const GrantBinding* Grant::find_binding(EvidenceKind kind) const {
  for (const auto& binding : bindings) {
    if (binding.kind == kind) {
      return &binding;
    }
  }
  return nullptr;
}

Digest256 Grant::compute_binding_digest() const {
  codec::Writer writer;
  writer.digest(request_digest);
  writer.u32(static_cast<std::uint32_t>(bindings.size()));
  for (const auto& binding : bindings) {
    binding.encode(writer);
  }
  writer.counter(control_epoch);
  writer.counter(sequence);
  writer.i64(issued_at.unix_nanos());
  writer.i64(expires_at.unix_nanos());
  scope.encode(writer);
  writer.ident(tenant);
  demand.encode(writer);
  writer.boolean(holds_capacity);
  return Digest256::of(writer.span());
}

void Grant::encode(codec::Writer& writer) const {
  writer.ident(grant_id);
  writer.counter(revision);
  writer.ident(request_id);
  writer.digest(request_digest);
  writer.digest(binding_digest);
  writer.u32(static_cast<std::uint32_t>(bindings.size()));
  for (const auto& binding : bindings) {
    binding.encode(writer);
  }
  writer.counter(control_epoch);
  writer.counter(sequence);
  writer.i64(issued_at.unix_nanos());
  writer.i64(expires_at.unix_nanos());
  scope.encode(writer);
  writer.ident(tenant);
  demand.encode(writer);
  writer.boolean(holds_capacity);
}

Result<Grant> Grant::decode(codec::Reader& reader) {
  Grant grant;
  auto grant_id = reader.ident<struct GrantIdTag>();
  if (!grant_id.ok()) return grant_id.error();
  grant.grant_id = grant_id.value();
  auto revision = reader.counter<struct GrantRevisionTag>();
  if (!revision.ok()) return revision.error();
  grant.revision = revision.value();
  auto request_id = reader.ident<struct RequestIdTag>();
  if (!request_id.ok()) return request_id.error();
  grant.request_id = request_id.value();
  auto request_digest = reader.digest();
  if (!request_digest.ok()) return request_digest.error();
  grant.request_digest = request_digest.value();
  auto binding_digest = reader.digest();
  if (!binding_digest.ok()) return binding_digest.error();
  grant.binding_digest = binding_digest.value();
  auto count = reader.sequence_count(kMaxBindings, 40);
  if (!count.ok()) return count.error();
  grant.bindings.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto binding = GrantBinding::decode(reader);
    if (!binding.ok()) return binding.error();
    grant.bindings.push_back(binding.take());
  }
  auto epoch = reader.counter<struct ControlEpochTag>();
  if (!epoch.ok()) return epoch.error();
  grant.control_epoch = epoch.value();
  auto sequence = reader.counter<struct LedgerSequenceTag>();
  if (!sequence.ok()) return sequence.error();
  grant.sequence = sequence.value();
  auto issued = reader.i64();
  if (!issued.ok()) return issued.error();
  auto expires = reader.i64();
  if (!expires.ok()) return expires.error();
  if (!is_valid_timestamp_nanos(issued.value()) || !is_valid_timestamp_nanos(expires.value())) {
    return make_error(ErrorCode::out_of_range, "grant carries a time outside the supported range");
  }
  grant.issued_at = Timestamp(issued.value());
  grant.expires_at = Timestamp(expires.value());
  auto scope = TargetScope::decode(reader);
  if (!scope.ok()) return scope.error();
  grant.scope = scope.take();
  auto tenant = reader.ident<struct TenantIdTag>();
  if (!tenant.ok()) return tenant.error();
  grant.tenant = tenant.value();
  auto demand = AmountVector::decode(reader);
  if (!demand.ok()) return demand.error();
  grant.demand = demand.take();
  auto holds = reader.boolean();
  if (!holds.ok()) return holds.error();
  grant.holds_capacity = holds.value();

  if (!(grant.issued_at < grant.expires_at)) {
    return make_error(ErrorCode::malformed_input, "grant validity window is empty");
  }
  if (grant.control_epoch.is_zero()) {
    return make_error(ErrorCode::malformed_input, "grant carries a zero control epoch");
  }
  if (!grant.demand.all_present()) {
    return make_error(ErrorCode::malformed_input, "grant carries an unmeasured demand");
  }
  for (std::size_t i = 0; i < grant.bindings.size(); ++i) {
    if (i > 0 && static_cast<std::uint8_t>(grant.bindings[i - 1].kind) >=
                     static_cast<std::uint8_t>(grant.bindings[i].kind)) {
      return make_error(ErrorCode::malformed_input, "grant bindings are not in ascending kind order");
    }
  }
  return grant;
}

std::optional<BlockerCode> Decision::primary_blocker() const {
  const Blocker* blocker = blockers.primary();
  if (blocker == nullptr) {
    return std::nullopt;
  }
  return blocker->code;
}

Digest256 Decision::compute_digest() const {
  codec::Writer writer;
  writer.ident(request_id);
  writer.digest(request_digest);
  writer.u8(static_cast<std::uint8_t>(verdict));
  writer.u32(static_cast<std::uint32_t>(blockers.items().size()));
  for (const auto& blocker : blockers.items()) {
    blocker.encode(writer);
  }
  writer.counter(control_epoch);
  writer.counter(sequence);
  writer.i64(decided_at.unix_nanos());
  evidence.encode(writer);
  assessment.encode(writer);
  writer.u32(static_cast<std::uint32_t>(explanation.size()));
  for (const auto& line : explanation) {
    writer.text(line);
  }
  writer.boolean(grant.has_value());
  if (grant.has_value()) {
    grant->encode(writer);
  }
  return Digest256::of(writer.span());
}

void Decision::encode(codec::Writer& writer) const {
  writer.ident(request_id);
  writer.digest(request_digest);
  writer.u8(static_cast<std::uint8_t>(verdict));
  writer.u32(static_cast<std::uint32_t>(blockers.items().size()));
  for (const auto& blocker : blockers.items()) {
    blocker.encode(writer);
  }
  writer.counter(control_epoch);
  writer.counter(sequence);
  writer.i64(decided_at.unix_nanos());
  evidence.encode(writer);
  assessment.encode(writer);
  writer.u32(static_cast<std::uint32_t>(explanation.size()));
  for (const auto& line : explanation) {
    writer.text(line);
  }
  writer.boolean(grant.has_value());
  if (grant.has_value()) {
    grant->encode(writer);
  }
  writer.digest(digest);
}

Result<Decision> Decision::decode(codec::Reader& reader) {
  Decision decision;
  auto request_id = reader.ident<struct RequestIdTag>();
  if (!request_id.ok()) return request_id.error();
  decision.request_id = request_id.value();
  auto request_digest = reader.digest();
  if (!request_digest.ok()) return request_digest.error();
  decision.request_digest = request_digest.value();
  auto verdict = detail::read_enum<Verdict>(reader, static_cast<std::uint8_t>(Verdict::defer), "verdict");
  if (!verdict.ok()) return verdict.error();
  decision.verdict = verdict.value();
  auto blocker_count = reader.sequence_count(kMaxBlockerRecords, 6);
  if (!blocker_count.ok()) return blocker_count.error();
  for (std::uint32_t i = 0; i < blocker_count.value(); ++i) {
    auto blocker = Blocker::decode(reader);
    if (!blocker.ok()) return blocker.error();
    const Status status = decision.blockers.add(blocker.take());
    if (!status.ok()) return status.error();
  }
  if (decision.blockers.implied_verdict() != decision.verdict) {
    return make_error(ErrorCode::malformed_input, "decision verdict disagrees with its blocker set");
  }
  auto epoch = reader.counter<struct ControlEpochTag>();
  if (!epoch.ok()) return epoch.error();
  decision.control_epoch = epoch.value();
  auto sequence = reader.counter<struct LedgerSequenceTag>();
  if (!sequence.ok()) return sequence.error();
  decision.sequence = sequence.value();
  auto decided_at = reader.i64();
  if (!decided_at.ok()) return decided_at.error();
  if (!is_valid_timestamp_nanos(decided_at.value())) {
    return make_error(ErrorCode::out_of_range, "decision time is outside the supported range");
  }
  decision.decided_at = Timestamp(decided_at.value());
  auto evidence = EvidenceSet::decode(reader);
  if (!evidence.ok()) return evidence.error();
  decision.evidence = evidence.take();
  auto assessment = CommitmentAssessment::decode(reader);
  if (!assessment.ok()) return assessment.error();
  decision.assessment = assessment.take();
  auto explanation_count = reader.sequence_count(kMaxBlockerRecords * 4, 4);
  if (!explanation_count.ok()) return explanation_count.error();
  decision.explanation.reserve(explanation_count.value());
  for (std::uint32_t i = 0; i < explanation_count.value(); ++i) {
    auto line = reader.text(kMaxReasonLength);
    if (!line.ok()) return line.error();
    if (!is_valid_reason(line.value())) {
      return make_error(ErrorCode::malformed_input, "explanation line is not usable text");
    }
    decision.explanation.push_back(line.take());
  }
  auto has_grant = reader.boolean();
  if (!has_grant.ok()) return has_grant.error();
  if (has_grant.value()) {
    auto grant = Grant::decode(reader);
    if (!grant.ok()) return grant.error();
    decision.grant = grant.take();
  }
  if (decision.verdict == Verdict::allow && !decision.grant.has_value()) {
    return make_error(ErrorCode::malformed_input, "an allowing decision carries no grant");
  }
  if (decision.verdict != Verdict::allow && decision.grant.has_value()) {
    return make_error(ErrorCode::malformed_input, "a non-allowing decision carries a grant");
  }
  auto digest = reader.digest();
  if (!digest.ok()) return digest.error();
  decision.digest = digest.value();
  if (!(decision.compute_digest() == decision.digest)) {
    return make_error(ErrorCode::digest_mismatch, "decision digest does not match its content");
  }
  return decision;
}

const char* to_string(CommitState state) noexcept {
  switch (state) {
    case CommitState::committed: return "committed";
    case CommitState::fenced: return "fenced";
    case CommitState::expired: return "expired";
    case CommitState::released: return "released";
  }
  return "unknown_commit_state";
}

void CommitOutcome::encode(codec::Writer& writer) const {
  writer.ident(grant_id);
  writer.u8(static_cast<std::uint8_t>(state));
  writer.u32(static_cast<std::uint32_t>(blockers.items().size()));
  for (const auto& blocker : blockers.items()) {
    blocker.encode(writer);
  }
  writer.boolean(commitment_id.has_value());
  writer.ident(commitment_id.value_or(CommitmentId{}));
  writer.counter(sequence);
  writer.i64(resolved_at.unix_nanos());
}

Result<CommitOutcome> CommitOutcome::decode(codec::Reader& reader) {
  CommitOutcome outcome;
  auto grant_id = reader.ident<struct GrantIdTag>();
  if (!grant_id.ok()) return grant_id.error();
  outcome.grant_id = grant_id.value();
  auto state = detail::read_enum<CommitState>(reader, static_cast<std::uint8_t>(CommitState::released),
                                              "commit state");
  if (!state.ok()) return state.error();
  outcome.state = state.value();
  auto count = reader.sequence_count(kMaxBlockerRecords, 6);
  if (!count.ok()) return count.error();
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto blocker = Blocker::decode(reader);
    if (!blocker.ok()) return blocker.error();
    const Status status = outcome.blockers.add(blocker.take());
    if (!status.ok()) return status.error();
  }
  auto has_commitment = reader.boolean();
  if (!has_commitment.ok()) return has_commitment.error();
  auto commitment_high = reader.u64();
  if (!commitment_high.ok()) return commitment_high.error();
  auto commitment_low = reader.u64();
  if (!commitment_low.ok()) return commitment_low.error();
  const CommitmentId commitment(commitment_high.value(), commitment_low.value());
  if (has_commitment.value()) {
    if (commitment.is_unset()) {
      return make_error(ErrorCode::invalid_argument, "a committed outcome carries a zero commitment");
    }
    outcome.commitment_id = commitment;
  } else if (!commitment.is_unset()) {
    return make_error(ErrorCode::malformed_input, "an outcome without a commitment carries one");
  }
  auto sequence = reader.counter<struct LedgerSequenceTag>();
  if (!sequence.ok()) return sequence.error();
  outcome.sequence = sequence.value();
  auto resolved = reader.i64();
  if (!resolved.ok()) return resolved.error();
  if (!is_valid_timestamp_nanos(resolved.value())) {
    return make_error(ErrorCode::out_of_range, "commit resolution time is outside the supported range");
  }
  outcome.resolved_at = Timestamp(resolved.value());
  if (outcome.state == CommitState::committed && !outcome.commitment_id.has_value()) {
    return make_error(ErrorCode::malformed_input, "a committed outcome carries no commitment");
  }
  if (outcome.state != CommitState::committed && outcome.commitment_id.has_value()) {
    return make_error(ErrorCode::malformed_input, "an uncommitted outcome carries a commitment");
  }
  if (outcome.state == CommitState::committed && !outcome.blockers.empty()) {
    return make_error(ErrorCode::malformed_input, "a committed outcome carries blockers");
  }
  if (outcome.state != CommitState::committed && outcome.blockers.empty()) {
    return make_error(ErrorCode::malformed_input, "an uncommitted outcome carries no blocker");
  }
  return outcome;
}

}  // namespace fac
