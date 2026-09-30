// Facility Admission Control - maintenance, incident, placement and policy evidence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Maintenance windows, incident state, placement policy and the facility policy
// result are all owned elsewhere. This repository translates them into admission
// inputs and keeps the translation explicit: state that was never observed is
// "unknown", never "normal", and a policy that returned nothing usable is
// "abstain", never "permit".

#ifndef FAC_MODEL_FACILITY_STATE_HPP
#define FAC_MODEL_FACILITY_STATE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fac/codec/codec.hpp"
#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"
#include "fac/core/time.hpp"
#include "fac/model/capacity.hpp"
#include "fac/model/quantity.hpp"
#include "fac/model/scope.hpp"

namespace fac {

enum class MaintenanceState : std::uint8_t {
  planned = 0,
  active = 1,
  completed = 2,
  unknown = 3,
};

[[nodiscard]] const char* to_string(MaintenanceState state) noexcept;
[[nodiscard]] Result<MaintenanceState> maintenance_state_from_string(std::string_view text);

enum class MaintenanceImpact : std::uint8_t {
  none = 0,
  capacity_reduction = 1,
  full_outage = 2,
  unknown = 3,
};

[[nodiscard]] const char* to_string(MaintenanceImpact impact) noexcept;
[[nodiscard]] Result<MaintenanceImpact> maintenance_impact_from_string(std::string_view text);

struct MaintenanceWindow {
  MaintenanceWindowId window;
  TargetScope scope;
  Timestamp start;
  Timestamp end;
  MaintenanceState state = MaintenanceState::unknown;
  MaintenanceImpact impact = MaintenanceImpact::unknown;

  [[nodiscard]] Status validate() const;

  // True when [start, end) intersects the other window. Half-open bounds keep
  // back-to-back windows from being reported as overlapping.
  [[nodiscard]] bool overlaps(Timestamp other_start, Timestamp other_end) const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<MaintenanceWindow> decode(codec::Reader& reader);

  friend bool operator==(const MaintenanceWindow&, const MaintenanceWindow&) = default;
};

struct MaintenanceSnapshot {
  MaintenanceGeneration generation;
  Timestamp observed_at;
  std::vector<MaintenanceWindow> windows;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<MaintenanceSnapshot> decode(codec::Reader& reader);

  friend bool operator==(const MaintenanceSnapshot&, const MaintenanceSnapshot&) = default;
};

enum class IncidentState : std::uint8_t {
  normal = 0,
  degraded = 1,
  major = 2,
  unknown = 3,
};

[[nodiscard]] const char* to_string(IncidentState state) noexcept;
[[nodiscard]] Result<IncidentState> incident_state_from_string(std::string_view text);

enum class CapacityTrust : std::uint8_t {
  trusted = 0,
  unreliable = 1,
  unknown = 2,
};

[[nodiscard]] const char* to_string(CapacityTrust trust) noexcept;
[[nodiscard]] Result<CapacityTrust> capacity_trust_from_string(std::string_view text);

struct IncidentSnapshot {
  IncidentGeneration generation;
  Timestamp observed_at;
  std::optional<IncidentId> incident;
  TargetScope scope;
  IncidentState state = IncidentState::unknown;
  CapacityTrust capacity_trust = CapacityTrust::unknown;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<IncidentSnapshot> decode(codec::Reader& reader);

  friend bool operator==(const IncidentSnapshot&, const IncidentSnapshot&) = default;
};

struct PlacementPolicySnapshot {
  PlacementGeneration generation;
  Timestamp observed_at;

  // An empty allow list means "no allow-list restriction was stated", which is
  // only usable when the caller requires placement conformance; the engine
  // treats a forbidden rack and a required zone as hard constraints.
  std::vector<RackId> allowed_racks;
  std::vector<RackId> forbidden_racks;
  std::optional<ZoneId> required_zone;
  std::optional<std::uint64_t> max_units_per_rack;
  std::optional<std::uint64_t> max_units_per_facility;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<PlacementPolicySnapshot> decode(codec::Reader& reader);

  friend bool operator==(const PlacementPolicySnapshot&, const PlacementPolicySnapshot&) = default;
};

enum class PolicyVerdict : std::uint8_t {
  permit = 0,
  deny = 1,
  abstain = 2,
};

[[nodiscard]] const char* to_string(PolicyVerdict verdict) noexcept;
[[nodiscard]] Result<PolicyVerdict> policy_verdict_from_string(std::string_view text);

enum class OvercommitMode : std::uint8_t {
  not_permitted = 0,
  permitted_unbounded = 1,
  permitted_up_to = 2,
};

[[nodiscard]] const char* to_string(OvercommitMode mode) noexcept;

struct OvercommitAllowance {
  OvercommitMode mode = OvercommitMode::not_permitted;
  AmountVector allowance;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<OvercommitAllowance> decode(codec::Reader& reader);

  friend bool operator==(const OvercommitAllowance&, const OvercommitAllowance&) = default;
};

// The Facility Policy Engine result for one facility. The digest is the policy
// engine's own content digest; the generation is what a decision binds to, so a
// later policy change is detectable as a different generation.
struct PolicySnapshot {
  PolicyId policy;
  PolicyGeneration generation;
  Timestamp observed_at;
  PolicyVerdict verdict = PolicyVerdict::abstain;
  OvercommitAllowance overcommit;
  Digest256 policy_digest;

  [[nodiscard]] Status validate() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<PolicySnapshot> decode(codec::Reader& reader);

  friend bool operator==(const PolicySnapshot&, const PolicySnapshot&) = default;
};

}  // namespace fac

#endif  // FAC_MODEL_FACILITY_STATE_HPP
