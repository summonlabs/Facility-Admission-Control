// Facility Admission Control - ledger record payload encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal only. One encode and one decode per record kind, used by the live
// path and by replay, so the two can never disagree about what a record means.

#ifndef FAC_LEDGER_RECORDS_HPP
#define FAC_LEDGER_RECORDS_HPP

#include <span>
#include <vector>

#include "fac/core/error.hpp"
#include "fac/durable/format.hpp"
#include "fac/ledger/ledger.hpp"

namespace fac::detail {

struct DecodedRecord {
  durable::RecordKind kind = durable::RecordKind::decision_recorded;
  DecisionRecorded decision;
  GrantCommitted committed;
  GrantFenced fenced;
  GrantExpired expired;
  GrantReleased released;
  CommitmentEvidenceRecorded evidence;
  CommitmentReleased released_commitment;
  CommitmentExpired expired_commitment;
};

[[nodiscard]] std::vector<std::byte> encode_decision_recorded(const DecisionEntry& entry);
[[nodiscard]] std::vector<std::byte> encode_grant_committed(const GrantCommitted& record);
[[nodiscard]] std::vector<std::byte> encode_grant_fenced(const GrantFenced& record);
[[nodiscard]] std::vector<std::byte> encode_grant_expired(const GrantExpired& record);
[[nodiscard]] std::vector<std::byte> encode_grant_released(const GrantReleased& record);
[[nodiscard]] std::vector<std::byte> encode_commitment_evidence(const CommitmentEvidenceRecorded& record);
[[nodiscard]] std::vector<std::byte> encode_commitment_released(const CommitmentReleased& record);
[[nodiscard]] std::vector<std::byte> encode_commitment_expired(const CommitmentExpired& record);

[[nodiscard]] Result<DecodedRecord> decode_record(durable::RecordKind kind,
                                                  std::span<const std::byte> payload);

}  // namespace fac::detail

#endif  // FAC_LEDGER_RECORDS_HPP
