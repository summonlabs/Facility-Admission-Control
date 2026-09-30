// Facility Admission Control - durable format.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The store is a directory with four files:
//
//   writer.lock    the OS single-writer exclusion
//   manifest.0     manifest slot A
//   manifest.1     manifest slot B
//   journal.facj   append-only frames
//   snapshot.facs  compacted state (optional; written by compaction)
//
// The atomic commit point is the publication of a manifest slot. A frame that
// was appended and flushed but not yet covered by a manifest is not committed
// and is discarded on the next open. A manifest that names more journal bytes
// than the journal holds is a hard failure: the store refuses to guess whether
// the missing bytes were ever written, because either answer would silently
// change which admissions happened.
//
// Two manifest slots are kept so a torn or reverted slot is detectable: the
// higher manifest_generation wins, and a slot that claims less data than the
// other while claiming a newer generation is a rollback and fails closed.

#ifndef FAC_DURABLE_FORMAT_HPP
#define FAC_DURABLE_FORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"

namespace fac::durable {

// "FACJ", "FACS", "FACM" as big-endian 32-bit values.
inline constexpr std::uint32_t kJournalFrameMagic = 0x4641434Au;
inline constexpr std::uint32_t kSnapshotMagic = 0x46414353u;
inline constexpr std::uint32_t kManifestMagic = 0x4641434Du;

inline constexpr std::size_t kManifestBytes = 128;
// A frame is a 20-byte header (magic, kind, flags, reserved field, sequence,
// payload length), the payload, and a 4-byte CRC over everything before it.
inline constexpr std::size_t kFrameHeaderBytes = 20;
// A snapshot is a 28-byte header (magic, revision, reserved field, sequence,
// control epoch, payload length), the payload, and a 4-byte CRC.
inline constexpr std::size_t kSnapshotHeaderBytes = 32;

inline constexpr std::string_view kLockFileName = "writer.lock";
inline constexpr std::string_view kManifestNames[2] = {"manifest.0", "manifest.1"};
inline constexpr std::string_view kJournalFileName = "journal.facj";
inline constexpr std::string_view kSnapshotFileName = "snapshot.facs";

enum class RecordKind : std::uint8_t {
  decision_recorded = 0,
  grant_committed = 1,
  grant_fenced = 2,
  grant_expired = 3,
  grant_released = 4,
  commitment_evidence_recorded = 5,
  commitment_released = 6,
  commitment_expired = 7,
};

inline constexpr std::size_t kRecordKindCount = 8;

[[nodiscard]] const char* to_string(RecordKind kind) noexcept;
[[nodiscard]] Result<RecordKind> record_kind_from_string(std::string_view text);

// A committed frame, as read back from the journal.
struct Frame {
  RecordKind kind = RecordKind::decision_recorded;
  LedgerSequence sequence;
  std::vector<std::byte> payload;
};

// The frame header plus CRC. The CRC covers the header and the payload, so a
// torn write, a flipped bit or a splice from another file is detected before
// the payload is decoded.
void encode_frame_header(codec::Writer& writer, RecordKind kind, LedgerSequence sequence,
                         std::uint32_t payload_length);

struct Manifest {
  std::uint16_t format_revision = 0;
  std::uint64_t manifest_generation = 0;
  std::uint64_t control_epoch = 0;
  std::uint64_t ledger_sequence = 0;
  std::uint64_t journal_length = 0;
  Digest256 journal_digest;
  bool snapshot_present = false;
  std::uint64_t snapshot_sequence = 0;
  Digest256 snapshot_digest;

  [[nodiscard]] std::vector<std::byte> encode() const;
  [[nodiscard]] static Result<Manifest> decode(std::span<const std::byte> bytes);
};

// The snapshot envelope: a header, a canonical ledger-state payload and a CRC.
struct Snapshot {
  std::uint16_t format_revision = 0;
  LedgerSequence sequence;
  ControlEpoch epoch;
  std::vector<std::byte> payload;

  [[nodiscard]] std::vector<std::byte> encode() const;
  [[nodiscard]] static Result<Snapshot> decode(std::span<const std::byte> bytes);
};

}  // namespace fac::durable

#endif  // FAC_DURABLE_FORMAT_HPP
