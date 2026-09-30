// Facility Admission Control - deterministic evaluation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The evaluation runs in two passes.
//
// Pass A validates the evidence itself, in evidence-kind order: presence,
// internal validity, scope, observation time, pinned generations and the
// generation watermark the ledger already holds. Each snapshot becomes one
// evidence reference, accepted or rejected, with the digest of the exact bytes
// it was consumed as. Only accepted evidence is usable afterwards.
//
// Pass B runs the semantic checks. The order of the checks below is the order
// blockers are discovered, which is deliberately not the order they are
// reported in: the verdict and the primary blocker come from the fixed
// precedence table, so the same inputs always produce the same decision.

#include "engine/evaluate.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "fac/core/checked.hpp"
#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"

namespace fac::detail {
namespace {

template <class T>
[[nodiscard]] Digest256 digest_of(const T& value) {
  codec::Writer writer;
  value.encode(writer);
  return Digest256::of(writer.span());
}

[[nodiscard]] std::string join_dimensions(const std::vector<Dimension>& dimensions) {
  std::string out;
  for (const Dimension dimension : dimensions) {
    if (!out.empty()) {
      out.append(", ");
    }
    out.append(to_string(dimension));
  }
  return out;
}

[[nodiscard]] std::string dimension_detail(Dimension dimension, const std::string& detail) {
  (void)dimension;
  return detail;
}

[[nodiscard]] bool contains_id(const std::vector<RackId>& ids, RackId wanted) {
  return std::find(ids.begin(), ids.end(), wanted) != ids.end();
}

// The accepted snapshots of one evaluation. A pointer is non-null only when
// that authority's evidence passed every check in pass A.
struct Accepted {
  const CapacitySnapshot* capacity = nullptr;
  const RedundancySnapshot* redundancy = nullptr;
  const TenantSnapshot* tenant = nullptr;
  const EnvelopeSnapshot* envelope = nullptr;
  const ServiceClassSnapshot* service_class = nullptr;
  const MaintenanceSnapshot* maintenance = nullptr;
  const IncidentSnapshot* incident = nullptr;
  const PlacementPolicySnapshot* placement = nullptr;
  const PolicySnapshot* policy = nullptr;
};

struct Evaluation {
  BlockerSet blockers;
  EvidenceSet evidence;
  CommitmentAssessment assessment;
  std::vector<std::string> explanation;
  Accepted accepted;
  Digest256 capacity_digest;
};

// Records a blocker. The set is bounded at kMaxBlockerRecords, which is larger
// than the number of blocker codes, so an entry can never be refused here.
void raise_blocker(BlockerSet& set, BlockerCode code, std::string detail,
                   std::optional<Dimension> dimension = std::nullopt) {
  const Status status = set.add(code, std::move(detail), dimension);
  (void)status;
}

void note(Evaluation& evaluation, std::string line, std::size_t limit) {
  if (evaluation.explanation.size() < limit) {
    evaluation.explanation.push_back(std::move(line));
  }
}

struct EvidenceInput {
  EvidenceKind kind = EvidenceKind::capacity;
  bool present = false;
  bool valid = false;
  std::uint64_t generation = 0;
  Digest256 digest;
  Timestamp observed_at;
  std::optional<std::uint64_t> pinned;
  Error failure;
};

// Pass A for one authority: everything that decides whether a snapshot may be
// used at all.
[[nodiscard]] bool accept_evidence(Evaluation& evaluation, const EvaluationContext& context,
                                   const EvidenceInput& input, BlockerCode missing_code,
                                   BlockerCode stale_code, BlockerCode unknown_code) {
  if (!input.present) {
    if (context.policy.required.needs(input.kind)) {
      raise_blocker(evaluation.blockers, missing_code,
                              std::string("no ") + fac::to_string(input.kind) + " evidence was supplied");
    }
    return false;
  }

  const auto reject = [&evaluation, &input](BlockerCode code, std::string detail) {
    EvidenceRef reference;
    reference.kind = input.kind;
    reference.generation = input.generation;
    reference.digest = input.digest;
    reference.observed_at = input.observed_at;
    reference.accepted = false;
    reference.rejection = code == BlockerCode::request_invalid ? ErrorCode::invalid_argument
                                                               : ErrorCode::state_unverified;
    (void)evaluation.evidence.add(reference);
    raise_blocker(evaluation.blockers, code, std::move(detail));
  };

  if (!input.valid) {
    reject(unknown_code, std::string("the ") + fac::to_string(input.kind) + " evidence did not validate: " +
                             input.failure.to_string());
    return false;
  }
  if (input.digest.is_unset()) {
    reject(unknown_code, std::string("the ") + fac::to_string(input.kind) + " evidence carries no digest");
    return false;
  }
  if (input.observed_at.is_unset()) {
    reject(unknown_code, std::string("the ") + fac::to_string(input.kind) + " evidence carries no observation time");
    return false;
  }

  auto skew = context.policy.max_future_skew;
  auto future_limit = context.now.checked_add(skew);
  if (!future_limit.ok()) {
    reject(unknown_code, "the evaluation clock is outside the supported range");
    return false;
  }
  if (future_limit.value() < input.observed_at) {
    reject(BlockerCode::evidence_future_dated,
           std::string("the ") + fac::to_string(input.kind) +
               " evidence is observed after the evaluation clock");
    return false;
  }

  // The future-skew check above already refused anything beyond the allowance,
  // so what remains is an observation at or slightly ahead of the clock. A small
  // skew is treated as age zero rather than as a refusal.
  const Nanos difference = context.now.unix_nanos() - input.observed_at.unix_nanos();
  const Duration age = difference > 0 ? Duration(difference) : Duration(0);
  const Duration budget = context.policy.freshness.for_kind(input.kind);
  if (budget.nanos() > 0 && age.nanos() > budget.nanos()) {
    reject(stale_code, std::string("the ") + fac::to_string(input.kind) + " evidence is " +
                           age.to_string() + " old, beyond its freshness budget of " +
                           budget.to_string());
    return false;
  }

  if (input.pinned.has_value() && !(input.pinned.value() == input.generation)) {
    reject(BlockerCode::generation_mismatch,
           std::string("the request pinned ") + fac::to_string(input.kind) + " generation " +
               std::to_string(input.pinned.value()) + " but the evidence is generation " +
               std::to_string(input.generation));
    return false;
  }

  const std::uint64_t watermark = context.ledger.watermark(input.kind, context.request.scope.facility);
  if (input.generation < watermark) {
    reject(BlockerCode::evidence_superseded,
           std::string("the ") + fac::to_string(input.kind) + " evidence is generation " +
               std::to_string(input.generation) + " but generation " + std::to_string(watermark) +
               " was already accepted for this facility");
    return false;
  }

  EvidenceRef reference;
  reference.kind = input.kind;
  reference.generation = input.generation;
  reference.digest = input.digest;
  reference.observed_at = input.observed_at;
  reference.accepted = true;
  const Status added = evaluation.evidence.add(reference);
  if (!added.ok()) {
    raise_blocker(evaluation.blockers, unknown_code, added.error().to_string());
    return false;
  }
  return true;
}

}  // namespace

Result<Decision> evaluate_request(const EvaluationContext& context) {
  const AdmissionRequest& request = context.request;
  const AdmissionPolicy& policy = context.policy;
  const Timestamp now = context.now;
  const FacilityId facility = request.scope.facility;

  Evaluation evaluation;
  const std::size_t explanation_limit = policy.max_explanation_lines;

  // -----------------------------------------------------------------------
  // Pass A: evidence.
  // -----------------------------------------------------------------------
  {
    EvidenceInput input;
    input.kind = EvidenceKind::capacity;
    input.present = context.evidence.capacity.has_value();
    if (input.present) {
      const CapacitySnapshot& snapshot = context.evidence.capacity.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
      evaluation.capacity_digest = input.digest;
    }
    input.pinned = request.pinned_generation(EvidenceKind::capacity);
    if (accept_evidence(evaluation, context, input, BlockerCode::capacity_missing, BlockerCode::capacity_stale,
                        BlockerCode::capacity_unknown)) {
      evaluation.accepted.capacity = &context.evidence.capacity.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::redundancy;
    input.present = context.evidence.redundancy.has_value();
    if (input.present) {
      const RedundancySnapshot& snapshot = context.evidence.redundancy.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
    }
    input.pinned = request.pinned_generation(EvidenceKind::redundancy);
    if (accept_evidence(evaluation, context, input, BlockerCode::redundancy_missing,
                        BlockerCode::redundancy_stale, BlockerCode::redundancy_unknown)) {
      evaluation.accepted.redundancy = &context.evidence.redundancy.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::tenant;
    input.present = context.evidence.tenant.has_value();
    if (input.present) {
      const TenantSnapshot& snapshot = context.evidence.tenant.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
      if (valid.ok() && !(snapshot.tenant == request.tenant)) {
        input.valid = false;
        input.failure = Error(ErrorCode::state_unverified, "the tenant snapshot names another tenant");
      }
    }
    input.pinned = request.pinned_generation(EvidenceKind::tenant);
    if (accept_evidence(evaluation, context, input, BlockerCode::tenant_missing, BlockerCode::tenant_stale,
                        BlockerCode::tenant_unknown)) {
      evaluation.accepted.tenant = &context.evidence.tenant.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::envelope;
    input.present = context.evidence.envelope.has_value();
    if (input.present) {
      const EnvelopeSnapshot& snapshot = context.evidence.envelope.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      if (valid.ok()) {
        if (!(snapshot.envelope == request.envelope) || !(snapshot.tenant == request.tenant)) {
          input.valid = false;
          input.failure = Error(ErrorCode::state_unverified, "the envelope snapshot names another entitlement");
        }
      }
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
    }
    input.pinned = request.pinned_generation(EvidenceKind::envelope);
    if (accept_evidence(evaluation, context, input, BlockerCode::envelope_missing,
                        BlockerCode::envelope_stale, BlockerCode::envelope_unknown)) {
      evaluation.accepted.envelope = &context.evidence.envelope.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::service_class;
    input.present = context.evidence.service_class.has_value();
    if (input.present) {
      const ServiceClassSnapshot& snapshot = context.evidence.service_class.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      if (valid.ok() && !(snapshot.service_class == request.service_class)) {
        input.valid = false;
        input.failure = Error(ErrorCode::state_unverified, "the service class snapshot names another class");
      }
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
    }
    input.pinned = request.pinned_generation(EvidenceKind::service_class);
    if (accept_evidence(evaluation, context, input, BlockerCode::service_class_missing,
                        BlockerCode::service_class_stale, BlockerCode::service_class_unknown)) {
      evaluation.accepted.service_class = &context.evidence.service_class.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::maintenance;
    input.present = context.evidence.maintenance.has_value();
    if (input.present) {
      const MaintenanceSnapshot& snapshot = context.evidence.maintenance.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
    }
    input.pinned = request.pinned_generation(EvidenceKind::maintenance);
    if (accept_evidence(evaluation, context, input, BlockerCode::maintenance_missing,
                        BlockerCode::maintenance_stale, BlockerCode::maintenance_unknown)) {
      evaluation.accepted.maintenance = &context.evidence.maintenance.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::incident;
    input.present = context.evidence.incident.has_value();
    if (input.present) {
      const IncidentSnapshot& snapshot = context.evidence.incident.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
    }
    input.pinned = request.pinned_generation(EvidenceKind::incident);
    if (accept_evidence(evaluation, context, input, BlockerCode::incident_missing,
                        BlockerCode::incident_stale, BlockerCode::incident_unknown)) {
      evaluation.accepted.incident = &context.evidence.incident.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::placement;
    input.present = context.evidence.placement.has_value();
    if (input.present) {
      const PlacementPolicySnapshot& snapshot = context.evidence.placement.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
    }
    input.pinned = request.pinned_generation(EvidenceKind::placement);
    if (accept_evidence(evaluation, context, input, BlockerCode::placement_missing,
                        BlockerCode::placement_stale, BlockerCode::placement_unknown)) {
      evaluation.accepted.placement = &context.evidence.placement.value();
    }
  }
  {
    EvidenceInput input;
    input.kind = EvidenceKind::policy;
    input.present = context.evidence.policy.has_value();
    if (input.present) {
      const PolicySnapshot& snapshot = context.evidence.policy.value();
      const Status valid = snapshot.validate();
      input.valid = valid.ok();
      if (!valid.ok()) input.failure = valid.error();
      input.generation = snapshot.generation.value();
      input.digest = digest_of(snapshot);
      input.observed_at = snapshot.observed_at;
    }
    input.pinned = request.pinned_generation(EvidenceKind::policy);
    if (accept_evidence(evaluation, context, input, BlockerCode::policy_missing, BlockerCode::policy_stale,
                        BlockerCode::policy_unknown)) {
      evaluation.accepted.policy = &context.evidence.policy.value();
    }
  }

  // -----------------------------------------------------------------------
  // Pass B: structural checks first.
  // -----------------------------------------------------------------------
  const Status valid = request.validate();
  if (!valid.ok()) {
    raise_blocker(evaluation.blockers, BlockerCode::request_invalid, valid.error().to_string());
  }
  if (request.commitment_start.is_unset() || request.commitment_end.is_unset()) {
    raise_blocker(evaluation.blockers, BlockerCode::commitment_window_unspecified,
                            "the request does not state the window the commitment covers");
  } else {
    auto horizon = context.policy.max_commitment_duration;
    if (request.commitment_duration().nanos() > horizon.nanos()) {
      raise_blocker(evaluation.blockers, BlockerCode::horizon_exceeded,
                              "the commitment window is longer than the configured maximum");
    }
    if (request.commitment_end <= now) {
      raise_blocker(evaluation.blockers, BlockerCode::request_expired,
                              "the commitment window ends before the evaluation time");
    }
  }
  if (!request.demand.all_present()) {
    raise_blocker(evaluation.blockers, BlockerCode::demand_unspecified,
                            "the demand does not state: " + join_dimensions(request.demand.missing()));
  }
  if (request.expected_epoch.has_value() && !(request.expected_epoch.value() == context.epoch)) {
    raise_blocker(evaluation.blockers, BlockerCode::control_epoch_superseded,
                            "the request was planned against control epoch " +
                                request.expected_epoch.value().to_string() + " but the current epoch is " +
                                context.epoch.to_string());
  }
  {
    auto age = age_of(request.requested_at, now);
    if (!age.ok()) {
      raise_blocker(evaluation.blockers, BlockerCode::evidence_future_dated,
                              "the request is dated after the evaluation clock");
    } else if (age.value().nanos() > policy.max_request_age.nanos()) {
      raise_blocker(evaluation.blockers, BlockerCode::request_expired,
                              "the request is " + age.value().to_string() + " old, beyond the configured maximum");
    }
  }

  // -----------------------------------------------------------------------
  // Tenancy.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.tenant != nullptr) {
    const TenantSnapshot& tenant = *evaluation.accepted.tenant;
    note(evaluation, "tenant " + tenant.tenant.to_string() + " status " + to_string(tenant.status) +
                         " at generation " + tenant.generation.to_string(),
         explanation_limit);
    switch (tenant.status) {
      case TenantStatus::active:
        break;
      case TenantStatus::suspended:
      case TenantStatus::closed:
        raise_blocker(evaluation.blockers, BlockerCode::tenant_not_active,
                                std::string("the tenant is ") + to_string(tenant.status));
        break;
      case TenantStatus::unknown:
        raise_blocker(evaluation.blockers, BlockerCode::tenant_unknown,
                                "the tenant status is unknown, which is not an active tenant");
        break;
    }
  }

  // -----------------------------------------------------------------------
  // Envelope.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.envelope != nullptr) {
    const EnvelopeSnapshot& envelope = *evaluation.accepted.envelope;
    for (const Dimension dimension : all_dimensions()) {
      const auto limit = envelope.limit.get(dimension);
      const auto consumed = envelope.consumed.get(dimension);
      const auto demand = request.demand.get(dimension);
      if (!limit.has_value() || !consumed.has_value()) {
        raise_blocker(evaluation.blockers, BlockerCode::envelope_unknown,
                                std::string("the envelope does not state the ") + to_string(dimension) +
                                    " limit and consumption",
                                dimension);
        continue;
      }
      if (!demand.has_value()) {
        continue;  // already reported as demand_unspecified
      }
      if (demand.value() > limit.value()) {
        raise_blocker(evaluation.blockers, BlockerCode::envelope_limit_exceeded,
                                std::string("the demand of ") + std::to_string(demand.value()) + " " +
                                    unit_label(dimension) + " exceeds the envelope limit of " +
                                    std::to_string(limit.value()),
                                dimension);
        continue;
      }
      if (consumed.value() > limit.value() ||
          demand.value() > limit.value() - consumed.value()) {
        raise_blocker(evaluation.blockers, BlockerCode::envelope_exhausted,
                                std::string("the envelope holds ") + std::to_string(consumed.value()) + " of " +
                                    std::to_string(limit.value()) + " " + unit_label(dimension) +
                                    ", which does not cover the demand",
                                dimension);
        continue;
      }
      note(evaluation, std::string("envelope ") + to_string(dimension) + ": limit " +
                           std::to_string(limit.value()) + ", consumed " + std::to_string(consumed.value()) +
                           ", demand " + std::to_string(demand.value()),
           explanation_limit);
    }
  }

  // -----------------------------------------------------------------------
  // Facility policy.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.policy != nullptr) {
    const PolicySnapshot& snapshot = *evaluation.accepted.policy;
    note(evaluation, std::string("facility policy ") + to_string(snapshot.verdict) + " at generation " +
                         snapshot.generation.to_string() + ", overcommit " +
                         to_string(snapshot.overcommit.mode),
         explanation_limit);
    switch (snapshot.verdict) {
      case PolicyVerdict::permit:
        break;
      case PolicyVerdict::deny:
        raise_blocker(evaluation.blockers, BlockerCode::policy_denied, "the facility policy denied this commitment");
        break;
      case PolicyVerdict::abstain:
        raise_blocker(evaluation.blockers, BlockerCode::policy_unknown,
                                "the facility policy returned no decision for this commitment");
        break;
    }
  }

  // -----------------------------------------------------------------------
  // Placement policy.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.placement != nullptr) {
    const PlacementPolicySnapshot& placement = *evaluation.accepted.placement;
    const bool rack_named = request.scope.rack.has_value();
    if (rack_named) {
      const RackId rack = request.scope.rack.value();
      if (contains_id(placement.forbidden_racks, rack)) {
        raise_blocker(evaluation.blockers, BlockerCode::placement_forbidden,
                                "the placement policy forbids rack " + rack.to_string());
      } else if (!placement.allowed_racks.empty() && !contains_id(placement.allowed_racks, rack)) {
        raise_blocker(evaluation.blockers, BlockerCode::placement_forbidden,
                                "the placement policy does not allow rack " + rack.to_string());
      }
    } else if (!placement.allowed_racks.empty() || !placement.forbidden_racks.empty() ||
               placement.max_units_per_rack.has_value()) {
      raise_blocker(evaluation.blockers, BlockerCode::placement_unknown,
                              "the placement policy constrains racks and the request names none");
    }
    if (placement.required_zone.has_value()) {
      if (!request.scope.zone.has_value() ||
          !(request.scope.zone.value() == placement.required_zone.value())) {
        raise_blocker(evaluation.blockers, BlockerCode::placement_forbidden,
                                "the placement policy requires zone " +
                                    placement.required_zone.value().to_string());
      }
    }
    const auto slots = request.demand.get(Dimension::slots);
    if (placement.max_units_per_rack.has_value() && rack_named && slots.has_value()) {
      ConsumptionQuery query;
      query.facility = facility;
      query.rack = request.scope.rack;
      query.observed_after = Timestamp{};
      const std::uint64_t used = context.ledger.consumed(Dimension::slots, query);
      std::uint64_t proposed = 0;
      if (add_overflow(used, slots.value(), proposed) || proposed > placement.max_units_per_rack.value()) {
        raise_blocker(evaluation.blockers, BlockerCode::placement_limit_exceeded,
                                "rack " + request.scope.rack->to_string() + " would hold " +
                                    std::to_string(proposed) + " units against a limit of " +
                                    std::to_string(placement.max_units_per_rack.value()));
      }
    }
    if (placement.max_units_per_facility.has_value() && slots.has_value()) {
      ConsumptionQuery query;
      query.facility = facility;
      query.observed_after = Timestamp{};
      const std::uint64_t used = context.ledger.consumed(Dimension::slots, query);
      std::uint64_t proposed = 0;
      if (add_overflow(used, slots.value(), proposed) ||
          proposed > placement.max_units_per_facility.value()) {
        raise_blocker(evaluation.blockers, BlockerCode::placement_limit_exceeded,
                                "the facility would hold " + std::to_string(proposed) +
                                    " units against a limit of " +
                                    std::to_string(placement.max_units_per_facility.value()));
      }
    }
  }

  // -----------------------------------------------------------------------
  // Redundancy obligation.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.redundancy != nullptr && evaluation.accepted.service_class != nullptr) {
    const RedundancySnapshot& redundancy = *evaluation.accepted.redundancy;
    const ServiceClassObligations& obligations = evaluation.accepted.service_class->obligations;
    if (obligations.minimum_redundancy_stated) {
      if (!redundancy.level_stated) {
        raise_blocker(evaluation.blockers, BlockerCode::redundancy_unknown,
                                "the redundancy level is unmeasured and the service class requires " +
                                    std::string(to_string(obligations.minimum_redundancy)));
      } else if (!redundancy_at_least(redundancy.level, obligations.minimum_redundancy)) {
        raise_blocker(evaluation.blockers, BlockerCode::redundancy_level_insufficient,
                                std::string("the facility is at ") + to_string(redundancy.level) +
                                    " and the service class requires " +
                                    to_string(obligations.minimum_redundancy));
      }
    }
    note(evaluation, std::string("redundancy level ") +
                         (redundancy.level_stated ? to_string(redundancy.level) : "unmeasured") +
                         " at generation " + redundancy.generation.to_string(),
         explanation_limit);
  }

  // -----------------------------------------------------------------------
  // Maintenance exposure.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.maintenance != nullptr) {
    const MaintenanceSnapshot& maintenance = *evaluation.accepted.maintenance;
    for (const MaintenanceWindow& window : maintenance.windows) {
      if (!window.scope.affects(request.scope)) {
        continue;
      }
      const bool active_now =
          window.state == MaintenanceState::active && window.start <= now && now < window.end;
      const bool overlaps =
          !request.commitment_start.is_unset() && !request.commitment_end.is_unset() &&
          window.overlaps(request.commitment_start, request.commitment_end);
      if (!active_now && !overlaps) {
        continue;
      }
      if (window.state == MaintenanceState::unknown) {
        raise_blocker(evaluation.blockers, BlockerCode::maintenance_state_unknown,
                                "a maintenance window covering this scope has an unknown state");
        continue;
      }
      if (window.impact == MaintenanceImpact::unknown) {
        raise_blocker(evaluation.blockers, BlockerCode::maintenance_unknown,
                                "a maintenance window covering this scope has an unknown impact");
        continue;
      }
      if (window.state == MaintenanceState::completed || window.impact == MaintenanceImpact::none) {
        continue;
      }
      if (active_now) {
        if (window.impact == MaintenanceImpact::full_outage) {
          raise_blocker(evaluation.blockers, BlockerCode::maintenance_outage,
                                  "maintenance window " + window.window.to_string() +
                                      " is removing capacity in this scope now");
        } else {
          raise_blocker(evaluation.blockers, BlockerCode::maintenance_active,
                                  "maintenance window " + window.window.to_string() +
                                      " is reducing capacity in this scope now");
        }
      }
      if (overlaps) {
        raise_blocker(evaluation.blockers, BlockerCode::maintenance_exposure,
                                "maintenance window " + window.window.to_string() +
                                    " overlaps the commitment window in this scope");
      }
    }
  }

  // -----------------------------------------------------------------------
  // Incident state.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.incident != nullptr) {
    const IncidentSnapshot& incident = *evaluation.accepted.incident;
    if (incident.scope.affects(request.scope)) {
      switch (incident.state) {
        case IncidentState::normal:
          note(evaluation, "incident state normal at generation " + incident.generation.to_string(),
               explanation_limit);
          break;
        case IncidentState::major:
          raise_blocker(evaluation.blockers, BlockerCode::incident_major,
                                  "a major incident covers this scope");
          break;
        case IncidentState::degraded:
          if (incident.capacity_trust != CapacityTrust::trusted) {
            raise_blocker(evaluation.blockers, BlockerCode::incident_degraded,
                                    std::string("the scope is degraded and capacity is ") +
                                        to_string(incident.capacity_trust));
          }
          break;
        case IncidentState::unknown:
          raise_blocker(evaluation.blockers, BlockerCode::incident_state_unknown,
                                  "incident state for this scope was never observed");
          break;
      }
    }
  }

  // -----------------------------------------------------------------------
  // Capacity arithmetic and overcommit.
  // -----------------------------------------------------------------------
  if (evaluation.accepted.capacity != nullptr) {
    const CapacitySnapshot& capacity = *evaluation.accepted.capacity;
    const ServiceClassObligations* obligations =
        evaluation.accepted.service_class != nullptr ? &evaluation.accepted.service_class->obligations
                                                    : nullptr;
    const RedundancySnapshot* redundancy = evaluation.accepted.redundancy;

    ConsumptionQuery query;
    query.facility = facility;
    query.rack = request.scope.rack;
    query.observed_after = capacity.observed_at;

    for (const Dimension dimension : all_dimensions()) {
      DimensionAssessment entry;
      entry.dimension = dimension;
      entry.total = capacity.total.get(dimension);
      entry.committed = capacity.committed.get(dimension);
      entry.reserved = capacity.reserved.get(dimension);
      entry.demand = request.demand.get(dimension);
      bool complete = true;
      do {

      if (!entry.total.has_value() || !entry.committed.has_value() || !entry.reserved.has_value()) {
        raise_blocker(evaluation.blockers, BlockerCode::capacity_unknown,
                                std::string("the capacity snapshot does not state total, committed and "
                                            "reserved for ") +
                                    to_string(dimension),
                                dimension);
        complete = false;
        break;
      }

      std::uint64_t already_committed = 0;
      if (add_overflow(entry.committed.value(), entry.reserved.value(), already_committed) ||
          already_committed > entry.total.value()) {
        raise_blocker(evaluation.blockers, BlockerCode::capacity_unknown,
                                std::string("the capacity snapshot reports more committed and reserved ") +
                                    to_string(dimension) + " than the total",
                                dimension);
        complete = false;
        break;
      }

      std::uint64_t available = entry.total.value() - already_committed;
      const std::uint64_t local = context.ledger.consumed(dimension, query);
      entry.protected_headroom = std::nullopt;
      if (local >= available) {
        available = 0;
      } else {
        available -= local;
      }
      entry.available = available;

      if (!entry.demand.has_value()) {
        complete = false;
        break;
      }
      const std::int64_t remaining =
          static_cast<std::int64_t>(available) - static_cast<std::int64_t>(entry.demand.value());
      entry.remaining = remaining;

      if (remaining < 0) {
        const auto need = static_cast<std::uint64_t>(-remaining);
        entry.overcommit = need;
        bool service_class_allows = obligations != nullptr && obligations->permits_overcommit;
        if (!service_class_allows) {
          raise_blocker(evaluation.blockers, BlockerCode::capacity_exhausted,
                                  std::string("available ") + to_string(dimension) + " is " +
                                      std::to_string(available) + " " + unit_label(dimension) +
                                      " against a demand of " + std::to_string(entry.demand.value()),
                                  dimension);
        } else if (evaluation.accepted.policy == nullptr) {
          raise_blocker(evaluation.blockers, BlockerCode::overcommit_not_permitted,
                                  std::string("overcommit of ") + to_string(dimension) +
                                      " is needed and no facility policy permits it",
                                  dimension);
        } else {
          const OvercommitAllowance& allowance = evaluation.accepted.policy->overcommit;
          switch (allowance.mode) {
            case OvercommitMode::not_permitted:
              raise_blocker(evaluation.blockers, BlockerCode::overcommit_not_permitted,
                                      std::string("overcommit of ") + std::to_string(need) + " " +
                                          unit_label(dimension) +
                                          " is needed and the facility policy forbids overcommit",
                                      dimension);
              break;
            case OvercommitMode::permitted_unbounded:
              note(evaluation, std::string("overcommit of ") + std::to_string(need) + " " +
                                   unit_label(dimension) + " is permitted without a stated limit",
                   explanation_limit);
              break;
            case OvercommitMode::permitted_up_to: {
              const auto limit = allowance.allowance.get(dimension);
              if (!limit.has_value() || need > limit.value()) {
                raise_blocker(evaluation.blockers, BlockerCode::overcommit_limit_exceeded,
                                        std::string("overcommit of ") + std::to_string(need) + " " +
                                            unit_label(dimension) +
                                            " exceeds the permitted allowance",
                                        dimension);
              } else {
                note(evaluation, std::string("overcommit of ") + std::to_string(need) + " " +
                                     unit_label(dimension) + " is within the permitted allowance of " +
                                     std::to_string(limit.value()),
                     explanation_limit);
              }
              break;
            }
          }
        }
      } else {
        entry.overcommit = 0;
      }

      // Protected headroom floor: what the service class requires and what the
      // redundancy posture reports must both still be there after the demand.
      std::uint64_t floor = 0;
      bool floor_measured = true;
      if (obligations != nullptr) {
        const auto required = obligations->required_protected_headroom.get(dimension);
        if (required.has_value()) {
          floor = std::max(floor, required.value());
        }
      }
      if (redundancy != nullptr) {
        const auto reported = redundancy->protected_headroom.get(dimension);
        if (reported.has_value()) {
          floor = std::max(floor, reported.value());
        } else if (redundancy->level_stated && redundancy->level != RedundancyLevel::none) {
          floor_measured = false;
        }
      }
      if (!floor_measured) {
        raise_blocker(evaluation.blockers, BlockerCode::redundancy_unknown,
                                std::string("the protected ") + to_string(dimension) +
                                    " headroom is unmeasured while the facility claims redundancy",
                                dimension);
      } else if (floor > 0) {
        entry.protected_headroom = floor;
        const std::uint64_t after =
            remaining > 0 ? static_cast<std::uint64_t>(remaining) : 0;
        if (after < floor) {
          raise_blocker(evaluation.blockers, BlockerCode::protected_headroom_insufficient,
                                  std::string("only ") + std::to_string(after) + " " +
                                      unit_label(dimension) + " would remain against a protected headroom of " +
                                      std::to_string(floor),
                                  dimension);
        }
      }

      note(evaluation,
           std::string("capacity ") + to_string(dimension) + ": total " + std::to_string(entry.total.value()) +
               ", committed " + std::to_string(entry.committed.value()) + ", reserved " +
               std::to_string(entry.reserved.value()) + ", ledger " + std::to_string(local) +
               ", available " + std::to_string(available) + ", demand " +
               std::to_string(entry.demand.value()) + ", remaining " + std::to_string(remaining) +
               (entry.overcommit.value_or(0) > 0
                    ? (", overcommit " + std::to_string(entry.overcommit.value_or(0)))
                    : std::string()),
           explanation_limit);
      } while (false);

      // Exactly one assessment entry per dimension, in canonical order.
      (void)evaluation.assessment.add(entry);
      (void)complete;
    }
  } else {
    // Without usable capacity evidence the assessment still states what was
    // asked for, so a refusal or a deferral never silently omits the demand.
    for (const Dimension dimension : all_dimensions()) {
      DimensionAssessment entry;
      entry.dimension = dimension;
      entry.demand = request.demand.get(dimension);
      (void)evaluation.assessment.add(entry);
    }
  }

  // -----------------------------------------------------------------------
  // Verdict.
  // -----------------------------------------------------------------------
  Decision decision;
  decision.request_id = request.request_id;
  decision.request_digest = request.digest();
  decision.verdict = evaluation.blockers.implied_verdict();
  decision.blockers = evaluation.blockers;
  decision.control_epoch = context.epoch;
  decision.sequence = context.sequence;
  decision.decided_at = now;
  decision.evidence = evaluation.evidence;
  decision.assessment = evaluation.assessment;
  decision.explanation = evaluation.explanation;

  if (decision.verdict == Verdict::allow) {
    Grant grant;
    grant.request_id = request.request_id;
    grant.request_digest = decision.request_digest;
    grant.revision = GrantRevision(1);
    grant.control_epoch = context.epoch;
    grant.sequence = context.sequence;
    grant.issued_at = now;
    auto expires = now.checked_add(policy.grant_validity);
    if (!expires.ok()) {
      return expires.error();
    }
    grant.expires_at = expires.value();
    grant.scope = request.scope;
    grant.tenant = request.tenant;
    grant.demand = request.demand;
    grant.holds_capacity = policy.grant_holds_capacity;
    for (const auto& reference : decision.evidence.refs()) {
      if (!reference.accepted) {
        continue;
      }
      GrantBinding binding;
      binding.kind = reference.kind;
      binding.generation = reference.generation;
      binding.evidence_digest = reference.digest;
      grant.bindings.push_back(binding);
    }

    // The grant identity is derived from content, so the same request evaluated
    // against the same evidence at the same epoch always names the same grant.
    codec::Writer seed;
    seed.text("facility-admission-control/grant/v1");
    seed.ident(request.request_id);
    seed.digest(grant.request_digest);
    seed.counter(context.epoch);
    seed.counter(context.sequence);
    const Digest256 derived = Digest256::of(seed.span());
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (int i = 0; i < 8; ++i) {
      high = (high << 8) | static_cast<std::uint64_t>(
                               std::to_integer<std::uint8_t>(derived.bytes()[static_cast<std::size_t>(i)]));
      low = (low << 8) | static_cast<std::uint64_t>(
                             std::to_integer<std::uint8_t>(derived.bytes()[static_cast<std::size_t>(i) + 8]));
    }
    if (high == 0 && low == 0) {
      low = 1;
    }
    grant.grant_id = GrantId(high, low);
    grant.binding_digest = grant.compute_binding_digest();
    decision.grant = grant;
  }

  decision.digest = decision.compute_digest();
  return decision;
}

}  // namespace fac::detail