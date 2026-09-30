// Facility Admission Control - durable format.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/durable/format.hpp"

#include "fac/core/crc32c.hpp"
#include "fac/core/limits.hpp"
#include "fac/version.hpp"
#include "detail/enum_codec.hpp"

namespace fac::durable {
namespace {

constexpr std::pair<std::string_view, RecordKind> kRecordKindNames[] = {
    {"decision_recorded", RecordKind::decision_recorded},
    {"grant_committed", RecordKind::grant_committed},
    {"grant_fenced", RecordKind::grant_fenced},
    {"grant_expired", RecordKind::grant_expired},
    {"grant_released", RecordKind::grant_released},
    {"commitment_evidence_recorded", RecordKind::commitment_evidence_recorded},
    {"commitment_released", RecordKind::commitment_released},
    {"commitment_expired", RecordKind::commitment_expired},
};

[[nodiscard]] Status check_revision(std::uint16_t revision, const char* what) {
  if (revision != kDurableFormatRevision) {
    return make_error(ErrorCode::format_revision_unsupported,
                      std::string(what) + " was written by an unsupported durable format revision");
  }
  return Status::success();
}

}  // namespace

const char* to_string(RecordKind kind) noexcept {
  return detail::enum_to_string(kind, kRecordKindNames, "unknown_record_kind");
}

Result<RecordKind> record_kind_from_string(std::string_view text) {
  return detail::enum_from_string(text, kRecordKindNames, "record kind");
}

void encode_frame_header(codec::Writer& writer, RecordKind kind, LedgerSequence sequence,
                         std::uint32_t payload_length) {
  writer.u32(kJournalFrameMagic);
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u8(0);  // flags, reserved and required to be zero
  writer.u16(0);  // reserved
  writer.counter(sequence);
  writer.u32(payload_length);
}

std::vector<std::byte> Manifest::encode() const {
  codec::Writer writer;
  writer.reserve(kManifestBytes);
  writer.u32(kManifestMagic);
  writer.u16(format_revision);
  writer.u16(0);
  writer.u64(manifest_generation);
  writer.u64(control_epoch);
  writer.u64(ledger_sequence);
  writer.u64(journal_length);
  writer.digest(journal_digest);
  writer.boolean(snapshot_present);
  writer.u64(snapshot_sequence);
  writer.raw(snapshot_digest.bytes());

  const std::size_t body = writer.size();
  if (body + 4 > kManifestBytes) {
    return {};
  }
  writer.reserved(kManifestBytes - 4 - body);
  const std::uint32_t checksum = crc32c(writer.span());
  writer.u32(checksum);

  auto bytes = writer.data();
  return bytes;
}

Result<Manifest> Manifest::decode(std::span<const std::byte> bytes) {
  if (bytes.size() != kManifestBytes) {
    return make_error(ErrorCode::manifest_invalid, "manifest is not the expected size");
  }
  const std::uint32_t expected_crc =
      crc32c(bytes.subspan(0, kManifestBytes - 4));
  std::uint32_t stored_crc = 0;
  for (int i = 0; i < 4; ++i) {
    stored_crc = (stored_crc << 8) |
                 static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[kManifestBytes - 4 + static_cast<std::size_t>(i)]));
  }
  if (stored_crc != expected_crc) {
    return make_error(ErrorCode::checksum_mismatch, "manifest checksum does not match its content");
  }

  codec::Reader reader(bytes.subspan(0, kManifestBytes - 4));
  auto magic = reader.u32();
  if (!magic.ok()) return magic.error();
  if (magic.value() != kManifestMagic) {
    return make_error(ErrorCode::manifest_invalid, "manifest magic does not match");
  }
  auto revision = reader.u16();
  if (!revision.ok()) return revision.error();
  const Status revision_status = check_revision(revision.value(), "manifest");
  if (!revision_status.ok()) {
    return revision_status.error();
  }
  const Status reserved = reader.reserved(2);
  if (!reserved.ok()) {
    return reserved.error();
  }
  Manifest manifest;
  manifest.format_revision = revision.value();
  auto generation = reader.u64();
  if (!generation.ok()) return generation.error();
  manifest.manifest_generation = generation.value();
  auto epoch = reader.u64();
  if (!epoch.ok()) return epoch.error();
  manifest.control_epoch = epoch.value();
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.error();
  manifest.ledger_sequence = sequence.value();
  auto length = reader.u64();
  if (!length.ok()) return length.error();
  manifest.journal_length = length.value();
  if (manifest.journal_length > kMaxJournalBytes) {
    return make_error(ErrorCode::manifest_invalid, "manifest declares a journal longer than the maximum");
  }
  auto journal_digest = reader.digest();
  if (!journal_digest.ok()) return journal_digest.error();
  manifest.journal_digest = journal_digest.value();
  auto present = reader.boolean();
  if (!present.ok()) return present.error();
  manifest.snapshot_present = present.value();
  auto snapshot_sequence = reader.u64();
  if (!snapshot_sequence.ok()) return snapshot_sequence.error();
  manifest.snapshot_sequence = snapshot_sequence.value();
  auto snapshot_bytes = reader.raw(Digest256::kBytes);
  if (!snapshot_bytes.ok()) return snapshot_bytes.error();
  Digest256 snapshot_digest;
  std::copy(snapshot_bytes.value().begin(), snapshot_bytes.value().end(), snapshot_digest.bytes().begin());
  manifest.snapshot_digest = snapshot_digest;

  const std::size_t consumed = reader.offset();
  const Status padding = reader.reserved(kManifestBytes - 4 - consumed);
  if (!padding.ok()) {
    return padding.error();
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end.error();
  }

  if (manifest.control_epoch == 0) {
    return make_error(ErrorCode::manifest_invalid, "manifest carries a zero control epoch");
  }
  if (manifest.ledger_sequence > kMaxCounter || manifest.snapshot_sequence > kMaxCounter) {
    return make_error(ErrorCode::manifest_invalid, "manifest carries a counter beyond the supported range");
  }
  if (manifest.snapshot_present) {
    if (manifest.snapshot_digest.is_unset()) {
      return make_error(ErrorCode::manifest_invalid, "manifest names a snapshot without a digest");
    }
    if (manifest.snapshot_sequence > manifest.ledger_sequence) {
      return make_error(ErrorCode::snapshot_ahead_of_journal,
                        "manifest snapshot is ahead of the ledger sequence it commits");
    }
  } else {
    if (!manifest.snapshot_digest.is_unset() || manifest.snapshot_sequence != 0) {
      return make_error(ErrorCode::manifest_invalid, "manifest without a snapshot carries snapshot data");
    }
  }
  return manifest;
}

std::vector<std::byte> Snapshot::encode() const {
  codec::Writer writer;
  writer.reserve(kSnapshotHeaderBytes + payload.size());
  writer.u32(kSnapshotMagic);
  writer.u16(format_revision);
  writer.u16(0);
  writer.counter(sequence);
  writer.counter(epoch);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.raw(payload);
  const std::uint32_t checksum = crc32c(writer.span());
  writer.u32(checksum);
  auto bytes = writer.data();
  return bytes;
}

Result<Snapshot> Snapshot::decode(std::span<const std::byte> bytes) {
  if (bytes.size() < kSnapshotHeaderBytes) {
    return make_error(ErrorCode::truncated_input, "snapshot is shorter than its header");
  }
  const std::uint32_t expected_crc = crc32c(bytes.subspan(0, bytes.size() - 4));
  std::uint32_t stored_crc = 0;
  for (int i = 0; i < 4; ++i) {
    stored_crc = (stored_crc << 8) |
                 static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[bytes.size() - 4 + static_cast<std::size_t>(i)]));
  }
  if (stored_crc != expected_crc) {
    return make_error(ErrorCode::checksum_mismatch, "snapshot checksum does not match its content");
  }

  codec::Reader reader(bytes.subspan(0, bytes.size() - 4));
  auto magic = reader.u32();
  if (!magic.ok()) return magic.error();
  if (magic.value() != kSnapshotMagic) {
    return make_error(ErrorCode::manifest_invalid, "snapshot magic does not match");
  }
  auto revision = reader.u16();
  if (!revision.ok()) return revision.error();
  const Status revision_status = check_revision(revision.value(), "snapshot");
  if (!revision_status.ok()) {
    return revision_status.error();
  }
  const Status reserved = reader.reserved(2);
  if (!reserved.ok()) {
    return reserved.error();
  }
  Snapshot snapshot;
  snapshot.format_revision = revision.value();
  auto sequence = reader.counter<struct LedgerSequenceTag>();
  if (!sequence.ok()) return sequence.error();
  snapshot.sequence = sequence.value();
  auto epoch = reader.counter<struct ControlEpochTag>();
  if (!epoch.ok()) return epoch.error();
  snapshot.epoch = epoch.value();
  auto length = reader.u32();
  if (!length.ok()) return length.error();
  if (static_cast<std::size_t>(length.value()) > kMaxSnapshotBytes) {
    return make_error(ErrorCode::limit_exceeded, "snapshot payload exceeds the configured maximum");
  }
  auto payload = reader.raw(length.value());
  if (!payload.ok()) return payload.error();
  snapshot.payload.assign(payload.value().begin(), payload.value().end());
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end.error();
  }
  if (snapshot.epoch.is_zero()) {
    return make_error(ErrorCode::manifest_invalid, "snapshot carries a zero control epoch");
  }
  return snapshot;
}

}  // namespace fac::durable
