// Facility Admission Control - evidence references.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/evidence.hpp"

#include "fac/core/limits.hpp"
#include "detail/enum_codec.hpp"

namespace fac {
namespace {

constexpr std::pair<std::string_view, EvidenceKind> kKindNames[] = {
    {"capacity", EvidenceKind::capacity},       {"redundancy", EvidenceKind::redundancy},
    {"tenant", EvidenceKind::tenant},           {"envelope", EvidenceKind::envelope},
    {"service_class", EvidenceKind::service_class}, {"maintenance", EvidenceKind::maintenance},
    {"incident", EvidenceKind::incident},       {"placement", EvidenceKind::placement},
    {"policy", EvidenceKind::policy},
};

}  // namespace

const char* to_string(EvidenceKind kind) noexcept {
  return detail::enum_to_string(kind, kKindNames, "unknown_evidence");
}

Result<EvidenceKind> evidence_kind_from_string(std::string_view text) {
  return detail::enum_from_string(text, kKindNames, "evidence kind");
}

void EvidenceRef::encode(codec::Writer& writer) const {
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u64(generation);
  writer.digest(digest);
  writer.i64(observed_at.unix_nanos());
  writer.boolean(accepted);
  writer.u16(static_cast<std::uint16_t>(rejection));
}

Result<EvidenceRef> EvidenceRef::decode(codec::Reader& reader) {
  auto kind = detail::read_enum<EvidenceKind>(reader, static_cast<std::uint8_t>(kEvidenceKindCount - 1),
                                              "evidence kind");
  if (!kind.ok()) {
    return kind.error();
  }
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.error();
  }
  auto digest = reader.digest();
  if (!digest.ok()) {
    return digest.error();
  }
  auto observed = reader.i64();
  if (!observed.ok()) {
    return observed.error();
  }
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "evidence observation time is outside the supported range");
  }
  auto accepted = reader.boolean();
  if (!accepted.ok()) {
    return accepted.error();
  }
  auto rejection = reader.u16();
  if (!rejection.ok()) {
    return rejection.error();
  }
  if (rejection.value() > static_cast<std::uint16_t>(ErrorCode::internal)) {
    return make_error(ErrorCode::malformed_input, "evidence carries an unknown rejection code");
  }

  EvidenceRef reference;
  reference.kind = kind.value();
  reference.generation = generation.value();
  reference.digest = digest.value();
  reference.observed_at = Timestamp(observed.value());
  reference.accepted = accepted.value();
  reference.rejection = static_cast<ErrorCode>(rejection.value());
  if (reference.accepted && reference.rejection != ErrorCode::ok) {
    return make_error(ErrorCode::malformed_input, "accepted evidence carries a rejection code");
  }
  if (!reference.accepted && reference.rejection == ErrorCode::ok) {
    return make_error(ErrorCode::malformed_input, "rejected evidence carries no rejection code");
  }
  return reference;
}

Status EvidenceSet::add(const EvidenceRef& reference) {
  if (!refs_.empty()) {
    const auto previous = static_cast<std::uint8_t>(refs_.back().kind);
    const auto current = static_cast<std::uint8_t>(reference.kind);
    if (current < previous) {
      return make_error(ErrorCode::invalid_argument, "evidence must be added in ascending kind order");
    }
    if (current == previous) {
      return make_error(ErrorCode::duplicate_field, "evidence kind already present in the set");
    }
  }
  if (refs_.size() >= kEvidenceKindCount) {
    return make_error(ErrorCode::limit_exceeded, "evidence set is full");
  }
  refs_.push_back(reference);
  return Status::success();
}

bool EvidenceSet::contains(EvidenceKind kind) const { return find(kind) != nullptr; }

const EvidenceRef* EvidenceSet::find(EvidenceKind kind) const {
  for (const auto& reference : refs_) {
    if (reference.kind == kind) {
      return &reference;
    }
  }
  return nullptr;
}

Digest256 EvidenceSet::digest() const {
  codec::Writer writer;
  writer.u32(static_cast<std::uint32_t>(refs_.size()));
  for (const auto& reference : refs_) {
    reference.encode(writer);
  }
  return Digest256::of(writer.span());
}

void EvidenceSet::encode(codec::Writer& writer) const {
  writer.u32(static_cast<std::uint32_t>(refs_.size()));
  for (const auto& reference : refs_) {
    reference.encode(writer);
  }
}

Result<EvidenceSet> EvidenceSet::decode(codec::Reader& reader) {
  // 1 byte kind + 8 generation + 32 digest + 8 observed + 1 accepted + 2 rejection.
  auto count = reader.sequence_count(kEvidenceKindCount, 52);
  if (!count.ok()) {
    return count.error();
  }
  EvidenceSet set;
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto reference = EvidenceRef::decode(reader);
    if (!reference.ok()) {
      return reference.error();
    }
    const Status status = set.add(reference.value());
    if (!status.ok()) {
      return status.error();
    }
  }
  return set;
}

}  // namespace fac
