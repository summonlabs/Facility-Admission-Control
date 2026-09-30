// Facility Admission Control - the append-only journal.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Frames are appended, flushed to the device and then covered by a manifest
// publication, which is the commit point. The journal never claims more than
// the manifest: on open, bytes past the committed length are an uncommitted
// tail and are removed by a writer, and a frame that fails its CRC inside the
// committed range fails the whole open.
//
// The chain digest of the committed region is maintained incrementally while
// the store is open and recomputed from disk when it is opened and when it is
// verified. The two are compared, so an incremental digest can never drift
// from what is actually in the file without being detected.

#ifndef FAC_DURABLE_JOURNAL_HPP
#define FAC_DURABLE_JOURNAL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/durable/format.hpp"

namespace fac::durable {

class Journal {
 public:
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  ~Journal();

  // Opens a journal whose committed region is committed_length bytes. A writer
  // removes any bytes past that point before returning.
  [[nodiscard]] static Result<std::unique_ptr<Journal>> open(const std::string& path, bool read_only,
                                                             std::uint64_t committed_length);

  [[nodiscard]] std::uint64_t length() const noexcept { return length_; }
  [[nodiscard]] bool read_only() const noexcept { return read_only_; }

  // Appends one frame and flushes it. The append is not committed until the
  // manifest covering the new length is published.
  [[nodiscard]] Status append(RecordKind kind, LedgerSequence sequence,
                              std::span<const std::byte> payload);

  // Reads and validates every frame in [from, to). The range must end exactly
  // on a frame boundary and the sequences must be consecutive.
  [[nodiscard]] Result<std::vector<Frame>> read_range(std::uint64_t from, std::uint64_t to);

  // SHA-256 over the byte range [0, length), recomputed from the file.
  [[nodiscard]] Result<Digest256> digest_prefix(std::uint64_t length);

  // The running digest of the committed region.
  [[nodiscard]] Digest256 chain_digest() const;

  // Discards every committed frame and restarts the journal. Used by
  // compaction, and only after the manifest that activates the snapshot which
  // replaces those frames has been published.
  [[nodiscard]] Status reset();

 private:
  Journal() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint64_t length_ = 0;
  bool read_only_ = false;
};

}  // namespace fac::durable

#endif  // FAC_DURABLE_JOURNAL_HPP
