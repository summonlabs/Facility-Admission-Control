// Facility Admission Control - shared enum decoding helpers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal only. An enum arrives from a durable file or from another process,
// so it is validated against the known set before it can reach a switch that
// would otherwise treat an impossible value as "the default case".

#ifndef FAC_DETAIL_ENUM_CODEC_HPP
#define FAC_DETAIL_ENUM_CODEC_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "fac/codec/codec.hpp"
#include "fac/core/error.hpp"

namespace fac::detail {

template <class E>
[[nodiscard]] Result<E> read_enum(codec::Reader& reader, std::uint8_t max_value, const char* what) {
  auto raw = reader.u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() > max_value) {
    return make_error(ErrorCode::malformed_input,
                      std::string("impossible enum value for ") + what);
  }
  return static_cast<E>(raw.value());
}

template <class E, std::size_t N>
[[nodiscard]] Result<E> enum_from_string(std::string_view text,
                                         const std::pair<std::string_view, E> (&table)[N],
                                         const char* what) {
  for (const auto& entry : table) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return make_error(ErrorCode::invalid_argument, std::string("unknown ") + what + " value");
}

template <class E, std::size_t N>
[[nodiscard]] const char* enum_to_string(E value, const std::pair<std::string_view, E> (&table)[N],
                                         const char* fallback) noexcept {
  for (const auto& entry : table) {
    if (entry.second == value) {
      return entry.first.data();
    }
  }
  return fallback;
}

}  // namespace fac::detail

#endif  // FAC_DETAIL_ENUM_CODEC_HPP
