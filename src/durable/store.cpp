// Facility Admission Control - the durable store.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/durable/store.hpp"

#include <algorithm>

#include "fac/core/limits.hpp"
#include "fac/core/sha256.hpp"
#include "fac/version.hpp"
#include "durable/file_handle.hpp"

namespace fac::durable {
namespace {

struct SlotRead {
  bool present = false;
  bool valid = false;
  Manifest manifest;
  Error error;
};

[[nodiscard]] SlotRead read_slot(const std::string& path) {
  SlotRead result;
  if (!path_exists(path)) {
    return result;
  }
  result.present = true;
  std::vector<std::byte> bytes;
  const Status read = read_file(path, bytes, kManifestBytes * 2);
  if (!read.ok()) {
    result.error = read.error();
    return result;
  }
  auto manifest = Manifest::decode(std::span<const std::byte>(bytes.data(), bytes.size()));
  if (!manifest.ok()) {
    result.error = manifest.error();
    return result;
  }
  result.valid = true;
  result.manifest = manifest.take();
  return result;
}

}  // namespace

Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::open_writer(const std::string& directory,
                                                  const StoreOptions& options) {
  auto store = std::unique_ptr<Store>(new Store());
  StoreOptions effective = options;
  effective.create = options.create || options.advance_epoch;
  const Status status = store->open_common(directory, false, effective);
  if (!status.ok()) {
    return status.error();
  }
  return store;
}

Result<std::unique_ptr<Store>> Store::open_reader(const std::string& directory) {
  auto store = std::unique_ptr<Store>(new Store());
  StoreOptions options;
  options.create = false;
  options.advance_epoch = false;
  const Status status = store->open_common(directory, true, options);
  if (!status.ok()) {
    return status.error();
  }
  return store;
}

Status Store::open_common(const std::string& directory, bool read_only, const StoreOptions& options) {
  auto normalized = normalize_directory(directory);
  if (!normalized.ok()) {
    return normalized.error();
  }
  directory_ = normalized.take();
  read_only_ = read_only;

  if (options.create) {
    const Status created = ensure_directory(directory_);
    if (!created.ok()) {
      return created.error();
    }
  } else if (!path_exists(directory_)) {
    return make_error(ErrorCode::file_not_found, "store directory does not exist");
  }

  const std::string lock_path = join_path(directory_, kLockFileName);
  auto lock = read_only ? WriterLock::acquire_reader(lock_path) : WriterLock::acquire_writer(lock_path);
  if (!lock.ok()) {
    return lock.error();
  }
  lock_ = lock.take();

  // ---------------------------------------------------------------------
  // Manifest selection. Two slots are kept so that a torn or reverted slot is
  // detectable rather than authoritative.
  // ---------------------------------------------------------------------
  SlotRead slots[2];
  for (std::size_t i = 0; i < 2; ++i) {
    slots[i] = read_slot(join_path(directory_, kManifestNames[i]));
  }
  const bool any_present = slots[0].present || slots[1].present;
  const bool any_valid = slots[0].valid || slots[1].valid;

  if (!any_valid) {
    if (any_present) {
      return make_error(ErrorCode::manifest_invalid, "no manifest slot could be verified");
    }
    const bool journal_exists = path_exists(join_path(directory_, kJournalFileName));
    const bool snapshot_exists = path_exists(join_path(directory_, kSnapshotFileName));
    if (journal_exists || snapshot_exists) {
      return make_error(ErrorCode::manifest_missing,
                        "the store holds data but no manifest describes it");
    }
    if (read_only) {
      return make_error(ErrorCode::file_not_found, "the store has never been written");
    }
    manifest_ = Manifest{};
    manifest_.format_revision = static_cast<std::uint16_t>(kDurableFormatRevision);
    manifest_.manifest_generation = 1;
    manifest_.control_epoch = 1;
    manifest_.ledger_sequence = 0;
    manifest_.journal_length = 0;
    manifest_.journal_digest = Digest256::of({});
    epoch_ = ControlEpoch(1);
    sequence_ = LedgerSequence(0);
    report_.created = true;
    report_.notes.push_back("store created");
    const std::string journal_path = join_path(directory_, kJournalFileName);
    auto journal = Journal::open(journal_path, false, 0);
    if (!journal.ok()) {
      return journal.error();
    }
    journal_ = journal.take();
    const Status published =
        publish_manifest(manifest_.manifest_generation, 0, manifest_.journal_digest);
    if (!published.ok()) {
      return published.error();
    }
    report_.epoch = epoch_;
    report_.sequence = sequence_;
    report_.manifest_generation = 1;
    return Status::success();
  }

  std::size_t chosen = slots[0].valid ? 0 : 1;
  if (slots[0].valid && slots[1].valid) {
    if (slots[0].manifest.manifest_generation > slots[1].manifest.manifest_generation) {
      chosen = 0;
    } else if (slots[1].manifest.manifest_generation > slots[0].manifest.manifest_generation) {
      chosen = 1;
    } else {
      const std::vector<std::byte> left = slots[0].manifest.encode();
      const std::vector<std::byte> right = slots[1].manifest.encode();
      if (left != right) {
        return make_error(ErrorCode::manifest_invalid,
                          "two manifest slots share a generation but disagree");
      }
      chosen = 0;
    }
    const Manifest& winner = slots[chosen].manifest;
    const Manifest& other = slots[1 - chosen].manifest;
    // Rollback means the other slot claims a *newer committed state* than the
    // slot with the higher generation. Journal length is deliberately not
    // compared: compaction shrinks the journal while keeping the same committed
    // sequence, so a shorter journal in the newer slot is normal.
    if (other.ledger_sequence > winner.ledger_sequence) {
      return make_error(ErrorCode::rollback_detected,
                        "a manifest slot claims a newer committed sequence than the newer slot");
    }
  } else {
    report_.notes.push_back("recovered from a single manifest slot");
  }
  if (slots[0].present && !slots[0].valid) {
    report_.notes.push_back("manifest slot 0 was unreadable and was ignored");
  }
  if (slots[1].present && !slots[1].valid) {
    report_.notes.push_back("manifest slot 1 was unreadable and was ignored");
  }

  manifest_ = slots[chosen].manifest;
  epoch_ = ControlEpoch(manifest_.control_epoch);
  sequence_ = LedgerSequence(manifest_.ledger_sequence);

  // ---------------------------------------------------------------------
  // Journal: verify the committed region and discard any uncommitted tail.
  // ---------------------------------------------------------------------
  const std::string journal_path = join_path(directory_, kJournalFileName);
  auto actual_size = file_size(journal_path);
  if (!actual_size.ok() && actual_size.error().code != ErrorCode::file_not_found) {
    return actual_size.error();
  }
  const std::uint64_t actual_bytes = actual_size.ok() ? actual_size.value() : 0;
  if (actual_bytes < manifest_.journal_length) {
    return make_error(ErrorCode::journal_truncated,
                      "the journal is shorter than the length the manifest commits");
  }
  report_.discarded_tail_bytes = actual_bytes - manifest_.journal_length;

  auto journal = Journal::open(journal_path, read_only, manifest_.journal_length);
  if (!journal.ok()) {
    return journal.error();
  }
  journal_ = journal.take();
  report_.journal_bytes = journal_->length();

  auto recomputed = journal_->digest_prefix(manifest_.journal_length);
  if (!recomputed.ok()) {
    return recomputed.error();
  }
  if (!(recomputed.value() == manifest_.journal_digest)) {
    return make_error(ErrorCode::digest_mismatch,
                      "the committed journal region does not match the digest the manifest records");
  }

  auto frames = journal_->read_range(0, manifest_.journal_length);
  if (!frames.ok()) {
    return frames.error();
  }
  frames_ = frames.take();
  report_.applied_frames = frames_.size();
  if (!frames_.empty()) {
    if (!(frames_.back().sequence == sequence_)) {
      return make_error(ErrorCode::corrupt_store,
                        "the journal does not end at the sequence the manifest commits");
    }
  } else if (!sequence_.is_zero()) {
    // No frames are present, so every record must be inside the snapshot.
    if (!manifest_.snapshot_present || manifest_.snapshot_sequence != sequence_.value()) {
      return make_error(ErrorCode::corrupt_store,
                        "the manifest commits a sequence that no frame and no snapshot carries");
    }
  }

  // ---------------------------------------------------------------------
  // Snapshot.
  // ---------------------------------------------------------------------
  if (manifest_.snapshot_present) {
    const std::string snapshot_path = join_path(directory_, kSnapshotFileName);
    std::vector<std::byte> bytes;
    const Status read = read_file(snapshot_path, bytes, kMaxSnapshotBytes);
    if (!read.ok()) {
      return read.error().code == ErrorCode::file_not_found
                 ? make_error(ErrorCode::state_unverified,
                              "the manifest names a snapshot that is not present")
                 : read.error();
    }
    const Digest256 digest = Digest256::of(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!(digest == manifest_.snapshot_digest)) {
      return make_error(ErrorCode::digest_mismatch, "the snapshot does not match the manifest digest");
    }
    auto snapshot = Snapshot::decode(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!snapshot.ok()) {
      return snapshot.error();
    }
    if (snapshot.value().sequence.value() != manifest_.snapshot_sequence) {
      return make_error(ErrorCode::corrupt_store,
                        "the snapshot sequence disagrees with the manifest");
    }
    if (snapshot.value().sequence > sequence_) {
      return make_error(ErrorCode::snapshot_ahead_of_journal,
                        "the snapshot is ahead of the sequence the manifest commits");
    }
    snapshot_ = snapshot.take();
    snapshot_digest_ = digest;
    report_.snapshot_loaded = true;
    report_.snapshot_sequence = snapshot_->sequence.value();
  } else if (manifest_.snapshot_sequence != 0) {
    return make_error(ErrorCode::corrupt_store, "the manifest names a snapshot sequence but no snapshot");
  }

  if (!frames_.empty()) {
    const LedgerSequence first = frames_.front().sequence;
    const LedgerSequence expected =
        snapshot_.has_value() ? snapshot_->sequence.checked_next().value() : LedgerSequence(1);
    if (!(first == expected)) {
      return make_error(ErrorCode::corrupt_store,
                        "the journal does not continue from the snapshot sequence");
    }
  }

  report_.epoch = epoch_;
  report_.sequence = sequence_;
  report_.manifest_generation = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(manifest_.manifest_generation, 0xFFFFFFFFull));

  // ---------------------------------------------------------------------
  // A writer incarnation advances the control epoch. Publishing the manifest
  // that carries the new epoch is what makes it authoritative, and it is what
  // fences grants a previous process recorded.
  // ---------------------------------------------------------------------
  if (!read_only && options.advance_epoch) {
    auto next = epoch_.checked_next();
    if (!next.ok()) {
      return next.error();
    }
    epoch_ = next.value();
    const Status published =
        publish_manifest(manifest_.manifest_generation + 1, manifest_.journal_length,
                         manifest_.journal_digest);
    if (!published.ok()) {
      return published.error();
    }
    report_.epoch = epoch_;
    report_.notes.push_back("control epoch advanced on open");
  }
  return Status::success();
}

Status Store::publish_manifest(std::uint64_t manifest_generation, std::uint64_t journal_length,
                               const Digest256& journal_digest) {
  if (read_only_) {
    return make_error(ErrorCode::read_only_store, "the store is open for reading only");
  }
  Manifest next;
  next.format_revision = static_cast<std::uint16_t>(kDurableFormatRevision);
  next.manifest_generation = manifest_generation;
  next.control_epoch = epoch_.value();
  next.ledger_sequence = sequence_.value();
  next.journal_length = journal_length;
  next.journal_digest = journal_digest;
  next.snapshot_present = snapshot_.has_value();
  next.snapshot_sequence = snapshot_.has_value() ? snapshot_->sequence.value() : 0;
  next.snapshot_digest = snapshot_.has_value() ? snapshot_digest_ : Digest256{};

  const std::vector<std::byte> bytes = next.encode();
  if (bytes.size() != kManifestBytes) {
    return make_error(ErrorCode::internal, "manifest encoding is not the declared size");
  }
  const std::size_t slot = static_cast<std::size_t>(manifest_generation % 2);
  const std::string path = join_path(directory_, kManifestNames[slot]);
  const Status written = write_file_atomic(path, std::span<const std::byte>(bytes.data(), bytes.size()));
  if (!written.ok()) {
    return written.error();
  }
  const Status flushed = flush_directory(directory_);
  if (!flushed.ok()) {
    return flushed.error();
  }
  manifest_ = next;
  report_.manifest_generation = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(manifest_generation, 0xFFFFFFFFull));
  return Status::success();
}

Status Store::append(RecordKind kind, LedgerSequence sequence, std::span<const std::byte> payload) {
  if (read_only_) {
    return make_error(ErrorCode::read_only_store, "the store is open for reading only");
  }
  auto expected = sequence_.checked_next();
  if (!expected.ok()) {
    return expected.error();
  }
  if (!(expected.value() == sequence)) {
    return make_error(ErrorCode::conflict, "record sequence is not the next committed sequence");
  }
  const Status appended = journal_->append(kind, sequence, payload);
  if (!appended.ok()) {
    return appended.error();
  }
  // The manifest is published after the frame is durable, and it names the
  // sequence it commits. Advancing the in-memory sequence before publishing is
  // what keeps the manifest and the journal describing the same state.
  sequence_ = sequence;
  const Status published =
      publish_manifest(manifest_.manifest_generation + 1, journal_->length(), journal_->chain_digest());
  if (!published.ok()) {
    return published.error();
  }
  report_.journal_bytes = journal_->length();
  return Status::success();
}

Status Store::publish_snapshot(const Snapshot& snapshot) {
  if (read_only_) {
    return make_error(ErrorCode::read_only_store, "the store is open for reading only");
  }
  if (!(snapshot.sequence == sequence_)) {
    return make_error(ErrorCode::conflict, "snapshot sequence is not the committed sequence");
  }
  Snapshot published_snapshot = snapshot;
  published_snapshot.format_revision = static_cast<std::uint16_t>(kDurableFormatRevision);
  published_snapshot.epoch = epoch_;
  const std::vector<std::byte> bytes = published_snapshot.encode();
  const std::string path = join_path(directory_, kSnapshotFileName);
  const Status written = write_file_atomic(path, std::span<const std::byte>(bytes.data(), bytes.size()));
  if (!written.ok()) {
    return written.error();
  }
  snapshot_ = published_snapshot;
  snapshot_digest_ = Digest256::of(std::span<const std::byte>(bytes.data(), bytes.size()));

  // The manifest that names the snapshot is the commit point for compaction:
  // before it is published the old journal is still authoritative, after it the
  // snapshot is, and the journal bytes that remain are an uncommitted tail.
  const Status manifest_status = publish_manifest(manifest_.manifest_generation + 1, 0, Digest256::of({}));
  if (!manifest_status.ok()) {
    return manifest_status.error();
  }
  const Status reset = journal_->reset();
  if (!reset.ok()) {
    return reset.error();
  }
  frames_.clear();
  report_.journal_bytes = 0;
  report_.snapshot_loaded = true;
  report_.snapshot_sequence = snapshot_->sequence.value();
  return Status::success();
}

Status Store::verify() {
  // Re-runs the whole validation from disk, without trusting anything that is
  // already in memory.
  SlotRead slots[2];
  for (std::size_t i = 0; i < 2; ++i) {
    slots[i] = read_slot(join_path(directory_, kManifestNames[i]));
  }
  if (!slots[0].valid && !slots[1].valid) {
    return make_error(ErrorCode::manifest_invalid, "no manifest slot could be verified");
  }
  std::size_t chosen = slots[0].valid ? 0 : 1;
  if (slots[0].valid && slots[1].valid &&
      slots[1].manifest.manifest_generation > slots[0].manifest.manifest_generation) {
    chosen = 1;
  }
  const Manifest& manifest = slots[chosen].manifest;
  if (slots[0].valid && slots[1].valid) {
    const Manifest& other = slots[1 - chosen].manifest;
    if (other.ledger_sequence > manifest.ledger_sequence) {
      return make_error(ErrorCode::rollback_detected,
                        "a manifest slot claims a newer committed sequence than the newer slot");
    }
  }

  const std::string journal_path = join_path(directory_, kJournalFileName);
  auto size = file_size(journal_path);
  const std::uint64_t actual = size.ok() ? size.value() : 0;
  if (actual < manifest.journal_length) {
    return make_error(ErrorCode::journal_truncated,
                      "the journal is shorter than the length the manifest commits");
  }
  auto journal = Journal::open(journal_path, true, manifest.journal_length);
  if (!journal.ok()) {
    return journal.error();
  }
  auto digest = journal.value()->digest_prefix(manifest.journal_length);
  if (!digest.ok()) {
    return digest.error();
  }
  if (!(digest.value() == manifest.journal_digest)) {
    return make_error(ErrorCode::digest_mismatch, "the committed journal region fails its digest");
  }
  auto frames = journal.value()->read_range(0, manifest.journal_length);
  if (!frames.ok()) {
    return frames.error();
  }
  if (!frames.value().empty() && !(frames.value().back().sequence.value() == manifest.ledger_sequence)) {
    return make_error(ErrorCode::corrupt_store, "the journal does not end at the committed sequence");
  }

  if (manifest.snapshot_present) {
    std::vector<std::byte> bytes;
    const Status read = read_file(join_path(directory_, kSnapshotFileName), bytes, kMaxSnapshotBytes);
    if (!read.ok()) {
      return read.error();
    }
    const Digest256 snapshot_digest = Digest256::of(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!(snapshot_digest == manifest.snapshot_digest)) {
      return make_error(ErrorCode::digest_mismatch, "the snapshot fails its manifest digest");
    }
    auto snapshot = Snapshot::decode(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!snapshot.ok()) {
      return snapshot.error();
    }
    if (snapshot.value().sequence.value() != manifest.snapshot_sequence) {
      return make_error(ErrorCode::corrupt_store, "the snapshot sequence disagrees with the manifest");
    }
  }
  return Status::success();
}

}  // namespace fac::durable
