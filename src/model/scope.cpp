// Facility Admission Control - facility scope.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/scope.hpp"

#include "fac/core/text.hpp"

namespace fac {

Result<TargetScope> TargetScope::make(FacilityId facility, std::optional<RackId> rack,
                                      std::optional<ZoneId> zone) {
  if (facility.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "scope requires a facility identity");
  }
  if (rack.has_value() && rack->is_unset()) {
    return make_error(ErrorCode::invalid_argument, "scope carries a zero rack identity");
  }
  if (zone.has_value() && zone->is_unset()) {
    return make_error(ErrorCode::invalid_argument, "scope carries a zero zone identity");
  }
  TargetScope scope;
  scope.facility = facility;
  scope.rack = rack;
  scope.zone = zone;
  return scope;
}

bool TargetScope::names_rack(RackId candidate) const {
  return rack.has_value() && rack.value() == candidate;
}

bool TargetScope::names_zone(ZoneId candidate) const {
  return zone.has_value() && zone.value() == candidate;
}

bool TargetScope::affects(const TargetScope& other) const {
  if (!(facility == other.facility)) {
    return false;
  }
  // A constraint that names a rack affects a request only when the request is
  // about that rack or is not narrowed to one. A request narrowed to another
  // rack is unaffected; a facility-wide request is conservatively affected.
  if (rack.has_value()) {
    if (other.rack.has_value() && !(other.rack.value() == rack.value())) {
      return false;
    }
  }
  if (zone.has_value()) {
    if (other.zone.has_value() && !(other.zone.value() == zone.value())) {
      return false;
    }
  }
  return true;
}

bool TargetScope::contains(const TargetScope& other) const {
  if (!(facility == other.facility)) {
    return false;
  }
  if (rack.has_value() && !(other.rack.has_value() && other.rack.value() == rack.value())) {
    return false;
  }
  if (zone.has_value() && !(other.zone.has_value() && other.zone.value() == zone.value())) {
    return false;
  }
  return true;
}

std::string TargetScope::to_string() const {
  std::string out = "facility=" + facility.to_string();
  if (rack.has_value()) {
    out += " rack=" + rack->to_string();
  }
  if (zone.has_value()) {
    out += " zone=" + zone->to_string();
  }
  return out;
}

void TargetScope::encode(codec::Writer& writer) const {
  writer.ident(facility);
  writer.boolean(rack.has_value());
  writer.ident(rack.value_or(RackId{}));
  writer.boolean(zone.has_value());
  writer.ident(zone.value_or(ZoneId{}));
}

Result<TargetScope> TargetScope::decode(codec::Reader& reader) {
  auto facility = reader.ident<struct FacilityIdTag>();
  if (!facility.ok()) {
    return facility.error();
  }
  auto has_rack = reader.boolean();
  if (!has_rack.ok()) {
    return has_rack.error();
  }
  std::optional<RackId> rack;
  if (has_rack.value()) {
    auto value = reader.ident<struct RackIdTag>();
    if (!value.ok()) {
      return value.error();
    }
    rack = value.value();
  } else {
    auto zero = reader.u64();
    auto zero_low = reader.u64();
    if (!zero.ok()) return zero.error();
    if (!zero_low.ok()) return zero_low.error();
    if (zero.value() != 0 || zero_low.value() != 0) {
      return make_error(ErrorCode::malformed_input, "absent rack carries a non-zero identity");
    }
  }
  auto has_zone = reader.boolean();
  if (!has_zone.ok()) {
    return has_zone.error();
  }
  std::optional<ZoneId> zone;
  if (has_zone.value()) {
    auto value = reader.ident<struct ZoneIdTag>();
    if (!value.ok()) {
      return value.error();
    }
    zone = value.value();
  } else {
    auto zero = reader.u64();
    auto zero_low = reader.u64();
    if (!zero.ok()) return zero.error();
    if (!zero_low.ok()) return zero_low.error();
    if (zero.value() != 0 || zero_low.value() != 0) {
      return make_error(ErrorCode::malformed_input, "absent zone carries a non-zero identity");
    }
  }
  return TargetScope::make(facility.value(), rack, zone);
}

}  // namespace fac
