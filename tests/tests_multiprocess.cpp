// Facility Admission Control - multiprocess tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every claim here is made with real operating system processes. Threads never
// stand in for processes: the writer lock is a kernel lock on a real file, and
// the crash tests kill a real process in the middle of real work.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "fac/core/error.hpp"
#include "fac/durable/format.hpp"
#include "fac/durable/lock.hpp"
#include "fac/durable/store.hpp"
#include "fac/ledger/ledger.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

namespace {

using namespace fac;
using namespace fac_test;

// A bounded poll. Nothing here attaches a timeout to a command or a test: the
// loop reports the condition it waited for as a failure instead of hanging.
constexpr int kPollIterations = 4000;  // 4000 * 5 ms = 20 s
constexpr std::uintmax_t kSeveralFramesBytes = 16u * 1024u;

template <class Predicate>
void wait_until(Predicate predicate, const std::string& condition) {
  for (int attempt = 0; attempt < kPollIterations; ++attempt) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  FAC_FAIL("waited 20 seconds for " + condition);
}

std::filesystem::path fs_path(const std::string& utf8) {
  std::u8string text;
  text.resize(utf8.size());
  for (std::size_t i = 0; i < utf8.size(); ++i) {
    text[i] = static_cast<char8_t>(static_cast<unsigned char>(utf8[i]));
  }
  return std::filesystem::path(text);
}

std::string join_path(const std::string& directory, const std::string& name) {
  const std::filesystem::path combined = fs_path(directory) / fs_path(name);
  const std::u8string text = combined.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// The journal only grows while a writer holds the store, and the size of a file
// can be read without opening it, so this observes durable progress without
// competing with the child for the writer lock.
std::uintmax_t journal_size(const std::string& journal_path) {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(fs_path(journal_path), code);
  return code ? 0 : size;
}

// True once some other process holds the store write lock. The reader lock
// never competes with a writer for the exclusive lock, so polling with it
// cannot starve the child.
bool writer_lock_observed(const std::string& directory) {
  auto probe = durable::WriterLock::acquire_reader(join_path(directory, std::string(durable::kLockFileName)));
  if (probe.ok()) {
    return false;
  }
  return probe.error().code == ErrorCode::writer_lock_held;
}

Decision admit_fixture(Engine& engine, std::uint64_t index) {
  const Fixture step = make_fixture(index);
  auto decision = engine.admit(step.request, step.evidence);
  FAC_CHECK_OK(decision);
  return decision.value();
}

// Every recorded decision must be complete: it binds the request it names, its
// digest covers its own body, its verdict is the verdict its blockers imply,
// and every identity it mentions resolves inside the same ledger.
void check_ledger_consistency(const Ledger& ledger) {
  for (const auto& entry : ledger.decisions()) {
    const DecisionEntry& recorded = entry.second;
    FAC_CHECK(entry.first == recorded.request.request_id);
    FAC_CHECK(recorded.decision.request_id == recorded.request.request_id);
    FAC_CHECK(recorded.decision.request_digest == recorded.request.digest());
    FAC_CHECK(recorded.decision.digest == recorded.decision.compute_digest());
    FAC_CHECK(recorded.decision.verdict == recorded.decision.blockers.implied_verdict());
    FAC_CHECK(!recorded.decision.sequence.is_zero());
    FAC_CHECK_EQ(recorded.decision.grant.has_value(), recorded.decision.verdict == Verdict::allow);
    if (recorded.decision.grant.has_value()) {
      const Grant& grant = recorded.decision.grant.value();
      FAC_CHECK(grant.request_id == recorded.request.request_id);
      FAC_CHECK(grant.binding_digest == grant.compute_binding_digest());
      FAC_CHECK(grant.control_epoch == recorded.decision.control_epoch);
      FAC_CHECK(grant.sequence == recorded.decision.sequence);
    }
  }
  for (const auto& entry : ledger.grants()) {
    const GrantEntry& grant = entry.second;
    FAC_CHECK(entry.first == grant.grant.grant_id);
    if (grant.commitment_id.has_value()) {
      FAC_CHECK(ledger.find_commitment(grant.commitment_id.value()) != nullptr);
    }
  }
  for (const auto& entry : ledger.commitments()) {
    const CommitmentRecord& commitment = entry.second;
    FAC_CHECK(entry.first == commitment.commitment_id);
    FAC_CHECK(ledger.find_grant(commitment.grant_id) != nullptr);
    FAC_CHECK(ledger.find_decision(commitment.request_id) != nullptr);
    FAC_CHECK(commitment.grant_id == commitment.intent.grant_id);
  }
}

// The crash-recovery claim is registered under two suites: multiprocess, so
// the ordinary suite run covers it, and multiprocess_crash, which is the suite
// the repository's CTest entry fac.crash_recovery invokes.
void crash_consistency_body();

const fac_test::Registrar multiprocess_crash_consistency_registrar(
    "multiprocess", "crash_consistency", &crash_consistency_body);
const fac_test::Registrar multiprocess_crash_recovery_registrar(
    "multiprocess_crash", "crash_consistency", &crash_consistency_body);

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// While one process holds the store, another real process is refused with
// writer_lock_held and says so.
FAC_TEST(multiprocess, writer_lock_is_exclusive) {
  TempDir dir("mp-lock");
  const Fixture fixture = make_fixture();
  Harness holder = make_durable_harness(fixture, dir.path(), true);

  const std::string child = crash_child_path();
  FAC_CHECK(std::filesystem::exists(fs_path(child)));

  const ProcessResult rejected = run_process(child, {"reject", dir.path()});
  FAC_CHECK(rejected.started);
  FAC_CHECK_EQ(rejected.exit_code, 0);
  FAC_CHECK(rejected.output.find("lock-held") != std::string::npos);

  // Release the store and run the very same child again: the refusal was the
  // lock and nothing else about the second process.
  holder.engine.reset();

  const ProcessResult acquired = run_process(child, {"reject", dir.path()});
  FAC_CHECK(acquired.started);
  FAC_CHECK_EQ(acquired.exit_code, 1);
  FAC_CHECK(acquired.output.find("lock-acquired") != std::string::npos);
}

// Killing the holding process abruptly must not leave the store locked: the
// kernel owns the lock and releases it with the process.
FAC_TEST(multiprocess, abrupt_death_releases_kernel_lock) {
  TempDir dir("mp-hold");
  const Fixture fixture = make_fixture();

  ChildProcess holder;
  FAC_CHECK(holder.start(crash_child_path(), {"hold", dir.path()}));

  // The child prints "holding" once it owns the store. Its output is not
  // piped, so the parent observes the same fact through the lock: only the
  // holder can be holding it.
  wait_until([&]() { return writer_lock_observed(dir.path()) || !holder.running(); },
             "the holding child takes the store write lock");
  if (!holder.running()) {
    const int code = holder.wait();
    FAC_FAIL("the holding child exited with code " + std::to_string(code) + " before taking the lock");
  }
  FAC_CHECK(writer_lock_observed(dir.path()));

  holder.terminate();
  FAC_CHECK_NE(holder.wait(), 0);

  // The kernel released the lock with the process, so a new writer opens the
  // same store and commits a record.
  Harness reopened = make_durable_harness(fixture, dir.path(), false);
  const Decision decision = admit_fixture(*reopened.engine, 1);
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK_EQ(reopened.engine->sequence().value(), std::uint64_t{1});
  FAC_CHECK_OK(reopened.engine->verify());
}

// A process killed in the middle of admitting leaves a store the parent can
// open, and recovery is deterministic: the same bytes give the same state.
void crash_consistency_body() {
  TempDir dir("mp-crash");
  const Fixture fixture = make_fixture();
  const std::string journal = join_path(dir.path(), std::string(durable::kJournalFileName));

  ChildProcess child;
  FAC_CHECK(child.start(crash_child_path(), {"loop", dir.path()}));

  wait_until(
      [&]() { return journal_size(journal) >= kSeveralFramesBytes || !child.running(); },
      "the looping child commits several records");
  if (!child.running()) {
    const int code = child.wait();
    FAC_FAIL("the looping child exited with code " + std::to_string(code) +
             " before committing several records");
  }
  FAC_CHECK(journal_size(journal) >= kSeveralFramesBytes);

  // Kill it abruptly, exactly where it is.
  child.terminate();
  (void)child.wait();

  Harness first = make_durable_harness(fixture, dir.path(), false);
  FAC_CHECK(first.engine->recovery() != nullptr);
  FAC_CHECK(!first.engine->recovery()->created);
  FAC_CHECK(first.engine->sequence().value() >= 4);
  FAC_CHECK_OK(first.engine->verify());
  check_ledger_consistency(first.engine->ledger());

  const Digest256 digest = first.engine->ledger().state_digest();
  const std::uint64_t sequence = first.engine->sequence().value();
  const auto decisions = first.engine->ledger().decisions();
  const std::uint64_t discarded = first.engine->recovery()->discarded_tail_bytes;
  first.engine.reset();

  // Reopening twice reproduces the state byte for byte, whatever tail the kill
  // left behind.
  Harness second = make_durable_harness(fixture, dir.path(), false);
  FAC_CHECK(second.engine->ledger().state_digest() == digest);
  FAC_CHECK_EQ(second.engine->sequence().value(), sequence);
  FAC_CHECK(second.engine->ledger().decisions() == decisions);
  check_ledger_consistency(second.engine->ledger());
  second.engine.reset();

  Harness third = make_durable_harness(fixture, dir.path(), false);
  FAC_CHECK(third.engine->ledger().state_digest() == digest);
  FAC_CHECK_EQ(third.engine->sequence().value(), sequence);
  FAC_CHECK_OK(third.engine->verify());
  // The first recovery may have discarded an uncommitted tail; by now there is
  // nothing left to discard, which is what makes the state reproducible.
  FAC_CHECK_EQ(third.engine->recovery()->discarded_tail_bytes, std::uint64_t{0});
  FAC_CHECK(discarded <= journal_size(journal));
}

// Two real processes contend for the same store: one admits a run of requests,
// the other tries to take the store while it is held. Exactly one of them ever
// holds the writer lock, the loser is refused rather than served, and the winner
// completes its whole run.
FAC_TEST(multiprocess, write_and_reject_are_mutually_exclusive) {
  TempDir dir("mp-contend");
  const Fixture fixture = make_fixture();
  const std::string journal = join_path(dir.path(), std::string(durable::kJournalFileName));
  constexpr std::uint64_t kWriterRequests = 400;

  ChildProcess writer;
  FAC_CHECK(writer.start(crash_child_path(),
                         {"write", dir.path(), std::to_string(kWriterRequests)}));

  // Several committed records prove the writer owns the store, and the run is
  // long enough that it is still owning it when the second child starts.
  wait_until([&]() { return journal_size(journal) >= 4096u || !writer.running(); },
             "the writing child commits several records");
  if (!writer.running()) {
    const int code = writer.wait();
    FAC_FAIL("the writing child exited with code " + std::to_string(code) + " before its first records");
  }
  FAC_CHECK(writer.running());

  ChildProcess rejected;
  FAC_CHECK(rejected.start(crash_child_path(), {"reject", dir.path()}));
  // Exit code 0 is the child's "lock-held" path; 1 would mean it acquired the
  // lock, which is the mutual-exclusion violation.
  FAC_CHECK_EQ(rejected.wait(), 0);

  // The holder never lost the store: it finished every request it was asked for.
  FAC_CHECK(writer.running());
  FAC_CHECK_EQ(writer.wait(), 0);

  Harness reopened = make_durable_harness(fixture, dir.path(), false);
  FAC_CHECK(reopened.engine->sequence().value() >= kWriterRequests);
  FAC_CHECK_OK(reopened.engine->verify());
  check_ledger_consistency(reopened.engine->ledger());
}

// The child's documented contract for mode "write <dir> <count>" is that it
// admits <count> requests and exits 0, so this asserts the contract and the
// committed count together. It fails if the child's evidence ever stops
// satisfying the policy the child configures for itself, which is exactly the
// shape of a real regression in the harness.
FAC_TEST(multiprocess, write_child_reaches_its_count) {
  TempDir dir("mp-write");
  const ProcessResult result = run_process(crash_child_path(), {"write", dir.path(), "3"});
  FAC_CHECK(result.started);
  if (result.exit_code != 0) {
    FAC_FAIL("fac_crash_child write <dir> 3 exited with code " + std::to_string(result.exit_code) +
             " instead of admitting three records: " + result.output);
  }
  FAC_CHECK_EQ(result.exit_code, 0);

  auto reader = FAC_TAKE(durable::Store::open_reader(dir.path()));
  FAC_CHECK_EQ(reader->sequence().value(), std::uint64_t{3});
}

}  // namespace
