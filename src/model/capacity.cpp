// Facility Admission Control - capacity and redundancy evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/capacity.hpp"

#include "detail/enum_codec.hpp"

namespace fac {
namespace {

constexpr std::pair<std::string_view, RedundancyLevel> kLevelNames[] = {
    {"none", RedundancyLevel::none},
    {"n_plus_one", RedundancyLevel::n_plus_one},
    {"n_plus_two", RedundancyLevel::n_plus_two},
    {"two_n", RedundancyLevel::two_n},
};

}  // namespace

Status CapacitySnapshot::validate() const {
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "capacity snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "capacity snapshot carries no observation time");
  }
  return Status::success();
}

void CapacitySnapshot::encode(codec::Writer& writer) const {
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  total.encode(writer);
  committed.encode(writer);
  reserved.encode(writer);
}

Result<CapacitySnapshot> CapacitySnapshot::decode(codec::Reader& reader) {
  CapacitySnapshot snapshot;
  auto generation = reader.counter<struct CapacityGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "capacity observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto total = AmountVector::decode(reader);
  if (!total.ok()) return total.error();
  snapshot.total = total.take();
  auto committed = AmountVector::decode(reader);
  if (!committed.ok()) return committed.error();
  snapshot.committed = committed.take();
  auto reserved = AmountVector::decode(reader);
  if (!reserved.ok()) return reserved.error();
  snapshot.reserved = reserved.take();
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

const char* to_string(RedundancyLevel level) noexcept {
  return detail::enum_to_string(level, kLevelNames, "unknown_redundancy_level");
}

Result<RedundancyLevel> redundancy_level_from_string(std::string_view text) {
  return detail::enum_from_string(text, kLevelNames, "redundancy level");
}

bool redundancy_at_least(RedundancyLevel observed, RedundancyLevel required) noexcept {
  return static_cast<std::uint8_t>(observed) >= static_cast<std::uint8_t>(required);
}

Status RedundancySnapshot::validate() const {
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "redundancy snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "redundancy snapshot carries no observation time");
  }
  if (!level_stated && level != RedundancyLevel::none) {
    return make_error(ErrorCode::invalid_argument, "unstated redundancy level carries a level value");
  }
  return Status::success();
}

void RedundancySnapshot::encode(codec::Writer& writer) const {
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  writer.boolean(level_stated);
  writer.u8(static_cast<std::uint8_t>(level));
  protected_headroom.encode(writer);
}

Result<RedundancySnapshot> RedundancySnapshot::decode(codec::Reader& reader) {
  RedundancySnapshot snapshot;
  auto generation = reader.counter<struct RedundancyGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "redundancy observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto stated = reader.boolean();
  if (!stated.ok()) return stated.error();
  snapshot.level_stated = stated.value();
  auto level = detail::read_enum<RedundancyLevel>(reader, static_cast<std::uint8_t>(RedundancyLevel::two_n),
                                                  "redundancy level");
  if (!level.ok()) return level.error();
  snapshot.level = level.value();
  auto headroom = AmountVector::decode(reader);
  if (!headroom.ok()) return headroom.error();
  snapshot.protected_headroom = headroom.take();
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

}  // namespace fac
