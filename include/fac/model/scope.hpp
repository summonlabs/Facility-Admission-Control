// Facility Admission Control - facility scope.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A scope names where a commitment lands: a facility, optionally narrowed to a
// rack or a zone. Scope comparison is deliberately conservative. When a
// maintenance window or an incident is recorded against a facility and a
// request names only a rack, the exposure check answers "affected", because
// proving the opposite would require evidence this runtime does not own.

#ifndef FAC_MODEL_SCOPE_HPP
#define FAC_MODEL_SCOPE_HPP

#include <optional>

#include "fac/codec/codec.hpp"
#include "fac/core/error.hpp"
#include "fac/core/ident.hpp"

namespace fac {

struct TargetScope {
  FacilityId facility;
  std::optional<RackId> rack;
  std::optional<ZoneId> zone;

  [[nodiscard]] static Result<TargetScope> make(FacilityId facility, std::optional<RackId> rack,
                                                std::optional<ZoneId> zone);

  [[nodiscard]] bool names_rack(RackId candidate) const;
  [[nodiscard]] bool names_zone(ZoneId candidate) const;

  // True when a constraint recorded at this scope can affect a request made at
  // the other scope. Conservative: an unstated narrowing means "everything".
  [[nodiscard]] bool affects(const TargetScope& other) const;

  // True when every part of the other scope is inside this scope.
  [[nodiscard]] bool contains(const TargetScope& other) const;

  [[nodiscard]] std::string to_string() const;

  void encode(codec::Writer& writer) const;
  [[nodiscard]] static Result<TargetScope> decode(codec::Reader& reader);

  friend bool operator==(const TargetScope&, const TargetScope&) = default;
};

}  // namespace fac

#endif  // FAC_MODEL_SCOPE_HPP
