# Facility Admission Control

The generation-bound facility admission runtime for the Summon Software Labs
Data Center Control Plane (DCCP), repository 44 of the canonical 72-runtime
DCCP.

Facility Admission Control answers one question:

> May this facility commitment be accepted now, given the capacity, protection,
> tenancy, obligations and policy that are authoritative at this moment â€” and
> if not, exactly which authority, generation or constraint says no?

It is a C++20 library with no third-party dependencies, a command line tool, a
durable admission ledger, and a test suite whose claims are backed by real
processes, real files and real installed artifacts.

---

## Systems boundary

**This repository owns**:

* the admission request and its immutable binding;
* deterministic evaluation of a proposed facility commitment;
* the explicit allow / refuse / defer verdict, with a fixed blocker precedence;
* the evidence set, the blocker set and the explanation behind a decision;
* generation-bound, content-bound grants and their fencing;
* the commitment lifecycle: provisional, confirmed, partial, released, expired;
* the bounded reservation intent emitted to the authority that owns reservation;
* idempotent request and commit replay;
* a durable, integrity-checked admission ledger with a single-writer lock.

**This repository explicitly does not own**:

* measuring capacity â€” Facility Capacity, Rack Capacity, Power Capacity,
  Cooling Capacity and Space Capacity measure and report it;
* creating capacity â€” nothing here provisions anything;
* workload scheduling â€” a commitment is not a placement;
* network path allocation â€” Network Admission Fabric and the path authorities
  own that;
* physical placement execution â€” Facility Placement Planner executes placements;
* the tenant registry, service class definitions, maintenance windows, incident
  state, placement policy or facility policy â€” each arrives as a
  generation-stamped snapshot from its owner;
* the reservation itself â€” this runtime emits a bounded intent to the owner of
  reservation and consumes the evidence that comes back;
* final resource-entitlement lifecycle â€” Resource Entitlement owns it, and this
  runtime never performs it.

The boundary is enforced in the type system rather than promised in prose: every
input is a snapshot struct carrying an owner-issued generation, every output that
would cause an external effect is a reservation intent record, and there is no
API here that measures, reserves, schedules or places anything.

## The three verdicts

| Verdict | Meaning |
| --- | --- |
| `allow` | Every required check passed against fresh, generation-identified evidence and the commitment fits under the stated policy. A generation-bound grant is issued. |
| `refuse` | A hard condition failed. Retrying the same request against the same authoritative state cannot change the answer. |
| `defer` | The runtime cannot conclude: evidence is missing, stale, superseded by a newer generation, in conflict with a pinned generation, or a temporal condition (active maintenance, an open incident) makes the answer depend on state that is still moving. |

"Defer" is how this runtime expresses "unknown". There is deliberately no fourth
verdict, because a caller that cannot tell "refused" from "cannot tell" retries
the wrong class of request.

## Core doctrine

These are not slogans; each one is a property the code and the tests enforce.

| Statement | How it is enforced |
| --- | --- |
| Observation is not authority | A capacity snapshot is an input, never a permission. Only a policy verdict of `permit`, combined with every other check, produces a grant. |
| Acknowledgement is not effect | A committed grant emits a reservation intent. The commitment stays provisional until the owner returns the defined evidence; nothing here reports a reservation as done. |
| Requested state is not observed state | The demand (requested) and the capacity (observed) are separate inputs and are compared explicitly per dimension. |
| Missing is never zero | An unmeasured dimension stays absent through evaluation, the decision record and the durable format. A request whose demand leaves a dimension unstated defers with `demand_unspecified`. |
| Unknown is never available | Unmeasured total, committed or reserved capacity defers with `capacity_unknown`; it never becomes headroom. |
| Missing protection is never protection | An unmeasured redundancy level or protected headroom defers with `redundancy_unknown`; it never becomes "no protection needed". |
| Stale authority is fenced, not inherited | Every evidence kind has its own freshness budget; evidence observed within tolerance is accepted, and evidence whose generation is older than one already accepted for that facility defers with `evidence_superseded`. |
| Recovered state is not fresh authority | Opening a store as a writer advances the control epoch and publishes it atomically. A grant recorded under an older epoch is fenced at commit with `control_epoch_superseded`. |
| Replay before staleness | Idempotent replay is resolved first: a request that was already decided returns the recorded decision byte for byte, even when every generation in it has since gone stale. |
| Every mutating decision binds what it used | A grant names each authority's generation and the SHA-256 of the exact evidence bytes, plus a binding digest over the request content, the epoch, the sequence, the window, the scope, the tenant and the demand. |
| Acknowledgement is not durable reservation without evidence | Only reservation evidence naming the intent, the owner, the owner's generation and digest, and the outcome moves a commitment out of provisional. Less than that is refused. |
| No overcommit without explicit permission | Overcommit requires the service class to tolerate it *and* a policy verdict that permits it, with an unbounded or per-dimension bounded allowance. The decision records the amount used. |
| Capacity at the boundary is accepted exactly once | `available == demand` is allowed with a recorded remaining of zero; one unit more is refused. Grants hold their demand, so two competing requests cannot both be admitted at the boundary. |

## Principal invariants

1. A decision's verdict always equals the verdict implied by its own blocker set,
   and the blockers are ordered by the documented precedence table.
2. A decision's digest is the SHA-256 of its own canonical encoding, and a
   decoded decision whose digest does not match is refused.
3. A grant's binding digest covers every field the grant depends on; a grant
   whose content was altered no longer matches its own digest.
4. The ledger sequence advances by exactly one per applied record, and replay
   applies the same records through the same code path.
5. Consumption is the snapshot's committed and reserved amounts plus every claim
   recorded at or after the snapshot's observation instant, and a claim counts
   from the moment it exists, not from the moment someone claims success.
6. A commitment in a consuming state (provisional, confirmed, partial) is
   counted against availability; released and expired commitments are not.
7. Recovery either reproduces exactly one authoritative state or fails closed.
   It never merges, guesses, or starts empty while durable state exists.

## Authority, generations and fencing

| Field | Owner | Invalidates |
| --- | --- | --- |
| `CapacityGeneration` | facility/rack/power/cooling/space capacity | every decision that used an older capacity snapshot for that facility |
| `RedundancyGeneration` | redundancy / failure-domain authority | the protection posture a grant was issued under |
| `TenantGeneration` | Tenant Registry | tenancy standing |
| `EnvelopeGeneration` | Resource Envelope | entitlement headroom |
| `ServiceClassGeneration` | Service Class Registry | the obligations a service class imposes |
| `MaintenanceGeneration` | Maintenance Policy / Coordinator | maintenance exposure of a commitment window |
| `IncidentGeneration` | incident/degraded-state authority | capacity trust and outage state |
| `PlacementGeneration` | Facility Placement Policy | allowed, forbidden and limited placement |
| `PolicyGeneration` and policy digest | Facility Policy Engine | whether the commitment is permitted at all |
| `ControlEpoch` | this runtime's ledger, advanced on every writer open | every grant issued by a previous process incarnation |
| `LedgerSequence` | this runtime's ledger | ordering of records and the identity of a commit point |

A change to any bound generation fences the grant: the commit re-checks the
generation *and* the content digest, and a mismatch is a recorded fence with the
specific cause. A change that merely adds evidence â€” a newer snapshot of an
authority the decision did not use â€” does not fence anything, because the grant
binds only what it used.

## The evidence model

Nine authorities are consumed, each as an optional, generation-stamped snapshot:
`capacity`, `redundancy`, `tenant`, `envelope`, `service_class`,
`maintenance`, `incident`, `placement`, `policy`. Every one of them can be
declared optional by configuration; anything required and absent defers with its
own blocker.

For each authority the evaluator checks, in this order: presence, internal
validity, scope (the tenant, envelope and service class must be the ones the
request names), observation time against the freshness budget, future skew,
pinned generation, and the generation watermark the ledger already holds. The
outcome is recorded as an evidence reference â€” accepted or rejected, with the
reason â€” and only accepted evidence is used afterwards.

Reading a requirement vector (a service class obligation, a policy allowance) is
different from reading a measured vector (capacity, headroom). In a
*requirement*, an absent dimension means the owner stated no requirement for it.
In a *measurement*, an absent dimension means unmeasured, and unmeasured never
becomes zero.

## Blocker precedence

Blockers are declared in precedence order, lower is more decisive, and the
primary blocker is the first one in that order â€” not the first one discovered.
Every applicable blocker is kept, so a refusal can name its cause and still show
what else was wrong.

Hard refusals: `request_invalid`, `idempotency_conflict`,
`control_epoch_superseded`, `evidence_scope_mismatch`, `tenant_not_active`,
`envelope_limit_exceeded`, `envelope_exhausted`, `policy_denied`,
`placement_forbidden`, `placement_limit_exceeded`,
`redundancy_level_insufficient`, `maintenance_outage`, `incident_major`,
`capacity_exhausted`, `protected_headroom_insufficient`,
`overcommit_not_permitted`, `overcommit_limit_exceeded`, `horizon_exceeded`,
`request_expired`, `grant_not_found`, `grant_expired`, `grant_fenced`,
`grant_state_conflict`.

Deferrals: `commitment_window_unspecified`, `demand_unspecified`,
`capacity_missing`, `capacity_stale`, `capacity_unknown`,
`redundancy_missing`, `redundancy_stale`, `redundancy_unknown`,
`tenant_missing`, `tenant_stale`, `tenant_unknown`, `envelope_missing`,
`envelope_stale`, `envelope_unknown`, `service_class_missing`,
`service_class_stale`, `service_class_unknown`, `maintenance_missing`,
`maintenance_stale`, `maintenance_unknown`, `maintenance_state_unknown`,
`maintenance_active`, `maintenance_exposure`, `incident_missing`,
`incident_stale`, `incident_unknown`, `incident_state_unknown`,
`incident_degraded`, `placement_missing`, `placement_stale`,
`placement_unknown`, `policy_missing`, `policy_stale`, `policy_unknown`,
`evidence_future_dated`, `evidence_superseded`, `generation_mismatch`.

## Lifecycle

A **grant** is issued by an allowing decision and is usable until it expires, is
fenced, or is released. While it is issued it holds its demand against the ledger
(unless the policy turns holds off), so a competing request at the boundary is
refused rather than both being admitted.

A **commitment** is created when a grant is executed. It moves through exactly
these states:

| State | Meaning | Consumes capacity |
| --- | --- | --- |
| `provisional` | The reservation intent was emitted; the owner has not answered. | yes |
| `confirmed` | The owner returned evidence for a full reservation. | yes |
| `partial` | The owner confirmed less than was asked for. | yes, the whole demand |
| `released` | An explicit release, or owner evidence that rejected the intent. | no |
| `expired` | The intent's validity window passed without evidence. | no |

A partial acknowledgement keeps the whole demand counted: an owner that confirmed
less than was asked for has not returned the difference, and this runtime will
not invent it.

## Persistence and recovery

A store is a directory:

    writer.lock     OS single-writer exclusion
    manifest.0      manifest slot A
    manifest.1      manifest slot B
    journal.facj    append-only frames
    snapshot.facs   compacted state (written by compaction)

Every frame is a 20-byte header (magic, kind, flags, reserved field, sequence,
payload length), the payload, and a CRC-32C over everything before it. The
manifest is a fixed 128-byte record carrying the format revision, a monotonically
increasing manifest generation, the control epoch, the committed ledger sequence,
the committed journal length, the SHA-256 of that exact journal byte range, and
the snapshot sequence and digest when a snapshot exists. Every integer is
big-endian and every reserved field must be zero.

**The atomic commit point is the publication of a manifest slot.** A frame that
was appended and flushed but is not covered by a manifest is not committed: on
the next open those bytes are an uncommitted tail, are discarded, and are
reported in the recovery report's discarded-tail counter. A manifest that names
more journal bytes than the journal holds fails the open, because the runtime
refuses to guess whether the missing bytes were ever written.

Recovery, in order: take the lock; read both manifest slots; refuse if neither
verifies while journal or snapshot data exists; take the slot with the higher
manifest generation, and fail closed with `rollback_detected` if the other slot
claims a newer committed sequence; open the journal at the committed length and
discard the tail; recompute the SHA-256 of the committed region and compare it
with the manifest; read and validate every frame (magic, flags, reserved field,
CRC, consecutive sequences); decode the snapshot and verify it against the
manifest digest; replay the frames through the same ledger apply path the live
process uses; and require the replayed sequence to equal the sequence the
manifest commits. Any failure is an error, never a silent repair.

Compaction writes a snapshot, publishes the manifest that names it, and only then
resets the journal. Before the manifest publication the journal is authoritative;
after it the snapshot is. A crash in between leaves journal bytes past the
committed length, which the next writer discards.

Rollback protection: two manifest slots with monotonic generations detect a
reverted or torn slot, and the control epoch advance on every writer open means a
restored older state cannot resurrect live authority â€” grants from the older
epoch are fenced. Rolling back an entire store directory to an earlier consistent
state cannot be detected without an external monotonic anchor, and that
limitation is stated rather than hidden.

## Concurrency model

One non-recursive mutex per engine, taken once per public call. There are no
callbacks, no nested locks, and no lock order to get wrong inside the process;
every lock acquisition is the first and only one on that path. Journal and
manifest I/O happen under that lock because single-writer ordering is the
guarantee, and blocking I/O under a lock is the price of that guarantee rather
than an oversight.

Across processes the store is protected by the operating system: on Windows the
lock file is opened with a share mode of zero, so no other process can open it at
all, and on POSIX an exclusive `flock` is taken. The kernel releases the lock
when the owning process exits, including when it is killed, so a crashed writer
never bricks a store. A reader takes a share mode that coexists with other
readers and not with a writer, so read-only inspection never observes a store
that is being mutated.

A grant is authority for one control epoch. That is deliberate: a restart
advances the epoch and fences the previous incarnation's grants, so recovered
state is never inherited as live authority.

## Command line

    facctl <command> [options]

| Command | Purpose |
| --- | --- |
| `init --store DIR` | create an empty durable store |
| `session --store DIR SCRIPT` | run a script of commands in one process (one control epoch) |
| `admit --scenario FILE [--store DIR]` | evaluate an admission request |
| `commit --grant ID [--scenario FILE]` | execute a grant and emit its reservation intent |
| `ack --intent ID --reservation ID --owner ID --outcome OUTCOME --generation N --digest HEX [--confirmed d=v,...]` | record the reservation owner's evidence |
| `release --commitment ID --reason TEXT` | release a commitment explicitly |
| `release-grant --grant ID --reason TEXT` | release an uncommitted grant |
| `fence --grant ID --code CODE --reason TEXT` | fence a grant explicitly |
| `show (--request ID \| --grant ID \| --commitment ID)` | render a recorded record |
| `list decisions\|grants\|commitments\|intents` | list records in identity order |
| `verify --store DIR` | re-verify the durable store from disk |
| `compact --store DIR` | compact the durable history |
| `stats --store DIR` | report ledger and recovery counters |
| `epoch --store DIR` | report the current control epoch |
| `version` | report the library version and durable format revision |

Options: `--in-memory` (volatile engine), `--create`, `--now NANOS`
(evaluation time as Unix nanoseconds), `--owner ID` (the reservation owner
intents are addressed to, and the authority whose evidence is accepted),
`--grant-validity SEC`, `--intent-validity SEC`, `--no-holds`.

Exit codes: **0** allow or committed, **1** refuse, defer or fenced, **2** usage
or environment error, **3** the operation could not be completed.

Because a new process incarnation advances the control epoch, a lifecycle that
spans admit, commit and acknowledge must run in one process. That is what
`session` is for, and it is why the CLI cannot commit a grant issued by a
previous invocation:

    facctl session --store ./store --create --now 1700000000000000000 script.txt

    # script.txt
    admit --scenario admit.txt
    commit --grant @grant
    ack --intent @intent --reservation 12121212-1212-1212-1212-121212121212 \
        --outcome reserved --generation 1 --confirmed power=200,cooling=100,space=1,slots=1 \
        --digest 0011223344556677889900112233445566778899001122334455667788990011
    show --commitment @commitment
    verify
    stats

`@grant`, `@commitment`, `@intent` and `@request` refer to the identity
produced by an earlier command in the same session.

### Scenario files

A scenario is line oriented, whitespace separated, with `#` starting a comment.
Identities are written in the canonical 8-4-4-4-12 form. Amounts can be written
`power=200` or `power 200`, and `unknown` leaves a dimension unmeasured.

    request <id>
    tenant <id>
    service-class <id>
    envelope <id>
    scope facility <id> [rack <id>] [zone <id>]
    demand power <n> cooling <n> space <n> slots <n>
    requested-at <unix-nanos>
    window <start-nanos> <end-nanos>
    epoch <n>
    pin <kind> <generation>

    capacity           generation <n> observed <nanos> total <amounts> committed <amounts> reserved <amounts>
    redundancy         generation <n> observed <nanos> level <n|n_plus_one|n_plus_two|two_n> headroom <amounts>
    tenant-evidence    generation <n> observed <nanos> status <active|suspended|closed|unknown>
    envelope-evidence  generation <n> observed <nanos> limit <amounts> consumed <amounts>
    service-class-evidence generation <n> observed <nanos> [min-redundancy <level>] [headroom <amounts>] [clearance yes|no] [overcommit yes|no]
    maintenance        generation <n> observed <nanos>
    maintenance-window id <id> facility <id> [rack <id>] [zone <id>] start <n> end <n> state <s> impact <i>
    incident           generation <n> observed <nanos> facility <id> [rack <id>] state <s> trust <t> [id <id>]
    placement          generation <n> observed <nanos> [allow-rack <id>]... [forbid-rack <id>]... [required-zone <id>] [max-units-per-rack <n>] [max-units-per-facility <n>]
    policy             generation <n> observed <nanos> verdict <permit|deny|abstain> overcommit <none|unbounded> | allowance <amounts> digest <hex> id <id>

Responses are canonical text with a stable key order, so a test or an operator
can compare them byte for byte:

    verdict allow
    grant dfd62277-db9a-2859-8570-d1adc9f94f43
    assessment power total=1000 committed=0 reserved=0 available=1000 demand=200 remaining=800 protected=100 overcommit=0
    blocker capacity_exhausted[power]: available power is 0 W against a demand of 200
    evidence capacity generation 1 accepted observed 2023-11-14T22:13:20.000000000Z digest <hex>
    decision-digest <hex>

## Library integration

    #include <fac/fac.hpp>

    fac::AdmissionPolicy policy;
    policy.reservation_owner = owner_id;            // the authority intents go to
    policy.grant_holds_capacity = true;
    policy.freshness.capacity = fac::Duration::from_seconds(60);

    auto clock = std::make_shared<fac::FixedClock>(now);   // or fac::SystemClock
    auto engine = fac::Engine::open_durable("./store", policy, clock, /*create=*/true);

    auto decision = engine.value()->admit(request, evidence);      // evaluates
    if (decision.ok() && decision.value().verdict == fac::Verdict::allow) {
      auto outcome = engine.value()->commit(decision.value().grant->grant_id);
      // outcome.value().commitment_id names a provisional commitment and the
      // reservation intent that was emitted; nothing external has happened yet.
    }

The engine owns its clock through a shared pointer, takes no callbacks, and
returns errors as values. The ledger exposes the authoritative records for
inspection, and the engine can re-validate the durable store from disk.

## Build, install and consume

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release --config Release
    ctest --test-dir build-release -C Release --output-on-failure
    cmake --install build-release --config Release --prefix /some/prefix

A downstream project needs only the installed prefix:

    find_package(FAC 1.0 CONFIG REQUIRED)
    target_link_libraries(my_target PRIVATE FAC::facility_admission_control)

The exported package is `FAC`, the imported target is
`FAC::facility_admission_control`, and `FAC_VERSION` carries the package
version. Options: `FAC_BUILD_TESTS`, `FAC_BUILD_TOOLS`, `FAC_BUILD_EXAMPLES`,
`FAC_BUILD_BENCHMARKS`, `FAC_WARNINGS_AS_ERRORS`, `FAC_ENABLE_ASAN`.

## Validation performed

Host: one Windows 11 x64 machine, Visual Studio 2022 Build Tools, MSVC
19.44.35207, CMake 4.3.2, no other compiler installed. All numbers below were
produced on this host and describe this host.

| Configuration | Build | Tests |
| --- | --- | --- |
| Release (`/W4 /WX /permissive-`) | clean, no warnings | 209 tests in 15 suites, 0 failures |
| Debug (`/W4 /WX /permissive-`) | clean, no warnings | 209 tests in 15 suites, 0 failures |
| Release with `/fsanitize=address` | clean | 209 tests in 15 suites, 0 failures, no sanitizer report |

The suite is one registered test binary with suites `core`, `codec`, `model`,
`adversarial`, `verdicts`, `boundary`, `staleness`, `idempotency`,
`fencing`, `ledger_replay`, `durable`, `multiprocess`, `multiprocess_crash`,
`concurrency` and `property`. `ctest` runs it as `fac.tests`, runs the crash
consistency suite again as `fac.crash_recovery`, and runs the installed-artifact
consumer check as `fac.installed_consumer`; all three pass in both
configurations.

What the suites actually prove:

* **core** â€” SHA-256 against published NIST vectors (empty, "abc", block
  boundaries, one million 'a'), CRC-32C known answers, checked arithmetic at the
  exact limits, UTF-8 validation including overlong forms, identity and digest
  parsing refusals, timestamp bounds and formatting.
* **codec** â€” round trips of every primitive, canonical refusals (non-zero
  reserved fields, impossible booleans, lengths that exceed the input, trailing
  bytes, zero digests), a truncation sweep that decodes every strict prefix, and
  a byte-flip sweep that never crashes.
* **model** â€” encode/decode round trips for every domain type, validation
  refusals at the persistence boundary, and explicit-unknown semantics.
* **adversarial** â€” absurd declared sizes, impossible enum values, hostile text,
  boundary integers, duplicate identities and store paths that are empty, files
  rather than directories, or too long for the platform.
* **verdicts, boundary, staleness, idempotency, fencing** â€” one authoritative
  input changed per test, with the expected verdict, primary blocker and
  secondary blockers asserted; exact capacity boundaries; every freshness and
  supersession rule; replay resolved before staleness; and each fence cause.
* **ledger_replay** â€” replay through the same apply path, out-of-order and
  duplicate record refusal, compaction and reopen digest equality.
* **durable** â€” real store files: uncommitted tail discarded with an exact byte
  count, a five-case corruption sweep asserting exact error codes through both
  the writer and the reader, forged-manifest rollback detection, compaction with
  a loaded snapshot, recovery from the surviving manifest slot, and a read-only
  open that leaves every file's size and SHA-256 unchanged.
* **multiprocess** â€” real independent processes: lock refusal
  (`writer_lock_held`), kernel lock release after an abrupt `TerminateProcess`
  of the holder, a child killed mid-write whose store the parent then reopens
  with a reproducible state digest, and a writer-versus-contender race.
* **concurrency** â€” eight threads admitting concurrently with the ledger
  sequence advancing exactly once per request, forty rounds of two threads at an
  exact capacity boundary (always exactly one allow and one `capacity_exhausted`),
  and a reader thread inspecting while a writer admits.
* **property** â€” seeded randomized state machines (seeds printed) with the
  invariants of this document checked after every action, plus replay and
  compaction determinism.

Packaging and downstream proof:

    cmake --install build-release --config Release --prefix <prefix>
    cmake -S tests/consumer -B <consumer-build> -DCMAKE_PREFIX_PATH=<prefix>
    cmake --build <consumer-build> --config Release
    <consumer-build>/Release/fac_consumer

This runs automatically as the `fac.installed_consumer` test. The consumer is a
separate CMake project that uses `find_package(FAC 1.0 CONFIG REQUIRED)` against
the installed prefix only, links `FAC::facility_admission_control`, and then
exercises real behaviour: it checks the installed version against the installed
headers, evaluates an admission request, commits the grant, prints the emitted
reservation intent and the ledger state digest, and ends with `consumer-ok`.

## Hardening defects found and fixed

The suite found real defects. Each one is listed with what it was and what it
would have done in production; every fix is covered by a test that fails against
the old behaviour.

| Defect | Consequence | Fix |
| --- | --- | --- |
| The per-dimension capacity assessment was pre-populated and then refused its own entries as duplicates | Decisions reported `unknown` for every dimension while the verdict was computed from the real numbers, so an explanation could not be trusted | Each dimension is added exactly once, and the fallback path adds demand-only entries when no capacity evidence was usable |
| A claim recorded at the snapshot's own observation instant was treated as already included in the snapshot | Two admissions could both be accepted against capacity that only one of them fitted, whenever timestamps coincided | Claims recorded at or after the observation instant are counted; over-counting can only refuse capacity, never grant it |
| A grant's own hold counted as erosion of its own headroom | Any grant with a demand greater than half the free capacity fenced itself at commit and could never be executed | The committing grant excludes itself from the erosion query |
| The manifest rollback rule compared journal bytes across slots | Compaction shrank the journal, so a compacted store could never be reopened or verified again â€” the store was bricked | The rule compares the committed ledger sequence, which compaction preserves |
| An issued grant could not be decoded from a snapshot | A snapshot containing an outstanding grant failed recovery with `state_unverified`, so compaction was unusable | An empty resolution detail is valid; a non-empty one must still be usable text |
| The commitment lifecycle disagreed with itself: an answered commitment was refused by its own decoder | Every snapshot containing a confirmed or partial commitment failed to decode | The lifecycle is stated once: provisional has no answer, confirmed and partial have an answer and still consume, released and expired are closed |
| Future-dated evidence inside the skew allowance was still rejected | A facility whose clock ran a few seconds ahead could not be admitted at all | Evidence at or slightly ahead of the clock is treated as age zero rather than refused |
| `is_valid_utf8` accepted overlong two-byte sequences (`0xC1 0xBF`) | Hostile or corrupted text could pass an identifier or reason check | Two-byte sequences must encode at least U+0080, as the three- and four-byte branches already required |
| `narrow<uint64_t>` refused every signed source | A caller could not convert a signed value that fits | Bounds are compared in the common type of source and target |
| `EvidenceSet::decode` refused its own canonical encoding | A standalone evidence set could never be decoded, and a decision with enough evidence references failed to replay | The decoder's minimum element size matches the real encoding |
| `MaintenanceSnapshot::decode` refused its own canonical encoding | Any maintenance snapshot with windows could not be read back | Same class of fix, with the window's real size |
| `fac::sha256_hex` was declared and never defined | A consumer that called it failed to link | Defined in the library and covered by the core suite |
| The manifest was published before the in-memory sequence advanced | A store whose first record was written described a journal with a sequence the manifest did not name, and the next open refused it | The sequence advances before the manifest is published |

## Benchmarks

The benchmark measures completed operations only. The timer wraps one whole
synchronous `admit` call and stops when it returns, so no enqueue, submission or
queueing figure exists or is reported. Both sections print their own provenance
and neither claims a speedup; the two sections measure different code paths.

* Provenance: **SYNTHETIC** workload data (generated requests against fixed
  authoritative snapshots) on the **REAL** code path. Single host, one process,
  one thread, `std::chrono::steady_clock`, Release build with optimizations on.
  No physical hardware behaviour is claimed or measured.
* The durable section includes the real durable path: record encoding, journal
  append with flush, and the atomic manifest publication, plus a read-back
  verification of every committed frame.

Measured on this host:

| Section | Operations | Throughput | Mean | p50 | Min / max |
| --- | --- | --- | --- | --- | --- |
| VOLATILE (in-memory evaluation, decision, ledger) | 2000 completed (100 warm-up) | 10762.9 ops/s | 92.7 us | 84.9 us | 59.2 / 204.3 us |
| DURABLE (append, flush, manifest publication) | 200 completed (10 warm-up) | 254.7 ops/s | 3925.3 us | 3791.3 us | 2975.3 / 7478.5 us |

The durable figure is dominated by the durability guarantee itself: each
admission publishes a manifest, which is two device flushes and an atomic
replacement, so a completed durable admission costs about 3.9 ms on this host.
That is the price of the commit point, not an implementation accident, and it is
reported as measured rather than adjusted.

## Platform and hardware validation status

Validated: Windows 11 x64 with MSVC 19.44 in Release, Debug and AddressSanitizer
configurations, on one host, with a local filesystem.

Not validated, and stated rather than implied:

* the POSIX code paths (`flock`, `fsync`, `pread`/`pwrite`) are implemented and
  guarded but were never compiled or executed on this host, because no POSIX
  toolchain is installed here;
* no physical facility hardware, capacity controller, power controller or
  cooling controller was involved: every snapshot in the suite is synthetic data
  on the real code path;
* no network filesystem, no multi-host shared store, and no store larger than
  the bounded limits in `fac/core/limits.hpp` was exercised;
* rolling back an entire store directory to an earlier consistent state cannot
  be detected without an external monotonic anchor, as described above.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
