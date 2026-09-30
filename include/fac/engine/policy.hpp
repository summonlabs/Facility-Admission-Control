// Facility Admission Control - evaluation policy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This policy is the caller's configuration of the evaluator, not the facility
// policy. The facility policy is evidence: it arrives from the Facility Policy
// Engine as a generation-stamped snapshot. What is configured here is which
// evidence is required, how old each authority's answer may be, how long a
// grant stays valid and whether a grant holds capacity in this ledger while it
// waits to be committed.

#ifndef FAC_ENGINE_POLICY_HPP
#define FAC_ENGINE_POLICY_HPP

#include <cstddef>
#include <cstdint>

#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/model/evidence.hpp"

namespace fac {

// How old an authority's answer may be before the evaluator refuses to conclude
// from it. A budget of zero means "this authority is not consulted for age",
// which is only usable for evidence the caller also marks as not required.
struct FreshnessBudgets {
  Duration capacity = Duration::from_seconds(300);
  Duration redundancy = Duration::from_seconds(300);
  Duration tenant = Duration::from_seconds(3600);
  Duration envelope = Duration::from_seconds(3600);
  Duration service_class = Duration::from_seconds(86400);
  Duration maintenance = Duration::from_seconds(300);
  Duration incident = Duration::from_seconds(120);
  Duration placement = Duration::from_seconds(86400);
  Duration policy = Duration::from_seconds(300);

  [[nodiscard]] Duration for_kind(EvidenceKind kind) const;
  [[nodiscard]] Status validate() const;
};

struct EvidenceRequirements {
  bool capacity = true;
  bool redundancy = true;
  bool tenant = true;
  bool envelope = true;
  bool service_class = true;
  bool maintenance = true;
  bool incident = true;
  bool placement = true;
  bool policy = true;

  // Named 'needs' rather than 'requires', which is a C++20 keyword.
  [[nodiscard]] bool needs(EvidenceKind kind) const;
  [[nodiscard]] Status validate() const;
};

struct AdmissionPolicy {
  FreshnessBudgets freshness;
  EvidenceRequirements required;

  // A snapshot observed more than this far in the future is not evidence: it is
  // either a clock problem or a forgery, and neither may authorize capacity.
  Duration max_future_skew = Duration::from_seconds(60);

  // How long an issued grant remains usable before it must be evaluated again.
  Duration grant_validity = Duration::from_seconds(300);

  // How old a request may be when it is evaluated.
  Duration max_request_age = Duration::from_seconds(300);

  // How long the emitted reservation intent stays valid for the owner to
  // answer. A provisional commitment whose window passes without evidence is
  // expired deterministically rather than holding capacity forever.
  Duration intent_validity = Duration::from_seconds(600);

  // The longest commitment window the evaluator will authorize.
  Duration max_commitment_duration = Duration::from_seconds(10 * 365 * 24 * 3600);

  // A grant holds its demand against this ledger until it is committed,
  // released or expired. With this off, a grant is a pure point-in-time
  // decision and competing grants are resolved at commit time instead.
  bool grant_holds_capacity = true;

  // The authority that owns reservation for the facilities this runtime
  // decides for. A commit emits an intent addressed to it, and nothing here
  // executes the reservation itself.
  OwnerId reservation_owner;

  // Analysis mode: a grant is issued with an intent but the caller must resolve
  // the outcome explicitly. This never changes the verdict.
  std::size_t max_explanation_lines = 64;

  [[nodiscard]] Status validate() const;
};

}  // namespace fac

#endif  // FAC_ENGINE_POLICY_HPP
