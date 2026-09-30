// Facility Admission Control - maintenance, incident, placement and policy evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/model/facility_state.hpp"

#include <algorithm>

#include "fac/core/limits.hpp"
#include "detail/enum_codec.hpp"

namespace fac {
namespace {

constexpr std::pair<std::string_view, MaintenanceState> kMaintenanceStateNames[] = {
    {"planned", MaintenanceState::planned},
    {"active", MaintenanceState::active},
    {"completed", MaintenanceState::completed},
    {"unknown", MaintenanceState::unknown},
};

constexpr std::pair<std::string_view, MaintenanceImpact> kMaintenanceImpactNames[] = {
    {"none", MaintenanceImpact::none},
    {"capacity_reduction", MaintenanceImpact::capacity_reduction},
    {"full_outage", MaintenanceImpact::full_outage},
    {"unknown", MaintenanceImpact::unknown},
};

constexpr std::pair<std::string_view, IncidentState> kIncidentStateNames[] = {
    {"normal", IncidentState::normal},
    {"degraded", IncidentState::degraded},
    {"major", IncidentState::major},
    {"unknown", IncidentState::unknown},
};

constexpr std::pair<std::string_view, CapacityTrust> kCapacityTrustNames[] = {
    {"trusted", CapacityTrust::trusted},
    {"unreliable", CapacityTrust::unreliable},
    {"unknown", CapacityTrust::unknown},
};

constexpr std::pair<std::string_view, PolicyVerdict> kPolicyVerdictNames[] = {
    {"permit", PolicyVerdict::permit},
    {"deny", PolicyVerdict::deny},
    {"abstain", PolicyVerdict::abstain},
};

// Bounds the rack lists a placement snapshot may carry. The lists are authored
// by another authority, but they still cross a process boundary.
constexpr std::size_t kMaxPlacementRacks = 4096;

}  // namespace

const char* to_string(MaintenanceState state) noexcept {
  return detail::enum_to_string(state, kMaintenanceStateNames, "unknown_maintenance_state");
}

Result<MaintenanceState> maintenance_state_from_string(std::string_view text) {
  return detail::enum_from_string(text, kMaintenanceStateNames, "maintenance state");
}

const char* to_string(MaintenanceImpact impact) noexcept {
  return detail::enum_to_string(impact, kMaintenanceImpactNames, "unknown_maintenance_impact");
}

Result<MaintenanceImpact> maintenance_impact_from_string(std::string_view text) {
  return detail::enum_from_string(text, kMaintenanceImpactNames, "maintenance impact");
}

Status MaintenanceWindow::validate() const {
  if (window.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "maintenance window carries no identity");
  }
  if (start.is_unset() || end.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "maintenance window carries no time bounds");
  }
  if (!(start < end)) {
    return make_error(ErrorCode::invalid_argument, "maintenance window end is not after its start");
  }
  return Status::success();
}

bool MaintenanceWindow::overlaps(Timestamp other_start, Timestamp other_end) const {
  return start < other_end && other_start < end;
}

void MaintenanceWindow::encode(codec::Writer& writer) const {
  writer.ident(window);
  scope.encode(writer);
  writer.i64(start.unix_nanos());
  writer.i64(end.unix_nanos());
  writer.u8(static_cast<std::uint8_t>(state));
  writer.u8(static_cast<std::uint8_t>(impact));
}

Result<MaintenanceWindow> MaintenanceWindow::decode(codec::Reader& reader) {
  MaintenanceWindow window;
  auto identity = reader.ident<struct MaintenanceWindowIdTag>();
  if (!identity.ok()) return identity.error();
  window.window = identity.value();
  auto scope = TargetScope::decode(reader);
  if (!scope.ok()) return scope.error();
  window.scope = scope.take();
  auto start = reader.i64();
  if (!start.ok()) return start.error();
  auto end = reader.i64();
  if (!end.ok()) return end.error();
  if (!is_valid_timestamp_nanos(start.value()) || !is_valid_timestamp_nanos(end.value())) {
    return make_error(ErrorCode::out_of_range, "maintenance window bounds are outside the supported range");
  }
  window.start = Timestamp(start.value());
  window.end = Timestamp(end.value());
  auto state = detail::read_enum<MaintenanceState>(reader, static_cast<std::uint8_t>(MaintenanceState::unknown),
                                                  "maintenance state");
  if (!state.ok()) return state.error();
  window.state = state.value();
  auto impact = detail::read_enum<MaintenanceImpact>(reader,
                                                    static_cast<std::uint8_t>(MaintenanceImpact::unknown),
                                                    "maintenance impact");
  if (!impact.ok()) return impact.error();
  window.impact = impact.value();
  const Status status = window.validate();
  if (!status.ok()) {
    return status.error();
  }
  return window;
}

Status MaintenanceSnapshot::validate() const {
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "maintenance snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "maintenance snapshot carries no observation time");
  }
  if (windows.size() > kMaxMaintenanceWindows) {
    return make_error(ErrorCode::limit_exceeded, "maintenance snapshot carries too many windows");
  }
  for (std::size_t i = 0; i < windows.size(); ++i) {
    const Status status = windows[i].validate();
    if (!status.ok()) {
      return status.error();
    }
    for (std::size_t j = i + 1; j < windows.size(); ++j) {
      if (windows[i].window == windows[j].window) {
        return make_error(ErrorCode::duplicate_field, "maintenance snapshot repeats a window identity");
      }
    }
  }
  return Status::success();
}

void MaintenanceSnapshot::encode(codec::Writer& writer) const {
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  writer.u32(static_cast<std::uint32_t>(windows.size()));
  for (const auto& window : windows) {
    window.encode(writer);
  }
}

Result<MaintenanceSnapshot> MaintenanceSnapshot::decode(codec::Reader& reader) {
  MaintenanceSnapshot snapshot;
  auto generation = reader.counter<struct MaintenanceGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "maintenance observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  // 16 window identity + 50 scope + 8 + 8 bounds + 1 state + 1 impact.
  auto count = reader.sequence_count(kMaxMaintenanceWindows, 84);
  if (!count.ok()) return count.error();
  snapshot.windows.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto window = MaintenanceWindow::decode(reader);
    if (!window.ok()) return window.error();
    snapshot.windows.push_back(window.take());
  }
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

const char* to_string(IncidentState state) noexcept {
  return detail::enum_to_string(state, kIncidentStateNames, "unknown_incident_state");
}

Result<IncidentState> incident_state_from_string(std::string_view text) {
  return detail::enum_from_string(text, kIncidentStateNames, "incident state");
}

const char* to_string(CapacityTrust trust) noexcept {
  return detail::enum_to_string(trust, kCapacityTrustNames, "unknown_capacity_trust");
}

Result<CapacityTrust> capacity_trust_from_string(std::string_view text) {
  return detail::enum_from_string(text, kCapacityTrustNames, "capacity trust");
}

Status IncidentSnapshot::validate() const {
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "incident snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "incident snapshot carries no observation time");
  }
  if (incident.has_value() && incident->is_unset()) {
    return make_error(ErrorCode::invalid_argument, "incident snapshot carries a zero incident identity");
  }
  if (state == IncidentState::normal && incident.has_value()) {
    return make_error(ErrorCode::invalid_argument, "a normal incident state names an incident");
  }
  return Status::success();
}

void IncidentSnapshot::encode(codec::Writer& writer) const {
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  writer.boolean(incident.has_value());
  writer.ident(incident.value_or(IncidentId{}));
  scope.encode(writer);
  writer.u8(static_cast<std::uint8_t>(state));
  writer.u8(static_cast<std::uint8_t>(capacity_trust));
}

Result<IncidentSnapshot> IncidentSnapshot::decode(codec::Reader& reader) {
  IncidentSnapshot snapshot;
  auto generation = reader.counter<struct IncidentGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "incident observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto present = reader.boolean();
  if (!present.ok()) return present.error();
  if (present.value()) {
    auto identity = reader.ident<struct IncidentIdTag>();
    if (!identity.ok()) return identity.error();
    snapshot.incident = identity.value();
  } else {
    auto zero = reader.u64();
    auto zero_low = reader.u64();
    if (!zero.ok()) return zero.error();
    if (!zero_low.ok()) return zero_low.error();
    if (zero.value() != 0 || zero_low.value() != 0) {
      return make_error(ErrorCode::malformed_input, "absent incident carries a non-zero identity");
    }
  }
  auto scope = TargetScope::decode(reader);
  if (!scope.ok()) return scope.error();
  snapshot.scope = scope.take();
  auto state = detail::read_enum<IncidentState>(reader, static_cast<std::uint8_t>(IncidentState::unknown),
                                                "incident state");
  if (!state.ok()) return state.error();
  snapshot.state = state.value();
  auto trust = detail::read_enum<CapacityTrust>(reader, static_cast<std::uint8_t>(CapacityTrust::unknown),
                                                "capacity trust");
  if (!trust.ok()) return trust.error();
  snapshot.capacity_trust = trust.value();
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

Status PlacementPolicySnapshot::validate() const {
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "placement policy snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "placement policy snapshot carries no observation time");
  }
  if (allowed_racks.size() > kMaxPlacementRacks || forbidden_racks.size() > kMaxPlacementRacks) {
    return make_error(ErrorCode::limit_exceeded, "placement policy carries too many rack entries");
  }
  for (const RackId& rack : allowed_racks) {
    if (rack.is_unset()) {
      return make_error(ErrorCode::invalid_argument, "placement allow list carries a zero rack identity");
    }
    if (std::find(forbidden_racks.begin(), forbidden_racks.end(), rack) != forbidden_racks.end()) {
      return make_error(ErrorCode::conflict, "placement policy allows and forbids the same rack");
    }
  }
  for (const RackId& rack : forbidden_racks) {
    if (rack.is_unset()) {
      return make_error(ErrorCode::invalid_argument, "placement forbid list carries a zero rack identity");
    }
  }
  if (max_units_per_rack.has_value() && max_units_per_rack.value() == 0) {
    return make_error(ErrorCode::invalid_argument, "placement policy states a zero per-rack unit limit");
  }
  if (max_units_per_facility.has_value() && max_units_per_facility.value() == 0) {
    return make_error(ErrorCode::invalid_argument,
                      "placement policy states a zero per-facility unit limit");
  }
  return Status::success();
}

void PlacementPolicySnapshot::encode(codec::Writer& writer) const {
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  writer.u32(static_cast<std::uint32_t>(allowed_racks.size()));
  for (const auto& rack : allowed_racks) {
    writer.ident(rack);
  }
  writer.u32(static_cast<std::uint32_t>(forbidden_racks.size()));
  for (const auto& rack : forbidden_racks) {
    writer.ident(rack);
  }
  writer.boolean(required_zone.has_value());
  writer.ident(required_zone.value_or(ZoneId{}));
  writer.boolean(max_units_per_rack.has_value());
  writer.u64(max_units_per_rack.value_or(0));
  writer.boolean(max_units_per_facility.has_value());
  writer.u64(max_units_per_facility.value_or(0));
}

Result<PlacementPolicySnapshot> PlacementPolicySnapshot::decode(codec::Reader& reader) {
  PlacementPolicySnapshot snapshot;
  auto generation = reader.counter<struct PlacementGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "placement observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto allowed = reader.sequence_count(kMaxPlacementRacks, 16);
  if (!allowed.ok()) return allowed.error();
  snapshot.allowed_racks.reserve(allowed.value());
  for (std::uint32_t i = 0; i < allowed.value(); ++i) {
    auto rack = reader.ident<struct RackIdTag>();
    if (!rack.ok()) return rack.error();
    snapshot.allowed_racks.push_back(rack.value());
  }
  auto forbidden = reader.sequence_count(kMaxPlacementRacks, 16);
  if (!forbidden.ok()) return forbidden.error();
  snapshot.forbidden_racks.reserve(forbidden.value());
  for (std::uint32_t i = 0; i < forbidden.value(); ++i) {
    auto rack = reader.ident<struct RackIdTag>();
    if (!rack.ok()) return rack.error();
    snapshot.forbidden_racks.push_back(rack.value());
  }
  auto has_zone = reader.boolean();
  if (!has_zone.ok()) return has_zone.error();
  if (has_zone.value()) {
    auto zone = reader.ident<struct ZoneIdTag>();
    if (!zone.ok()) return zone.error();
    snapshot.required_zone = zone.value();
  } else {
    auto zero = reader.u64();
    auto zero_low = reader.u64();
    if (!zero.ok()) return zero.error();
    if (!zero_low.ok()) return zero_low.error();
    if (zero.value() != 0 || zero_low.value() != 0) {
      return make_error(ErrorCode::malformed_input, "absent required zone carries a non-zero identity");
    }
  }
  auto has_rack_limit = reader.boolean();
  if (!has_rack_limit.ok()) return has_rack_limit.error();
  auto rack_limit = reader.u64();
  if (!rack_limit.ok()) return rack_limit.error();
  if (has_rack_limit.value()) {
    snapshot.max_units_per_rack = rack_limit.value();
  } else if (rack_limit.value() != 0) {
    return make_error(ErrorCode::malformed_input, "absent per-rack limit carries a non-zero value");
  }
  auto has_facility_limit = reader.boolean();
  if (!has_facility_limit.ok()) return has_facility_limit.error();
  auto facility_limit = reader.u64();
  if (!facility_limit.ok()) return facility_limit.error();
  if (has_facility_limit.value()) {
    snapshot.max_units_per_facility = facility_limit.value();
  } else if (facility_limit.value() != 0) {
    return make_error(ErrorCode::malformed_input, "absent per-facility limit carries a non-zero value");
  }
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

const char* to_string(PolicyVerdict verdict) noexcept {
  return detail::enum_to_string(verdict, kPolicyVerdictNames, "unknown_policy_verdict");
}

Result<PolicyVerdict> policy_verdict_from_string(std::string_view text) {
  return detail::enum_from_string(text, kPolicyVerdictNames, "policy verdict");
}

const char* to_string(OvercommitMode mode) noexcept {
  switch (mode) {
    case OvercommitMode::not_permitted: return "not_permitted";
    case OvercommitMode::permitted_unbounded: return "permitted_unbounded";
    case OvercommitMode::permitted_up_to: return "permitted_up_to";
  }
  return "unknown_overcommit_mode";
}

Status OvercommitAllowance::validate() const {
  switch (mode) {
    case OvercommitMode::not_permitted:
    case OvercommitMode::permitted_unbounded:
      if (allowance.any_present()) {
        return make_error(ErrorCode::invalid_argument, "overcommit mode carries an allowance it does not use");
      }
      return Status::success();
    case OvercommitMode::permitted_up_to:
      if (!allowance.all_present()) {
        return make_error(ErrorCode::invalid_argument,
                          "a bounded overcommit allowance must state every dimension");
      }
      return Status::success();
  }
  return make_error(ErrorCode::malformed_input, "impossible overcommit mode");
}

void OvercommitAllowance::encode(codec::Writer& writer) const {
  writer.u8(static_cast<std::uint8_t>(mode));
  allowance.encode(writer);
}

Result<OvercommitAllowance> OvercommitAllowance::decode(codec::Reader& reader) {
  OvercommitAllowance result;
  auto mode = detail::read_enum<OvercommitMode>(reader, static_cast<std::uint8_t>(OvercommitMode::permitted_up_to),
                                                "overcommit mode");
  if (!mode.ok()) return mode.error();
  result.mode = mode.value();
  auto allowance = AmountVector::decode(reader);
  if (!allowance.ok()) return allowance.error();
  result.allowance = allowance.take();
  const Status status = result.validate();
  if (!status.ok()) {
    return status.error();
  }
  return result;
}

Status PolicySnapshot::validate() const {
  if (policy.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "policy snapshot carries no policy identity");
  }
  if (generation.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "policy snapshot carries no generation");
  }
  if (observed_at.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "policy snapshot carries no observation time");
  }
  if (policy_digest.is_unset()) {
    return make_error(ErrorCode::invalid_argument, "policy snapshot carries no content digest");
  }
  if (verdict != PolicyVerdict::permit && overcommit.mode != OvercommitMode::not_permitted) {
    return make_error(ErrorCode::invalid_argument,
                      "a policy that did not permit the commitment cannot permit overcommit");
  }
  return overcommit.validate();
}

void PolicySnapshot::encode(codec::Writer& writer) const {
  writer.ident(policy);
  writer.counter(generation);
  writer.i64(observed_at.unix_nanos());
  writer.u8(static_cast<std::uint8_t>(verdict));
  overcommit.encode(writer);
  writer.digest(policy_digest);
}

Result<PolicySnapshot> PolicySnapshot::decode(codec::Reader& reader) {
  PolicySnapshot snapshot;
  auto policy = reader.ident<struct PolicyIdTag>();
  if (!policy.ok()) return policy.error();
  snapshot.policy = policy.value();
  auto generation = reader.counter<struct PolicyGenerationTag>();
  if (!generation.ok()) return generation.error();
  snapshot.generation = generation.value();
  auto observed = reader.i64();
  if (!observed.ok()) return observed.error();
  if (!is_valid_timestamp_nanos(observed.value())) {
    return make_error(ErrorCode::out_of_range, "policy observation time is outside the supported range");
  }
  snapshot.observed_at = Timestamp(observed.value());
  auto verdict = detail::read_enum<PolicyVerdict>(reader, static_cast<std::uint8_t>(PolicyVerdict::abstain),
                                                 "policy verdict");
  if (!verdict.ok()) return verdict.error();
  snapshot.verdict = verdict.value();
  auto overcommit = OvercommitAllowance::decode(reader);
  if (!overcommit.ok()) return overcommit.error();
  snapshot.overcommit = overcommit.take();
  auto digest = reader.digest();
  if (!digest.ok()) return digest.error();
  snapshot.policy_digest = digest.value();
  const Status status = snapshot.validate();
  if (!status.ok()) {
    return status.error();
  }
  return snapshot;
}

}  // namespace fac
