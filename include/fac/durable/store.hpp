// Facility Admission Control - the durable store.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A writer opens the store, takes the OS writer lock, verifies what is on disk
// and advances the control epoch. Advanceing the epoch is what fences authority
// that a previous process held: a grant recorded under epoch N is not usable
// under epoch N+1, so a restart cannot inherit live authority by accident.
//
// The reader path runs exactly the same integrity and rollback checks as the
// writer path. It simply does not take the exclusive lock, does not advance the
// epoch and cannot append.

#ifndef FAC_DURABLE_STORE_HPP
#define FAC_DURABLE_STORE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fac/core/error.hpp"
#include "fac/durable/format.hpp"
#include "fac/durable/journal.hpp"
#include "fac/durable/lock.hpp"

namespace fac::durable {

struct StoreOptions {
  // Create the directory and an empty store when nothing is there yet.
  bool create = false;
  // Advance the control epoch on open. A read-only inspection never does.
  bool advance_epoch = true;
};

// What recovery actually did. Reported so a caller can see that a tail was
// discarded or that a manifest slot was recovered rather than silently
// accepting a repaired store.
struct RecoveryReport {
  bool created = false;
  ControlEpoch epoch;
  LedgerSequence sequence;
  std::uint64_t journal_bytes = 0;
  std::uint64_t discarded_tail_bytes = 0;
  std::uint64_t applied_frames = 0;
  bool snapshot_loaded = false;
  std::uint64_t snapshot_sequence = 0;
  std::uint32_t manifest_generation = 0;
  std::vector<std::string> notes;
};

class Store {
 public:
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  ~Store();

  [[nodiscard]] static Result<std::unique_ptr<Store>> open_writer(const std::string& directory,
                                                                 const StoreOptions& options);
  [[nodiscard]] static Result<std::unique_ptr<Store>> open_reader(const std::string& directory);

  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
  [[nodiscard]] bool read_only() const noexcept { return read_only_; }
  [[nodiscard]] ControlEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] LedgerSequence sequence() const noexcept { return sequence_; }
  [[nodiscard]] const RecoveryReport& report() const noexcept { return report_; }

  // The committed snapshot, when one exists. Loaded and verified during open.
  [[nodiscard]] const std::optional<Snapshot>& snapshot() const noexcept { return snapshot_; }

  // Every committed frame, in sequence order, after the snapshot sequence.
  // Loaded and verified during open.
  [[nodiscard]] const std::vector<Frame>& frames() const noexcept { return frames_; }

  // Appends one frame and publishes the manifest that commits it. The sequence
  // must be exactly one past the current committed sequence.
  [[nodiscard]] Status append(RecordKind kind, LedgerSequence sequence,
                              std::span<const std::byte> payload);

  // Compaction: publishes a snapshot and starts a new journal segment. The
  // snapshot sequence must equal the current committed sequence.
  [[nodiscard]] Status publish_snapshot(const Snapshot& snapshot);

  // Re-verifies the whole store from disk without changing it.
  [[nodiscard]] Status verify();

 private:
  Store() = default;

  [[nodiscard]] Status open_common(const std::string& directory, bool read_only,
                                   const StoreOptions& options);
  [[nodiscard]] Status publish_manifest(std::uint64_t manifest_generation,
                                        std::uint64_t journal_length,
                                        const Digest256& journal_digest);

  std::string directory_;
  bool read_only_ = false;
  WriterLock lock_;
  std::unique_ptr<Journal> journal_;
  Manifest manifest_;
  std::optional<Snapshot> snapshot_;
  Digest256 snapshot_digest_;
  std::vector<Frame> frames_;
  ControlEpoch epoch_;
  LedgerSequence sequence_;
  RecoveryReport report_;
};

}  // namespace fac::durable

#endif  // FAC_DURABLE_STORE_HPP
