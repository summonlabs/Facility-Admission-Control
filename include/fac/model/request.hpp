// Facility Admission Control - the admission request.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A request is an immutable statement of intent: this tenant wants this much
// capacity for this class of service, in this scope, over this window. It
// carries no authority of its own. The digest of its canonical encoding is the
// identity of its content, which is what makes replay safe: the same identity
// with the same digest is the same request, and the same identity with a
// different digest is a conflict rather than a second admission.

#ifndef FAC_MODEL_REQUEST_HPP
#define FAC_MODEL_REQUEST_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/model/evidence.hpp"
#include "fac/model/quantity.hpp"
#include "fac/model/scope.hpp"

namespace fac {

// A caller may pin the exact generation of one authority it evaluated against.
// A pinned generation that no longer matches what the authority reports is a
// staleness refusal, not a silent re-evaluation against newer evidence.
struct GenerationPin {
  EvidenceKind kind = EvidenceKind::capacity;
  std::uint64_t generation = 0;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<GenerationPin> decode(codec::Reader& reader);

  friend bool operator==(const GenerationPin&, const GenerationPin&) = default;
};

struct AdmissionRequest {
  RequestId request_id;
  TenantId tenant;
  ServiceClassId service_class;
  EnvelopeId envelope;
  TargetScope scope;
  AmountVector demand;

  Timestamp requested_at;
  Timestamp commitment_start;
  Timestamp commitment_end;

  // The control epoch the caller believes is current. Absent means the caller
  // asserts nothing and the engine binds the epoch it finds at evaluation.
  std::optional<ControlEpoch> expected_epoch;

  // Optional pins on the generations the caller evaluated against, ordered by
  // evidence kind and unique per kind.
  std::vector<GenerationPin> pins;

  [[nodiscard]] Status validate() const;

  [[nodiscard]] Duration commitment_duration() const;

  [[nodiscard]] std::optional<std::uint64_t> pinned_generation(EvidenceKind kind) const;

  [[nodiscard]] Digest256 digest() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<AdmissionRequest> decode(codec::Reader& reader);

  friend bool operator==(const AdmissionRequest&, const AdmissionRequest&) = default;
};

}  // namespace fac

#endif  // FAC_MODEL_REQUEST_HPP
