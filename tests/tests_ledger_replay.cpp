// Facility Admission Control - ledger replay tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The ledger is the authoritative in-memory state of one process, and it is
// rebuilt by replaying the durable record stream. Live mutation and recovery
// replay run the same apply() path over the same encoded records, so the state
// after a restart cannot differ from the state before it. These tests prove
// that the only way it can be proven: by building state through the engine,
// reloading it from disk, and comparing the state digest of the two.
//
// Every assertion here is about state that was actually written to a real
// store directory and read back. Nothing is simulated.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fac/durable/store.hpp"
#include "fac/engine/engine.hpp"
#include "fac/ledger/ledger.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

using namespace fac;
using namespace fac_test;

namespace {

// The canonical lifecycle: two evaluations, one execution, and the defined
// evidence that answers the emitted reservation intent. Every durable record
// kind that carries ledger state is produced at least once, so a replay that
// reproduces the digest has replayed all of them.
struct Lifecycle {
  GrantId first_grant;
  GrantId second_grant;
  CommitmentId first_commitment;
  IntentId first_intent;
};

// Not [[nodiscard]]: two callers only care about the state it leaves behind.
Lifecycle run_lifecycle(Engine& engine, const Fixture& fixture) {
  Lifecycle lifecycle;

  auto first = FAC_TAKE(engine.admit(fixture.request, fixture.evidence));
  FAC_CHECK_EQ(first.verdict, Verdict::allow);
  FAC_CHECK(first.grant.has_value());
  lifecycle.first_grant = first.grant.value().grant_id;

  Fixture second = make_fixture(2);
  auto second_decision = FAC_TAKE(engine.admit(second.request, second.evidence));
  FAC_CHECK_EQ(second_decision.verdict, Verdict::allow);
  FAC_CHECK(second_decision.grant.has_value());
  lifecycle.second_grant = second_decision.grant.value().grant_id;

  auto committed = FAC_TAKE(engine.commit(lifecycle.first_grant));
  FAC_CHECK_EQ(committed.state, CommitState::committed);
  FAC_CHECK(committed.commitment_id.has_value());
  lifecycle.first_commitment = committed.commitment_id.value();

  const CommitmentRecord* commitment = engine.ledger().find_commitment(lifecycle.first_commitment);
  FAC_CHECK(commitment != nullptr);
  if (commitment == nullptr) {
    return lifecycle;
  }
  lifecycle.first_intent = commitment->intent.intent_id;

  ReservationEvidence evidence;
  evidence.intent_id = lifecycle.first_intent;
  evidence.reservation = ReservationId(0x00000000000000ABull, 0x00000000000000CDull);
  evidence.owner = fixture.policy.reservation_owner;
  evidence.outcome = ReservationOutcome::reserved;
  evidence.confirmed = fixture.request.demand;
  evidence.owner_generation = 1;
  evidence.owner_digest = fixture_digest("owner-acknowledgement");
  evidence.acknowledged_at = fixture.now;

  auto recorded = FAC_TAKE(engine.record_evidence(evidence));
  FAC_CHECK_EQ(recorded.state, CommitmentState::confirmed);
  FAC_CHECK(recorded.evidence.has_value());
  return lifecycle;
}

}  // namespace

// A durable store rebuilds exactly the state that produced it, and it rebuilds
// it twice: once through a new writer incarnation, once through a read-only
// inspection. The volatile engine that ran the same operations is the reference
// because it never serialized anything at all.
FAC_TEST(ledger_replay, durable_replay_reproduces_the_state_that_produced_it) {
  TempDir directory("ledger-replay");
  const std::string store_path = directory.child("store");
  Fixture fixture = make_fixture();

  Harness memory = make_harness(fixture);
  const Lifecycle volatile_lifecycle = run_lifecycle(*memory.engine, fixture);
  const Digest256 reference_digest = memory.engine->ledger().state_digest();
  FAC_CHECK(!reference_digest.is_unset());

  Digest256 durable_digest;
  {
    auto durable = FAC_TAKE(Engine::open_durable(store_path, fixture.policy, memory.clock, true));
    const Lifecycle durable_lifecycle = run_lifecycle(*durable, fixture);

    // The same inputs produce the same identities on both engines, which is
    // what makes the digest comparison meaningful in the first place.
    FAC_CHECK_EQ(durable_lifecycle.first_grant, volatile_lifecycle.first_grant);
    FAC_CHECK_EQ(durable_lifecycle.second_grant, volatile_lifecycle.second_grant);
    FAC_CHECK_EQ(durable_lifecycle.first_commitment, volatile_lifecycle.first_commitment);
    FAC_CHECK_EQ(durable_lifecycle.first_intent, volatile_lifecycle.first_intent);

    durable_digest = durable->ledger().state_digest();
    FAC_CHECK_OK(durable->verify());
    FAC_CHECK(durable->durable());
    FAC_CHECK(!durable->read_only());
  }
  FAC_CHECK_EQ(durable_digest, reference_digest);

  // First reopen: a second writer incarnation, which advances the control epoch
  // and must otherwise replay the identical state.
  {
    auto reopened = FAC_TAKE(Engine::open_durable(store_path, fixture.policy, memory.clock, false));
    FAC_CHECK_EQ(reopened->ledger().state_digest(), durable_digest);
    FAC_CHECK_EQ(reopened->ledger().decisions(), memory.engine->ledger().decisions());
    FAC_CHECK_EQ(reopened->ledger().grants(), memory.engine->ledger().grants());
    FAC_CHECK_EQ(reopened->ledger().commitments(), memory.engine->ledger().commitments());
    FAC_CHECK_EQ(reopened->ledger().sequence(), LedgerSequence(4));
    FAC_CHECK_EQ(reopened->epoch(), ControlEpoch(2));
    const durable::RecoveryReport* report = reopened->recovery();
    FAC_CHECK(report != nullptr);
    if (report != nullptr) {
      FAC_CHECK_EQ(report->applied_frames, static_cast<std::uint64_t>(4));
      FAC_CHECK(!report->snapshot_loaded);
      FAC_CHECK_EQ(report->discarded_tail_bytes, static_cast<std::uint64_t>(0));
    }
  }

  // Second reopen: read-only inspection, which takes no writer lock and
  // advances no epoch. It must land on the same digest again.
  {
    auto reader = FAC_TAKE(Engine::open_reader(store_path, fixture.policy, memory.clock));
    FAC_CHECK_EQ(reader->ledger().state_digest(), durable_digest);
    FAC_CHECK(reader->read_only());
    FAC_CHECK_OK(reader->verify());
    const CommitmentRecord* commitment = reader->ledger().find_commitment(
        volatile_lifecycle.first_commitment);
    FAC_CHECK(commitment != nullptr);
    if (commitment != nullptr) {
      FAC_CHECK_EQ(commitment->state, CommitmentState::confirmed);
      FAC_CHECK_EQ(commitment->intent.intent_id, volatile_lifecycle.first_intent);
      FAC_CHECK(commitment->evidence.has_value());
    }
  }
}

// apply() is the single entry point for both live mutation and replay, and it
// is the only place the sequence is checked. The bytes asserted here are the
// bytes the engine committed to the journal, read back through the public
// store API, and they replay to the digest the volatile engine produced.
FAC_TEST(ledger_replay, apply_refuses_an_out_of_order_sequence_and_a_duplicate_decision) {
  TempDir directory("ledger-apply");
  const std::string store_path = directory.child("store");
  Fixture fixture = make_fixture();

  Harness memory = make_harness(fixture);
  FAC_CHECK_OK(memory.engine->admit(fixture.request, fixture.evidence));
  const Digest256 reference_digest = memory.engine->ledger().state_digest();

  {
    auto durable = FAC_TAKE(Engine::open_durable(store_path, fixture.policy, memory.clock, true));
    auto decision = FAC_TAKE(durable->admit(fixture.request, fixture.evidence));
    FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  }

  auto store = FAC_TAKE(durable::Store::open_reader(store_path));
  const std::vector<durable::Frame>& frames = store->frames();
  FAC_CHECK_EQ(frames.size(), static_cast<std::size_t>(1));
  if (frames.empty()) {
    return;
  }
  const durable::Frame& frame = frames.front();
  FAC_CHECK_EQ(frame.kind, durable::RecordKind::decision_recorded);
  const std::span<const std::byte> payload(frame.payload.data(), frame.payload.size());

  // Replaying the recorded bytes at their own sequence reproduces the state the
  // volatile engine reached by applying the same record live.
  Ledger replayed;
  FAC_CHECK_OK(replayed.apply(frame.kind, frame.sequence, payload));
  FAC_CHECK_EQ(replayed.sequence(), LedgerSequence(1));
  FAC_CHECK(replayed.find_decision(fixture.request.request_id) != nullptr);
  FAC_CHECK_EQ(replayed.state_digest(), reference_digest);

  // A record that skips a sequence is refused: replay can neither reorder nor
  // silently drop a record.
  Ledger out_of_order;
  FAC_CHECK_ERR(out_of_order.apply(frame.kind, LedgerSequence(9), payload), ErrorCode::conflict);
  FAC_CHECK_EQ(out_of_order.sequence(), LedgerSequence(0));

  // A second decision for a request that is already decided is refused rather
  // than overwriting the first one.
  Ledger duplicated;
  FAC_CHECK_OK(duplicated.apply(frame.kind, LedgerSequence(1), payload));
  FAC_CHECK_ERR(duplicated.apply(frame.kind, LedgerSequence(2), payload),
                ErrorCode::duplicate_field);
  FAC_CHECK_EQ(duplicated.sequence(), LedgerSequence(1));
  FAC_CHECK_EQ(duplicated.decisions().size(), static_cast<std::size_t>(1));
  FAC_CHECK_EQ(duplicated.state_digest(), reference_digest);
}

// Compaction publishes a snapshot and starts a new journal segment. It changes
// where the state is stored, never what the state is.
FAC_TEST(ledger_replay, compacting_and_reopening_preserves_the_state_digest) {
  TempDir directory("ledger-compact");
  const std::string store_path = directory.child("store");
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);

  Digest256 before;
  {
    auto engine = FAC_TAKE(Engine::open_durable(store_path, fixture.policy, harness.clock, true));
    run_lifecycle(*engine, fixture);
    before = engine->ledger().state_digest();

    FAC_CHECK_OK(engine->compact());
    FAC_CHECK_EQ(engine->ledger().state_digest(), before);
    FAC_CHECK_EQ(engine->ledger().sequence(), LedgerSequence(4));
    FAC_CHECK_OK(engine->verify());
  }

  {
    auto reader = FAC_TAKE(Engine::open_reader(store_path, fixture.policy, harness.clock));
    FAC_CHECK_EQ(reader->ledger().state_digest(), before);
    FAC_CHECK_OK(reader->verify());
    const durable::RecoveryReport* report = reader->recovery();
    FAC_CHECK(report != nullptr);
    if (report != nullptr) {
      FAC_CHECK(report->snapshot_loaded);
      FAC_CHECK_EQ(report->snapshot_sequence, static_cast<std::uint64_t>(4));
      FAC_CHECK_EQ(report->applied_frames, static_cast<std::uint64_t>(0));
    }
  }

  {
    // A writer incarnation reopens the same compacted store and advances the
    // epoch without changing the ledger state.
    auto writer = FAC_TAKE(Engine::open_durable(store_path, fixture.policy, harness.clock, false));
    FAC_CHECK_EQ(writer->ledger().state_digest(), before);
    FAC_CHECK_EQ(writer->ledger().sequence(), LedgerSequence(4));
    FAC_CHECK_EQ(writer->epoch(), ControlEpoch(2));
  }
}

// A store that was compacted and then written to again holds a snapshot plus a
// later journal suffix. Replaying that pair must land on exactly the state that
// replaying the whole journal alone lands on: compaction is a storage decision,
// not a semantic one.
FAC_TEST(ledger_replay, snapshot_plus_a_journal_suffix_replays_like_the_journal_alone) {
  TempDir directory("ledger-suffix");
  Fixture fixture = make_fixture();
  Harness harness = make_harness(fixture);

  const std::string compacted_path = directory.child("compacted");
  Digest256 compacted_digest;
  {
    auto engine = FAC_TAKE(Engine::open_durable(compacted_path, fixture.policy, harness.clock, true));
    run_lifecycle(*engine, fixture);
    FAC_CHECK_EQ(engine->ledger().sequence(), LedgerSequence(4));

    // The snapshot is published here, and everything after it is the suffix.
    FAC_CHECK_OK(engine->compact());

    Fixture third = make_fixture(3);
    auto decision = FAC_TAKE(engine->admit(third.request, third.evidence));
    FAC_CHECK_EQ(decision.verdict, Verdict::allow);
    FAC_CHECK(decision.grant.has_value());
    auto committed = FAC_TAKE(engine->commit(decision.grant.value().grant_id));
    FAC_CHECK_EQ(committed.state, CommitState::committed);
    FAC_CHECK_EQ(engine->ledger().sequence(), LedgerSequence(6));

    compacted_digest = engine->ledger().state_digest();
  }

  const std::string plain_path = directory.child("plain");
  {
    // The same operations, in the same order, against a store that is never
    // compacted. Only the layout differs.
    auto engine = FAC_TAKE(Engine::open_durable(plain_path, fixture.policy, harness.clock, true));
    run_lifecycle(*engine, fixture);
    Fixture third = make_fixture(3);
    auto decision = FAC_TAKE(engine->admit(third.request, third.evidence));
    FAC_CHECK(decision.grant.has_value());
    auto committed = FAC_TAKE(engine->commit(decision.grant.value().grant_id));
    FAC_CHECK_EQ(committed.state, CommitState::committed);
    FAC_CHECK_EQ(engine->ledger().sequence(), LedgerSequence(6));
    FAC_CHECK_EQ(engine->ledger().state_digest(), compacted_digest);
  }

  auto compacted = FAC_TAKE(Engine::open_reader(compacted_path, fixture.policy, harness.clock));
  auto plain = FAC_TAKE(Engine::open_reader(plain_path, fixture.policy, harness.clock));

  FAC_CHECK_EQ(compacted->ledger().state_digest(), compacted_digest);
  FAC_CHECK_EQ(plain->ledger().state_digest(), compacted_digest);
  FAC_CHECK_EQ(compacted->ledger().decisions(), plain->ledger().decisions());
  FAC_CHECK_EQ(compacted->ledger().grants(), plain->ledger().grants());
  FAC_CHECK_EQ(compacted->ledger().commitments(), plain->ledger().commitments());
  FAC_CHECK_EQ(compacted->ledger().sequence(), plain->ledger().sequence());

  const durable::RecoveryReport* report = compacted->recovery();
  FAC_CHECK(report != nullptr);
  if (report != nullptr) {
    FAC_CHECK(report->snapshot_loaded);
    FAC_CHECK_EQ(report->snapshot_sequence, static_cast<std::uint64_t>(4));
    FAC_CHECK_EQ(report->applied_frames, static_cast<std::uint64_t>(2));
  }
  FAC_CHECK_OK(compacted->verify());
  FAC_CHECK_OK(plain->verify());
}
