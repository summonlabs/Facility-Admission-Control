// Facility Admission Control - cli tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every test here runs the real facctl binary as a real operating system
// process. Nothing is simulated and no output is invented: the assertions are
// made against the bytes the tool printed and the exit code it returned, and
// the exit codes are the documented interface:
//
//   0  allow or committed
//   1  refuse, defer or fenced
//   2  usage or environment error
//   3  the operation could not be completed
//
// The scenario grammar used below is the one src/cli/cli.cpp implements: one
// directive per line, '#' starts a comment, "dimension=value" and
// "dimension value" are both accepted, and every identity is the canonical
// 8-4-4-4-12 hexadecimal form.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fac/engine/engine.hpp"
#include "fac/version.hpp"
#include "test_support.hpp"

using namespace fac;
using namespace fac_test;

namespace {

constexpr std::int64_t kNanosPerSecond = 1'000'000'000LL;

// One evaluation instant for every scenario below, so the freshness budgets
// and the decision timestamps are explicit rather than wall-clock dependent.
constexpr std::int64_t kNow = 1'700'000'000'000'000'000LL;

// Identities in the shape tests/fixtures.hpp uses, so a scenario file and a
// library-level fixture describe the same facility.
constexpr const char* kRequestId = "00000000-0000-0000-0000-0000000000a1";
constexpr const char* kTenantId = "55555555-5555-5555-5555-555555555551";
constexpr const char* kServiceClassId = "66666666-6666-6666-6666-666666666661";
constexpr const char* kEnvelopeId = "77777777-7777-7777-7777-777777777771";
constexpr const char* kFacilityId = "11111111-1111-1111-1111-111111111111";
constexpr const char* kRackId = "33333333-3333-3333-3333-333333333331";
constexpr const char* kRack2Id = "33333333-3333-3333-3333-333333333332";
constexpr const char* kZoneId = "44444444-4444-4444-4444-444444444441";
constexpr const char* kPolicyId = "88888888-8888-8888-8888-888888888881";
constexpr const char* kReservationId = "00000000-0000-0000-0000-0000000000b2";
constexpr const char* kPolicyDigest =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
constexpr const char* kOwnerDigest =
    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

void write_text_file(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  FAC_CHECK(stream.good());
  stream << text;
  stream.flush();
  FAC_CHECK(stream.good());
}

struct ScenarioSpec {
  // Everything else is the conforming shape; a test that wants a refusal or a
  // deferral changes exactly one of these two.
  std::uint64_t capacity_total_power = 1000;
  std::int64_t capacity_observed_at = kNow;
};

// A complete, conforming scenario: fresh evidence from every authority, an
// active tenant, room in the envelope, a permitted facility policy and capacity
// that covers the demand.
[[nodiscard]] std::string scenario_text(const ScenarioSpec& spec) {
  const std::int64_t window_end = kNow + 3600LL * kNanosPerSecond;
  std::ostringstream out;
  out << "# Facility Admission Control scenario file.\n";
  out << "request " << kRequestId << "\n";
  out << "tenant " << kTenantId << "\n";
  out << "service-class " << kServiceClassId << "\n";
  out << "envelope " << kEnvelopeId << "\n";
  out << "scope facility " << kFacilityId << " rack " << kRackId << " zone " << kZoneId << "\n";
  out << "demand power=200 cooling=100 space=1 slots=1\n";
  out << "requested-at " << kNow << "\n";
  out << "window " << kNow << " " << window_end << "\n";
  out << "capacity generation 1 observed " << spec.capacity_observed_at
      << " total power=" << spec.capacity_total_power << " cooling=800 space=10 slots=20"
      << " committed power=0 cooling=0 space=0 slots=0"
      << " reserved power=0 cooling=0 space=0 slots=0\n";
  out << "redundancy generation 1 observed " << kNow
      << " level n_plus_one headroom power=100 cooling=0 space=0 slots=0\n";
  out << "tenant-evidence generation 1 observed " << kNow << " status active\n";
  out << "envelope-evidence generation 1 observed " << kNow
      << " limit power=5000 cooling=4000 space=40 slots=80"
      << " consumed power=0 cooling=0 space=0 slots=0\n";
  out << "service-class-evidence generation 1 observed " << kNow
      << " min-redundancy n_plus_one headroom power=50 cooling=0 space=0 slots=0"
      << " clearance no overcommit no\n";
  out << "maintenance generation 1 observed " << kNow << "\n";
  out << "incident generation 1 observed " << kNow << " facility " << kFacilityId
      << " state normal trust trusted\n";
  out << "placement generation 1 observed " << kNow << " allow-rack " << kRackId
      << " allow-rack " << kRack2Id << "\n";
  out << "policy generation 1 observed " << kNow << " verdict permit overcommit none id "
      << kPolicyId << " digest " << kPolicyDigest << "\n";
  return out.str();
}

// The lifecycle script. It runs in one process, which is one control epoch:
// a second process would advance the epoch and fence the grant it issued.
[[nodiscard]] std::string session_script(const std::string& scenario_path) {
  std::ostringstream out;
  out << "# admit -> commit -> acknowledge -> inspect\n";
  out << "admit --scenario " << scenario_path << "\n";
  out << "commit --grant @grant\n";
  out << "ack --intent @intent --reservation " << kReservationId
      << " --outcome reserved --generation 1 --digest " << kOwnerDigest
      << " --confirmed power=200,cooling=100,space=1,slots=1\n";
  out << "show --commitment @commitment\n";
  out << "verify\n";
  out << "stats\n";
  return out.str();
}

[[nodiscard]] bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

FAC_TEST(cli, version_reports_the_library_and_the_durable_format_revision) {
  const ProcessResult result = run_process(cli_path(), {"version"});
  FAC_CHECK(result.started);
  FAC_CHECK_EQ(result.exit_code, 0);
  FAC_CHECK(contains(result.output, "facility-admission-control " + version_string()));
  FAC_CHECK(contains(result.output,
                     "durable-format-revision " + std::to_string(kDurableFormatRevision)));
}

FAC_TEST(cli, help_prints_the_usage) {
  for (const std::string& spelling : {std::string("help"), std::string("--help")}) {
    const ProcessResult result = run_process(cli_path(), {spelling});
    FAC_CHECK(result.started);
    FAC_CHECK_EQ(result.exit_code, 0);
    FAC_CHECK(contains(result.output, "Usage: facctl <command> [options]"));
    FAC_CHECK(contains(result.output, "Exit codes: 0 allow/committed"));
  }
}

FAC_TEST(cli, an_unknown_command_is_a_usage_error) {
  const ProcessResult result = run_process(cli_path(), {"frobnicate"});
  FAC_CHECK(result.started);
  FAC_CHECK_EQ(result.exit_code, 2);
  FAC_CHECK(contains(result.output, "error unknown command frobnicate"));
}

FAC_TEST(cli, no_arguments_at_all_is_a_usage_error) {
  const ProcessResult result = run_process(cli_path(), {});
  FAC_CHECK(result.started);
  FAC_CHECK_EQ(result.exit_code, 2);
  FAC_CHECK(contains(result.output, "Usage: facctl <command> [options]"));
}

FAC_TEST(cli, session_runs_the_full_admit_commit_acknowledge_lifecycle) {
  TempDir directory("cli-session");
  const std::string store = directory.child("store");
  const std::string scenario = directory.child("scenario.txt");
  const std::string script = directory.child("script.txt");
  write_text_file(scenario, scenario_text(ScenarioSpec{}));
  write_text_file(script, session_script(scenario));

  const ProcessResult result =
      run_process(cli_path(), {"session", "--store", store, "--now", std::to_string(kNow), script,
                               "--create"});
  FAC_CHECK(result.started);
  // Every script line is echoed as "> <command>". The echo is checked without
  // its line terminator, because the tool writes through the platform's text
  // mode and a captured stream may hold either convention.
  FAC_CHECK(contains(result.output, "> admit"));
  FAC_CHECK(contains(result.output, "> commit"));
  FAC_CHECK(contains(result.output, "> ack"));
  FAC_CHECK(contains(result.output, "> show"));
  FAC_CHECK(contains(result.output, "> verify"));
  FAC_CHECK(contains(result.output, "> stats"));
  FAC_CHECK(contains(result.output, "verdict allow"));
  FAC_CHECK(contains(result.output, "commit-state committed"));
  FAC_CHECK(contains(result.output, "state confirmed"));
  FAC_CHECK(contains(result.output, "verified true"));
  FAC_CHECK(contains(result.output, "consuming-commitments 1"));
  FAC_CHECK(contains(result.output, "control-epoch 1"));
  FAC_CHECK_EQ(result.exit_code, 0);

  // The session is gone, so the store is free: a separate reader process sees
  // the same committed state from disk.
  const ProcessResult inspection = run_process(
      cli_path(), {"stats", "--store", store, "--now", std::to_string(kNow + kNanosPerSecond)});
  FAC_CHECK(inspection.started);
  FAC_CHECK_EQ(inspection.exit_code, 0);
  FAC_CHECK(contains(inspection.output, "decisions 1"));
  FAC_CHECK(contains(inspection.output, "commitments 1"));
  FAC_CHECK(contains(inspection.output, "consuming-commitments 1"));
  FAC_CHECK(contains(inspection.output, "state-digest "));
}

FAC_TEST(cli, a_demand_above_the_available_capacity_is_refused) {
  TempDir directory("cli-refuse");
  const std::string scenario = directory.child("scenario.txt");
  ScenarioSpec spec;
  spec.capacity_total_power = 100;  // the demand is 200 W
  write_text_file(scenario, scenario_text(spec));

  const ProcessResult result =
      run_process(cli_path(), {"admit", "--scenario", scenario, "--in-memory", "--now",
                               std::to_string(kNow)});
  FAC_CHECK(result.started);
  FAC_CHECK(contains(result.output, "verdict refuse"));
  FAC_CHECK(contains(result.output, "primary-blocker capacity_exhausted"));
  FAC_CHECK_EQ(result.exit_code, 1);
}

FAC_TEST(cli, a_stale_capacity_observation_is_deferred) {
  TempDir directory("cli-defer");
  const std::string scenario = directory.child("scenario.txt");
  ScenarioSpec spec;
  // One hour old against a freshness budget of five minutes.
  spec.capacity_observed_at = kNow - 3600LL * kNanosPerSecond;
  write_text_file(scenario, scenario_text(spec));

  const ProcessResult result =
      run_process(cli_path(), {"admit", "--scenario", scenario, "--in-memory", "--now",
                               std::to_string(kNow)});
  FAC_CHECK(result.started);
  FAC_CHECK(contains(result.output, "verdict defer"));
  FAC_CHECK(contains(result.output, "primary-blocker capacity_stale"));
  FAC_CHECK_EQ(result.exit_code, 1);
}

FAC_TEST(cli, a_missing_store_is_an_environment_error) {
  TempDir directory("cli-missing");
  const std::string missing = directory.child("no-such-store");
  for (const std::string& command : {std::string("stats"), std::string("verify"),
                                     std::string("list")}) {
    std::vector<std::string> arguments{command};
    if (command == "list") {
      arguments.push_back("decisions");
    }
    arguments.push_back("--store");
    arguments.push_back(missing);
    const ProcessResult result = run_process(cli_path(), arguments);
    FAC_CHECK(result.started);
    FAC_CHECK(contains(result.output, "error "));
    FAC_CHECK_EQ(result.exit_code, 2);
  }
}

FAC_TEST(cli, a_store_that_is_a_file_is_an_environment_error) {
  TempDir directory("cli-file-store");
  const std::string path = directory.child("store-file");
  write_text_file(path, "this is not a store directory\n");

  const ProcessResult initialized =
      run_process(cli_path(), {"init", "--store", path, "--now", std::to_string(kNow)});
  FAC_CHECK(initialized.started);
  FAC_CHECK(contains(initialized.output, "error "));
  FAC_CHECK_EQ(initialized.exit_code, 2);

  const ProcessResult inspected =
      run_process(cli_path(), {"stats", "--store", path, "--now", std::to_string(kNow)});
  FAC_CHECK(inspected.started);
  FAC_CHECK(contains(inspected.output, "error "));
  FAC_CHECK_EQ(inspected.exit_code, 2);
}

// A session owns the store write lock for its whole script, so a reader that
// runs while one is open must report a lock conflict rather than read a store
// that is being mutated. Holding the engine open here is exactly that state.
FAC_TEST(cli, read_only_inspection_is_refused_while_a_writer_holds_the_store) {
  TempDir directory("cli-lock-writer");
  const std::string store = directory.child("store");

  AdmissionPolicy policy;
  policy.reservation_owner = FAC_TAKE(OwnerId::from_hex(kPolicyId));
  auto clock = std::make_shared<FixedClock>(Timestamp(kNow));
  auto writer = FAC_TAKE(Engine::open_durable(store, policy, clock, true));
  FAC_CHECK_EQ(writer->epoch(), ControlEpoch(1));

  const ProcessResult listing =
      run_process(cli_path(), {"list", "decisions", "--store", store, "--now",
                               std::to_string(kNow)});
  FAC_CHECK(listing.started);
  FAC_CHECK(contains(listing.output, "error "));
  FAC_CHECK(contains(listing.output, "writer_lock_held"));
  FAC_CHECK_EQ(listing.exit_code, 2);

  const ProcessResult statistics =
      run_process(cli_path(), {"stats", "--store", store, "--now", std::to_string(kNow)});
  FAC_CHECK(statistics.started);
  FAC_CHECK(contains(statistics.output, "writer_lock_held"));
  FAC_CHECK_EQ(statistics.exit_code, 2);

  // Once the writer is gone, the same inspection answers normally: the refusal
  // was the held lock and nothing else.
  writer.reset();
  const ProcessResult after =
      run_process(cli_path(), {"list", "decisions", "--store", store, "--now",
                               std::to_string(kNow)});
  FAC_CHECK(after.started);
  FAC_CHECK_EQ(after.exit_code, 0);
  FAC_CHECK(after.output.empty());
}

// The same exclusion, proven against a genuinely separate process: the crash
// child takes the writer lock and holds it until it is killed. A reader must
// answer with its documented environment error instead of crashing.
FAC_TEST(cli, read_only_inspection_is_refused_while_another_process_holds_the_store) {
  TempDir directory("cli-lock-child");
  const std::string store = directory.child("store");

  const ProcessResult created =
      run_process(cli_path(), {"init", "--store", store, "--now", std::to_string(kNow)});
  FAC_CHECK(created.started);
  FAC_CHECK_EQ(created.exit_code, 0);

  ChildProcess holder;
  FAC_CHECK(holder.start(crash_child_path(), {"hold", store}));

  // There is no timeout here: each inspection is a complete process that either
  // reports the free store (exit 0, empty list) or the held lock (exit 2). The
  // loop only waits for the holder to take the lock it never gives back.
  bool held = false;
  ProcessResult listing;
  for (int attempt = 0; attempt < 600 && !held; ++attempt) {
    FAC_CHECK(holder.running());
    listing = run_process(cli_path(), {"list", "decisions", "--store", store, "--now",
                                       std::to_string(kNow)});
    FAC_CHECK(listing.started);
    if (listing.exit_code == 2) {
      held = true;
      break;
    }
    FAC_CHECK_EQ(listing.exit_code, 0);
    FAC_CHECK(listing.output.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  FAC_CHECK(held);
  FAC_CHECK(contains(listing.output, "writer_lock_held"));
  FAC_CHECK_EQ(listing.exit_code, 2);

  holder.terminate();
  (void)holder.wait();

  // The kernel releases the lock when the holder dies, and the store is intact.
  const ProcessResult after =
      run_process(cli_path(), {"stats", "--store", store, "--now", std::to_string(kNow)});
  FAC_CHECK(after.started);
  FAC_CHECK_EQ(after.exit_code, 0);
  FAC_CHECK(contains(after.output, "decisions 0"));
}
