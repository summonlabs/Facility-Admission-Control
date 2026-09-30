// Facility Admission Control - durability tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every test below works on a real store directory. The journal, the two
// manifest slots and the snapshot are read back as bytes, corrupted as bytes
// and reopened through the public Store and Engine API, so a claim about
// durability is a claim about what is actually on disk after a failure.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "fac/core/digest.hpp"
#include "fac/core/error.hpp"
#include "fac/durable/format.hpp"
#include "fac/durable/store.hpp"
#include "fac/version.hpp"
#include "fixtures.hpp"
#include "test_support.hpp"

namespace {

using namespace fac;
using namespace fac_test;

// ---------------------------------------------------------------------------
// Paths and raw bytes. A durable claim is checked against the file, never
// against what the process remembers writing.
// ---------------------------------------------------------------------------

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

std::vector<std::byte> read_bytes(const std::string& path) {
  std::ifstream stream(fs_path(path), std::ios::binary);
  if (!stream) {
    return {};
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff length = stream.tellg();
  stream.seekg(0, std::ios::beg);
  if (length <= 0) {
    return {};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(length));
  stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return bytes;
}

void write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(fs_path(path), std::ios::binary | std::ios::trunc);
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  stream.flush();
  FAC_CHECK(stream.good());
}

void truncate_bytes(const std::string& path, std::uint64_t length) {
  std::error_code code;
  std::filesystem::resize_file(fs_path(path), length, code);
  FAC_CHECK(!code);
}

std::uint64_t size_of(const std::string& path) {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(fs_path(path), code);
  FAC_CHECK(!code);
  return static_cast<std::uint64_t>(size);
}

Digest256 digest_of(const std::vector<std::byte>& bytes) {
  return Digest256::of(std::span<const std::byte>(bytes.data(), bytes.size()));
}

void copy_directory(const std::string& from, const std::string& to) {
  std::error_code code;
  std::filesystem::create_directories(fs_path(to), code);
  FAC_CHECK(!code);
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(fs_path(from), code)) {
    FAC_CHECK(!code);
    if (!entry.is_regular_file(code)) {
      FAC_CHECK(!code);
      continue;
    }
    std::filesystem::copy_file(entry.path(), fs_path(to) / entry.path().filename(),
                               std::filesystem::copy_options::overwrite_existing, code);
    FAC_CHECK(!code);
  }
}

struct FileFingerprint {
  std::string name;
  std::uint64_t size = 0;
  Digest256 digest;

  friend bool operator==(const FileFingerprint&, const FileFingerprint&) = default;
};

// Size and SHA-256 of every file in the store directory. Opening a store for
// reading is only honest if this is byte for byte unchanged afterwards.
std::vector<FileFingerprint> fingerprint_directory(const std::string& directory) {
  std::vector<std::string> names;
  std::error_code code;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(fs_path(directory), code)) {
    FAC_CHECK(!code);
    if (!entry.is_regular_file(code)) {
      FAC_CHECK(!code);
      continue;
    }
    const std::u8string text = entry.path().filename().u8string();
    names.emplace_back(reinterpret_cast<const char*>(text.data()), text.size());
  }
  FAC_CHECK(!code);
  std::sort(names.begin(), names.end());
  std::vector<FileFingerprint> fingerprints;
  for (const std::string& name : names) {
    FileFingerprint fingerprint;
    fingerprint.name = name;
    fingerprint.size = size_of(join_path(directory, name));
    fingerprint.digest = digest_of(read_bytes(join_path(directory, name)));
    fingerprints.push_back(fingerprint);
  }
  return fingerprints;
}

// The manifest facts of a closed store, read straight from both slots.
struct StoreFacts {
  durable::Manifest winner;
  durable::Manifest other;
  std::size_t winner_slot = 0;
  std::size_t other_slot = 0;
  std::uint64_t journal_bytes = 0;
  std::vector<durable::Frame> frames;
};

std::vector<std::byte> read_slot_bytes(const std::string& directory, std::size_t slot) {
  return read_bytes(join_path(directory, std::string(durable::kManifestNames[slot])));
}

StoreFacts read_store_facts(const std::string& directory) {
  StoreFacts facts;
  {
    auto reader = FAC_TAKE(durable::Store::open_reader(directory));
    facts.journal_bytes = reader->report().journal_bytes;
    facts.frames = reader->frames();
  }
  durable::Manifest slots[2];
  for (std::size_t i = 0; i < 2; ++i) {
    const std::vector<std::byte> bytes = read_slot_bytes(directory, i);
    FAC_CHECK_EQ(bytes.size(), durable::kManifestBytes);
    slots[i] = FAC_TAKE(durable::Manifest::decode(std::span<const std::byte>(bytes.data(), bytes.size())));
  }
  FAC_CHECK(slots[0].manifest_generation != slots[1].manifest_generation);
  const bool first_is_newer = slots[0].manifest_generation > slots[1].manifest_generation;
  facts.winner_slot = first_is_newer ? 0 : 1;
  facts.other_slot = first_is_newer ? 1 : 0;
  facts.winner = slots[facts.winner_slot];
  facts.other = slots[facts.other_slot];
  return facts;
}

durable::StoreOptions writer_options() {
  durable::StoreOptions options;
  options.create = true;
  options.advance_epoch = true;
  return options;
}

// A failed open must fail closed through both the writer and the reader path.
void expect_open_fails(const std::string& directory, ErrorCode code) {
  FAC_CHECK_ERR(durable::Store::open_writer(directory, writer_options()), code);
  FAC_CHECK_ERR(durable::Store::open_reader(directory), code);
}

Decision admit_fixture(Engine& engine, std::uint64_t index) {
  const Fixture step = make_fixture(index);
  auto decision = engine.admit(step.request, step.evidence);
  FAC_CHECK_OK(decision);
  return decision.value();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// A committed store survives a close and a reopen with exactly the sequence it
// committed, and the frames read back are the same bytes both times.
FAC_TEST(durable, clean_round_trip) {
  TempDir dir("durable-round-trip");
  const Fixture fixture = make_fixture();

  std::vector<RequestId> request_ids;
  std::vector<Decision> recorded;
  Digest256 digest_before;
  {
    Harness harness = make_durable_harness(fixture, dir.path(), true);
    for (std::uint64_t index = 1; index <= 3; ++index) {
      const Decision decision = admit_fixture(*harness.engine, index);
      FAC_CHECK_EQ(decision.verdict, Verdict::allow);
      request_ids.push_back(decision.request_id);
      recorded.push_back(decision);
    }
    FAC_CHECK_EQ(harness.engine->sequence().value(), std::uint64_t{3});
    digest_before = harness.engine->ledger().state_digest();
  }

  std::vector<durable::Frame> first_read;
  {
    auto reader = FAC_TAKE(durable::Store::open_reader(dir.path()));
    FAC_CHECK(reader->read_only());
    FAC_CHECK_EQ(reader->sequence().value(), std::uint64_t{3});
    FAC_CHECK_EQ(reader->report().discarded_tail_bytes, std::uint64_t{0});
    FAC_CHECK_EQ(reader->report().applied_frames, std::uint64_t{3});
    FAC_CHECK_OK(reader->verify());
    first_read = reader->frames();
  }
  FAC_CHECK_EQ(first_read.size(), std::size_t{3});
  for (std::size_t i = 0; i < first_read.size(); ++i) {
    FAC_CHECK(first_read[i].kind == durable::RecordKind::decision_recorded);
    FAC_CHECK_EQ(first_read[i].sequence.value(), static_cast<std::uint64_t>(i + 1));
    FAC_CHECK(!first_read[i].payload.empty());
  }

  {
    Harness reopened = make_durable_harness(fixture, dir.path(), false);
    const Ledger& ledger = reopened.engine->ledger();
    FAC_CHECK_EQ(ledger.sequence().value(), std::uint64_t{3});
    FAC_CHECK(ledger.state_digest() == digest_before);
    FAC_CHECK_EQ(ledger.decisions().size(), std::size_t{3});
    FAC_CHECK_EQ(ledger.grants().size(), std::size_t{3});
    for (std::size_t i = 0; i < request_ids.size(); ++i) {
      const DecisionEntry* entry = ledger.find_decision(request_ids[i]);
      FAC_CHECK(entry != nullptr);
      if (entry != nullptr) {
        FAC_CHECK(entry->decision == recorded[i]);
        FAC_CHECK(entry->request.digest() == recorded[i].request_digest);
      }
    }
    // Replaying the same requests returns the recorded decisions, byte for
    // byte, and does not append a second record for the same identity.
    for (std::size_t i = 0; i < request_ids.size(); ++i) {
      const Fixture step = make_fixture(static_cast<std::uint64_t>(i + 1));
      auto replay = reopened.engine->admit(step.request, step.evidence);
      FAC_CHECK_OK(replay);
      FAC_CHECK(replay.value().replayed);
      Decision copy = replay.value();
      copy.replayed = false;
      FAC_CHECK(copy == recorded[i]);
    }
    FAC_CHECK_EQ(reopened.engine->sequence().value(), std::uint64_t{3});
  }

  // A second reader sees exactly the same committed frames.
  auto reader = FAC_TAKE(durable::Store::open_reader(dir.path()));
  const std::vector<durable::Frame>& second_read = reader->frames();
  FAC_CHECK_EQ(second_read.size(), first_read.size());
  for (std::size_t i = 0; i < second_read.size(); ++i) {
    FAC_CHECK(second_read[i].kind == first_read[i].kind);
    FAC_CHECK(second_read[i].sequence == first_read[i].sequence);
    FAC_CHECK(second_read[i].payload == first_read[i].payload);
  }
}

// Bytes past the committed journal length are an uncommitted tail. Even a
// byte-valid frame there is discarded, because no manifest covers it.
FAC_TEST(durable, uncommitted_tail_is_discarded) {
  TempDir dir("durable-tail");
  const Fixture fixture = make_fixture();
  const std::string journal_path = join_path(dir.path(), std::string(durable::kJournalFileName));

  Digest256 digest_before;
  std::uint64_t committed = 0;
  std::vector<std::byte> duplicate_frame;
  {
    Harness harness = make_durable_harness(fixture, dir.path(), true);
    (void)admit_fixture(*harness.engine, 1);
    (void)admit_fixture(*harness.engine, 2);
    digest_before = harness.engine->ledger().state_digest();
  }
  {
    auto reader = FAC_TAKE(durable::Store::open_reader(dir.path()));
    committed = reader->report().journal_bytes;
    FAC_CHECK_EQ(reader->frames().size(), std::size_t{2});
    const durable::Frame& last = reader->frames().back();
    const std::size_t frame_bytes = durable::kFrameHeaderBytes + last.payload.size() + 4;
    FAC_CHECK(frame_bytes <= committed);
    const std::vector<std::byte> journal = read_bytes(journal_path);
    FAC_CHECK_EQ(journal.size(), committed);
    duplicate_frame.assign(journal.end() - static_cast<std::ptrdiff_t>(frame_bytes), journal.end());
  }

  // Append one byte-valid frame that no manifest commits, plus a torn fragment.
  const std::vector<std::byte> tail(duplicate_frame);
  std::vector<std::byte> garbage(7, std::byte{0xAB});
  {
    std::ofstream stream(fs_path(journal_path), std::ios::binary | std::ios::app);
    stream.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    stream.write(reinterpret_cast<const char*>(garbage.data()), static_cast<std::streamsize>(garbage.size()));
    stream.flush();
    FAC_CHECK(stream.good());
  }
  const std::uint64_t tail_bytes = static_cast<std::uint64_t>(tail.size() + garbage.size());
  FAC_CHECK_EQ(size_of(journal_path), committed + tail_bytes);

  {
    auto writer = FAC_TAKE(durable::Store::open_writer(dir.path(), writer_options()));
    FAC_CHECK_EQ(writer->report().discarded_tail_bytes, tail_bytes);
    FAC_CHECK_EQ(writer->report().journal_bytes, committed);
    FAC_CHECK_EQ(writer->sequence().value(), std::uint64_t{2});
    FAC_CHECK_EQ(writer->frames().size(), std::size_t{2});
    FAC_CHECK_OK(writer->verify());
  }
  // The uncommitted bytes are gone from the file, not merely ignored.
  FAC_CHECK_EQ(size_of(journal_path), committed);

  Harness reopened = make_durable_harness(fixture, dir.path(), false);
  FAC_CHECK_EQ(reopened.engine->sequence().value(), std::uint64_t{2});
  FAC_CHECK(reopened.engine->ledger().state_digest() == digest_before);
}

// Every corruption of the committed bytes fails closed with the error code that
// names the corruption, through the reader path and the writer path alike.
FAC_TEST(durable, corruption_sweep) {
  TempDir dir("durable-corruption");
  const Fixture fixture = make_fixture();
  const std::string journal_name(durable::kJournalFileName);

  // A pristine store with two committed records.
  std::uint64_t committed = 0;
  {
    Harness harness = make_durable_harness(fixture, dir.path(), true);
    (void)admit_fixture(*harness.engine, 1);
    (void)admit_fixture(*harness.engine, 2);
  }
  {
    auto reader = FAC_TAKE(durable::Store::open_reader(dir.path()));
    committed = reader->report().journal_bytes;
    FAC_CHECK(committed > 0);
    FAC_CHECK_EQ(reader->frames().size(), std::size_t{2});
  }

  // A second pristine store: two records, a snapshot and one frame after it.
  const std::string snapshot_source = dir.child("snapshot-source");
  {
    Harness harness = make_durable_harness(fixture, snapshot_source, true);
    (void)admit_fixture(*harness.engine, 1);
    (void)admit_fixture(*harness.engine, 2);
    FAC_CHECK_OK(harness.engine->compact());
    (void)admit_fixture(*harness.engine, 3);
  }

  // (a) One flipped bit inside the committed region.
  {
    const std::string case_dir = dir.child("bit-flip");
    copy_directory(dir.path(), case_dir);
    const std::string journal = join_path(case_dir, journal_name);
    std::vector<std::byte> bytes = read_bytes(journal);
    FAC_CHECK_EQ(bytes.size(), committed);
    bytes[committed / 2] ^= std::byte{0x01};
    write_bytes(journal, bytes);
    expect_open_fails(case_dir, ErrorCode::digest_mismatch);
  }

  // (b) The committed region truncated.
  {
    const std::string case_dir = dir.child("truncated");
    copy_directory(dir.path(), case_dir);
    truncate_bytes(join_path(case_dir, journal_name), committed - 1);
    expect_open_fails(case_dir, ErrorCode::journal_truncated);
  }

  // (c) Both manifest CRCs corrupted: no slot can be verified, so the store
  // refuses to guess which publications happened.
  {
    const std::string case_dir = dir.child("manifest-crc");
    copy_directory(dir.path(), case_dir);
    for (std::size_t slot = 0; slot < 2; ++slot) {
      const std::string path = join_path(case_dir, std::string(durable::kManifestNames[slot]));
      std::vector<std::byte> bytes = read_slot_bytes(case_dir, slot);
      FAC_CHECK_EQ(bytes.size(), durable::kManifestBytes);
      for (std::size_t i = 0; i < 4; ++i) {
        bytes[durable::kManifestBytes - 4 + i] ^= std::byte{0xFF};
      }
      write_bytes(path, bytes);
      auto decoded = durable::Manifest::decode(std::span<const std::byte>(bytes.data(), bytes.size()));
      FAC_CHECK_ERR(decoded, ErrorCode::checksum_mismatch);
    }
    expect_open_fails(case_dir, ErrorCode::manifest_invalid);
  }

  // (d) A corrupted snapshot, which the manifest still describes by digest.
  {
    const std::string case_dir = dir.child("snapshot-digest");
    copy_directory(snapshot_source, case_dir);
    const std::string snapshot_path = join_path(case_dir, std::string(durable::kSnapshotFileName));
    std::vector<std::byte> bytes = read_bytes(snapshot_path);
    FAC_CHECK(bytes.size() > durable::kSnapshotHeaderBytes);
    bytes[bytes.size() / 2] ^= std::byte{0x01};
    write_bytes(snapshot_path, bytes);
    expect_open_fails(case_dir, ErrorCode::digest_mismatch);
  }

  // (e) Both manifest slots zeroed: the store holds data that nothing
  // describes, and it says so instead of inventing an empty history.
  {
    const std::string case_dir = dir.child("manifest-zeroed");
    copy_directory(dir.path(), case_dir);
    const std::vector<std::byte> zeros(durable::kManifestBytes, std::byte{0});
    for (std::size_t slot = 0; slot < 2; ++slot) {
      write_bytes(join_path(case_dir, std::string(durable::kManifestNames[slot])), zeros);
      auto decoded = durable::Manifest::decode(std::span<const std::byte>(zeros.data(), zeros.size()));
      FAC_CHECK_ERR(decoded, ErrorCode::checksum_mismatch);
    }
    expect_open_fails(case_dir, ErrorCode::manifest_invalid);
  }

  // The snapshot source itself is still a healthy store, so the sweep proves
  // the corruption and not a broken fixture.
  {
    auto reader = FAC_TAKE(durable::Store::open_reader(snapshot_source));
    FAC_CHECK(reader->report().snapshot_loaded);
    FAC_CHECK_OK(reader->verify());
  }
}

// A slot that claims a newer generation but less committed data than the slot
// it supersedes is a rollback. The store refuses to pick either history.
FAC_TEST(durable, rollback_detection) {
  TempDir dir("durable-rollback");
  const Fixture fixture = make_fixture();
  {
    Harness harness = make_durable_harness(fixture, dir.path(), true);
    (void)admit_fixture(*harness.engine, 1);
    (void)admit_fixture(*harness.engine, 2);
  }

  const StoreFacts facts = read_store_facts(dir.path());
  FAC_CHECK(facts.winner.manifest_generation > facts.other.manifest_generation);
  FAC_CHECK(facts.frames.size() == 2);
  FAC_CHECK(facts.winner.journal_length > facts.other.journal_length);
  FAC_CHECK_EQ(facts.winner.journal_length, facts.journal_bytes);

  const std::uint64_t first_frame_bytes =
      static_cast<std::uint64_t>(durable::kFrameHeaderBytes) +
      static_cast<std::uint64_t>(facts.frames.front().payload.size()) + 4;
  FAC_CHECK(first_frame_bytes < facts.journal_bytes);
  const std::vector<std::byte> journal = read_bytes(join_path(dir.path(), std::string(durable::kJournalFileName)));
  FAC_CHECK_EQ(journal.size(), facts.journal_bytes);

  // A newer generation that commits only the first frame: it is newer than the
  // genuine slot but names less history than the slot it must supersede.
  durable::Manifest forged;
  forged.format_revision = static_cast<std::uint16_t>(kDurableFormatRevision);
  forged.manifest_generation = facts.winner.manifest_generation + 1;
  forged.control_epoch = facts.winner.control_epoch;
  forged.ledger_sequence = facts.frames.front().sequence.value();
  forged.journal_length = first_frame_bytes;
  forged.journal_digest = digest_of(std::vector<std::byte>(
      journal.begin(), journal.begin() + static_cast<std::ptrdiff_t>(first_frame_bytes)));
  forged.snapshot_present = false;
  forged.snapshot_sequence = 0;
  forged.snapshot_digest = Digest256{};

  const std::vector<std::byte> encoded = forged.encode();
  FAC_CHECK_EQ(encoded.size(), durable::kManifestBytes);
  const std::size_t forged_slot = static_cast<std::size_t>(forged.manifest_generation % 2);
  FAC_CHECK(forged_slot != facts.winner_slot);
  write_bytes(join_path(dir.path(), std::string(durable::kManifestNames[forged_slot])), encoded);

  // The forged slot decodes and is the higher generation, so it is selected -
  // and then refused, because it claims less than the slot it supersedes.
  const durable::Manifest decoded =
      FAC_TAKE(durable::Manifest::decode(std::span<const std::byte>(encoded.data(), encoded.size())));
  FAC_CHECK_EQ(decoded.manifest_generation, forged.manifest_generation);
  expect_open_fails(dir.path(), ErrorCode::rollback_detected);
}

// Compaction publishes a snapshot and restarts the journal. The authoritative
// state before and after is the same state, and a reopen loads the snapshot.
FAC_TEST(durable, compaction_preserves_state) {
  TempDir dir("durable-compaction");
  const Fixture fixture = make_fixture();
  Harness harness = make_durable_harness(fixture, dir.path(), true);

  GrantId grant_id;
  for (std::uint64_t index = 1; index <= 3; ++index) {
    const Decision decision = admit_fixture(*harness.engine, index);
    FAC_CHECK_EQ(decision.verdict, Verdict::allow);
    if (index == 1) {
      FAC_CHECK(decision.grant.has_value());
      grant_id = decision.grant->grant_id;
    }
  }

  auto outcome = harness.engine->commit(grant_id);
  FAC_CHECK_OK(outcome);
  FAC_CHECK_EQ(outcome.value().state, CommitState::committed);
  FAC_CHECK(outcome.value().commitment_id.has_value());
  const CommitmentId commitment_id = outcome.value().commitment_id.value();

  // The owner rejects the intent: a terminal, non-consuming state that the
  // snapshot format carries. (A full confirmation is covered by
  // durable.confirmed_commitment_round_trip.)
  const CommitmentRecord* commitment = harness.engine->ledger().find_commitment(commitment_id);
  FAC_CHECK(commitment != nullptr);
  ReservationEvidence evidence;
  evidence.intent_id = commitment->intent.intent_id;
  evidence.reservation = ReservationId(0x5A5A5A5A5A5A5A5Aull, 0x1234567812345678ull);
  evidence.owner = fixture.policy.reservation_owner;
  evidence.outcome = ReservationOutcome::rejected;
  evidence.owner_generation = 1;
  evidence.owner_digest = fixture_digest("owner-rejection");
  evidence.acknowledged_at = fixture.now;
  auto recorded = harness.engine->record_evidence(evidence);
  FAC_CHECK_OK(recorded);
  FAC_CHECK_EQ(recorded.value().state, CommitmentState::released);

  const Digest256 digest_before = harness.engine->ledger().state_digest();
  const std::uint64_t sequence_before = harness.engine->sequence().value();
  const auto decisions_before = harness.engine->ledger().decisions();
  const auto grants_before = harness.engine->ledger().grants();
  const auto commitments_before = harness.engine->ledger().commitments();

  FAC_CHECK_OK(harness.engine->compact());
  FAC_CHECK(harness.engine->ledger().state_digest() == digest_before);
  FAC_CHECK_EQ(harness.engine->sequence().value(), sequence_before);

  // The store compaction just published must verify as it stands, before
  // anything else touches it: the surviving manifest slot claims a longer
  // journal than the snapshot-bearing manifest that supersedes it, and that is
  // a legitimate state, not a rollback.
  FAC_CHECK_OK(harness.engine->verify());
  harness.engine.reset();

  {
    auto reader = FAC_TAKE(durable::Store::open_reader(dir.path()));
    FAC_CHECK(reader->report().snapshot_loaded);
    FAC_CHECK_EQ(reader->report().snapshot_sequence, sequence_before);
    FAC_CHECK(reader->snapshot().has_value());
    FAC_CHECK_EQ(reader->frames().size(), std::size_t{0});
    FAC_CHECK_EQ(reader->report().journal_bytes, std::uint64_t{0});
    FAC_CHECK_EQ(size_of(join_path(dir.path(), std::string(durable::kJournalFileName))), std::uint64_t{0});
    FAC_CHECK(size_of(join_path(dir.path(), std::string(durable::kSnapshotFileName))) >
              durable::kSnapshotHeaderBytes);
    FAC_CHECK_OK(reader->verify());
  }

  {
    Harness reopened = make_durable_harness(fixture, dir.path(), false);
    const Ledger& ledger = reopened.engine->ledger();
    FAC_CHECK_EQ(ledger.sequence().value(), sequence_before);
    FAC_CHECK(ledger.state_digest() == digest_before);
    FAC_CHECK(ledger.decisions() == decisions_before);
    FAC_CHECK(ledger.grants() == grants_before);
    FAC_CHECK(ledger.commitments() == commitments_before);
    FAC_CHECK(reopened.engine->recovery() != nullptr);
    FAC_CHECK(reopened.engine->recovery()->snapshot_loaded);
  }
}

// Deleting or damaging one manifest slot must not lose the store: the other
// slot is authoritative and the recovery report says so rather than repairing
// the store silently.
FAC_TEST(durable, manifest_slot_recovery) {
  TempDir dir("durable-slot-recovery");
  const Fixture fixture = make_fixture();
  const auto build_store = [&fixture](const std::string& path) {
    Harness harness = make_durable_harness(fixture, path, true);
    (void)admit_fixture(*harness.engine, 1);
    (void)admit_fixture(*harness.engine, 2);
  };

  // (a) The newer slot is deleted.
  {
    const std::string store = dir.child("deleted");
    build_store(store);
    const StoreFacts facts = read_store_facts(store);
    FAC_CHECK(facts.winner.journal_length > facts.other.journal_length);
    FAC_CHECK_EQ(facts.winner.journal_length, facts.journal_bytes);

    std::error_code code;
    std::filesystem::remove(fs_path(join_path(store, std::string(durable::kManifestNames[facts.winner_slot]))), code);
    FAC_CHECK(!code);

    auto opened = FAC_TAKE(durable::Store::open_writer(store, writer_options()));
    FAC_CHECK_EQ(opened->report().sequence.value(), facts.other.ledger_sequence);
    FAC_CHECK_EQ(opened->report().journal_bytes, facts.other.journal_length);
    FAC_CHECK_EQ(opened->report().discarded_tail_bytes, facts.journal_bytes - facts.other.journal_length);
    FAC_CHECK_EQ(opened->frames().size(), std::size_t{1});
    FAC_CHECK_OK(opened->verify());
    bool noted = false;
    for (const std::string& note : opened->report().notes) {
      if (note.find("single manifest slot") != std::string::npos) {
        noted = true;
      }
    }
    FAC_CHECK(noted);
  }

  // (b) The newer slot is unreadable garbage rather than absent.
  {
    const std::string store = dir.child("corrupt");
    build_store(store);
    const StoreFacts facts = read_store_facts(store);
    const std::vector<std::byte> zeros(durable::kManifestBytes, std::byte{0});
    write_bytes(join_path(store, std::string(durable::kManifestNames[facts.winner_slot])), zeros);

    auto opened = FAC_TAKE(durable::Store::open_writer(store, writer_options()));
    FAC_CHECK_EQ(opened->report().sequence.value(), facts.other.ledger_sequence);
    FAC_CHECK_EQ(opened->report().journal_bytes, facts.other.journal_length);
    FAC_CHECK_EQ(opened->report().discarded_tail_bytes, facts.journal_bytes - facts.other.journal_length);
    bool recovered = false;
    bool ignored = false;
    for (const std::string& note : opened->report().notes) {
      if (note.find("single manifest slot") != std::string::npos) {
        recovered = true;
      }
      if (note.find("unreadable") != std::string::npos) {
        ignored = true;
      }
    }
    FAC_CHECK(recovered);
    FAC_CHECK(ignored);
    FAC_CHECK_OK(opened->verify());
  }
}

// Read-only inspection runs the same integrity checks and writes nothing at
// all: every file in the store is byte for byte what it was.
//
// The store is deliberately not compacted here: the read-only claim is about
// inspection never writing, and it is cleaner to prove it on a store whose only
// content is committed journal frames. Compaction and snapshot reloads are
// proven by durable.compaction_preserves_state,
// durable.snapshot_grant_detail_round_trip and
// durable.confirmed_commitment_round_trip.
FAC_TEST(durable, read_only_open_does_not_modify) {
  TempDir dir("durable-read-only");
  const Fixture fixture = make_fixture();
  {
    Harness harness = make_durable_harness(fixture, dir.path(), true);
    (void)admit_fixture(*harness.engine, 1);
    (void)admit_fixture(*harness.engine, 2);
    (void)admit_fixture(*harness.engine, 3);
  }

  const std::vector<FileFingerprint> before = fingerprint_directory(dir.path());
  FAC_CHECK(before.size() >= 4);

  {
    auto reader = FAC_TAKE(durable::Store::open_reader(dir.path()));
    FAC_CHECK(reader->read_only());
    FAC_CHECK(!reader->report().snapshot_loaded);
    FAC_CHECK(!reader->snapshot().has_value());
    FAC_CHECK_EQ(reader->frames().size(), std::size_t{3});
    FAC_CHECK_EQ(reader->report().discarded_tail_bytes, std::uint64_t{0});
    FAC_CHECK_OK(reader->verify());
  }

  {
    Harness inspection = make_durable_harness(fixture, dir.path(), false, true);
    FAC_CHECK(inspection.engine->read_only());
    FAC_CHECK(inspection.engine->durable());
    FAC_CHECK_EQ(inspection.engine->sequence().value(), std::uint64_t{3});
    const Fixture step = make_fixture(4);
    FAC_CHECK_ERR(inspection.engine->admit(step.request, step.evidence), ErrorCode::read_only_store);
    FAC_CHECK_ERR(inspection.engine->compact(), ErrorCode::read_only_store);
    FAC_CHECK_OK(inspection.engine->verify());
  }

  const std::vector<FileFingerprint> after = fingerprint_directory(dir.path());
  FAC_CHECK_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < before.size() && i < after.size(); ++i) {
    FAC_CHECK_EQ(after[i].name, before[i].name);
    FAC_CHECK_EQ(after[i].size, before[i].size);
    FAC_CHECK(after[i].digest == before[i].digest);
  }
  FAC_CHECK(after == before);
}

// An answered commitment must round trip. A full acknowledgement resolves the
// question - the owner answered - while the capacity stays consumed, and that
// state has to survive the record codec, the ledger snapshot codec, compaction
// and a reopen. The lifecycle rule asserted here is the one the durable decoder
// enforces: provisional is unanswered and carries no resolution time, an
// answered commitment carries both its resolution time and the owner's answer,
// and a released or expired commitment carries a resolution time.
FAC_TEST(durable, confirmed_commitment_round_trip) {
  TempDir dir("durable-confirmed");
  const Fixture fixture = make_fixture();
  Harness harness = make_durable_harness(fixture, dir.path(), true);

  const Decision decision = admit_fixture(*harness.engine, 1);
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK(decision.grant.has_value());
  const GrantId grant_id = decision.grant->grant_id;

  auto outcome = harness.engine->commit(grant_id);
  FAC_CHECK_OK(outcome);
  FAC_CHECK_EQ(outcome.value().state, CommitState::committed);
  FAC_CHECK(outcome.value().commitment_id.has_value());
  const CommitmentId commitment_id = outcome.value().commitment_id.value();

  const CommitmentRecord* commitment = harness.engine->ledger().find_commitment(commitment_id);
  FAC_CHECK(commitment != nullptr);
  FAC_CHECK_EQ(commitment->state, CommitmentState::provisional);
  FAC_CHECK(commitment->consumes());
  FAC_CHECK(!commitment->resolved_at.has_value());
  FAC_CHECK(!commitment->evidence.has_value());

  ReservationEvidence evidence;
  evidence.intent_id = commitment->intent.intent_id;
  evidence.reservation = ReservationId(0x1111222233334444ull, 0x5555666677778888ull);
  evidence.owner = fixture.policy.reservation_owner;
  evidence.outcome = ReservationOutcome::reserved;
  evidence.confirmed = commitment->demand;
  evidence.owner_generation = 7;
  evidence.owner_digest = fixture_digest("owner-confirmation");
  evidence.acknowledged_at = fixture.now;

  auto recorded = harness.engine->record_evidence(evidence);
  FAC_CHECK_OK(recorded);
  FAC_CHECK_EQ(recorded.value().state, CommitmentState::confirmed);
  FAC_CHECK(recorded.value().consumes());
  FAC_CHECK(recorded.value().resolved_at.has_value());
  FAC_CHECK(recorded.value().evidence.has_value());
  FAC_CHECK(recorded.value().evidence.value() == evidence);

  // The record encodes and decodes unchanged.
  codec::Writer writer;
  recorded.value().encode(writer);
  codec::Reader reader(writer.span());
  auto decoded = CommitmentRecord::decode(reader);
  FAC_CHECK_OK(decoded);
  FAC_CHECK(decoded.value() == recorded.value());

  // The live ledger state encodes and decodes unchanged.
  const Digest256 digest_before = harness.engine->ledger().state_digest();
  const std::uint64_t sequence_before = harness.engine->sequence().value();
  std::vector<std::byte> state;
  FAC_CHECK_OK(harness.engine->ledger().encode_state(state));
  auto ledger_decoded = Ledger::decode_state(std::span<const std::byte>(state.data(), state.size()));
  FAC_CHECK_OK(ledger_decoded);
  FAC_CHECK(ledger_decoded.value().state_digest() == digest_before);

  // And it survives compaction and a reopen.
  FAC_CHECK_OK(harness.engine->compact());
  harness.engine.reset();

  Harness reopened = make_durable_harness(fixture, dir.path(), false);
  FAC_CHECK_EQ(reopened.engine->sequence().value(), sequence_before);
  FAC_CHECK(reopened.engine->ledger().state_digest() == digest_before);
  FAC_CHECK(reopened.engine->recovery() != nullptr);
  FAC_CHECK(reopened.engine->recovery()->snapshot_loaded);
  FAC_CHECK_OK(reopened.engine->verify());
  const CommitmentRecord* reloaded = reopened.engine->ledger().find_commitment(commitment_id);
  FAC_CHECK(reloaded != nullptr);
  if (reloaded != nullptr) {
    FAC_CHECK_EQ(reloaded->state, CommitmentState::confirmed);
    FAC_CHECK(reloaded->consumes());
    FAC_CHECK(reloaded->resolved_at.has_value());
    FAC_CHECK(reloaded->evidence.has_value());
    FAC_CHECK(reloaded->evidence.value() == evidence);
  }
}

// The ledger state the engine holds must survive its own snapshot encoding,
// including an outstanding grant: an issued or committed grant has no
// resolution detail yet, and the codec has to carry that faithfully rather than
// refuse the snapshot.
FAC_TEST(durable, snapshot_grant_detail_round_trip) {
  TempDir dir("durable-grant-detail");
  const Fixture fixture = make_fixture();
  Harness harness = make_durable_harness(fixture, dir.path(), true);

  const Decision decision = admit_fixture(*harness.engine, 1);
  FAC_CHECK_EQ(decision.verdict, Verdict::allow);
  FAC_CHECK(decision.grant.has_value());
  const GrantId grant_id = decision.grant->grant_id;
  const DecisionEntry original = *harness.engine->ledger().find_decision(decision.request_id);
  const Digest256 live_digest = harness.engine->ledger().state_digest();

  // The smallest form: encode the live ledger state and decode it back.
  std::vector<std::byte> state;
  FAC_CHECK_OK(harness.engine->ledger().encode_state(state));
  auto decoded = Ledger::decode_state(std::span<const std::byte>(state.data(), state.size()));
  FAC_CHECK_OK(decoded);
  FAC_CHECK(decoded.value().state_digest() == live_digest);

  // The durable form: compaction publishes exactly that payload. One deferral
  // record is appended afterwards so the manifest rollback check stays quiet
  // and this proves the publish and reload path on its own.
  FAC_CHECK_OK(harness.engine->compact());
  const Fixture deferral = make_fixture(2);
  auto deferred = harness.engine->admit(deferral.request, EvidenceBundle{});
  FAC_CHECK_OK(deferred);
  FAC_CHECK_EQ(deferred.value().verdict, Verdict::defer);
  const std::uint64_t sequence_before = harness.engine->sequence().value();
  harness.engine.reset();

  Harness reopened = make_durable_harness(fixture, dir.path(), false);
  FAC_CHECK_EQ(reopened.engine->sequence().value(), sequence_before);
  FAC_CHECK(reopened.engine->recovery() != nullptr);
  FAC_CHECK(reopened.engine->recovery()->snapshot_loaded);
  const DecisionEntry* reloaded = reopened.engine->ledger().find_decision(decision.request_id);
  FAC_CHECK(reloaded != nullptr);
  if (reloaded != nullptr) {
    FAC_CHECK(*reloaded == original);
  }
  const GrantEntry* grant = reopened.engine->ledger().find_grant(grant_id);
  FAC_CHECK(grant != nullptr);
  if (grant != nullptr) {
    FAC_CHECK_EQ(grant->state, GrantState::issued);
    FAC_CHECK(grant->grant == decision.grant.value());
  }
}

}  // namespace

