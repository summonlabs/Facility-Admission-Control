// Facility Admission Control - evaluation policy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/engine/policy.hpp"

namespace fac {

Duration FreshnessBudgets::for_kind(EvidenceKind kind) const {
  switch (kind) {
    case EvidenceKind::capacity: return capacity;
    case EvidenceKind::redundancy: return redundancy;
    case EvidenceKind::tenant: return tenant;
    case EvidenceKind::envelope: return envelope;
    case EvidenceKind::service_class: return service_class;
    case EvidenceKind::maintenance: return maintenance;
    case EvidenceKind::incident: return incident;
    case EvidenceKind::placement: return placement;
    case EvidenceKind::policy: return policy;
  }
  return Duration{};
}

Status FreshnessBudgets::validate() const {
  const Duration budgets[] = {capacity, redundancy, tenant,     envelope, service_class,
                              maintenance, incident, placement, policy};
  for (const Duration budget : budgets) {
    if (budget.nanos() < 0) {
      return make_error(ErrorCode::invalid_argument, "a freshness budget is negative");
    }
  }
  return Status::success();
}

bool EvidenceRequirements::needs(EvidenceKind kind) const {
  switch (kind) {
    case EvidenceKind::capacity: return capacity;
    case EvidenceKind::redundancy: return redundancy;
    case EvidenceKind::tenant: return tenant;
    case EvidenceKind::envelope: return envelope;
    case EvidenceKind::service_class: return service_class;
    case EvidenceKind::maintenance: return maintenance;
    case EvidenceKind::incident: return incident;
    case EvidenceKind::placement: return placement;
    case EvidenceKind::policy: return policy;
  }
  return true;
}

Status EvidenceRequirements::validate() const {
  // Nothing to check structurally: every flag is a boolean decision by the
  // caller. The function exists so a future requirement (for example a minimum
  // set of authorities) has one place to live, and so callers can rely on the
  // policy being validated as a whole.
  return Status::success();
}

Status AdmissionPolicy::validate() const {
  const Status budgets = freshness.validate();
  if (!budgets.ok()) {
    return budgets.error();
  }
  const Status requirements = required.validate();
  if (!requirements.ok()) {
    return requirements.error();
  }
  if (max_future_skew.nanos() < 0 || grant_validity.nanos() <= 0 || max_request_age.nanos() < 0 ||
      max_commitment_duration.nanos() <= 0 || intent_validity.nanos() <= 0) {
    return make_error(ErrorCode::invalid_argument, "admission policy carries an unusable duration");
  }
  if (max_explanation_lines == 0 || max_explanation_lines > 1024) {
    return make_error(ErrorCode::invalid_argument, "admission policy explanation bound is unusable");
  }
  return Status::success();
}

}  // namespace fac
