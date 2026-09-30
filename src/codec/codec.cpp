// Facility Admission Control - canonical binary encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/codec/codec.hpp"

#include <algorithm>

#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"

namespace fac::codec {

void Writer::u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

void Writer::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void Writer::u32(std::uint32_t value) {
  u8(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void Writer::u64(std::uint64_t value) {
  u32(static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFu));
  u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

void Writer::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void Writer::boolean(bool value) { u8(value ? 1u : 0u); }

void Writer::raw(std::span<const std::byte> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void Writer::bytes(std::span<const std::byte> bytes) {
  u32(static_cast<std::uint32_t>(bytes.size()));
  raw(bytes);
}

void Writer::text(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  std::span<const std::byte> as_bytes(reinterpret_cast<const std::byte*>(value.data()), value.size());
  raw(as_bytes);
}

void Writer::digest(const Digest256& value) { raw(value.bytes()); }

void Writer::reserved(std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    u8(0);
  }
}

Result<std::span<const std::byte>> Reader::take(std::size_t count) {
  if (count > remaining()) {
    return make_error(ErrorCode::truncated_input, "declared field length exceeds the remaining input");
  }
  const auto slice = bytes_.subspan(offset_, count);
  offset_ += count;
  return slice;
}

Result<std::uint8_t> Reader::u8() {
  auto bytes = take(1);
  if (!bytes.ok()) return bytes.error();
  return std::to_integer<std::uint8_t>(bytes.value()[0]);
}

Result<std::uint16_t> Reader::u16() {
  auto bytes = take(2);
  if (!bytes.ok()) return bytes.error();
  const auto* p = bytes.value().data();
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) << 8) |
                                    static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1])));
}

Result<std::uint32_t> Reader::u32() {
  auto bytes = take(4);
  if (!bytes.ok()) return bytes.error();
  const auto* p = bytes.value().data();
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value = (value << 8) | static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i]));
  }
  return value;
}

Result<std::uint64_t> Reader::u64() {
  auto bytes = take(8);
  if (!bytes.ok()) return bytes.error();
  const auto* p = bytes.value().data();
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
  }
  return value;
}

Result<std::int64_t> Reader::i64() {
  auto value = u64();
  if (!value.ok()) return value.error();
  return static_cast<std::int64_t>(value.value());
}

Result<bool> Reader::boolean() {
  auto value = u8();
  if (!value.ok()) return value.error();
  if (value.value() > 1u) {
    return make_error(ErrorCode::malformed_input, "boolean field is neither 0 nor 1");
  }
  return value.value() == 1u;
}

Result<std::span<const std::byte>> Reader::raw(std::size_t count) { return take(count); }

Result<std::span<const std::byte>> Reader::bytes(std::size_t max_length) {
  auto length = u32();
  if (!length.ok()) return length.error();
  if (static_cast<std::size_t>(length.value()) > max_length) {
    return make_error(ErrorCode::limit_exceeded, "declared byte length exceeds the configured maximum");
  }
  if (static_cast<std::size_t>(length.value()) > remaining()) {
    return make_error(ErrorCode::truncated_input, "declared byte length exceeds the remaining input");
  }
  return take(length.value());
}

Result<std::string> Reader::text(std::size_t max_length) {
  auto bytes = this->bytes(max_length);
  if (!bytes.ok()) return bytes.error();
  const auto view = std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
  if (!is_valid_utf8(view)) {
    return make_error(ErrorCode::invalid_utf8, "text field is not valid UTF-8");
  }
  return std::string(view);
}

Result<Digest256> Reader::digest() {
  auto bytes = take(Digest256::kBytes);
  if (!bytes.ok()) return bytes.error();
  Digest256 value;
  std::copy(bytes.value().begin(), bytes.value().end(), value.bytes().begin());
  if (value.is_unset()) {
    return make_error(ErrorCode::malformed_input, "digest field is zero");
  }
  return value;
}

Status Reader::reserved(std::size_t count) {
  auto bytes = take(count);
  if (!bytes.ok()) return bytes.error();
  for (const std::byte b : bytes.value()) {
    if (b != std::byte{0}) {
      return make_error(ErrorCode::reserved_field_nonzero, "reserved field is not zero");
    }
  }
  return Status::success();
}

Status Reader::expect_end() const {
  if (remaining() != 0) {
    return make_error(ErrorCode::trailing_bytes, "input has trailing bytes after the canonical encoding");
  }
  return Status::success();
}

Result<std::uint32_t> Reader::sequence_count(std::size_t max_elements, std::size_t element_min_bytes) {
  auto count = u32();
  if (!count.ok()) return count.error();
  const auto value = static_cast<std::size_t>(count.value());
  if (value > max_elements) {
    return make_error(ErrorCode::limit_exceeded, "declared sequence length exceeds the configured maximum");
  }
  if (element_min_bytes > 0 && value > remaining() / element_min_bytes) {
    return make_error(ErrorCode::truncated_input, "declared sequence length cannot fit in the remaining input");
  }
  return count.value();
}

}  // namespace fac::codec
