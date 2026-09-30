// Facility Admission Control - crash and lock-injection child.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The multiprocess and crash tests launch this program as a real second
// process. It writes through the ordinary public API and is killed by its
// parent; nothing in the library is aware that it is a test.
//
//   write <dir> <count>   admit <count> requests, then exit 0
//   loop  <dir>           admit requests until the parent kills this process
//   hold  <dir>           take the writer lock and wait to be killed
//   reject <dir>          try to take a writer lock another process holds

#include <chrono>
#include <cstdlib>
#include <memory>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fac/engine/engine.hpp"

namespace {

[[nodiscard]] std::string hex_id(std::uint64_t value) {
  std::string text(32, '0');
  const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    text[15 - i] = digits[(value >> (i * 4)) & 0xF];
  }
  text.insert(20, 1, '-');
  text.insert(16, 1, '-');
  text.insert(12, 1, '-');
  text.insert(8, 1, '-');
  return text;
}

[[nodiscard]] fac::AdmissionRequest make_request(std::uint64_t index, fac::Timestamp now) {
  fac::AdmissionRequest request;
  request.request_id = fac::RequestId(0x3000000000000000ull + index, 0x4000000000000000ull + index);
  request.tenant = fac::TenantId(0x11, 0x11);
  request.service_class = fac::ServiceClassId(0x22, 0x22);
  request.envelope = fac::EnvelopeId(0x33, 0x33);
  request.scope = fac::TargetScope{};
  request.scope.facility = fac::FacilityId(0x44, 0x44);
  request.demand = {};
  (void)request.demand.set(fac::Dimension::power, 1);
  (void)request.demand.set(fac::Dimension::cooling, 1);
  (void)request.demand.set(fac::Dimension::space, 0);
  (void)request.demand.set(fac::Dimension::slots, 0);
  request.requested_at = now;
  request.commitment_start = now;
  request.commitment_end = fac::Timestamp(now.unix_nanos() + 1000000000LL);
  return request;
}

[[nodiscard]] fac::EvidenceBundle make_evidence(fac::Timestamp now, std::uint64_t generation) {
  fac::EvidenceBundle evidence;
  fac::CapacitySnapshot capacity;
  capacity.generation = fac::CapacityGeneration(generation);
  capacity.observed_at = now;
  (void)capacity.total.set(fac::Dimension::power, 1000000);
  (void)capacity.total.set(fac::Dimension::cooling, 1000000);
  (void)capacity.total.set(fac::Dimension::space, 1000);
  (void)capacity.total.set(fac::Dimension::slots, 1000);
  (void)capacity.committed.set(fac::Dimension::power, 0);
  (void)capacity.committed.set(fac::Dimension::cooling, 0);
  (void)capacity.committed.set(fac::Dimension::space, 0);
  (void)capacity.committed.set(fac::Dimension::slots, 0);
  (void)capacity.reserved.set(fac::Dimension::power, 0);
  (void)capacity.reserved.set(fac::Dimension::cooling, 0);
  (void)capacity.reserved.set(fac::Dimension::space, 0);
  (void)capacity.reserved.set(fac::Dimension::slots, 0);
  evidence.capacity = capacity;

  fac::TenantSnapshot tenant;
  tenant.tenant = fac::TenantId(0x11, 0x11);
  tenant.generation = fac::TenantGeneration(generation);
  tenant.observed_at = now;
  tenant.status = fac::TenantStatus::active;
  evidence.tenant = tenant;

  fac::EnvelopeSnapshot envelope;
  envelope.envelope = fac::EnvelopeId(0x33, 0x33);
  envelope.tenant = fac::TenantId(0x11, 0x11);
  envelope.generation = fac::EnvelopeGeneration(generation);
  envelope.observed_at = now;
  (void)envelope.limit.set(fac::Dimension::power, 1000000);
  (void)envelope.limit.set(fac::Dimension::cooling, 1000000);
  (void)envelope.limit.set(fac::Dimension::space, 1000);
  (void)envelope.limit.set(fac::Dimension::slots, 1000);
  (void)envelope.consumed.set(fac::Dimension::power, 0);
  (void)envelope.consumed.set(fac::Dimension::cooling, 0);
  (void)envelope.consumed.set(fac::Dimension::space, 0);
  (void)envelope.consumed.set(fac::Dimension::slots, 0);
  evidence.envelope = envelope;

  fac::ServiceClassSnapshot service_class;
  service_class.service_class = fac::ServiceClassId(0x22, 0x22);
  service_class.generation = fac::ServiceClassGeneration(generation);
  service_class.observed_at = now;
  evidence.service_class = service_class;

  fac::RedundancySnapshot redundancy;
  redundancy.generation = fac::RedundancyGeneration(generation);
  redundancy.observed_at = now;
  redundancy.level = fac::RedundancyLevel::n_plus_one;
  redundancy.level_stated = true;
  (void)redundancy.protected_headroom.set(fac::Dimension::power, 0);
  (void)redundancy.protected_headroom.set(fac::Dimension::cooling, 0);
  (void)redundancy.protected_headroom.set(fac::Dimension::space, 0);
  (void)redundancy.protected_headroom.set(fac::Dimension::slots, 0);
  evidence.redundancy = redundancy;

  fac::MaintenanceSnapshot maintenance;
  maintenance.generation = fac::MaintenanceGeneration(generation);
  maintenance.observed_at = now;
  evidence.maintenance = maintenance;

  fac::IncidentSnapshot incident;
  incident.generation = fac::IncidentGeneration(generation);
  incident.observed_at = now;
  incident.scope.facility = fac::FacilityId(0x44, 0x44);
  incident.state = fac::IncidentState::normal;
  incident.capacity_trust = fac::CapacityTrust::trusted;
  evidence.incident = incident;

  fac::PlacementPolicySnapshot placement;
  placement.generation = fac::PlacementGeneration(generation);
  placement.observed_at = now;
  evidence.placement = placement;

  fac::PolicySnapshot policy;
  policy.policy = fac::PolicyId(0x55, 0x55);
  policy.generation = fac::PolicyGeneration(generation);
  policy.observed_at = now;
  policy.verdict = fac::PolicyVerdict::permit;
  fac::codec::Writer writer;
  writer.text("permit");
  policy.policy_digest = fac::Digest256::of(writer.span());
  evidence.policy = policy;
  return evidence;
}

[[nodiscard]] int run_write(const std::string& directory, std::uint64_t count) {
  fac::AdmissionPolicy policy;
  policy.reservation_owner = fac::OwnerId(0x66, 0x66);
  auto clock = std::make_shared<fac::SystemClock>();
  auto engine = fac::Engine::open_durable(directory, policy, clock, true);
  if (!engine.ok()) {
    std::cerr << "open failed: " << engine.error().to_string() << "\n";
    return 2;
  }
  for (std::uint64_t index = 0; index < count; ++index) {
    const fac::Timestamp now = clock->now();
    auto decision = engine.value()->admit(make_request(index, now), make_evidence(now, 1 + index));
    if (!decision.ok()) {
      std::cerr << "admit failed: " << decision.error().to_string() << "\n";
      return 3;
    }
    if (decision.value().verdict != fac::Verdict::allow) {
      std::cerr << "unexpected verdict\n";
      return 4;
    }
    // The parent may kill this process at any point; the loop exists to make
    // that likely.
    std::cout << "wrote " << index << "\n" << std::flush;
  }
  return 0;
}

[[nodiscard]] int run_loop(const std::string& directory) {
  fac::AdmissionPolicy policy;
  policy.reservation_owner = fac::OwnerId(0x66, 0x66);
  auto clock = std::make_shared<fac::SystemClock>();
  auto engine = fac::Engine::open_durable(directory, policy, clock, true);
  if (!engine.ok()) {
    std::cerr << "open failed: " << engine.error().to_string() << "\n";
    return 2;
  }
  for (std::uint64_t index = 0;; ++index) {
    const fac::Timestamp now = clock->now();
    auto decision = engine.value()->admit(make_request(index, now), make_evidence(now, 1 + index));
    if (!decision.ok()) {
      std::cerr << "admit failed: " << decision.error().to_string() << "\n";
      return 3;
    }
    if ((index % 8) == 0) {
      std::cout << "wrote " << index << "\n" << std::flush;
    }
  }
}

[[nodiscard]] int run_hold(const std::string& directory) {
  fac::AdmissionPolicy policy;
  policy.reservation_owner = fac::OwnerId(0x66, 0x66);
  auto clock = std::make_shared<fac::SystemClock>();
  auto engine = fac::Engine::open_durable(directory, policy, clock, true);
  if (!engine.ok()) {
    std::cerr << "open failed: " << engine.error().to_string() << "\n";
    return 2;
  }
  std::cout << "holding\n" << std::flush;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

[[nodiscard]] int run_reject(const std::string& directory) {
  fac::AdmissionPolicy policy;
  policy.reservation_owner = fac::OwnerId(0x66, 0x66);
  auto clock = std::make_shared<fac::SystemClock>();
  auto engine = fac::Engine::open_durable(directory, policy, clock, true);
  if (!engine.ok()) {
    if (engine.error().code == fac::ErrorCode::writer_lock_held) {
      std::cout << "lock-held\n";
      return 0;
    }
    std::cerr << "unexpected error: " << engine.error().to_string() << "\n";
    return 2;
  }
  std::cout << "lock-acquired\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: fac_crash_child <write|loop|hold|reject> <directory> [count]\n";
    return 2;
  }
  const std::string mode = argv[1];
  const std::string directory = argv[2];
  if (mode == "write") {
    const std::uint64_t count = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
    return run_write(directory, count);
  }
  if (mode == "loop") {
    return run_loop(directory);
  }
  if (mode == "hold") {
    return run_hold(directory);
  }
  if (mode == "reject") {
    return run_reject(directory);
  }
  std::cerr << "unknown mode\n";
  return 2;
}
