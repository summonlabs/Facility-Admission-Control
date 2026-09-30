// Facility Admission Control - command line interface.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The command line tool is the operating surface for the runtime: it evaluates
// a request described by a scenario file, executes grants, records the evidence
// that comes back from the reservation owner, and inspects the durable store.
// Every response is deterministic canonical text, so an operator or a test can
// compare it byte for byte.
//
// Exit codes are part of the interface:
//   0  the command completed and the verdict was allow or committed
//   1  the command completed and the verdict was refuse, defer or fenced
//   2  usage or environment error
//   3  the operation could not be completed

#include "fac/cli/cli.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"
#include "fac/engine/engine.hpp"
#include "fac/version.hpp"

namespace fac::cli {
namespace {

// ---------------------------------------------------------------------------
// Small argument and text helpers
// ---------------------------------------------------------------------------

constexpr char kDimensionSeparator = ',';

[[nodiscard]] std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == separator) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  return parts;
}

[[nodiscard]] std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char c : line) {
    if (c == ' ' || c == '\t') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

[[nodiscard]] Result<std::uint64_t> parse_u64(const std::string& text, const char* what) {
  if (text.empty() || text.size() > 20) {
    return make_error(ErrorCode::invalid_argument, std::string("unusable ") + what);
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return make_error(ErrorCode::invalid_argument, std::string("unusable ") + what);
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return make_error(ErrorCode::out_of_range, std::string(what) + " is out of range");
    }
    value = value * 10 + digit;
  }
  return value;
}

[[nodiscard]] bool parse_yes_no(const std::string& text, bool& out) {
  if (text == "yes" || text == "true" || text == "1") {
    out = true;
    return true;
  }
  if (text == "no" || text == "false" || text == "0") {
    out = false;
    return true;
  }
  return false;
}

struct Options {
  std::map<std::string, std::string> values;
  std::map<std::string, std::vector<std::string>> repeated;
  std::vector<std::string> positional;

  [[nodiscard]] bool has(const std::string& name) const { return values.count(name) > 0; }
  [[nodiscard]] std::string get(const std::string& name, const std::string& fallback = {}) const {
    const auto found = values.find(name);
    return found == values.end() ? fallback : found->second;
  }
  [[nodiscard]] Result<std::string> require(const std::string& name) const {
    const auto found = values.find(name);
    if (found == values.end() || found->second.empty()) {
      return make_error(ErrorCode::invalid_argument, "missing required option --" + name);
    }
    return found->second;
  }
};

[[nodiscard]] Result<Options> parse_options(const std::vector<std::string>& arguments,
                                            std::size_t first) {
  Options options;
  std::size_t index = first;
  while (index < arguments.size()) {
    const std::string& token = arguments[index];
    if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
      const std::string name = token.substr(2);
      if (index + 1 < arguments.size() &&
          !(arguments[index + 1].size() > 2 && arguments[index + 1][0] == '-' &&
            arguments[index + 1][1] == '-')) {
        options.values[name] = arguments[index + 1];
        options.repeated[name].push_back(arguments[index + 1]);
        index += 2;
      } else {
        options.values[name] = "true";
        options.repeated[name].push_back("true");
        index += 1;
      }
      continue;
    }
    options.positional.push_back(token);
    ++index;
  }
  return options;
}

[[nodiscard]] Result<OwnerId> default_owner() {
  codec::Writer seed;
  seed.text("facility-admission-control/cli/default-reservation-owner/v1");
  const Digest256 digest = Digest256::of(seed.span());
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (int i = 0; i < 8; ++i) {
    high = (high << 8) |
           static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(digest.bytes()[static_cast<std::size_t>(i)]));
    low = (low << 8) |
          static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(digest.bytes()[static_cast<std::size_t>(i) + 8]));
  }
  return OwnerId(high, low);
}

// ---------------------------------------------------------------------------
// Scenario files
// ---------------------------------------------------------------------------

struct Scenario {
  AdmissionRequest request;
  EvidenceBundle evidence;
};

// Accepts both "power=200" and "power 200" spellings, so a scenario line can be
// written the way it reads best. The value "unknown" leaves the dimension
// unmeasured, which is different from stating a zero.
[[nodiscard]] Result<AmountVector> parse_amounts(const std::vector<std::string>& tokens,
                                                 std::size_t first) {
  AmountVector vector;
  std::size_t i = first;
  while (i < tokens.size()) {
    std::string name;
    std::string text;
    const auto pieces = split(tokens[i], '=');
    if (pieces.size() == 2) {
      name = pieces[0];
      text = pieces[1];
      ++i;
    } else if (pieces.size() == 1 && i + 1 < tokens.size()) {
      name = tokens[i];
      text = tokens[i + 1];
      i += 2;
    } else {
      return make_error(ErrorCode::invalid_argument, "amount must be written dimension=value");
    }
    auto dimension = dimension_from_string(name);
    if (!dimension.ok()) {
      return dimension.error();
    }
    if (text == "unknown") {
      continue;
    }
    auto value = parse_u64(text, "amount");
    if (!value.ok()) {
      return value.error();
    }
    const Status set = vector.set(dimension.value(), value.value());
    if (!set.ok()) {
      return set.error();
    }
  }
  return vector;
}

[[nodiscard]] Result<std::uint64_t> parse_nanos(const std::string& text, const char* what) {
  auto value = parse_u64(text, what);
  if (!value.ok()) {
    return value.error();
  }
  if (!is_valid_timestamp_nanos(static_cast<Nanos>(value.value()))) {
    return make_error(ErrorCode::out_of_range, std::string(what) + " is outside the supported range");
  }
  return value;
}

[[nodiscard]] Result<RackId> parse_rack(const std::string& text) { return RackId::from_hex(text); }
[[nodiscard]] Result<ZoneId> parse_zone(const std::string& text) { return ZoneId::from_hex(text); }

[[nodiscard]] Result<Scenario> parse_scenario(const std::string& text) {
  Scenario scenario;
  bool have_request = false;
  bool have_window = false;
  std::string section;
  MaintenanceWindow current_window;
  bool window_open = false;

  const auto fail = [](std::size_t line, const std::string& detail) {
    return make_error(ErrorCode::invalid_argument,
                      "scenario line " + std::to_string(line) + ": " + detail);
  };

  std::istringstream stream(text);
  std::string raw;
  std::size_t line_number = 0;
  while (std::getline(stream, raw)) {
    ++line_number;
    if (!raw.empty() && raw.back() == '\r') {
      raw.pop_back();
    }
    const std::size_t comment = raw.find('#');
    if (comment != std::string::npos) {
      raw.resize(comment);
    }
    const std::vector<std::string> tokens = tokenize(raw);
    if (tokens.empty()) {
      continue;
    }
    if (tokens.size() > 64) {
      return fail(line_number, "too many tokens on one line");
    }
    for (const auto& token : tokens) {
      if (!is_valid_utf8(token) || token.size() > kMaxNameLength * 4) {
        return fail(line_number, "token is not usable text");
      }
    }
    const std::string& head = tokens[0];

    if (head == "request") {
      if (tokens.size() != 2) return fail(line_number, "request takes one identity");
      auto id = RequestId::from_hex(tokens[1]);
      if (!id.ok()) return fail(line_number, id.error().to_string());
      scenario.request.request_id = id.value();
      have_request = true;
      continue;
    }
    if (head == "tenant") {
      if (tokens.size() != 2) return fail(line_number, "tenant takes one identity");
      auto id = TenantId::from_hex(tokens[1]);
      if (!id.ok()) return fail(line_number, id.error().to_string());
      scenario.request.tenant = id.value();
      continue;
    }
    if (head == "service-class") {
      if (tokens.size() != 2) return fail(line_number, "service-class takes one identity");
      auto id = ServiceClassId::from_hex(tokens[1]);
      if (!id.ok()) return fail(line_number, id.error().to_string());
      scenario.request.service_class = id.value();
      continue;
    }
    if (head == "envelope") {
      if (tokens.size() != 2) return fail(line_number, "envelope takes one identity");
      auto id = EnvelopeId::from_hex(tokens[1]);
      if (!id.ok()) return fail(line_number, id.error().to_string());
      scenario.request.envelope = id.value();
      continue;
    }
    if (head == "scope") {
      if (tokens.size() < 3 || tokens[1] != "facility") {
        return fail(line_number, "scope must start with 'facility'");
      }
      auto facility = FacilityId::from_hex(tokens[2]);
      if (!facility.ok()) return fail(line_number, facility.error().to_string());
      std::optional<RackId> rack;
      std::optional<ZoneId> zone;
      for (std::size_t i = 3; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "rack") {
          auto value = parse_rack(tokens[i + 1]);
          if (!value.ok()) return fail(line_number, value.error().to_string());
          rack = value.value();
        } else if (tokens[i] == "zone") {
          auto value = parse_zone(tokens[i + 1]);
          if (!value.ok()) return fail(line_number, value.error().to_string());
          zone = value.value();
        } else {
          return fail(line_number, "unknown scope key");
        }
      }
      auto scope = TargetScope::make(facility.value(), rack, zone);
      if (!scope.ok()) return fail(line_number, scope.error().to_string());
      scenario.request.scope = scope.take();
      continue;
    }
    if (head == "demand") {
      auto amounts = parse_amounts(tokens, 1);
      if (!amounts.ok()) return fail(line_number, amounts.error().to_string());
      scenario.request.demand = amounts.take();
      continue;
    }
    if (head == "requested-at") {
      if (tokens.size() != 2) return fail(line_number, "requested-at takes one value");
      auto value = parse_nanos(tokens[1], "requested-at");
      if (!value.ok()) return fail(line_number, value.error().to_string());
      scenario.request.requested_at = Timestamp(static_cast<Nanos>(value.value()));
      continue;
    }
    if (head == "window") {
      if (tokens.size() != 3) return fail(line_number, "window takes a start and an end");
      auto start = parse_nanos(tokens[1], "window start");
      if (!start.ok()) return fail(line_number, start.error().to_string());
      auto end = parse_nanos(tokens[2], "window end");
      if (!end.ok()) return fail(line_number, end.error().to_string());
      scenario.request.commitment_start = Timestamp(static_cast<Nanos>(start.value()));
      scenario.request.commitment_end = Timestamp(static_cast<Nanos>(end.value()));
      have_window = true;
      continue;
    }
    if (head == "epoch") {
      if (tokens.size() != 2) return fail(line_number, "epoch takes one value");
      auto value = parse_u64(tokens[1], "epoch");
      if (!value.ok()) return fail(line_number, value.error().to_string());
      auto epoch = ControlEpoch::from_value(value.value());
      if (!epoch.ok()) return fail(line_number, epoch.error().to_string());
      scenario.request.expected_epoch = epoch.value();
      continue;
    }
    if (head == "pin") {
      if (tokens.size() != 3) return fail(line_number, "pin takes an evidence kind and a generation");
      auto kind = evidence_kind_from_string(tokens[1]);
      if (!kind.ok()) return fail(line_number, kind.error().to_string());
      auto value = parse_u64(tokens[2], "generation");
      if (!value.ok()) return fail(line_number, value.error().to_string());
      GenerationPin pin;
      pin.kind = kind.value();
      pin.generation = value.value();
      scenario.request.pins.push_back(pin);
      continue;
    }

    // ---- evidence sections ----
    if (window_open) {
      auto window = current_window;
      const Status valid = window.validate();
      if (!valid.ok()) return fail(line_number, valid.error().to_string());
      if (!scenario.evidence.maintenance.has_value()) {
        return fail(line_number, "a maintenance window appears before its section header");
      }
      if (scenario.evidence.maintenance->windows.size() >= kMaxMaintenanceWindows) {
        return fail(line_number, "too many maintenance windows");
      }
      scenario.evidence.maintenance->windows.push_back(window);
      window_open = false;
    }

    if (head == "capacity") {
      section = "capacity";
      CapacitySnapshot snapshot;
      bool have_generation = false;
      bool have_observed = false;
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "generation" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "capacity generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = CapacityGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
          have_generation = true;
          i += 2;
        } else if (key == "observed" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "capacity observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
          have_observed = true;
          i += 2;
        } else if (key == "total" || key == "committed" || key == "reserved") {
          std::vector<std::string> values;
          std::size_t j = i + 1;
          while (j < tokens.size() && tokens[j].find('=') != std::string::npos) {
            values.push_back(tokens[j]);
            ++j;
          }
          auto amounts = parse_amounts(values, 0);
          if (!amounts.ok()) return fail(line_number, amounts.error().to_string());
          if (key == "total") snapshot.total = amounts.take();
          if (key == "committed") snapshot.committed = amounts.take();
          if (key == "reserved") snapshot.reserved = amounts.take();
          i = j;
        } else {
          return fail(line_number, "unknown capacity key");
        }
      }
      if (!have_generation || !have_observed) {
        return fail(line_number, "capacity needs a generation and an observation time");
      }
      scenario.evidence.capacity = snapshot;
      continue;
    }
    if (head == "redundancy") {
      section = "redundancy";
      RedundancySnapshot snapshot;
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "generation" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "redundancy generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = RedundancyGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
          i += 2;
        } else if (key == "observed" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "redundancy observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "level" && i + 1 < tokens.size()) {
          auto level = redundancy_level_from_string(tokens[i + 1]);
          if (!level.ok()) return fail(line_number, level.error().to_string());
          snapshot.level = level.value();
          snapshot.level_stated = true;
          i += 2;
        } else if (key == "headroom") {
          std::vector<std::string> values;
          std::size_t j = i + 1;
          while (j < tokens.size() && tokens[j].find('=') != std::string::npos) {
            values.push_back(tokens[j]);
            ++j;
          }
          auto amounts = parse_amounts(values, 0);
          if (!amounts.ok()) return fail(line_number, amounts.error().to_string());
          snapshot.protected_headroom = amounts.take();
          i = j;
        } else {
          return fail(line_number, "unknown redundancy key");
        }
      }
      scenario.evidence.redundancy = snapshot;
      continue;
    }
    if (head == "tenant-evidence") {
      section = "tenant";
      TenantSnapshot snapshot;
      for (std::size_t i = 1; i + 1 < tokens.size(); i += 2) {
        const std::string& key = tokens[i];
        if (key == "generation") {
          auto value = parse_u64(tokens[i + 1], "tenant generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = TenantGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
        } else if (key == "observed") {
          auto value = parse_nanos(tokens[i + 1], "tenant observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
        } else if (key == "status") {
          auto status = tenant_status_from_string(tokens[i + 1]);
          if (!status.ok()) return fail(line_number, status.error().to_string());
          snapshot.status = status.value();
        } else {
          return fail(line_number, "unknown tenant key");
        }
      }
      snapshot.tenant = scenario.request.tenant;
      scenario.evidence.tenant = snapshot;
      continue;
    }
    if (head == "envelope-evidence") {
      section = "envelope";
      EnvelopeSnapshot snapshot;
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "generation" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "envelope generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = EnvelopeGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
          i += 2;
        } else if (key == "observed" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "envelope observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "limit" || key == "consumed") {
          std::vector<std::string> values;
          std::size_t j = i + 1;
          while (j < tokens.size() && tokens[j].find('=') != std::string::npos) {
            values.push_back(tokens[j]);
            ++j;
          }
          auto amounts = parse_amounts(values, 0);
          if (!amounts.ok()) return fail(line_number, amounts.error().to_string());
          if (key == "limit") snapshot.limit = amounts.take();
          if (key == "consumed") snapshot.consumed = amounts.take();
          i = j;
        } else {
          return fail(line_number, "unknown envelope key");
        }
      }
      snapshot.envelope = scenario.request.envelope;
      snapshot.tenant = scenario.request.tenant;
      scenario.evidence.envelope = snapshot;
      continue;
    }
    if (head == "service-class-evidence") {
      section = "service-class";
      ServiceClassSnapshot snapshot;
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "generation" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "service class generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = ServiceClassGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
          i += 2;
        } else if (key == "observed" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "service class observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "min-redundancy" && i + 1 < tokens.size()) {
          auto level = redundancy_level_from_string(tokens[i + 1]);
          if (!level.ok()) return fail(line_number, level.error().to_string());
          snapshot.obligations.minimum_redundancy = level.value();
          snapshot.obligations.minimum_redundancy_stated = true;
          i += 2;
        } else if (key == "headroom") {
          std::vector<std::string> values;
          std::size_t j = i + 1;
          while (j < tokens.size() && tokens[j].find('=') != std::string::npos) {
            values.push_back(tokens[j]);
            ++j;
          }
          auto amounts = parse_amounts(values, 0);
          if (!amounts.ok()) return fail(line_number, amounts.error().to_string());
          snapshot.obligations.required_protected_headroom = amounts.take();
          i = j;
        } else if ((key == "clearance" || key == "overcommit") && i + 1 < tokens.size()) {
          bool flag = false;
          if (!parse_yes_no(tokens[i + 1], flag)) {
            return fail(line_number, "expected yes or no");
          }
          if (key == "clearance") snapshot.obligations.requires_maintenance_clearance = flag;
          if (key == "overcommit") snapshot.obligations.permits_overcommit = flag;
          i += 2;
        } else {
          return fail(line_number, "unknown service class key");
        }
      }
      snapshot.service_class = scenario.request.service_class;
      scenario.evidence.service_class = snapshot;
      continue;
    }
    if (head == "maintenance") {
      section = "maintenance";
      MaintenanceSnapshot snapshot;
      for (std::size_t i = 1; i + 1 < tokens.size(); i += 2) {
        if (tokens[i] == "generation") {
          auto value = parse_u64(tokens[i + 1], "maintenance generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = MaintenanceGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
        } else if (tokens[i] == "observed") {
          auto value = parse_nanos(tokens[i + 1], "maintenance observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
        } else {
          return fail(line_number, "unknown maintenance key");
        }
      }
      scenario.evidence.maintenance = snapshot;
      continue;
    }
    if (head == "maintenance-window") {
      if (section != "maintenance" || !scenario.evidence.maintenance.has_value()) {
        return fail(line_number, "a maintenance window appears before its section header");
      }
      current_window = MaintenanceWindow{};
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "id" && i + 1 < tokens.size()) {
          auto id = MaintenanceWindowId::from_hex(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          current_window.window = id.value();
          i += 2;
        } else if (key == "facility" && i + 1 < tokens.size()) {
          auto id = FacilityId::from_hex(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          current_window.scope.facility = id.value();
          i += 2;
        } else if (key == "rack" && i + 1 < tokens.size()) {
          auto id = parse_rack(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          current_window.scope.rack = id.value();
          i += 2;
        } else if (key == "zone" && i + 1 < tokens.size()) {
          auto id = parse_zone(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          current_window.scope.zone = id.value();
          i += 2;
        } else if (key == "start" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "window start");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          current_window.start = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "end" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "window end");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          current_window.end = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "state" && i + 1 < tokens.size()) {
          auto state = maintenance_state_from_string(tokens[i + 1]);
          if (!state.ok()) return fail(line_number, state.error().to_string());
          current_window.state = state.value();
          i += 2;
        } else if (key == "impact" && i + 1 < tokens.size()) {
          auto impact = maintenance_impact_from_string(tokens[i + 1]);
          if (!impact.ok()) return fail(line_number, impact.error().to_string());
          current_window.impact = impact.value();
          i += 2;
        } else {
          return fail(line_number, "unknown maintenance window key");
        }
      }
      window_open = true;
      continue;
    }
    if (head == "incident") {
      section = "incident";
      IncidentSnapshot snapshot;
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "generation" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "incident generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = IncidentGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
          i += 2;
        } else if (key == "observed" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "incident observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "id" && i + 1 < tokens.size()) {
          auto id = IncidentId::from_hex(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.incident = id.value();
          i += 2;
        } else if (key == "facility" && i + 1 < tokens.size()) {
          auto id = FacilityId::from_hex(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.scope.facility = id.value();
          i += 2;
        } else if (key == "rack" && i + 1 < tokens.size()) {
          auto id = parse_rack(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.scope.rack = id.value();
          i += 2;
        } else if (key == "zone" && i + 1 < tokens.size()) {
          auto id = parse_zone(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.scope.zone = id.value();
          i += 2;
        } else if (key == "state" && i + 1 < tokens.size()) {
          auto state = incident_state_from_string(tokens[i + 1]);
          if (!state.ok()) return fail(line_number, state.error().to_string());
          snapshot.state = state.value();
          i += 2;
        } else if (key == "trust" && i + 1 < tokens.size()) {
          auto trust = capacity_trust_from_string(tokens[i + 1]);
          if (!trust.ok()) return fail(line_number, trust.error().to_string());
          snapshot.capacity_trust = trust.value();
          i += 2;
        } else {
          return fail(line_number, "unknown incident key");
        }
      }
      scenario.evidence.incident = snapshot;
      continue;
    }
    if (head == "placement") {
      section = "placement";
      PlacementPolicySnapshot snapshot;
      snapshot.allowed_racks.clear();
      snapshot.forbidden_racks.clear();
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "generation" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "placement generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = PlacementGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
          i += 2;
        } else if (key == "observed" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "placement observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "allow-rack" && i + 1 < tokens.size()) {
          auto id = parse_rack(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.allowed_racks.push_back(id.value());
          i += 2;
        } else if (key == "forbid-rack" && i + 1 < tokens.size()) {
          auto id = parse_rack(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.forbidden_racks.push_back(id.value());
          i += 2;
        } else if (key == "required-zone" && i + 1 < tokens.size()) {
          auto id = parse_zone(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.required_zone = id.value();
          i += 2;
        } else if (key == "max-units-per-rack" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "per-rack unit limit");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.max_units_per_rack = value.value();
          i += 2;
        } else if (key == "max-units-per-facility" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "per-facility unit limit");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.max_units_per_facility = value.value();
          i += 2;
        } else {
          return fail(line_number, "unknown placement key");
        }
      }
      scenario.evidence.placement = snapshot;
      continue;
    }
    if (head == "policy") {
      section = "policy";
      PolicySnapshot snapshot;
      for (std::size_t i = 1; i < tokens.size();) {
        const std::string& key = tokens[i];
        if (key == "generation" && i + 1 < tokens.size()) {
          auto value = parse_u64(tokens[i + 1], "policy generation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          auto generation = PolicyGeneration::from_value(value.value());
          if (!generation.ok()) return fail(line_number, generation.error().to_string());
          snapshot.generation = generation.value();
          i += 2;
        } else if (key == "observed" && i + 1 < tokens.size()) {
          auto value = parse_nanos(tokens[i + 1], "policy observation");
          if (!value.ok()) return fail(line_number, value.error().to_string());
          snapshot.observed_at = Timestamp(static_cast<Nanos>(value.value()));
          i += 2;
        } else if (key == "verdict" && i + 1 < tokens.size()) {
          auto verdict = policy_verdict_from_string(tokens[i + 1]);
          if (!verdict.ok()) return fail(line_number, verdict.error().to_string());
          snapshot.verdict = verdict.value();
          i += 2;
        } else if (key == "overcommit" && i + 1 < tokens.size()) {
          const std::string& mode = tokens[i + 1];
          if (mode == "none") {
            snapshot.overcommit.mode = OvercommitMode::not_permitted;
          } else if (mode == "unbounded") {
            snapshot.overcommit.mode = OvercommitMode::permitted_unbounded;
          } else {
            return fail(line_number, "overcommit must be none, unbounded, or followed by an allowance");
          }
          i += 2;
        } else if (key == "allowance") {
          std::vector<std::string> values;
          std::size_t j = i + 1;
          while (j < tokens.size() && tokens[j].find('=') != std::string::npos) {
            values.push_back(tokens[j]);
            ++j;
          }
          auto amounts = parse_amounts(values, 0);
          if (!amounts.ok()) return fail(line_number, amounts.error().to_string());
          snapshot.overcommit.mode = OvercommitMode::permitted_up_to;
          snapshot.overcommit.allowance = amounts.take();
          i = j;
        } else if (key == "digest" && i + 1 < tokens.size()) {
          auto digest = Digest256::from_hex(tokens[i + 1]);
          if (!digest.ok()) return fail(line_number, digest.error().to_string());
          snapshot.policy_digest = digest.value();
          i += 2;
        } else if (key == "id" && i + 1 < tokens.size()) {
          auto id = PolicyId::from_hex(tokens[i + 1]);
          if (!id.ok()) return fail(line_number, id.error().to_string());
          snapshot.policy = id.value();
          i += 2;
        } else {
          return fail(line_number, "unknown policy key");
        }
      }
      scenario.evidence.policy = snapshot;
      continue;
    }
    return fail(line_number, "unknown scenario directive: " + escape_text(head));
  }

  if (window_open) {
    if (!scenario.evidence.maintenance.has_value()) {
      return make_error(ErrorCode::invalid_argument, "a maintenance window has no section header");
    }
    scenario.evidence.maintenance->windows.push_back(current_window);
  }
  if (!have_request) {
    return make_error(ErrorCode::invalid_argument, "scenario does not name a request identity");
  }
  if (!have_window) {
    scenario.request.commitment_start = Timestamp{};
    scenario.request.commitment_end = Timestamp{};
  }
  const Status valid = scenario.request.validate();
  if (!valid.ok()) {
    return make_error(ErrorCode::invalid_argument, "scenario request is not usable: " + valid.error().to_string());
  }
  return scenario;
}

[[nodiscard]] Result<std::string> read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::file_not_found, "scenario file could not be opened: " + escape_text(path));
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  std::string text = buffer.str();
  if (text.size() > (1u << 20)) {
    return make_error(ErrorCode::limit_exceeded, "scenario file is larger than one megabyte");
  }
  return text;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void render_amounts(std::ostream& out, const AmountVector& amounts) {
  for (const Dimension dimension : all_dimensions()) {
    const auto value = amounts.get(dimension);
    if (value.has_value()) {
      out << " " << to_string(dimension) << "=" << value.value();
    } else {
      out << " " << to_string(dimension) << "=unknown";
    }
  }
}

void render_evidence(std::ostream& out, const EvidenceSet& evidence) {
  for (const auto& reference : evidence.refs()) {
    out << "evidence " << to_string(reference.kind) << " generation " << reference.generation
        << (reference.accepted ? " accepted" : " rejected") << " observed "
        << reference.observed_at.to_string() << " digest " << reference.digest.to_hex() << "\n";
  }
}

void render_assessment(std::ostream& out, const CommitmentAssessment& assessment) {
  for (const auto& entry : assessment.dimensions()) {
    out << "assessment " << to_string(entry.dimension);
    const auto number = [&out](const char* name, const std::optional<std::uint64_t>& value) {
      out << " " << name << "=" << (value.has_value() ? std::to_string(value.value()) : std::string("unknown"));
    };
    number("total", entry.total);
    number("committed", entry.committed);
    number("reserved", entry.reserved);
    number("available", entry.available);
    number("demand", entry.demand);
    if (entry.remaining.has_value()) {
      out << " remaining=" << entry.remaining.value();
    } else {
      out << " remaining=unknown";
    }
    number("protected", entry.protected_headroom);
    number("overcommit", entry.overcommit);
    out << "\n";
  }
}

void render_decision(std::ostream& out, const Decision& decision) {
  out << "request " << decision.request_id.to_string() << "\n";
  out << "request-digest " << decision.request_digest.to_hex() << "\n";
  out << "verdict " << to_string(decision.verdict) << "\n";
  out << "control-epoch " << decision.control_epoch.to_string() << "\n";
  out << "sequence " << decision.sequence.to_string() << "\n";
  out << "decided-at " << decision.decided_at.to_string() << "\n";
  if (const Blocker* primary = decision.blockers.primary()) {
    out << "primary-blocker " << to_string(primary->code) << "\n";
  }
  for (const auto& blocker : decision.blockers.items()) {
    out << "blocker " << blocker.render() << "\n";
  }
  render_evidence(out, decision.evidence);
  render_assessment(out, decision.assessment);
  for (const auto& line : decision.explanation) {
    out << "explanation " << line << "\n";
  }
  if (decision.grant.has_value()) {
    const Grant& grant = decision.grant.value();
    out << "grant " << grant.grant_id.to_string() << "\n";
    out << "grant-expires " << grant.expires_at.to_string() << "\n";
    out << "grant-holds-capacity " << (grant.holds_capacity ? "true" : "false") << "\n";
    out << "grant-binding-digest " << grant.binding_digest.to_hex() << "\n";
    for (const auto& binding : grant.bindings) {
      out << "grant-binding " << to_string(binding.kind) << " generation " << binding.generation
          << " digest " << binding.evidence_digest.to_hex() << "\n";
    }
  }
  out << "decision-digest " << decision.digest.to_hex() << "\n";
  out << "replayed " << (decision.replayed ? "true" : "false") << "\n";
}

void render_commit(std::ostream& out, const CommitOutcome& outcome) {
  out << "grant " << outcome.grant_id.to_string() << "\n";
  out << "commit-state " << to_string(outcome.state) << "\n";
  out << "resolved-at " << outcome.resolved_at.to_string() << "\n";
  out << "sequence " << outcome.sequence.to_string() << "\n";
  if (outcome.commitment_id.has_value()) {
    out << "commitment " << outcome.commitment_id.value().to_string() << "\n";
  }
  for (const auto& blocker : outcome.blockers.items()) {
    out << "blocker " << blocker.render() << "\n";
  }
  out << "replayed " << (outcome.replayed ? "true" : "false") << "\n";
}

void render_commitment(std::ostream& out, const CommitmentRecord& commitment) {
  out << "commitment " << commitment.commitment_id.to_string() << "\n";
  out << "state " << to_string(commitment.state) << "\n";
  out << "request " << commitment.request_id.to_string() << "\n";
  out << "grant " << commitment.grant_id.to_string() << "\n";
  out << "intent " << commitment.intent.intent_id.to_string() << "\n";
  out << "owner " << commitment.intent.owner.to_string() << "\n";
  out << "recorded-at " << commitment.recorded_at.to_string() << "\n";
  out << "intent-valid-until " << commitment.expires_at.to_string() << "\n";
  if (commitment.resolved_at.has_value()) {
    out << "resolved-at " << commitment.resolved_at.value().to_string() << "\n";
  }
  if (!commitment.resolution_detail.empty()) {
    out << "detail " << commitment.resolution_detail << "\n";
  }
  if (commitment.evidence.has_value()) {
    const ReservationEvidence& evidence = commitment.evidence.value();
    out << "reservation " << evidence.reservation.to_string() << "\n";
    out << "outcome " << to_string(evidence.outcome) << "\n";
    out << "owner-generation " << evidence.owner_generation << "\n";
    out << "acknowledged-at " << evidence.acknowledged_at.to_string() << "\n";
  }
}

// ---------------------------------------------------------------------------
// Engine construction
// ---------------------------------------------------------------------------

struct EngineSetup {
  AdmissionPolicy policy;
  bool volatile_engine = false;
  bool create = false;
};

[[nodiscard]] Result<EngineSetup> engine_setup(const Options& options) {
  EngineSetup setup;
  auto owner = default_owner();
  if (!owner.ok()) {
    return owner.error();
  }
  setup.policy.reservation_owner = owner.value();
  if (options.has("owner")) {
    auto parsed = OwnerId::from_hex(options.get("owner"));
    if (!parsed.ok()) {
      return make_error(ErrorCode::invalid_argument, "--owner is not a usable identity");
    }
    setup.policy.reservation_owner = parsed.value();
  }
  const auto duration = [&options](const std::string& name, Duration& target) -> Status {
    if (!options.has(name)) {
      return Status::success();
    }
    auto seconds = parse_u64(options.get(name), name.c_str());
    if (!seconds.ok()) {
      return seconds.error();
    }
    target = Duration::from_seconds(static_cast<Nanos>(seconds.value()));
    return Status::success();
  };
  Status status = duration("grant-validity", setup.policy.grant_validity);
  if (!status.ok()) return status.error();
  status = duration("intent-validity", setup.policy.intent_validity);
  if (!status.ok()) return status.error();
  if (options.has("no-holds")) {
    setup.policy.grant_holds_capacity = false;
  }
  setup.volatile_engine = options.has("in-memory");
  setup.create = options.has("create");
  const Status valid = setup.policy.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  return setup;
}

[[nodiscard]] Result<std::unique_ptr<Engine>> open_engine(const Options& options,
                                                          std::optional<Timestamp> now,
                                                          bool reader) {
  auto setup = engine_setup(options);
  if (!setup.ok()) {
    return setup.error();
  }
  auto clock = std::make_shared<FixedClock>(now.value_or(Timestamp(1)));
  if (reader) {
    auto directory = options.require("store");
    if (!directory.ok()) {
      return directory.error();
    }
    return Engine::open_reader(directory.value(), setup.value().policy, clock);
  }
  if (setup.value().volatile_engine) {
    return Engine::open_in_memory(setup.value().policy, clock);
  }
  auto directory = options.require("store");
  if (!directory.ok()) {
    return directory.error();
  }
  return Engine::open_durable(directory.value(), setup.value().policy, clock, setup.value().create);
}

[[nodiscard]] Result<Timestamp> evaluation_time(const Options& options) {
  if (options.has("now")) {
    auto value = parse_nanos(options.get("now"), "now");
    if (!value.ok()) {
      return value.error();
    }
    return Timestamp(static_cast<Nanos>(value.value()));
  }
  const Timestamp value = SystemClock{}.now();
  if (value.is_unset()) {
    return make_error(ErrorCode::out_of_range, "the system clock is not usable");
  }
  return value;
}

[[nodiscard]] Result<Scenario> load_scenario(const Options& options) {
  auto path = options.require("scenario");
  if (!path.ok()) {
    return path.error();
  }
  auto text = read_text_file(path.value());
  if (!text.ok()) {
    return text.error();
  }
  auto scenario = parse_scenario(text.value());
  if (!scenario.ok()) {
    return scenario.error();
  }
  return scenario;
}

// ---------------------------------------------------------------------------
// Command execution context
// ---------------------------------------------------------------------------

// The result of the most recent command in a session, so a script can refer to
// it as @grant, @commitment, @intent or @request.
struct SessionState {
  std::optional<GrantId> last_grant;
  std::optional<CommitmentId> last_commitment;
  std::optional<IntentId> last_intent;
  std::optional<RequestId> last_request;
};

struct CommandContext {
  Engine* engine = nullptr;
  SessionState* session = nullptr;
};

// Owns an engine when the command opened one, or borrows the engine a session
// already opened. A command never opens a second engine for the same store.
class EngineHolder {
 public:
  // Result<T> requires a default-constructible payload; an empty holder is
  // never reachable through a successful Result.
  EngineHolder() = default;
  explicit EngineHolder(std::unique_ptr<Engine> owned) : owned_(std::move(owned)) {}
  explicit EngineHolder(Engine* borrowed) : borrowed_(borrowed) {}

  [[nodiscard]] Engine* get() const { return owned_ ? owned_.get() : borrowed_; }

 private:
  std::unique_ptr<Engine> owned_;
  Engine* borrowed_ = nullptr;
};

[[nodiscard]] Result<EngineHolder> acquire_engine(const CommandContext& context,
                                                  const Options& options, Timestamp now,
                                                  bool reader) {
  if (context.engine != nullptr) {
    return EngineHolder(context.engine);
  }
  auto opened = open_engine(options, now, reader);
  if (!opened.ok()) {
    return opened.error();
  }
  return EngineHolder(opened.take());
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int command_admit(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) {
    err << "error " << now.error().to_string() << "\n";
    return 2;
  }
  auto holder = acquire_engine(context, options, now.value(), false);
  if (!holder.ok()) {
    err << "error " << holder.error().to_string() << "\n";
    return 2;
  }
  Engine& engine = *holder.value().get();
  auto scenario = load_scenario(options);
  if (!scenario.ok()) {
    err << "error " << scenario.error().to_string() << "\n";
    return 2;
  }
  auto decision = engine.admit(scenario.value().request, scenario.value().evidence);
  if (!decision.ok()) {
    err << "error " << decision.error().to_string() << "\n";
    return 3;
  }
  render_decision(out, decision.value());
  if (context.session != nullptr) {
    context.session->last_request = decision.value().request_id;
    if (decision.value().grant.has_value()) {
      context.session->last_grant = decision.value().grant->grant_id;
    }
  }
  return decision.value().verdict == Verdict::allow ? 0 : 1;
}

int command_commit(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) {
    err << "error " << now.error().to_string() << "\n";
    return 2;
  }
  auto holder = acquire_engine(context, options, now.value(), false);
  if (!holder.ok()) {
    err << "error " << holder.error().to_string() << "\n";
    return 2;
  }
  Engine& engine = *holder.value().get();
  auto grant_text = options.require("grant");
  if (!grant_text.ok()) {
    err << "error " << grant_text.error().to_string() << "\n";
    return 2;
  }
  auto grant_id = GrantId::from_hex(grant_text.value());
  if (!grant_id.ok()) {
    err << "error " << grant_id.error().to_string() << "\n";
    return 2;
  }

  EvidenceBundle evidence;
  const EvidenceBundle* evidence_ptr = nullptr;
  if (options.has("scenario")) {
    auto scenario = load_scenario(options);
    if (!scenario.ok()) {
      err << "error " << scenario.error().to_string() << "\n";
      return 2;
    }
    evidence = scenario.value().evidence;
    evidence_ptr = &evidence;
  }
  auto outcome = engine.commit(grant_id.value(), evidence_ptr);
  if (!outcome.ok()) {
    err << "error " << outcome.error().to_string() << "\n";
    return 3;
  }
  render_commit(out, outcome.value());
  if (outcome.value().state == CommitState::committed && outcome.value().commitment_id.has_value()) {
    const CommitmentRecord* commitment =
        engine.ledger().find_commitment(outcome.value().commitment_id.value());
    if (commitment != nullptr) {
      render_commitment(out, *commitment);
    }
  }
  if (context.session != nullptr && outcome.value().commitment_id.has_value()) {
    context.session->last_commitment = outcome.value().commitment_id;
    const CommitmentRecord* committed = engine.ledger().find_commitment(outcome.value().commitment_id.value());
    if (committed != nullptr) {
      context.session->last_intent = committed->intent.intent_id;
    }
  }
  return outcome.value().state == CommitState::committed ? 0 : 1;
}

int command_ack(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) {
    err << "error " << now.error().to_string() << "\n";
    return 2;
  }
  auto holder = acquire_engine(context, options, now.value(), false);
  if (!holder.ok()) {
    err << "error " << holder.error().to_string() << "\n";
    return 2;
  }
  Engine& engine = *holder.value().get();
  ReservationEvidence evidence;
  auto intent = options.require("intent");
  if (!intent.ok()) { err << "error " << intent.error().to_string() << "\n"; return 2; }
  auto intent_id = IntentId::from_hex(intent.value());
  if (!intent_id.ok()) { err << "error " << intent_id.error().to_string() << "\n"; return 2; }
  evidence.intent_id = intent_id.value();

  auto reservation = options.require("reservation");
  if (!reservation.ok()) { err << "error " << reservation.error().to_string() << "\n"; return 2; }
  auto reservation_id = ReservationId::from_hex(reservation.value());
  if (!reservation_id.ok()) { err << "error " << reservation_id.error().to_string() << "\n"; return 2; }
  evidence.reservation = reservation_id.value();

  // The reservation owner is a property of the engine configuration, so the
  // default answers the authority the intent was addressed to. An explicit
  // --owner must still match, and the ledger refuses it when it does not.
  evidence.owner = engine.policy().reservation_owner;
  if (options.has("owner")) {
    auto owner_id = OwnerId::from_hex(options.get("owner"));
    if (!owner_id.ok()) { err << "error " << owner_id.error().to_string() << "\n"; return 2; }
    evidence.owner = owner_id.value();
  }

  auto outcome = options.require("outcome");
  if (!outcome.ok()) { err << "error " << outcome.error().to_string() << "\n"; return 2; }
  auto parsed_outcome = reservation_outcome_from_string(outcome.value());
  if (!parsed_outcome.ok()) { err << "error " << parsed_outcome.error().to_string() << "\n"; return 2; }
  evidence.outcome = parsed_outcome.value();

  auto generation = options.require("generation");
  if (!generation.ok()) { err << "error " << generation.error().to_string() << "\n"; return 2; }
  auto generation_value = parse_u64(generation.value(), "owner generation");
  if (!generation_value.ok()) { err << "error " << generation_value.error().to_string() << "\n"; return 2; }
  evidence.owner_generation = generation_value.value();

  if (options.has("confirmed")) {
    const auto pieces = split(options.get("confirmed"), kDimensionSeparator);
    auto amounts_value = parse_amounts(pieces, 0);
    if (!amounts_value.ok()) { err << "error " << amounts_value.error().to_string() << "\n"; return 2; }
    evidence.confirmed = amounts_value.take();
  }
  if (evidence.outcome == ReservationOutcome::reserved && !evidence.confirmed.all_present()) {
    err << "error a full reservation must state --confirmed for every dimension\n";
    return 2;
  }

  auto digest = options.require("digest");
  if (!digest.ok()) { err << "error " << digest.error().to_string() << "\n"; return 2; }
  auto digest_value = Digest256::from_hex(digest.value());
  if (!digest_value.ok()) { err << "error " << digest_value.error().to_string() << "\n"; return 2; }
  evidence.owner_digest = digest_value.value();

  auto acknowledged =
      options.has("acknowledged-at")
          ? parse_nanos(options.get("acknowledged-at"), "acknowledged-at")
          : Result<std::uint64_t>(static_cast<std::uint64_t>(now.value().unix_nanos()));
  if (!acknowledged.ok()) { err << "error " << acknowledged.error().to_string() << "\n"; return 2; }
  evidence.acknowledged_at = Timestamp(static_cast<Nanos>(acknowledged.value()));

  auto recorded = engine.record_evidence(evidence);
  if (!recorded.ok()) {
    err << "error " << recorded.error().to_string() << "\n";
    return 3;
  }
  render_commitment(out, recorded.value());
  if (context.session != nullptr) {
    context.session->last_commitment = recorded.value().commitment_id;
    context.session->last_intent = recorded.value().intent.intent_id;
  }
  return 0;
}

int command_release(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context,
                    bool grant) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), false);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  auto reason = options.require("reason");
  if (!reason.ok()) { err << "error " << reason.error().to_string() << "\n"; return 2; }
  if (grant) {
    auto id = options.require("grant");
    if (!id.ok()) { err << "error " << id.error().to_string() << "\n"; return 2; }
    auto parsed = GrantId::from_hex(id.value());
    if (!parsed.ok()) { err << "error " << parsed.error().to_string() << "\n"; return 2; }
    auto released = engine.release_grant(parsed.value(), reason.value());
    if (!released.ok()) { err << "error " << released.error().to_string() << "\n"; return 3; }
    out << "grant " << released.value().grant.grant_id.to_string() << "\n";
    out << "grant-state " << to_string(released.value().state) << "\n";
    out << "detail " << released.value().detail << "\n";
    return 0;
  }
  auto id = options.require("commitment");
  if (!id.ok()) { err << "error " << id.error().to_string() << "\n"; return 2; }
  auto parsed = CommitmentId::from_hex(id.value());
  if (!parsed.ok()) { err << "error " << parsed.error().to_string() << "\n"; return 2; }
  auto released = engine.release_commitment(parsed.value(), reason.value());
  if (!released.ok()) { err << "error " << released.error().to_string() << "\n"; return 3; }
  render_commitment(out, released.value());
  if (context.session != nullptr) {
    context.session->last_commitment = released.value().commitment_id;
  }
  return 0;
}

int command_fence(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), false);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  auto id = options.require("grant");
  if (!id.ok()) { err << "error " << id.error().to_string() << "\n"; return 2; }
  auto parsed = GrantId::from_hex(id.value());
  if (!parsed.ok()) { err << "error " << parsed.error().to_string() << "\n"; return 2; }
  auto code_text = options.require("code");
  if (!code_text.ok()) { err << "error " << code_text.error().to_string() << "\n"; return 2; }
  auto code = blocker_code_from_string(code_text.value());
  if (!code.ok()) { err << "error " << code.error().to_string() << "\n"; return 2; }
  auto reason = options.require("reason");
  if (!reason.ok()) { err << "error " << reason.error().to_string() << "\n"; return 2; }
  auto fenced = engine.fence_grant(parsed.value(), code.value(), reason.value());
  if (!fenced.ok()) { err << "error " << fenced.error().to_string() << "\n"; return 3; }
  out << "grant " << fenced.value().grant.grant_id.to_string() << "\n";
  out << "grant-state " << to_string(fenced.value().state) << "\n";
  for (const auto& blocker : fenced.value().blockers) {
    out << "blocker " << blocker.render() << "\n";
  }
  return 1;
}

int command_show(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), true);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  const Ledger& ledger = engine.ledger();
  int printed = 0;
  if (options.has("request")) {
    auto id = RequestId::from_hex(options.get("request"));
    if (!id.ok()) { err << "error " << id.error().to_string() << "\n"; return 2; }
    const DecisionEntry* entry = ledger.find_decision(id.value());
    if (entry == nullptr) { err << "error not-found\n"; return 1; }
    render_decision(out, entry->decision);
    ++printed;
  }
  if (options.has("grant")) {
    auto id = GrantId::from_hex(options.get("grant"));
    if (!id.ok()) { err << "error " << id.error().to_string() << "\n"; return 2; }
    const GrantEntry* entry = ledger.find_grant(id.value());
    if (entry == nullptr) { err << "error not-found\n"; return 1; }
    out << "grant " << entry->grant.grant_id.to_string() << "\n";
    out << "grant-state " << to_string(entry->state) << "\n";
    out << "request " << entry->grant.request_id.to_string() << "\n";
    out << "issued-at " << entry->grant.issued_at.to_string() << "\n";
    out << "expires-at " << entry->grant.expires_at.to_string() << "\n";
    out << "control-epoch " << entry->grant.control_epoch.to_string() << "\n";
    out << "holds-capacity " << (entry->grant.holds_capacity ? "true" : "false") << "\n";
    out << "binding-digest " << entry->grant.binding_digest.to_hex() << "\n";
    for (const auto& binding : entry->grant.bindings) {
      out << "binding " << to_string(binding.kind) << " generation " << binding.generation << " digest "
          << binding.evidence_digest.to_hex() << "\n";
    }
    if (entry->commitment_id.has_value()) {
      out << "commitment " << entry->commitment_id.value().to_string() << "\n";
    }
    if (!entry->resolved_at.is_unset()) {
      out << "resolved-at " << entry->resolved_at.to_string() << "\n";
    }
    if (!entry->detail.empty()) {
      out << "detail " << entry->detail << "\n";
    }
    ++printed;
  }
  if (options.has("commitment")) {
    auto id = CommitmentId::from_hex(options.get("commitment"));
    if (!id.ok()) { err << "error " << id.error().to_string() << "\n"; return 2; }
    const CommitmentRecord* entry = ledger.find_commitment(id.value());
    if (entry == nullptr) { err << "error not-found\n"; return 1; }
    render_commitment(out, *entry);
    ++printed;
  }
  if (printed == 0) {
    err << "error show needs --request, --grant or --commitment\n";
    return 2;
  }
  return 0;
}

int command_list(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), true);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  const Ledger& ledger = engine.ledger();
  if (options.positional.empty()) {
    err << "error list needs decisions, grants, commitments or intents\n";
    return 2;
  }
  const std::string& what = options.positional[0];
  if (what == "decisions") {
    for (const auto& entry : ledger.decisions()) {
      out << "decision " << entry.first.to_string() << " " << to_string(entry.second.decision.verdict)
          << " sequence " << entry.second.decision.sequence.to_string() << " digest "
          << entry.second.decision.digest.to_hex() << "\n";
    }
    return 0;
  }
  if (what == "grants") {
    for (const auto& entry : ledger.grants()) {
      out << "grant " << entry.first.to_string() << " " << to_string(entry.second.state) << " request "
          << entry.second.grant.request_id.to_string() << " expires "
          << entry.second.grant.expires_at.to_string() << "\n";
    }
    return 0;
  }
  if (what == "commitments") {
    for (const auto& entry : ledger.commitments()) {
      out << "commitment " << entry.first.to_string() << " " << to_string(entry.second.state)
          << " grant " << entry.second.grant_id.to_string() << " recorded "
          << entry.second.recorded_at.to_string() << "\n";
    }
    return 0;
  }
  if (what == "intents") {
    for (const auto& entry : ledger.commitments()) {
      out << "intent " << entry.second.intent.intent_id.to_string() << " owner "
          << entry.second.intent.owner.to_string() << " commitment " << entry.first.to_string()
          << " valid-until " << entry.second.intent.not_after.to_string() << " state "
          << to_string(entry.second.state) << "\n";
    }
    return 0;
  }
  err << "error unknown list target " << escape_text(what) << "\n";
  return 2;
}

int command_stats(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), true);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  const Ledger& ledger = engine.ledger();
  out << "control-epoch " << engine.epoch().to_string() << "\n";
  out << "sequence " << ledger.sequence().to_string() << "\n";
  out << "decisions " << ledger.decisions().size() << "\n";
  out << "grants " << ledger.grants().size() << "\n";
  out << "commitments " << ledger.commitments().size() << "\n";
  out << "consuming-commitments " << ledger.consuming_commitments() << "\n";
  out << "state-digest " << ledger.state_digest().to_hex() << "\n";
  if (const durable::RecoveryReport* report = engine.recovery()) {
    out << "recovered-epoch " << report->epoch.to_string() << "\n";
    out << "recovered-sequence " << report->sequence.to_string() << "\n";
    out << "recovered-frames " << report->applied_frames << "\n";
    out << "discarded-tail-bytes " << report->discarded_tail_bytes << "\n";
    out << "snapshot-loaded " << (report->snapshot_loaded ? "true" : "false") << "\n";
    for (const auto& note : report->notes) {
      out << "note " << note << "\n";
    }
  }
  return 0;
}

int command_verify(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), true);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  const Status verified = engine.verify();
  if (!verified.ok()) {
    err << "error " << verified.error().to_string() << "\n";
    return 3;
  }
  out << "verified true\n";
  out << "control-epoch " << engine.epoch().to_string() << "\n";
  out << "sequence " << engine.sequence().to_string() << "\n";
  out << "state-digest " << engine.ledger().state_digest().to_hex() << "\n";
  return 0;
}

int command_compact(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), false);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  const Status compacted = engine.compact();
  if (!compacted.ok()) {
    err << "error " << compacted.error().to_string() << "\n";
    return 3;
  }
  out << "compacted true\n";
  out << "sequence " << engine.sequence().to_string() << "\n";
  out << "state-digest " << engine.ledger().state_digest().to_hex() << "\n";
  return 0;
}

int command_epoch(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  auto now = evaluation_time(options);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, options, now.value(), true);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  out << "control-epoch " << engine.epoch().to_string() << "\n";
  out << "sequence " << engine.sequence().to_string() << "\n";
  return 0;
}

int command_init(const Options& options, std::ostream& out, std::ostream& err, CommandContext& context) {
  Options created = options;
  created.values["create"] = "true";
  auto now = evaluation_time(created);
  if (!now.ok()) { err << "error " << now.error().to_string() << "\n"; return 2; }
  auto holder = acquire_engine(context, created, now.value(), false);
  if (!holder.ok()) { err << "error " << holder.error().to_string() << "\n"; return 2; }
  Engine& engine = *holder.value().get();
  out << "initialized true\n";
  out << "control-epoch " << engine.epoch().to_string() << "\n";
  out << "sequence " << engine.sequence().to_string() << "\n";
  return 0;
}

int command_session(const Options& options, std::ostream& out, std::ostream& err);
int run_script(const std::string& script, const std::shared_ptr<FixedClock>& clock, Timestamp start,
               std::ostream& out, std::ostream& err, CommandContext& context);

// One dispatch point for a single command line, shared by the ordinary one
// command per process path and by a session.
int dispatch_command(const std::string& command, const Options& options, std::ostream& out,
                     std::ostream& err, CommandContext& context) {
  if (command == "help" || command == "--help" || command == "-h") {
    out << usage();
    return 0;
  }
  if (command == "version") {
    out << "facility-admission-control " << version_string() << "\n";
    out << "durable-format-revision " << kDurableFormatRevision << "\n";
    return 0;
  }
  if (command == "init") return command_init(options, out, err, context);
  if (command == "admit") return command_admit(options, out, err, context);
  if (command == "commit") return command_commit(options, out, err, context);
  if (command == "ack") return command_ack(options, out, err, context);
  if (command == "release") return command_release(options, out, err, context, false);
  if (command == "release-grant") return command_release(options, out, err, context, true);
  if (command == "fence") return command_fence(options, out, err, context);
  if (command == "show") return command_show(options, out, err, context);
  if (command == "list") return command_list(options, out, err, context);
  if (command == "verify") return command_verify(options, out, err, context);
  if (command == "compact") return command_compact(options, out, err, context);
  if (command == "stats") return command_stats(options, out, err, context);
  if (command == "epoch") return command_epoch(options, out, err, context);
  if (command == "session") return command_session(options, out, err);
  err << "error unknown command " << escape_text(command) << "\n";
  err << usage();
  return 2;
}

// Replaces the @grant, @commitment, @intent and @request placeholders with the
// identities produced by earlier commands in the same session.
[[nodiscard]] Result<std::string> substitute(const std::string& line, const SessionState& state) {
  const std::vector<std::string> tokens = tokenize(line);
  std::string result;
  for (const auto& token : tokens) {
    std::string resolved = token;
    if (token == "@grant") {
      if (!state.last_grant.has_value()) {
        return make_error(ErrorCode::invalid_argument, "@grant has no value yet in this session");
      }
      resolved = state.last_grant->to_string();
    } else if (token == "@commitment") {
      if (!state.last_commitment.has_value()) {
        return make_error(ErrorCode::invalid_argument, "@commitment has no value yet in this session");
      }
      resolved = state.last_commitment->to_string();
    } else if (token == "@intent") {
      if (!state.last_intent.has_value()) {
        return make_error(ErrorCode::invalid_argument, "@intent has no value yet in this session");
      }
      resolved = state.last_intent->to_string();
    } else if (token == "@request") {
      if (!state.last_request.has_value()) {
        return make_error(ErrorCode::invalid_argument, "@request has no value yet in this session");
      }
      resolved = state.last_request->to_string();
    }
    if (!result.empty()) {
      result.push_back(' ');
    }
    result.append(resolved);
  }
  return result;
}

int run_script(const std::string& script, const std::shared_ptr<FixedClock>& clock, Timestamp start,
               std::ostream& out, std::ostream& err, CommandContext& context) {
  Timestamp current = start;
  int worst = 0;
  std::istringstream stream(script);
  std::string raw;
  std::size_t line_number = 0;
  while (std::getline(stream, raw)) {
    ++line_number;
    if (!raw.empty() && raw.back() == '\r') {
      raw.pop_back();
    }
    const std::size_t comment = raw.find('#');
    if (comment != std::string::npos) {
      raw.resize(comment);
    }
    if (tokenize(raw).empty()) {
      continue;
    }
    auto resolved = substitute(raw, *context.session);
    if (!resolved.ok()) {
      err << "error line " << line_number << ": " << resolved.error().to_string() << "\n";
      return 2;
    }
    const std::vector<std::string> tokens = tokenize(resolved.value());
    if (tokens.empty()) {
      continue;
    }
    auto parsed = parse_options(tokens, 1);
    if (!parsed.ok()) {
      err << "error line " << line_number << ": " << parsed.error().to_string() << "\n";
      return 2;
    }
    Options line_options = parsed.value();
    if (line_options.has("now")) {
      auto value = parse_nanos(line_options.get("now"), "now");
      if (!value.ok()) {
        err << "error line " << line_number << ": " << value.error().to_string() << "\n";
        return 2;
      }
      current = Timestamp(static_cast<Nanos>(value.value()));
    } else {
      auto advanced = current.checked_add(Duration::from_seconds(1));
      if (!advanced.ok()) {
        err << "error line " << line_number << ": " << advanced.error().to_string() << "\n";
        return 2;
      }
      current = advanced.value();
      line_options.values["now"] = std::to_string(current.unix_nanos());
    }
    clock->set(current);
    out << "> " << tokens[0] << "\n";
    const int result = dispatch_command(tokens[0], line_options, out, err, context);
    if (result == 2 || result == 3) {
      worst = result;
    } else if (result == 1 && worst == 0) {
      worst = 1;
    }
  }
  return worst;
}

// Runs a sequence of commands in one process, which is one control epoch. This
// is the only way to exercise a full admit -> commit -> acknowledge lifecycle
// through the tool, and that is deliberate: a new process incarnation advances
// the control epoch and therefore fences grants issued by the previous one.
int command_session(const Options& options, std::ostream& out, std::ostream& err) {
  if (options.positional.size() != 1) {
    err << "error session needs a script file as its positional argument\n";
    return 2;
  }
  auto script = read_text_file(options.positional[0]);
  if (!script.ok()) {
    err << "error " << script.error().to_string() << "\n";
    return 2;
  }
  auto setup = engine_setup(options);
  if (!setup.ok()) {
    err << "error " << setup.error().to_string() << "\n";
    return 2;
  }
  Timestamp current = SystemClock{}.now();
  if (options.has("now")) {
    auto value = parse_nanos(options.get("now"), "now");
    if (!value.ok()) {
      err << "error " << value.error().to_string() << "\n";
      return 2;
    }
    current = Timestamp(static_cast<Nanos>(value.value()));
  }
  auto clock = std::make_shared<FixedClock>(current);
  SessionState state;
  CommandContext context;
  context.session = &state;
  if (setup.value().volatile_engine) {
    auto engine = Engine::open_in_memory(setup.value().policy, clock);
    if (!engine.ok()) {
      err << "error " << engine.error().to_string() << "\n";
      return 2;
    }
    context.engine = engine.value().get();
    return run_script(script.value(), clock, current, out, err, context);
  }
  auto directory = options.require("store");
  if (!directory.ok()) {
    err << "error " << directory.error().to_string() << "\n";
    return 2;
  }
  auto engine = Engine::open_durable(directory.value(), setup.value().policy, clock, setup.value().create);
  if (!engine.ok()) {
    err << "error " << engine.error().to_string() << "\n";
    return 2;
  }
  context.engine = engine.value().get();
  return run_script(script.value(), clock, current, out, err, context);
}

}  // namespace

std::string usage() {
  return R"(facctl - Facility Admission Control

Usage: facctl <command> [options]

Commands:
  init       --store DIR                       create an empty durable store
  session    --store DIR SCRIPT                run a script of commands in one process
                                               (one control epoch; @grant, @commitment,
                                                @intent and @request refer to earlier results)
  admit      --scenario FILE [--store DIR]     evaluate an admission request
  commit     --grant ID [--scenario FILE]      execute a grant and emit its reservation intent
  ack        --intent ID --reservation ID --owner ID --outcome OUTCOME --generation N
             --digest HEX [--confirmed d=v,...] [--acknowledged-at NANOS]
                                              record the reservation owner's evidence
  release    --commitment ID --reason TEXT     release a commitment explicitly
  release-grant --grant ID --reason TEXT       release an uncommitted grant
  fence      --grant ID --code CODE --reason TEXT
                                              fence a grant explicitly
  show       (--request ID | --grant ID | --commitment ID)
  list       decisions|grants|commitments|intents
  verify     --store DIR                       re-verify the durable store from disk
  compact    --store DIR                       compact the durable history
  stats      --store DIR                       report ledger and recovery counters
  epoch      --store DIR                       report the current control epoch
  version                                      report the library version

Options:
  --store DIR            durable store directory (omit with --in-memory)
  --in-memory            volatile engine: nothing is durable
  --create               create the store directory when it is empty
  --now NANOS            evaluation time as Unix nanoseconds (default: system clock)
  --owner ID             reservation owner identity for emitted intents
  --grant-validity SEC   how long an issued grant stays usable
  --intent-validity SEC  how long an emitted reservation intent stays open
  --no-holds             grants do not hold capacity while uncommitted

Exit codes: 0 allow/committed, 1 refuse/defer/fenced, 2 usage or environment, 3 operation failed.
)";
}

int run(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err) {
  if (arguments.size() < 2) {
    err << usage();
    return 2;
  }
  const std::string& command = arguments[1];
  auto options = parse_options(arguments, 2);
  if (!options.ok()) {
    err << "error " << options.error().to_string() << "\n";
    return 2;
  }
  CommandContext context;
  return dispatch_command(command, options.value(), out, err, context);
}

}  // namespace fac::cli
