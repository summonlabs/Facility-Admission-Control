// Facility Admission Control - evidence references.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime composes authoritative snapshots. It never measures anything
// itself. Every snapshot it consumed is recorded as an evidence reference that
// names the authority, the generation, the digest of the exact bytes and the
// observation time, so a decision can be audited later without the original
// input in hand. Rejected evidence is kept too: a refusal is more useful when
// it says which input was rejected and why.

#ifndef FAC_MODEL_EVIDENCE_HPP
#define FAC_MODEL_EVIDENCE_HPP

#include <cstdint>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/time.hpp"

namespace fac {

enum class EvidenceKind : std::uint8_t {
  capacity = 0,
  redundancy = 1,
  tenant = 2,
  envelope = 3,
  service_class = 4,
  maintenance = 5,
  incident = 6,
  placement = 7,
  policy = 8,
};

inline constexpr std::size_t kEvidenceKindCount = 9;

[[nodiscard]] const char* to_string(EvidenceKind kind) noexcept;
[[nodiscard]] Result<EvidenceKind> evidence_kind_from_string(std::string_view text);

struct EvidenceRef {
  EvidenceKind kind = EvidenceKind::capacity;
  std::uint64_t generation = 0;
  Digest256 digest;
  Timestamp observed_at;
  bool accepted = true;
  ErrorCode rejection = ErrorCode::ok;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<EvidenceRef> decode(codec::Reader& reader);

  friend bool operator==(const EvidenceRef&, const EvidenceRef&) = default;
};

// An ordered, duplicate-free evidence set. Entries are added in ascending kind
// order, which makes the canonical encoding independent of evaluation order.
class EvidenceSet {
 public:
  [[nodiscard]] Status add(const EvidenceRef& reference);

  [[nodiscard]] const std::vector<EvidenceRef>& refs() const noexcept { return refs_; }
  [[nodiscard]] bool empty() const noexcept { return refs_.empty(); }
  [[nodiscard]] bool contains(EvidenceKind kind) const;
  [[nodiscard]] const EvidenceRef* find(EvidenceKind kind) const;

  // Canonical digest over the whole set, used to bind a grant to the exact
  // evidence it was decided from.
  [[nodiscard]] Digest256 digest() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<EvidenceSet> decode(codec::Reader& reader);

  friend bool operator==(const EvidenceSet&, const EvidenceSet&) = default;

 private:
  std::vector<EvidenceRef> refs_;
};

}  // namespace fac

#endif  // FAC_MODEL_EVIDENCE_HPP
