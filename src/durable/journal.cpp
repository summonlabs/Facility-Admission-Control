// Facility Admission Control - the append-only journal.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/durable/journal.hpp"

#include <algorithm>
#include <cstring>

#include "fac/core/crc32c.hpp"
#include "fac/core/limits.hpp"
#include "fac/core/sha256.hpp"
#include "detail/enum_codec.hpp"
#include "durable/file_handle.hpp"

namespace fac::durable {
namespace {

// The largest single read used while validating a frame header.
constexpr std::size_t kReadChunkBytes = 1u << 16;

}  // namespace

struct Journal::Impl {
  FileHandle handle;
  Sha256 chain;
};

Journal::~Journal() = default;

Result<std::unique_ptr<Journal>> Journal::open(const std::string& path, bool read_only,
                                               std::uint64_t committed_length) {
  auto handle = read_only ? FileHandle::open_read(path) : FileHandle::open_read_write(path, true);
  if (!handle.ok()) {
    if (read_only && handle.error().code == ErrorCode::file_not_found) {
      // A reader may legitimately see a store whose journal was never created
      // because nothing has been committed yet. The manifest decides whether
      // that is consistent, and the caller checks it.
      auto journal = std::unique_ptr<Journal>(new Journal());
      journal->impl_ = std::make_unique<Impl>();
      journal->length_ = 0;
      journal->read_only_ = true;
      return journal;
    }
    return handle.error();
  }

  auto journal = std::unique_ptr<Journal>(new Journal());
  journal->impl_ = std::make_unique<Impl>();
  journal->impl_->handle = handle.take();
  journal->read_only_ = read_only;
  journal->length_ = committed_length;

  auto actual = journal->impl_->handle.size();
  if (!actual.ok()) {
    return actual.error();
  }
  if (actual.value() < committed_length) {
    return make_error(ErrorCode::journal_truncated,
                      "the journal is shorter than the length the manifest commits");
  }
  if (actual.value() > committed_length && !read_only) {
    const Status truncated = journal->impl_->handle.truncate_to(committed_length);
    if (!truncated.ok()) {
      return truncated.error();
    }
    const Status flushed = journal->impl_->handle.flush();
    if (!flushed.ok()) {
      return flushed.error();
    }
  }

  if (committed_length > kMaxJournalBytes) {
    return make_error(ErrorCode::limit_exceeded, "the committed journal region exceeds the maximum");
  }
  std::uint64_t offset = 0;
  std::vector<std::byte> buffer(kReadChunkBytes);
  while (offset < committed_length) {
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(kReadChunkBytes, committed_length - offset));
    std::size_t read = 0;
    const Status status = journal->impl_->handle.read_at(
        offset, std::span<std::byte>(buffer.data(), wanted), read);
    if (!status.ok()) {
      return status.error();
    }
    if (read != wanted) {
      return make_error(ErrorCode::journal_truncated, "the journal ended before its committed length");
    }
    journal->impl_->chain.update(std::span<const std::byte>(buffer.data(), read));
    offset += read;
  }
  return journal;
}

Status Journal::append(RecordKind kind, LedgerSequence sequence, std::span<const std::byte> payload) {
  if (read_only_) {
    return make_error(ErrorCode::read_only_store, "the journal is open for reading only");
  }
  if (payload.size() > kMaxFramePayload) {
    return make_error(ErrorCode::limit_exceeded, "record payload exceeds the frame maximum");
  }
  if (sequence.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "record sequence must be positive");
  }
  const auto payload_length = static_cast<std::uint32_t>(payload.size());

  codec::Writer header;
  encode_frame_header(header, kind, sequence, payload_length);
  if (header.size() != kFrameHeaderBytes) {
    return make_error(ErrorCode::internal, "frame header size is not the declared size");
  }

  Crc32c crc;
  crc.update(header.span());
  crc.update(payload);

  std::vector<std::byte> frame;
  frame.reserve(kFrameHeaderBytes + payload.size() + 4);
  frame.insert(frame.end(), header.data().begin(), header.data().end());
  frame.insert(frame.end(), payload.begin(), payload.end());
  const std::uint32_t checksum = crc.finish();
  frame.push_back(static_cast<std::byte>((checksum >> 24) & 0xFFu));
  frame.push_back(static_cast<std::byte>((checksum >> 16) & 0xFFu));
  frame.push_back(static_cast<std::byte>((checksum >> 8) & 0xFFu));
  frame.push_back(static_cast<std::byte>(checksum & 0xFFu));

  const Status written = impl_->handle.append(std::span<const std::byte>(frame.data(), frame.size()));
  if (!written.ok()) {
    return written.error();
  }
  const Status flushed = impl_->handle.flush();
  if (!flushed.ok()) {
    return flushed.error();
  }
  impl_->chain.update(std::span<const std::byte>(frame.data(), frame.size()));
  length_ += frame.size();
  return Status::success();
}

Result<std::vector<Frame>> Journal::read_range(std::uint64_t from, std::uint64_t to) {
  if (to > length_) {
    return make_error(ErrorCode::journal_truncated, "the requested range exceeds the committed journal");
  }
  if (from > to) {
    return make_error(ErrorCode::invalid_argument, "the requested range is inverted");
  }
  std::vector<Frame> frames;
  std::uint64_t offset = from;
  std::vector<std::byte> header(kFrameHeaderBytes);
  while (offset < to) {
    if (to - offset < kFrameHeaderBytes) {
      return make_error(ErrorCode::journal_truncated, "the committed region ends inside a frame header");
    }
    std::size_t read = 0;
    Status status = impl_->handle.read_at(offset, std::span<std::byte>(header.data(), header.size()), read);
    if (!status.ok()) {
      return status.error();
    }
    if (read != header.size()) {
      return make_error(ErrorCode::journal_truncated, "the committed region ends inside a frame header");
    }
    codec::Reader reader(std::span<const std::byte>(header.data(), header.size()));
    auto magic = reader.u32();
    if (!magic.ok()) return magic.error();
    if (magic.value() != kJournalFrameMagic) {
      return make_error(ErrorCode::corrupt_store, "a journal frame does not carry the frame magic");
    }
    auto kind = detail::read_enum<RecordKind>(reader, static_cast<std::uint8_t>(kRecordKindCount - 1),
                                              "record kind");
    if (!kind.ok()) return kind.error();
    const Status flags = reader.reserved(1);
    if (!flags.ok()) return flags.error();
    const Status reserved = reader.reserved(2);
    if (!reserved.ok()) return reserved.error();
    auto sequence = reader.counter<struct LedgerSequenceTag>();
    if (!sequence.ok()) return sequence.error();
    auto payload_length = reader.u32();
    if (!payload_length.ok()) return payload_length.error();
    if (static_cast<std::size_t>(payload_length.value()) > kMaxFramePayload) {
      return make_error(ErrorCode::corrupt_store, "a journal frame declares an oversized payload");
    }
    const std::uint64_t frame_end = offset + kFrameHeaderBytes + payload_length.value() + 4;
    if (frame_end > to) {
      return make_error(ErrorCode::journal_truncated, "a journal frame extends past the committed region");
    }

    std::vector<std::byte> payload(payload_length.value());
    if (!payload.empty()) {
      status = impl_->handle.read_at(offset + kFrameHeaderBytes,
                                     std::span<std::byte>(payload.data(), payload.size()), read);
      if (!status.ok()) {
        return status.error();
      }
      if (read != payload.size()) {
        return make_error(ErrorCode::journal_truncated, "a journal frame payload is incomplete");
      }
    }
    std::byte crc_bytes[4];
    status = impl_->handle.read_at(offset + kFrameHeaderBytes + payload_length.value(),
                                   std::span<std::byte>(crc_bytes, 4), read);
    if (!status.ok()) {
      return status.error();
    }
    if (read != 4) {
      return make_error(ErrorCode::journal_truncated, "a journal frame checksum is missing");
    }
    Crc32c crc;
    crc.update(std::span<const std::byte>(header.data(), header.size()));
    if (!payload.empty()) {
      crc.update(std::span<const std::byte>(payload.data(), payload.size()));
    }
    std::uint32_t stored_crc = 0;
    for (const std::byte byte : crc_bytes) {
      stored_crc = (stored_crc << 8) | static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(byte));
    }
    if (stored_crc != crc.finish()) {
      return make_error(ErrorCode::checksum_mismatch, "a journal frame fails its checksum");
    }

    Frame frame;
    frame.kind = kind.value();
    frame.sequence = sequence.value();
    frame.payload = std::move(payload);
    if (!frames.empty()) {
      auto expected = frames.back().sequence.checked_next();
      if (!expected.ok()) {
        return expected.error();
      }
      if (!(expected.value() == frame.sequence)) {
        return make_error(ErrorCode::corrupt_store, "journal frame sequences are not consecutive");
      }
    }
    frames.push_back(std::move(frame));
    offset = frame_end;
  }
  if (offset != to) {
    return make_error(ErrorCode::journal_truncated, "the committed region does not end on a frame boundary");
  }
  return frames;
}

Result<Digest256> Journal::digest_prefix(std::uint64_t length) {
  if (length > length_) {
    return make_error(ErrorCode::journal_truncated, "the requested digest covers bytes past the journal");
  }
  Sha256 hash;
  std::vector<std::byte> buffer(kReadChunkBytes);
  std::uint64_t offset = 0;
  while (offset < length) {
    const std::size_t wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(kReadChunkBytes, length - offset));
    std::size_t read = 0;
    const Status status =
        impl_->handle.read_at(offset, std::span<std::byte>(buffer.data(), wanted), read);
    if (!status.ok()) {
      return status.error();
    }
    if (read != wanted) {
      return make_error(ErrorCode::journal_truncated, "the journal ended while hashing a committed region");
    }
    hash.update(std::span<const std::byte>(buffer.data(), read));
    offset += read;
  }
  Digest256 digest;
  digest.bytes() = hash.finish();
  return digest;
}

Digest256 Journal::chain_digest() const {
  Sha256 copy = impl_->chain;
  Digest256 digest;
  digest.bytes() = copy.finish();
  return digest;
}

Status Journal::reset() {
  if (read_only_) {
    return make_error(ErrorCode::read_only_store, "the journal is open for reading only");
  }
  const Status truncated = impl_->handle.truncate_to(0);
  if (!truncated.ok()) {
    return truncated.error();
  }
  const Status flushed = impl_->handle.flush();
  if (!flushed.ok()) {
    return flushed.error();
  }
  impl_->chain = Sha256{};
  length_ = 0;
  return Status::success();
}

}  // namespace fac::durable
