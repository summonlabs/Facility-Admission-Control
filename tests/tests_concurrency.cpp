// Facility Admission Control - concurrency tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Threads inside one process. The engine documents one non-recursive mutex
// taken once per public call, so every claim below is about what that mutex
// does and does not cover: concurrent admissions serialize, capacity is decided
// exactly once at a boundary, and observation never crashes a writer.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "fac/core/error.hpp"
#include "fac/engine/engine.hpp"
#include "fac/ledger/ledger.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

namespace {

using namespace fac;
using namespace fac_test;

// One slot per concurrently admitted request. Each thread writes only its own
// slots, so nothing here needs a lock of its own.
struct Slot {
  bool ok = false;
  ErrorCode code = ErrorCode::ok;
  Decision decision;
};

AdmissionRequest request_with_demand(std::uint64_t index, const AmountVector& demand, Timestamp now) {
  AdmissionRequest request;
  request.request_id = request_id(index);
  request.tenant = tenant_alpha();
  request.service_class = class_gold();
  request.envelope = envelope_alpha();
  request.scope = FAC_TAKE(TargetScope::make(facility_a(), rack_1(), zone_a()));
  request.demand = demand;
  request.requested_at = now;
  request.commitment_start = now;
  request.commitment_end = at_seconds(3600);
  return request;
}

// The conforming fixture evidence, with the capacity arithmetic and every
// authority's generation and observation time under the caller's control.
// Observing one nanosecond before the evaluation is what makes a grant issued
// during this run count as consumption for the next evaluation.
EvidenceBundle make_evidence(Timestamp observed_at, const AmountVector& total,
                             const AmountVector& redundancy_headroom,
                             const AmountVector& required_headroom, std::uint64_t generation) {
  const Fixture base = make_fixture();
  EvidenceBundle evidence = base.evidence;
  evidence.capacity->generation = CapacityGeneration(generation);
  evidence.capacity->observed_at = observed_at;
  evidence.capacity->total = total;
  evidence.redundancy->generation = RedundancyGeneration(generation);
  evidence.redundancy->observed_at = observed_at;
  evidence.redundancy->protected_headroom = redundancy_headroom;
  evidence.tenant->generation = TenantGeneration(generation);
  evidence.tenant->observed_at = observed_at;
  evidence.envelope->generation = EnvelopeGeneration(generation);
  evidence.envelope->observed_at = observed_at;
  evidence.service_class->generation = ServiceClassGeneration(generation);
  evidence.service_class->observed_at = observed_at;
  evidence.service_class->obligations.required_protected_headroom = required_headroom;
  evidence.maintenance->generation = MaintenanceGeneration(generation);
  evidence.maintenance->observed_at = observed_at;
  evidence.incident->generation = IncidentGeneration(generation);
  evidence.incident->observed_at = observed_at;
  evidence.placement->generation = PlacementGeneration(generation);
  evidence.placement->observed_at = observed_at;
  evidence.policy->generation = PolicyGeneration(generation);
  evidence.policy->observed_at = observed_at;
  return evidence;
}

// A decision the engine returned must be internally complete whatever thread
// produced it.
void check_decision(const Decision& decision, RequestId expected, bool& failed, std::string& detail) {
  const auto note = [&failed, &detail](bool condition, const std::string& message) {
    if (!condition && !failed) {
      failed = true;
      detail = message;
    }
  };
  note(decision.request_id == expected, "the decision names another request");
  note(decision.digest == decision.compute_digest(), "the decision digest does not cover its body");
  note(decision.verdict == decision.blockers.implied_verdict(),
       "the verdict disagrees with the blocker set");
  note(!decision.sequence.is_zero(), "the decision carries no ledger sequence");
  note(decision.grant.has_value() == (decision.verdict == Verdict::allow),
       "an allow without a grant or a non-allow with one");
  if (decision.grant.has_value()) {
    const Grant& grant = decision.grant.value();
    note(grant.request_id == expected, "the grant names another request");
    note(grant.binding_digest == grant.compute_binding_digest(),
         "the grant binding digest does not cover its content");
    note(grant.control_epoch == decision.control_epoch, "the grant carries another control epoch");
    note(grant.sequence == decision.sequence, "the grant carries another ledger sequence");
  }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Many threads admit through one engine at once. Nothing crashes, every
// returned decision is complete, and the ledger advances exactly one sequence
// per admitted request.
FAC_TEST(concurrency, many_admitting_threads_share_one_engine) {
  TempDir dir("concurrency-engine");
  const Fixture fixture = make_fixture();
  Harness harness = make_durable_harness(fixture, dir.path(), true);

  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kPerThread = 25;
  constexpr std::size_t kRequests = kThreads * kPerThread;

  const Timestamp observed = Timestamp(fixture.now.unix_nanos() - 1);
  const AmountVector demand = amounts(200, 100, 1, 1);
  const EvidenceBundle evidence =
      make_evidence(observed, amounts(1000000, 1000000, 1000, 1000), amounts(100, 0, 0, 0),
                    amounts(50, 0, 0, 0), 1);

  std::vector<Slot> slots(kRequests);
  std::atomic<std::size_t> ready{0};
  std::latch gate(1);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&slots, &ready, &gate, &harness, &evidence, &demand, &fixture, thread]() {
      ready.fetch_add(1);
      gate.wait();
      for (std::size_t step = 0; step < kPerThread; ++step) {
        const std::size_t offset = thread * kPerThread + step;
        const AdmissionRequest request =
            request_with_demand(static_cast<std::uint64_t>(offset) + 1, demand, fixture.now);
        auto result = harness.engine->admit(request, evidence);
        Slot& slot = slots[offset];
        if (result.ok()) {
          slot.ok = true;
          slot.decision = result.value();
        } else {
          slot.code = result.error().code;
        }
      }
    });
  }
  for (int attempt = 0; attempt < 1000000 && ready.load() < kThreads; ++attempt) {
    std::this_thread::yield();
  }
  FAC_CHECK_EQ(ready.load(), kThreads);
  gate.count_down();
  for (std::thread& thread : threads) {
    thread.join();
  }

  bool failed = false;
  std::string detail;
  std::set<std::uint64_t> sequences;
  for (std::size_t offset = 0; offset < kRequests; ++offset) {
    const Slot& slot = slots[offset];
    if (!slot.ok) {
      FAC_FAIL("admit failed with " + std::string(to_string(slot.code)) + " for request " +
               std::to_string(offset + 1));
    }
    check_decision(slot.decision, request_id(static_cast<std::uint64_t>(offset) + 1), failed, detail);
    sequences.insert(slot.decision.sequence.value());
  }
  if (failed) {
    FAC_FAIL("a concurrently returned decision is not well formed: " + detail);
  }
  // One sequence per admitted request, and no sequence used twice.
  FAC_CHECK_EQ(sequences.size(), kRequests);
  FAC_CHECK_EQ(*sequences.begin(), std::uint64_t{1});
  FAC_CHECK_EQ(*sequences.rbegin(), static_cast<std::uint64_t>(kRequests));
  FAC_CHECK_EQ(harness.engine->sequence().value(), static_cast<std::uint64_t>(kRequests));
  FAC_CHECK_EQ(harness.engine->ledger().sequence().value(), static_cast<std::uint64_t>(kRequests));
  FAC_CHECK_EQ(harness.engine->ledger().decisions().size(), kRequests);
  FAC_CHECK_EQ(harness.engine->ledger().grants().size(), kRequests);
  FAC_CHECK_OK(harness.engine->verify());
}

// Competing admissions at an exact boundary: the whole capacity is exactly one
// request's demand. Whichever thread gets there first holds it, and the other
// is refused with capacity_exhausted rather than sharing the same capacity.
FAC_TEST(concurrency, competing_admissions_at_the_boundary) {
  constexpr std::uint64_t kRounds = 40;
  const AmountVector demand = amounts(200, 100, 1, 1);
  const AmountVector total = amounts(200, 100, 1, 1);
  const AmountVector no_headroom = amounts(0, 0, 0, 0);

  for (std::uint64_t round = 0; round < kRounds; ++round) {
    const Fixture fixture = make_fixture();
    Harness harness = make_harness(fixture);
    const Timestamp observed = Timestamp(fixture.now.unix_nanos() - 1);
    const EvidenceBundle evidence = make_evidence(observed, total, no_headroom, no_headroom, 1);

    std::vector<Slot> slots(2);
    std::latch gate(1);
    const auto work = [&slots, &gate, &harness, &evidence, &demand, &fixture](std::size_t index) {
      const AdmissionRequest request =
          request_with_demand(static_cast<std::uint64_t>(index) + 1, demand, fixture.now);
      gate.wait();
      auto result = harness.engine->admit(request, evidence);
      Slot& slot = slots[index];
      if (result.ok()) {
        slot.ok = true;
        slot.decision = result.value();
      } else {
        slot.code = result.error().code;
      }
    };
    std::thread first(work, std::size_t{0});
    std::thread second(work, std::size_t{1});
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    gate.count_down();
    first.join();
    second.join();

    for (const Slot& slot : slots) {
      if (!slot.ok) {
        FAC_FAIL("round " + std::to_string(round) + ": admit failed with " +
                 std::string(to_string(slot.code)));
      }
    }
    const bool first_allow = slots[0].decision.verdict == Verdict::allow;
    const bool second_allow = slots[1].decision.verdict == Verdict::allow;
    FAC_CHECK(first_allow != second_allow);
    const Decision& refused = first_allow ? slots[1].decision : slots[0].decision;
    FAC_CHECK_EQ(refused.verdict, Verdict::refuse);
    FAC_CHECK(has_blocker(refused, BlockerCode::capacity_exhausted));
    // The winner holds the demand, so exactly one grant exists and the ledger
    // recorded both decisions.
    FAC_CHECK_EQ(harness.engine->ledger().grants().size(), std::size_t{1});
    FAC_CHECK_EQ(harness.engine->sequence().value(), std::uint64_t{2});
  }
}

// A reader thread observes the ledger and re-verifies the durable store while
// the writer thread admits. Nothing may crash, and verification must keep
// succeeding against a store that is being appended to.
//
// Engine::verify(), sequence() and epoch() take the engine mutex, so they are
// safe against a concurrent admit. Engine::ledger() hands back a reference to
// the live ledger without taking that mutex: this test drives its accessors and
// its bounded identity lookups concurrently, which is the strongest use the
// documented model supports. It does not iterate the ledger's containers: a
// traversal racing an insertion has no bounded running time, and this harness
// deliberately has no timeout, so a hang would be undiagnosable.
FAC_TEST(concurrency, reader_observes_while_writer_admits) {
  TempDir dir("concurrency-reader");
  const Fixture fixture = make_fixture();
  Harness harness = make_durable_harness(fixture, dir.path(), true);

  constexpr std::size_t kWriterRequests = 300;
  constexpr std::size_t kMaxReaderPasses = 200000;

  const Timestamp observed = Timestamp(fixture.now.unix_nanos() - 1);
  const AmountVector demand = amounts(200, 100, 1, 1);
  const EvidenceBundle evidence =
      make_evidence(observed, amounts(1000000, 1000000, 1000, 1000), amounts(100, 0, 0, 0),
                    amounts(50, 0, 0, 0), 1);

  std::atomic<bool> finished{false};
  std::atomic<bool> reader_saw_failure{false};
  std::atomic<std::size_t> reader_passes{0};
  std::atomic<std::size_t> verifications{0};

  std::thread reader([&]() {
    std::size_t pass = 0;
    while (!finished.load() && pass < kMaxReaderPasses) {
      const Ledger& ledger = harness.engine->ledger();
      const LedgerSequence sequence = ledger.sequence();
      const std::size_t decisions = ledger.decisions().size();
      const std::size_t grants = ledger.grants().size();
      const std::size_t commitments = ledger.commitments().size();
      const std::uint64_t engine_sequence = harness.engine->sequence().value();
      const std::uint64_t epoch = harness.engine->epoch().value();
      // Bounded lookups rather than a traversal.
      const std::uint64_t wanted = 1 + (pass % kWriterRequests);
      const DecisionEntry* found = ledger.find_decision(request_id(wanted));
      const GrantEntry* grant = ledger.find_grant(GrantId(0, 0));
      if (sequence.value() > engine_sequence || decisions > kWriterRequests || grants > kWriterRequests ||
          commitments != 0 || epoch == 0 || grant != nullptr) {
        reader_saw_failure.store(true);
      }
      if (found != nullptr && found->decision.request_id != request_id(wanted)) {
        reader_saw_failure.store(true);
      }
      if ((pass % 64) == 0) {
        if (!harness.engine->verify().ok()) {
          reader_saw_failure.store(true);
        }
        verifications.fetch_add(1);
      }
      ++pass;
    }
    reader_passes.store(pass);
  });

  for (std::size_t index = 1; index <= kWriterRequests; ++index) {
    const AdmissionRequest request =
        request_with_demand(static_cast<std::uint64_t>(index), demand, fixture.now);
    auto result = harness.engine->admit(request, evidence);
    FAC_CHECK_OK(result);
    FAC_CHECK_EQ(result.value().verdict, Verdict::allow);
  }
  finished.store(true);
  reader.join();

  FAC_CHECK(!reader_saw_failure.load());
  FAC_CHECK(reader_passes.load() >= 1);
  FAC_CHECK(verifications.load() >= 1);
  FAC_CHECK_EQ(harness.engine->sequence().value(), static_cast<std::uint64_t>(kWriterRequests));
  FAC_CHECK_EQ(harness.engine->ledger().decisions().size(), kWriterRequests);
  FAC_CHECK_OK(harness.engine->verify());
}

}  // namespace
