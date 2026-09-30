// Facility Admission Control - canonical binary encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every digested or persisted representation goes through this encoder. The
// form is fixed so that two runs over the same authoritative inputs produce the
// same bytes and therefore the same digest:
//
//   integers   big endian, fixed width (u8, u16, u32, u64, i64)
//   boolean    one byte, 0 or 1; any other value is refused on decode
//   enum       one byte, validated against the known set by the type's decoder
//   optional   one presence byte (0 absent, 1 present) followed by the value
//   text       u32 byte length followed by the exact bytes
//   identity   16 bytes, high half then low half
//   counter    u64
//   digest     32 bytes, exactly SHA-256
//   sequence   u32 element count followed by the elements
//
// The decoder validates against the declared bounds in fac/core/limits.hpp
// before it allocates, refuses a non-canonical presence byte, refuses trailing
// bytes when asked to, and never accepts a length it has not checked against
// the bytes that actually remain.

#ifndef FAC_CODEC_CODEC_HPP
#define FAC_CODEC_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"

namespace fac::codec {

class Writer {
 public:
  Writer() = default;

  void reserve(std::size_t bytes) { buffer_.reserve(bytes); }

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void raw(std::span<const std::byte> bytes);
  void bytes(std::span<const std::byte> bytes);
  void text(std::string_view value);
  void digest(const Digest256& value);
  void reserved(std::size_t count);

  template <class Tag>
  void ident(const Ident128<Tag>& value) {
    u64(value.hi());
    u64(value.lo());
  }

  template <class Tag>
  void counter(const Counter<Tag>& value) {
    u64(value.value());
  }

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::span<const std::byte> span() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }

 private:
  std::vector<std::byte> buffer_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool empty() const noexcept { return remaining() == 0; }

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::span<const std::byte>> raw(std::size_t count);
  [[nodiscard]] Result<std::span<const std::byte>> bytes(std::size_t max_length);
  [[nodiscard]] Result<std::string> text(std::size_t max_length);
  [[nodiscard]] Result<Digest256> digest();

  // A reserved field must be present and must be zero. A non-zero value is a
  // different format, not a value this reader may ignore.
  [[nodiscard]] Status reserved(std::size_t count);

  template <class Tag>
  [[nodiscard]] Result<Ident128<Tag>> ident() {
    auto high = u64();
    if (!high.ok()) return high.error();
    auto low = u64();
    if (!low.ok()) return low.error();
    const Ident128<Tag> value(high.value(), low.value());
    if (value.is_unset()) {
      return make_error(ErrorCode::invalid_argument, "identity field is zero");
    }
    return value;
  }

  template <class Tag>
  [[nodiscard]] Result<Counter<Tag>> counter() {
    auto raw_value = u64();
    if (!raw_value.ok()) return raw_value.error();
    return Counter<Tag>::from_value(raw_value.value());
  }

  // Refuses any bytes left over. Canonical encodings are exact, so a decoder
  // that silently ignored a tail would accept a second interpretation of the
  // same bytes.
  [[nodiscard]] Status expect_end() const;

  // Bounds the number of elements a sequence may declare before allocating.
  [[nodiscard]] Result<std::uint32_t> sequence_count(std::size_t max_elements,
                                                     std::size_t element_min_bytes = 1);

 private:
  [[nodiscard]] Result<std::span<const std::byte>> take(std::size_t count);

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

}  // namespace fac::codec

#endif  // FAC_CODEC_CODEC_HPP
