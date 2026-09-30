// Facility Admission Control - evaluation internals.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal only. The evaluation is a fixed sequence of stages, and every stage
// records its blockers into one set. The verdict is derived from the set by the
// documented precedence order, never from the order the stages ran in, so the
// same authoritative inputs always produce the same decision.

#ifndef FAC_ENGINE_EVALUATE_HPP
#define FAC_ENGINE_EVALUATE_HPP

#include "fac/engine/engine.hpp"
#include "fac/engine/policy.hpp"
#include "fac/ledger/ledger.hpp"
#include "fac/model/decision.hpp"
#include "fac/model/request.hpp"

namespace fac::detail {

struct EvaluationContext {
  const Ledger& ledger;
  const AdmissionPolicy& policy;
  const AdmissionRequest& request;
  const EvidenceBundle& evidence;
  ControlEpoch epoch;
  LedgerSequence sequence;
  Timestamp now;
};

// Evaluates one request. Never mutates the ledger and never performs I/O.
[[nodiscard]] Result<Decision> evaluate_request(const EvaluationContext& context);

// Renders the ordered explanation lines for a decision.
[[nodiscard]] std::vector<std::string> render_explanation(const EvaluationContext& context,
                                                          const CommitmentAssessment& assessment);

}  // namespace fac::detail

#endif  // FAC_ENGINE_EVALUATE_HPP
