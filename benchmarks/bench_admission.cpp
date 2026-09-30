// Facility Admission Control - admission benchmark.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two sections, measured separately and reported separately:
//
//   VOLATILE  Engine::admit against an in-memory engine.
//   DURABLE   Engine::admit against a real store directory, which runs the
//             whole durable path: record encoding, journal append with
//             flush, and the atomic manifest publication that commits it.
//
// What is measured is one COMPLETED operation. admit() is synchronous and
// returns only after the work it describes is done, so the timer is started
// immediately before the call and stopped immediately after it returns. No
// enqueue, submission, queue-depth or batching figure is reported, because
// there is nothing of that kind here to report and reporting it would not be a
// latency.
//
// The workload is synthetic: generated requests and fixed authoritative
// snapshots. The code path is the real one. This is a single host, one process
// and one thread, and no physical hardware is involved. The two sections
// measure different code paths, so they are not a comparison and no speedup is
// claimed anywhere in this output.
//
// The clock is std::chrono::steady_clock. The engine's own clock is a
// FixedClock, so freshness, expiry and generation checks see a stable instant
// and the benchmark measures admission rather than the passage of time.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "fac/engine/engine.hpp"
#include "fac/version.hpp"

namespace {

using namespace fac;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Completed operations that are timed, and the unmeasured warm-up that runs
// first so the first-touch costs of the process are not part of the result.
constexpr std::uint64_t kVolatileOperations = 2000;
constexpr std::uint64_t kVolatileWarmUp = 100;
constexpr std::uint64_t kDurableOperations = 200;
constexpr std::uint64_t kDurableWarmUp = 10;

constexpr Nanos kNowNanos = 1'700'000'000'000'000'000LL;
constexpr Nanos kNanosPerSecond = 1'000'000'000LL;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "bench_admission: " << message << "\n";
  std::exit(2);
}

template <class T>
[[nodiscard]] T unwrap(Result<T> result, const char* what) {
  if (!result.ok()) {
    fail(std::string(what) + ": " + result.error().to_string());
  }
  return result.take();
}

void require(Status status, const char* what) {
  if (!status.ok()) {
    fail(std::string(what) + ": " + status.error().to_string());
  }
}

[[nodiscard]] AmountVector amounts(std::uint64_t power, std::uint64_t cooling, std::uint64_t space,
                                   std::uint64_t slots) {
  AmountVector vector;
  require(vector.set(Dimension::power, power), "power amount");
  require(vector.set(Dimension::cooling, cooling), "cooling amount");
  require(vector.set(Dimension::space, space), "space amount");
  require(vector.set(Dimension::slots, slots), "slots amount");
  return vector;
}

// ---------------------------------------------------------------------------
// Synthetic workload
// ---------------------------------------------------------------------------

struct Workload {
  AdmissionPolicy policy;
  EvidenceBundle evidence;
  TargetScope scope;
  TenantId tenant;
  ServiceClassId service_class;
  EnvelopeId envelope;
};

[[nodiscard]] Workload make_workload() {
  const Timestamp now(kNowNanos);
  Workload workload;

  workload.policy.reservation_owner =
      unwrap(OwnerId::from_hex("99999999-9999-9999-9999-999999999991"), "owner identity");
  workload.policy.grant_holds_capacity = true;

  workload.tenant =
      unwrap(TenantId::from_hex("55555555-5555-5555-5555-555555555551"), "tenant identity");
  workload.service_class =
      unwrap(ServiceClassId::from_hex("66666666-6666-6666-6666-666666666661"), "service class");
  workload.envelope =
      unwrap(EnvelopeId::from_hex("77777777-7777-7777-7777-777777777771"), "envelope identity");
  workload.scope = unwrap(
      TargetScope::make(unwrap(FacilityId::from_hex("11111111-1111-1111-1111-111111111111"), "facility"),
                        unwrap(RackId::from_hex("33333333-3333-3333-3333-333333333331"), "rack"),
                        unwrap(ZoneId::from_hex("44444444-4444-4444-4444-444444444441"), "zone")),
      "scope");

  CapacitySnapshot capacity;
  capacity.generation = CapacityGeneration(1);
  capacity.observed_at = now;
  capacity.total = amounts(100000000, 100000000, 100000000, 100000000);
  capacity.committed = amounts(0, 0, 0, 0);
  capacity.reserved = amounts(0, 0, 0, 0);
  workload.evidence.capacity = capacity;

  RedundancySnapshot redundancy;
  redundancy.generation = RedundancyGeneration(1);
  redundancy.observed_at = now;
  redundancy.level = RedundancyLevel::n_plus_one;
  redundancy.level_stated = true;
  redundancy.protected_headroom = amounts(100, 0, 0, 0);
  workload.evidence.redundancy = redundancy;

  TenantSnapshot tenant;
  tenant.tenant = workload.tenant;
  tenant.generation = TenantGeneration(1);
  tenant.observed_at = now;
  tenant.status = TenantStatus::active;
  workload.evidence.tenant = tenant;

  EnvelopeSnapshot envelope;
  envelope.envelope = workload.envelope;
  envelope.tenant = workload.tenant;
  envelope.generation = EnvelopeGeneration(1);
  envelope.observed_at = now;
  envelope.limit = amounts(100000000, 100000000, 100000000, 100000000);
  envelope.consumed = amounts(0, 0, 0, 0);
  workload.evidence.envelope = envelope;

  ServiceClassSnapshot service_class;
  service_class.service_class = workload.service_class;
  service_class.generation = ServiceClassGeneration(1);
  service_class.observed_at = now;
  service_class.obligations.minimum_redundancy = RedundancyLevel::n_plus_one;
  service_class.obligations.minimum_redundancy_stated = true;
  service_class.obligations.required_protected_headroom = amounts(50, 0, 0, 0);
  workload.evidence.service_class = service_class;

  MaintenanceSnapshot maintenance;
  maintenance.generation = MaintenanceGeneration(1);
  maintenance.observed_at = now;
  workload.evidence.maintenance = maintenance;

  IncidentSnapshot incident;
  incident.generation = IncidentGeneration(1);
  incident.observed_at = now;
  incident.scope = unwrap(
      TargetScope::make(workload.scope.facility, std::nullopt, std::nullopt), "incident scope");
  incident.state = IncidentState::normal;
  incident.capacity_trust = CapacityTrust::trusted;
  workload.evidence.incident = incident;

  PlacementPolicySnapshot placement;
  placement.generation = PlacementGeneration(1);
  placement.observed_at = now;
  placement.allowed_racks.push_back(workload.scope.rack.value());
  workload.evidence.placement = placement;

  PolicySnapshot policy;
  policy.policy = unwrap(PolicyId::from_hex("88888888-8888-8888-8888-888888888881"), "policy");
  policy.generation = PolicyGeneration(1);
  policy.observed_at = now;
  policy.verdict = PolicyVerdict::permit;
  policy.overcommit.mode = OvercommitMode::not_permitted;
  policy.policy_digest = unwrap(
      Digest256::from_hex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
      "policy digest");
  workload.evidence.policy = policy;

  return workload;
}

// One generated request. Every operation gets its own identity, so no
// operation is answered from the idempotency replay path instead of being
// evaluated, and the demand is small enough that thousands of grants still fit
// under the measured capacity.
[[nodiscard]] AdmissionRequest make_request(std::uint64_t index, const Workload& workload) {
  AdmissionRequest request;
  request.request_id = RequestId(0x1000000000000000ull + index, 0x2000000000000000ull + index);
  request.tenant = workload.tenant;
  request.service_class = workload.service_class;
  request.envelope = workload.envelope;
  request.scope = workload.scope;
  request.demand = amounts(1, 1, 0, 0);
  request.requested_at = Timestamp(kNowNanos);
  request.commitment_start = Timestamp(kNowNanos);
  request.commitment_end = Timestamp(kNowNanos + 3600 * kNanosPerSecond);
  return request;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

struct Section {
  std::string label;
  std::string engine_description;
  std::string provenance;
  std::uint64_t measured_operations = 0;
  std::uint64_t warm_up_operations = 0;
  double elapsed_seconds = 0.0;
  std::vector<double> microseconds;  // one sample per completed operation
  // Evidence that the measured operations really happened and really landed.
  std::uint64_t decision_checksum = 0;
  std::uint64_t sequence = 0;
  std::string state_digest;
  std::uint64_t recovered_frames = 0;
  bool read_back_verified = false;
};

[[nodiscard]] double microseconds_between(std::chrono::steady_clock::time_point start,
                                          std::chrono::steady_clock::time_point stop) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
  return static_cast<double>(elapsed.count()) / 1000.0;
}

[[nodiscard]] std::uint64_t decision_checksum_of(const Decision& decision) {
  std::uint64_t sum = 0;
  for (const std::byte byte : decision.digest.bytes()) {
    sum = sum * 131u + std::to_integer<std::uint64_t>(byte);
  }
  return sum;
}

// Runs the measured loop against one engine. The timer around each operation
// starts after the request has been built and stops the moment admit() returns,
// so what is recorded is the time to complete the operation and nothing else.
void run_operations(Engine& engine, const Workload& workload, std::uint64_t warm_up,
                    std::uint64_t measured, Section& section) {
  for (std::uint64_t index = 0; index < warm_up; ++index) {
    const AdmissionRequest request = make_request(index, workload);
    auto decision = engine.admit(request, workload.evidence);
    if (!decision.ok() || decision.value().verdict != Verdict::allow) {
      fail("a warm-up admission did not complete with an allow verdict");
    }
  }

  section.microseconds.reserve(static_cast<std::size_t>(measured));
  const std::uint64_t base = warm_up;
  const auto loop_start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < measured; ++index) {
    const AdmissionRequest request = make_request(base + index, workload);
    const auto start = std::chrono::steady_clock::now();
    auto decision = engine.admit(request, workload.evidence);
    const auto stop = std::chrono::steady_clock::now();
    if (!decision.ok()) {
      fail("a measured admission failed: " + decision.error().to_string());
    }
    if (decision.value().verdict != Verdict::allow) {
      fail("a measured admission did not complete with an allow verdict");
    }
    section.microseconds.push_back(microseconds_between(start, stop));
    section.decision_checksum ^= decision_checksum_of(decision.value());
  }
  const auto loop_stop = std::chrono::steady_clock::now();
  section.elapsed_seconds = std::chrono::duration<double>(loop_stop - loop_start).count();
  section.measured_operations = measured;
  section.warm_up_operations = warm_up;
  section.sequence = engine.sequence().value();
}

[[nodiscard]] std::string make_store_directory() {
  std::error_code code;
  std::filesystem::path base = std::filesystem::temp_directory_path(code);
  if (code) {
    fail("no temporary directory is available: " + code.message());
  }
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path candidate =
      base / ("fac-benchmark-" + std::to_string(static_cast<long long>(ticks)));
  std::filesystem::create_directories(candidate, code);
  if (code) {
    fail("the benchmark store directory could not be created: " + code.message());
  }
  const std::u8string text = candidate.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[nodiscard]] Section measure_volatile(const Workload& workload) {
  Section section;
  section.label = "VOLATILE";
  section.engine_description = "in-memory engine: evaluation, decision and ledger only";
  section.provenance =
      "SYNTHETIC workload data (generated requests, fixed authoritative snapshots) on the REAL\n"
      "                        volatile admission path; single host; no physical hardware involved";

  auto clock = std::make_shared<FixedClock>(Timestamp(kNowNanos));
  auto engine = unwrap(Engine::open_in_memory(workload.policy, clock), "open in-memory engine");
  run_operations(*engine, workload, kVolatileWarmUp, kVolatileOperations, section);
  section.state_digest = engine->ledger().state_digest().to_hex();
  return section;
}

[[nodiscard]] Section measure_durable(const Workload& workload, const std::string& store) {
  Section section;
  section.label = "DURABLE";
  section.engine_description =
      "durable store directory: evaluation, decision, record encoding, journal append with\n"
      "  flush, and the atomic manifest publication that commits the record";
  section.provenance =
      "SYNTHETIC workload data (generated requests, fixed authoritative snapshots) on the REAL\n"
      "                        durable admission path, including append + flush + manifest publication\n"
      "                        into a real store directory on this host's filesystem; single host;\n"
      "                        no physical hardware involved";

  auto clock = std::make_shared<FixedClock>(Timestamp(kNowNanos));
  {
    auto engine =
        unwrap(Engine::open_durable(store, workload.policy, clock, true), "open durable engine");
    run_operations(*engine, workload, kDurableWarmUp, kDurableOperations, section);
    section.state_digest = engine->ledger().state_digest().to_hex();
    require(engine->verify(), "verify the durable store from disk");
  }

  // Read back: a reader incarnation replays the committed journal from disk.
  // This is outside every timed region; it exists to show that the measured
  // operations are really in the store rather than in a buffer.
  {
    auto reader = unwrap(Engine::open_reader(store, workload.policy, clock), "reopen the store");
    section.read_back_verified = reader->ledger().state_digest().to_hex() == section.state_digest;
    section.recovered_frames = reader->recovery() != nullptr
                                   ? reader->recovery()->applied_frames
                                   : static_cast<std::uint64_t>(0);
    section.sequence = reader->ledger().sequence().value();
  }
  return section;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

[[nodiscard]] std::string build_configuration() {
#if defined(_MSC_VER)
  const std::string compiler = "MSVC " + std::to_string(static_cast<long long>(_MSC_VER));
#elif defined(__clang__)
  const std::string compiler = std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
  const std::string compiler = "GCC " + std::to_string(static_cast<long long>(__GNUC__)) + "." +
                               std::to_string(static_cast<long long>(__GNUC_MINOR__));
#else
  const std::string compiler = "unknown compiler";
#endif
#if defined(NDEBUG)
  const std::string flavour = "Release (NDEBUG, optimizations on)";
#else
  const std::string flavour = "Debug (no NDEBUG)";
#endif
  const std::string width = sizeof(void*) == 8 ? "64-bit" : "32-bit";
  return flavour + ", C++20, " + compiler + ", " + width;
}

void report(const Section& section) {
  const std::vector<double>& samples = section.microseconds;
  if (samples.empty()) {
    fail("the section recorded no completed operations");
  }
  std::vector<double> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const double total = std::accumulate(sorted.begin(), sorted.end(), 0.0);
  const double mean = total / static_cast<double>(sorted.size());
  const double p50 = sorted[sorted.size() / 2];
  const double fastest = sorted.front();
  const double slowest = sorted.back();
  const double operations_per_second =
      section.elapsed_seconds > 0.0 ? static_cast<double>(section.measured_operations) /
                                          section.elapsed_seconds
                                    : 0.0;

  std::cout << "\n" << section.label << "\n";
  std::cout << "  engine                " << section.engine_description << "\n";
  std::cout << "  provenance            " << section.provenance << "\n";
  std::cout << "  measured operations   " << section.measured_operations << " (completed only)\n";
  std::cout << "  warm-up operations    " << section.warm_up_operations << " (not measured)\n";
  std::cout << "  elapsed               " << section.elapsed_seconds << " s\n";
  std::cout << "  operations/second     " << operations_per_second << "\n";
  std::cout << "  mean latency          " << mean << " us\n";
  std::cout << "  p50 latency           " << p50 << " us\n";
  std::cout << "  min / max latency     " << fastest << " us / " << slowest << " us\n";
  std::cout << "  committed sequence    " << section.sequence << "\n";
  std::cout << "  state digest          " << section.state_digest << "\n";
  if (section.recovered_frames > 0 || section.read_back_verified) {
    std::cout << "  read-back frames      " << section.recovered_frames << "\n";
    std::cout << "  read-back verified    " << (section.read_back_verified ? "true" : "false")
              << "\n";
  }
  std::cout << "  decision checksum     0x" << std::hex << section.decision_checksum << std::dec
            << " (operations really produced decisions)\n";
}

}  // namespace

int main() {
  std::cout << "Facility Admission Control admission benchmark\n";
  std::cout << "  library version       " << version_string() << "\n";
  std::cout << "  durable format        revision " << kDurableFormatRevision << "\n";
  std::cout << "  build configuration   " << build_configuration() << "\n";
  std::cout << "  clock                 std::chrono::steady_clock (monotonic)\n";
  std::cout << "  host                  single host, one process, one thread\n";
  std::cout << "  method                the timer wraps one whole synchronous admit() call and stops\n";
  std::cout << "                        when it returns, so only COMPLETED operations are counted;\n";
  std::cout << "                        no enqueue, submission or queueing figure is measured or\n";
  std::cout << "                        reported; p50 is the median per-operation latency;\n";
  std::cout << "                        operations/second is measured operations over the measured\n";
  std::cout << "                        loop's wall time on this host\n";
  std::cout << "  claim                 none: the two sections measure different code paths and no\n";
  std::cout << "                        speedup is claimed; these numbers describe this host only\n";

  const Workload workload = make_workload();

  const Section volatile_section = measure_volatile(workload);
  report(volatile_section);

  const std::string store = make_store_directory();
  const Section durable_section = measure_durable(workload, store);
  report(durable_section);

  std::cout << "\nstore directory " << store << "\n";
  std::cout << "  removed after the read-back verified every measured record\n";

  std::error_code code;
  std::filesystem::remove_all(std::filesystem::path(store), code);

  const bool durable_read_back = durable_section.read_back_verified;
  if (!durable_read_back) {
    std::cerr << "bench_admission: the durable section did not read back identically\n";
    return 1;
  }
  std::cout << "done: both sections recorded only completed operations\n";
  return 0;
}
