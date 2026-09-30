// Facility Admission Control - the admission engine.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The engine is the whole public runtime: it evaluates a request against
// generation-stamped evidence, records the decision durably, issues a
// generation-bound grant when the verdict is Allow, executes a grant by
// emitting a bounded reservation intent, and records the evidence that comes
// back from the authority that owns the reservation.
//
// Concurrency model: one non-recursive mutex, taken once per public call and
// never held across a call into anything the caller supplies. There are no
// callbacks and no nested locks, so there is no lock order to get wrong inside
// the process. Across processes, the durable store is protected by the OS
// writer lock, which the kernel releases when the owning process dies.

#ifndef FAC_ENGINE_ENGINE_HPP
#define FAC_ENGINE_ENGINE_HPP

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "fac/core/error.hpp"
#include "fac/core/time.hpp"
#include "fac/durable/store.hpp"
#include "fac/engine/policy.hpp"
#include "fac/ledger/ledger.hpp"
#include "fac/model/capacity.hpp"
#include "fac/model/decision.hpp"
#include "fac/model/facility_state.hpp"
#include "fac/model/request.hpp"
#include "fac/model/reservation.hpp"
#include "fac/model/tenancy.hpp"

namespace fac {

// The authoritative snapshots an evaluation is allowed to use. An option that
// is not set is missing evidence, which is a deferral, never an assumption.
struct EvidenceBundle {
  std::optional<CapacitySnapshot> capacity;
  std::optional<RedundancySnapshot> redundancy;
  std::optional<TenantSnapshot> tenant;
  std::optional<EnvelopeSnapshot> envelope;
  std::optional<ServiceClassSnapshot> service_class;
  std::optional<MaintenanceSnapshot> maintenance;
  std::optional<IncidentSnapshot> incident;
  std::optional<PlacementPolicySnapshot> placement;
  std::optional<PolicySnapshot> policy;

  [[nodiscard]] bool has(EvidenceKind kind) const;
};

class Engine {
 public:
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine();

  // The engine owns its clock through a shared pointer, so a caller cannot
  // leave it holding a reference to a clock that has already been destroyed.
  using ClockHandle = std::shared_ptr<const Clock>;

  // A volatile engine: decisions are still deterministic, but nothing is
  // durable and no authority survives the process.
  [[nodiscard]] static Result<std::unique_ptr<Engine>> open_in_memory(const AdmissionPolicy& policy,
                                                                     ClockHandle clock);

  // A durable engine. With create, an empty store is created when the directory
  // holds nothing; without it, a missing store is an error rather than a new
  // empty one.
  [[nodiscard]] static Result<std::unique_ptr<Engine>> open_durable(
      const std::string& directory, const AdmissionPolicy& policy, ClockHandle clock, bool create);

  // Inspection only. The ledger is recovered and verified, and every mutating
  // call is refused with read_only_store.
  [[nodiscard]] static Result<std::unique_ptr<Engine>> open_reader(const std::string& directory,
                                                                  const AdmissionPolicy& policy,
                                                                  ClockHandle clock);

  // Evaluates a request. A replay of a request that was already decided returns
  // the recorded decision, byte for byte, before any staleness check runs.
  [[nodiscard]] Result<Decision> admit(const AdmissionRequest& request,
                                       const EvidenceBundle& evidence);

  // Executes a grant: re-verifies every binding, re-checks that its capacity is
  // still there, records a commitment and emits the reservation intent. If the
  // same grant is executed twice, the recorded outcome is returned.
  //
  // Passing the current evidence re-verifies the grant's bindings against it: a
  // generation that moved or content that changed under the same generation
  // fences the grant instead of committing it. Omitting it re-checks only the
  // headroom this ledger itself consumed.
  [[nodiscard]] Result<CommitOutcome> commit(GrantId grant_id, const EvidenceBundle* evidence = nullptr);

  // Records the defined evidence from the reservation owner. Less than the full
  // evidence is refused rather than recorded.
  [[nodiscard]] Result<CommitmentRecord> record_evidence(const ReservationEvidence& evidence);

  // Explicit release of a commitment or of an uncommitted grant. Both are
  // recorded with a reason: capacity is never returned silently.
  [[nodiscard]] Result<CommitmentRecord> release_commitment(CommitmentId commitment_id,
                                                            std::string reason);
  [[nodiscard]] Result<GrantEntry> release_grant(GrantId grant_id, std::string reason);

  // Fences a grant explicitly, recording which condition invalidated it.
  [[nodiscard]] Result<GrantEntry> fence_grant(GrantId grant_id, BlockerCode code,
                                               std::string reason);

  // Compacts the durable history into a snapshot.
  [[nodiscard]] Status compact();

  // Re-verifies the durable store from disk.
  [[nodiscard]] Status verify();

  [[nodiscard]] ControlEpoch epoch() const;
  [[nodiscard]] LedgerSequence sequence() const;

  // The live ledger. This accessor deliberately does not take the engine mutex,
  // so it is for a single-threaded caller: reading it while another thread is
  // inside a mutating call would race. Use ledger_snapshot() when a caller needs
  // to traverse the records while other threads keep working.
  [[nodiscard]] const Ledger& ledger() const;

  // A copy of the authoritative records, taken under the engine mutex. Bounded
  // by the ledger's own record limits.
  [[nodiscard]] Ledger ledger_snapshot() const;
  [[nodiscard]] const AdmissionPolicy& policy() const;
  [[nodiscard]] bool read_only() const;
  [[nodiscard]] bool durable() const;
  [[nodiscard]] const durable::RecoveryReport* recovery() const;

 private:
  Engine() = default;

  [[nodiscard]] Status initialize(const std::string& directory, const AdmissionPolicy& policy,
                                  ClockHandle clock, bool read_only, bool create);
  [[nodiscard]] Status apply_record(durable::RecordKind kind, std::span<const std::byte> payload);
  [[nodiscard]] Status sweep_expired_holds(Timestamp now);
  [[nodiscard]] Status require_writable() const;

  AdmissionPolicy policy_;
  ClockHandle clock_;
  mutable std::mutex mutex_;
  Ledger ledger_;
  std::unique_ptr<durable::Store> store_;
  std::optional<durable::RecoveryReport> recovery_;
  ControlEpoch epoch_;
};

}  // namespace fac

#endif  // FAC_ENGINE_ENGINE_HPP
