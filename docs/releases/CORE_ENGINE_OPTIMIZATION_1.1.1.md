# PacificDB Core Engine Optimization — v1.1.1

## 1. Executive Summary

This release preserves the public Community architecture and qualifies
the retained database-first package/UI changes against the exact public 1.0.0
engine. The newly implemented performance change is in the **Node SDK pool**:
a completed request now releases its connection slot before the caller submits
its next burst. Request and connection-promise ownership guards preserve error,
timeout and reconnect behavior. No fsync, Raft quorum, commit/apply ordering,
storage-format or authorization rule was weakened.

Local qualification and stable publication were separate decisions during the
candidate phase. This report uses only PacificDB v1.0.0 and v1.1.1 evidence and
does not make a cross-product performance claim.

## 2. Previous Architecture / Behavior

The public engine accepts newline-delimited JSON over TCP/TLS. Connection workers
perform framing/admission and the command layer dispatches into engine/storage
and Raft. RF1 and RF3 durable commit/apply paths already existed in 1.0.0.
The Node pool owns at most its configured number of sockets, each with one active
request and a FIFO waiting queue. Previously pool release was delayed after a
response, and a global pending-release condition queued new callers rather than
creating additional slots. A serial warmup followed by a concurrent burst could
therefore remain serialized on its one warm socket.

Older peers may close a connection after replying. The candidate only reuses a
socket after explicit `_pacificdb_connection_keepalive:true`. An unknown legacy
peer is retired; timing a delayed FIN is not a safe keepalive contract.

## 3. Problems Identified

1. A real peer test with pool size four observed only one simultaneous request
   after warmup. The failure was deterministic and did not depend on database
   benchmark noise.
2. Immediate handoff initially exposed stale asynchronous connect/write callbacks
   that could reject a later queued request. A real refused-connection test failed
   before request-identity guards.
3. A timeout during TLS establishment left a pending promise tied to a destroyed
   socket; the next request inherited its failure. A controlled handshake signal
   on real sockets reproduced this before the reconnect fix.
4. A source-archive container build discarded the supplied commit and embedded
   `unknown`, despite a valid image revision label. CMake discovery now respects
   explicitly supplied metadata; container smoke checks the engine against labels.
5. Release validation runners had an omitted URL-fixture argument and historical
   hardcoded version expectations. These were corrected; their initial failed
   attempts are retained rather than counted as passes.
6. The batch-10 regression persisted in every matched round. A separate engine-only
   concurrency diagnostic now attributes it to the collection lock spanning the
   durable WAL wait, rather than to the corrected Node connection pool.

## 4. Baseline Measurements

The baseline is the official GitHub Debian 1.0.0 artifact, verified against its
published checksum and release manifest. Its SHA-256 is
`04d8893cd05a8401c19b484e9a2b39ad62c757e872798addd46b435216ca0301`.
The peeled source commit is `2f14213ff46721e6219ed241e0f842edab795cd1`;
`4baec69124edb32a1ba1a3f1eaf1aed5bebaa52c` is the annotated tag object,
not a conflicting source commit. The matching npm 1.0.0 tarball SHA-256 is
`c02b4d2ad3be35f712d7300ad786e1c90e2c80c543ea53e4270ac8d78ea38c20`.

An initial local comparison discovered the pool defect and was explicitly
aborted. Its raw completed trials remain in `standalone/` with
`series-status.json = ABORTED_DIAGNOSTIC`. One cancelled phase overlapped a unit
regression test. Those measurements are diagnostic, not final headline results.
The fresh completed series belongs in `standalone-final/` and `rf3-final/`.

## 5. Bottlenecks

| Subsystem | Evidence | Root cause | Impact/scope |
|---|---|---|---|
| Node pool dispatch | Warmed-burst RED peak 1 versus configured 4, GREEN peak 4 | Delayed slot return plus global release gate | Serializes bursts; SDK-specific, not an engine CPU share |
| Failed connection ownership | Refused-connect and handshake RED/GREEN | Old promise/callback survives immediate handoff | Spurious queued-request failures; correctness blocker until fixed |
| Small-batch write contention | 12 engine-only trials: at concurrency 8, v1.1.1 median mean request lock wait is 25.992 ms versus 0.480 ms in v1.0.0; candidate throughput stays near concurrency 1 | Source comparison shows v1.1.1 holds the collection mutex across `WAL::logPutBatch` completion so concurrent requests cannot enter the WAL group together; the guard keeps WAL and memtable application order aligned | Major explained regression for short concurrent batches; retained as a correctness/performance trade-off |
| Low-cardinality index rebuild | Audited old/new source plus dedicated rebuild benchmark | Prior repeated linear array membership scans | Hash dedup retains insertion order; transient memory trade-off |
| Durable write path | Durable configuration and client latency counters | Sync/ordered apply/replication remain required | Per-stage causal contribution not measured in these runs |
| Vector search | Audited search source and exact vector checks | Visibility scan/signature/rebuild work | No constant-time or unbounded-scale search claim |
| Shared-host scheduling | Full repeated ranges/resource counters | Laptop shares CPU/disk with other services | Exact shares not established; do not remove slow trials |

A source suspicion is not a measured bottleneck. Per-stage CPU/flamegraph,
allocation counts, hardware IOPS and network attribution are not provided by the
`/proc` snapshots. The tables distinguish measured end-to-end work from audited
implementation costs.

## 6. Optimization Methods

### Node pool release and reconnect ownership

**Problem:** concurrent callers remained queued on one warm socket.
**Previous implementation:** release timer and global pending-release gate.
**New implementation:** return the bounded slot before resolving/rejecting the
external job; dispatch the FIFO waiter immediately if present.
**Why it can be faster:** ready callers can use additional existing/allowed slots
without waiting for an artificial release delay.
**Files:** `sdk/node/src/index.js`, `sdk/node/test/client.test.js`.
**Correctness:** callbacks act only on their captured request; connection promise
cleanup acts only on its own generation; a destroyed pending socket is replaced.
Legacy peers remain conservatively retired. Writes are never automatically
retried, and closing a pool refuses queued calls.
**Measured impact:** deterministic utilization 1→4 in the focused test; actual
release-to-release throughput/latency/resource measurements are reported below.
**Cost:** up to the configured pool can now be active, increasing concurrent
server work/sockets compared with accidental serialization. No new coordinator
or dependency. Reversible at source level, but reverting also restores the defect.

### Retained low-cardinality index deduplication

**Problem/previous implementation:** duplicate row IDs in one secondary-index
value were checked by repeated linear searches through its array.
**New implementation:** per-value hash membership tracking while retaining output
array order. The public branch's fix was preserved during snapshot import.
**Why it can be faster:** avoids quadratic membership comparisons.
**Files/components:** `engine/src/lsm.cpp`, secondary-index rebuild.
**Correctness:** checked index/recovery paths remain intact; this is not a new
index format. **Measured impact:** dedicated rebuild rows only; a whole-release
comparison cannot isolate this change from other retained fixes.
**Cost/reversibility:** temporary hash memory; source revert is format-neutral.

### Retained durability and media improvements

Retained engine changes compared with 1.0.0 include holding the collection lock
across batch WAL/memtable ordering, restart-safe monotonic versions/tombstones,
recovery of provably uncommitted torn Raft tails and deterministic chunk transfer
with compact progress/paged orphan handling. These are correctness and bounded
work changes. They are not all attributed independent speedups. Committed
corruption still fails closed. Media final verification preserves complete hashes
and remains O(number of chunks); it is not a constant-time upload protocol.

### Immutable build identity

CMake uses Git discovery only when an explicit commit was not supplied. A real
archive-configuration test checks explicit revision retention and truthful
`unknown` when no metadata exists. Container smoke compares embedded version and
revision with OCI labels. This fixes artifact traceability, not database speed.
The first archive-test attempt lacked the required license fixture and stopped
before the target assertion; the corrected RED run actually asserted the defect.

## 7. Request Path

Direct client → bounded connection pool → TCP/TLS framing/admission → authenticated
command/query layer → engine/LSM and indexes → durable Raft log/commit where used
→ quorum for RF3 → ordered storage apply and visibility → JSON response → pool
release. Community's local Workbench HTTP bridge maps UI calls into the SDK; it
is not a production REST gateway. The browser bridge checks local host/origin and
session controls. Hosted distributed orchestration is outside this branch.

RF1 committed Raft application with `writeWal=false` already existed in stable;
it is not the unreleased one-sync prototype. WAL, MemTables, SSTables, compaction,
visibility rules and replica digest checks retain the audited Community paths.
No stage latency percentages are obtained by adding overlapping samples.

## 8. Memory Optimization

No speculative allocator/cache rewrite was introduced. The pool no longer keeps
per-response release timers. Bounded client pools and SDK response limits remain.
Media streams one chunk at a time with checksum validation; manifests/progress
are still proportional to relevant chunk metadata. Hash dedup adds temporary
index memory. Peak RSS is measured, with setup and process-lifetime high-water
limitations explicitly described. Allocation behavior: **not measured**.

## 9. Network Optimization

The SDK scheduling fix improves use of existing bounded persistent sockets.
There is no new framing, compression or automatic retry. SDKs retain explicit
legacy keepalive detection and TCP/TLS verification. Sequential requests can
reuse a single slot; concurrent bursts can use the configured pool. URL/parser
and scope-snapshot contracts were tested across Node/Python/Java/native CLI.
Any measured full-stack gain includes both engine and version-matched client.

## 10. Storage Optimization

Batch lock ordering, monotonic versions, index dedup and torn-tail handling are
retained source changes relative to stable. Storage format stays 2. WAL,
manifest/SSTable checksums and checkpoint transitions are covered by existing
crash-boundary tests. No synchronous durability setting was disabled for timing.
There is no new compaction policy or unchecked filesystem path cache. Detailed
compaction activity and hardware sync latency: **not measured in this series**.
The isolated batch diagnostic measures the collection-lock effect directly; it
does not justify moving the WAL call outside the lock because overlapping writes
could then apply in a different order from replay unless a separately verified
ordered-apply mechanism is introduced.

## 11. Replication Optimization

No quorum weakening or new replication algorithm was introduced. RF3 uses three
engines, majority two, synchronous replication and durable settings. Partition,
restart, digest/convergence and 64/128-client smoke tests execute in the Community
suite. The comparative RF3 benchmark separately measures end-to-end write calls
and follower restart/catch-up, then verifies every expected value on all nodes
after all-node SIGKILL/restart. Append, replication, quorum and commit stage
latencies are **not separately measured**; client latency includes the full path.

## 12. Concurrency Optimization

Each pooled connection has exactly one active request. The FIFO queue and maximum
pool bound are retained. A ready slot is handed off immediately, with request
ownership guards preventing old callbacks from affecting its successor. Holding
the storage lock across `putMany` ordering improves correctness but can lengthen
critical sections. The diagnostic confirms this cost: v1.1.1 concurrency-8
request lock wait is 25.992 ms per call by median while concurrency-1 is effectively
zero. Worker model/admission settings are fixed across compared artifacts; no
auto-scaling feature was added.

## 13. Benchmark Environment

`method.json` records timestamp, engine paths/hashes/version strings, source
revision, matched SDK source hashes, Node version, kernel/OS, CPU inventory,
logical CPUs, RAM and mounted filesystem. Disposable fresh databases use loopback
JSON/TCP on the same host. RF1 specifies two engine cores, four queue shards/two
workers per shard, connection min/max 4/64, admission adaptation off and fsync on.
The RF3 helper records the actual synchronous majority configuration separately.
No container resource limit is implied for standalone process tests.
Record payload character lengths differ from complete serialized byte lengths;
each trial records an example JSON document's exact byte size.

This is an exact release-artifact comparison, not a same-toolchain microbenchmark:
the public 1.0.0 binary reports GNU 11.4.0 and the local 1.1.1 candidate reports
GNU 13.3.0. The compiler difference is a confounder, so whole-release results are
not attributed solely to individual source edits. Both artifacts ran on the same
host/kernel/filesystem with the same runtime configuration.

## 14. Benchmark Methodology

Run after local builds/tests finish, without overlapping build/test/profiler work:

```sh
node benchmarks/release-comparison.mjs \
  build-baseline-installed/usr/bin/db_engine build/db_engine \
  benchmarks/results/v1.1.1/standalone-final
python3 benchmarks/summarize-release.py benchmarks/results/v1.1.1/standalone-final
node benchmarks/release-rf3-comparison.mjs \
  build-baseline-installed/usr/bin/db_engine build/db_engine \
  benchmarks/results/v1.1.1/rf3-final
python3 benchmarks/summarize-release.py benchmarks/results/v1.1.1/rf3-final
```

Output directories must be unused. The baseline SDK is extracted from the exact
public 1.0.0 tarball; `BENCH_BASELINE_SDK` can point to that source. Three independent
fresh databases per version/case alternate old/new, new/old, old/new. No automatic
retry during timing. RF1 seeds 1,000 records (3,000 mixed; 5,000 index), warms 100
reads and 20 additional writes for relevant phases, and times finite work:
10,000 reads; 1,000 CRUD calls; 3,000 mixed 70% reads/30% updates; batches 10/100/1000;
pools 1/4/8/16/32; payload 128/1024/16384 ASCII characters; vector dimension 32.
Index rebuild times three requests over 5,000 low-cardinality rows. A batch's
calls/s differs from documents/s; both are retained. Seed/warmup/recovery are not
included in the timed call phase. Warmup duration is finite, not steady-state proof.

Resource sampling uses Linux process counters before/after, plus 500ms RSS samples.
CPU is process user+system time divided by completed calls. Read/write bytes are
Linux attributed I/O, not physical-device amplification. RF1 context switches are
main-thread only; RF3 sums three engine CPU/RSS/I/O counters. No hardware allocation,
IOPS or network-bandwidth attribution is claimed. Separate catch-up/recovery checks
occur after timing, so they do not silently inflate its rate.

## 15. Results

The final RF1 series contains 108 successful measured trials: 18 workloads, two
versions and three independent rounds. The separate RF3 series contains six
successful measured trials. Every trial completed without a timed error and then
passed its documented integrity and SIGKILL recovery checks. These are full-stack,
version-matched SDK/engine results; they do not isolate the engine from the client.

| Workload | v1.0.0 calls/s median (range) | v1.1.1 calls/s median (range) | Median paired change | P95 ms old → new | P99 ms old → new |
|---|---:|---:|---:|---:|---:|
| Read | 3,850.9 (3,610.3–3,955.0) | 19,990.6 (18,538.8–20,756.1) | +413.50% | 3.009 → 0.578 | 3.583 → 0.743 |
| Insert | 620.5 (618.8–649.6) | 648.2 (645.6–648.8) | +4.33% | 14.460 → 14.165 | 21.079 → 21.024 |
| Update | 226.5 (214.5–625.6) | 638.4 (630.4–656.8) | +181.83% | 41.922 → 15.263 | 43.594 → 17.869 |
| Delete | 594.6 (589.7–610.1) | 620.2 (598.7–690.8) | +1.66% | 15.127 → 14.758 | 20.859 → 23.888 |
| Mixed 70% read / 30% update | 2,101.1 (302.0–2,176.1) | 2,148.7 (2,063.8–2,310.0) | +6.15% | 9.256 → 11.330 | 10.822 → 13.135 |
| Batch 10 | 756.8 (693.8–1,187.4) | 237.5 (235.3–242.6) | **−68.62%** | 14.522 → 39.436 | 23.436 → 57.407 |
| Batch 100 | 82.3 (73.9–164.3) | 116.7 (98.7–121.7) | +19.82% | 95.559 → 79.119 | 95.559 → 79.119 |
| Batch 1,000 | 35.7 (20.8–40.3) | 32.4 (28.7–33.5) | **−5.99%** | 241.945 → 245.532 | 241.945 → 245.532 |
| Vector insert, dimension 32 | 590.9 (206.0–604.6) | 639.1 (622.9–645.5) | +9.23% | 16.592 → 14.527 | 24.738 → 23.735 |
| Vector search, dimension 32 | 845.9 (757.8–849.0) | 906.2 (859.4–906.5) | +7.13% | 14.308 → 12.049 | 19.618 → 13.518 |
| Low-cardinality index rebuild | 4.9 (4.8–5.0) | 21.6 (19.8–21.9) | +342.74% | 206.934 → 46.733 | 206.934 → 46.733 |
| RF3 insert | 649.2 (151.5–755.5) | 703.9 (639.7–791.9) | +4.82% | 19.051 → 17.410 | 28.569 → 23.629 |

Pool scaling was also measured at sizes 1/4/8/16/32. Candidate medians were
4,044.8, 13,681.8, 20,474.1, 23,896.4 and 24,761.7 calls/s respectively, versus
435.5, 1,751.9, 3,866.9, 7,165.3 and 12,902.3 for 1.0.0. Several baseline pool
rounds were severe slow outliers; the per-round data is retained and the table
does not turn those outliers into a general engine claim.

The read median used 321 CPU µs/call and 23.59 MiB peak RSS in 1.1.1 versus
896 CPU µs/call and 23.66 MiB in 1.0.0. Insert used 1,480 versus 1,410 CPU µs/call
and 30.29 versus 29.75 MiB RSS: the small throughput gain costs slightly more
CPU and memory. Low-cardinality rebuild used 43,333 versus 203,333 CPU µs/call
and 91.12 versus 90.79 MiB RSS. RF3 used 3,640 versus 3,860 CPU µs/call and
56.73 versus 56.10 MiB RSS. These process counters are not hardware energy or
device-level I/O measurements.

The completed data does not support a blanket improvement claim. Batch-10 is a
large repeatable regression, batch-1,000 is slightly slower, delete P99 rises
14.5%, and mixed P95/P99 rise about 22.4%/21.4%. Those results block presenting
1.1.1 as uniformly faster even though reads, index rebuild and vectors improve.
No number from `standalone/`, whose status is `ABORTED_DIAGNOSTIC`, is used here.

The engine-only diagnostic uses the v1.1.1 Node client for both binaries, removing
the client-version variable from the batch result:

| Engine | Concurrency | Batch-10 calls/s median | Mean latency median | Mean request lock wait median |
|---|---:|---:|---:|---:|
| v1.0.0 | 1 | 169.5 | 5.870 ms | 0.000 ms |
| v1.1.1 | 1 | 210.6 | 4.720 ms | 0.000 ms |
| v1.0.0 | 8 | 848.9 | 9.137 ms | 0.480 ms |
| v1.1.1 | 8 | 233.7 | 33.232 ms | 25.992 ms |

All four rows are medians of three alternating fresh-data trials. Each trial made
100 `insertMany` calls containing ten 1 KiB documents with fsync enabled and
verified the exact count before and after SIGKILL. The candidate is faster at
concurrency 1 but does not scale the short-batch path because the collection lock
serializes its durable WAL waits.

## 16. Reproducibility

All retained rows have three independent measured rounds. Selected per-round
throughput demonstrates both repeatability and observed variance:

| Workload / run | v1.0.0 calls/s | v1.1.1 calls/s | Paired change |
|---|---:|---:|---:|
| Read 1 | 3,955.0 | 19,990.6 | +405.45% |
| Read 2 | 3,610.3 | 18,538.8 | +413.50% |
| Read 3 | 3,850.9 | 20,756.1 | +439.00% |
| Insert 1 | 618.8 | 645.6 | +4.33% |
| Insert 2 | 620.5 | 648.2 | +4.48% |
| Insert 3 | 649.6 | 648.8 | −0.12% |
| Batch-10 1 | 693.8 | 242.6 | −65.04% |
| Batch-10 2 | 1,187.4 | 235.3 | −80.18% |
| Batch-10 3 | 756.8 | 237.5 | −68.62% |
| Index rebuild 1 | 4.859 | 19.803 | +307.59% |
| Index rebuild 2 | 4.957 | 21.946 | +342.74% |
| Index rebuild 3 | 4.814 | 21.634 | +349.40% |
| RF3 insert 1 | 649.2 | 639.7 | −1.47% |
| RF3 insert 2 | 755.5 | 791.9 | +4.82% |
| RF3 insert 3 | 151.5 | 703.9 | +364.48% |

The RF3 baseline's third-round collapse makes its apparent median improvement too
variable for a headline RF3 speed claim. Raw trial identity/order and slow runs
are retained. `summary.json` includes mean, median, min, max and sample standard
deviation for throughput, latency, CPU and RSS. Throughput percentage is
`100 * (new / old - 1)`; paired-change median and ratio-of-medians remain distinct.
Finite runs on a shared laptop do not establish a universal production advantage.
The separate `batch-lock-diagnostic-final/summary.json` contains 12 additional
trials and exact lock-wait telemetry. Its reusable command is
`node benchmarks/diagnose-batch-lock.mjs OLD_ENGINE NEW_ENGINE OUTPUT`; the output
directory must be unused so earlier evidence cannot be overwritten.

## 17. Trade-Offs

Better pool utilization can increase active sockets, CPU demand and server thread
pressure; it does not offer free concurrency. Batches amortize request overhead
but can increase lock residence/tail latency. Hash dedup spends transient memory
for fewer comparisons. Conservative legacy-socket retirement costs connections
but preserves correctness with delayed FIN. No throughput-for-durability trade
was authorized or introduced. The measured batch-10 and mixed/delete tail-latency
regressions are release trade-offs, not hidden noise. Source-only architecture
improvements are not individually assigned whole-release gains.

For short concurrent batches, v1.1.1 chooses WAL/apply ordering over the baseline's
greater overlap. The cost is the measured lock queue and lost batch throughput;
the benefit is that live memtable order cannot diverge from durable replay order.
A future optimization must allow concurrent WAL coalescing while enforcing ordered
application (and prove same-key, checkpoint and crash behavior). Simply moving
`WAL::logPutBatch` outside the lock was rejected as an unsafe benchmark-only edit.

## 18. Remaining Bottlenecks

The short-batch collection lock is a confirmed high-severity bottleneck at
concurrency 8. Whole-request serialization/copies, other lock/admission behavior,
ordered Raft apply, durable sync and vector visibility/signature scanning remain
investigation candidates. Severity must be tied to measured rows rather than
source intuition. Hardware-counter profiles, device sync histograms, network
bytes, queue depth and compaction/stall attribution remain **not measured**. The
main full-stack comparison cannot separate client scheduling from engine
contributions; the narrow batch diagnostic does isolate the engine and lock wait
for that one workload.

## 19. Failure Testing

Community checks exercise malformed framing/commands, authentication/RBAC,
port/root-owner conflicts, memory/queue limits, WAL segments, manifests/checkpoints,
snapshot metadata and crash-boundary recovery. Disk-full runs use disposable
filesystems. RF3 checks include partitions, quorum failure/recovery, node restart,
digest convergence and mixed-version upgrade against the exact public artifact.
Actual SIGKILL is distinguished from graceful shutdown. Power interruption,
controller-cache persistence and independent security certification are external
requirements, not simulated by killing a process.

## 20. Data Integrity

Standalone trials check every tracked expected application field, deleted-ID
absence and exact vectors before/after SIGKILL; vector count/search remains an
additional recovery check. This is disjoint-ID CRUD/mixed data, not a proof of all
same-key transactional histories. RF3 checks all expected fields on all three
nodes, follower catch-up and all-node crash recovery. The cross-SDK suite verifies
160 exact documents per client and 700KB file bytes across authenticated TCP/TLS
and SIGKILL recovery. Partial/unverified attempts are retained but not labeled PASS.

## 21. Compatibility

On-disk format 2, Raft protocol 2 and established document/vector/media APIs remain.
Legacy project APIs/data survive; ordinary CLI/Workbench flow becomes database-first,
a deliberate user-visible change. Existing client constructors and raw responses
are retained. Missing namespace, tenant visibility and invalid credential behavior
are checked. The mixed-version RF3 test covers follower-first upgrade, interrupted
upgrade, old leader replacement, restart and acknowledged-ID convergence. It does
not certify arbitrary concurrent update/delete/index/media/auth histories, a real
candidate-to-old rollback or restore of every backup artifact.

## 22. Risks

Required physical-power and independent security evidence are absent. Windows/macOS
signing/notarization setup and Python/Maven publication ownership are incomplete.
Repository protection/scanning checks fail. The Electron build dependency chain
has a recorded unpatched advisory; runtime-only npm audit reports zero. Hosted
platform results and local Linux qualification must not be conflated. Build identity
is checked against metadata, not manufactured to match an unrelated binary.
No arbitrary source edit is justified solely to reach a benchmark target.

The release branch remediates all five Jackson advisories reported for the Java
SDK by resolving Jackson 2.18.11. Post-tag dependency review identified two
additional high-severity advisories in 2.18.10. Maven's 29-test package build,
dependency tree, exact packaged-client suite and release examples pass. The
immutable release tag contains the earlier Java dependency, so the website pins
the audited post-tag source commit; no Java artifact was published from the tag.

## 23. Future Optimization Opportunities

An approved post-publication branch follow-up hardens Workbench startup-error
diagnostics by retaining and reading the original log descriptor, with an explicit
position and an 8,000-byte window before the existing 2,000-string-unit tail limit.
File/symlink replacement, Unicode tails, startup failure/timeout/cancellation,
descriptor cleanup and write-only-log compatibility pass locally. Equivalent
website/upgrade test checks preserve missing-file/fragment, nonexecutable and
traverse-only-directory behavior. This is not an engine speedup, does not establish
remote exploitability, and is not present in the immutable published Workbench.
The separate CodeQL gate failed at `40c19b0`; PR #31 tracks the follow-up's hosted
recheck without dismissing alerts or disabling checks.

Use stable dedicated hosts and sustained/out-of-cache workloads; profile full CPU
and waits before changing storage/replication. Investigate prewarmed pools against
server thread capacity, batch-size/tail-latency balance and measured vector/index
scale. Add independent rollback/restore and arbitrary same-key history evidence.
These are future work, not completed 1.1.1 features. Rejected format-3, anchored,
publication-token and apply-run experiments are not silently layered into this release.

## 24. Final Validation Matrix

| Area | Test | Result | Evidence |
|---|---|---|---|
| Build | Linux CMake build and native CLI link | PASS | `cmake --build build -j2` on 4 October 2026 |
| CRUD/documents | Basic/nested/Unicode/bulk/query/update/delete/count and malformed input | PASS | Community comprehensive suite: 58/58; authenticated E2E: 71 shell commands |
| WAL | Group commit, segment/torn-tail/checksum and six checkpoint crash boundaries | PASS | `scripts/test-community.sh build` native sections |
| Persistence | Graceful restart, SIGKILL recovery, 200k-row bounded recovery | PASS | E2E/restart suites and benchmark integrity fields |
| Replication | RF3 majority, partition, follower restart/catch-up, convergence | PASS | RF3 suite; minority acknowledgements 0, majority acknowledgements 2 |
| Mixed-version upgrade | Published 1.0.0 → candidate 1.1.1 | PASS | `rf3-upgrade-final.json` |
| Vector | Insert/retrieval/search/invalid dimensions and recovery | PASS | vector correctness suite and six timed vector trials |
| Binary/media | small/large chunked transfer, checksums, resume and recovery | PASS | catalog/media tests; cross-SDK 700,000-byte exact files |
| Authentication/RBAC | TCP/TLS login, invalid/missing credentials, API keys and scope | PASS | engine auth boundary and cross-SDK authenticated suite |
| SDK | Node, Python and Java tests plus named operation coverage | PASS | Node 31/31; CLI 27/27; Python-inclusive suite 133/133; 135 dispatch bindings |
| Java dependency remediation | Jackson 2.18.11 resolution, package and installed consumer | PASS (release branch) | `security-advisories.json`; Maven 29/29; exact packaged TCP/TLS/recovery suite; release examples pass |
| Workbench | Browser CRUD/UI and desktop bundled-engine lifecycle | PASS (local + hosted + public) | local browser/desktop checks; hosted matrix `37176501189`; exact-tag publication run `37191983730`; public checksums and bundled versions verified |
| Hosted platform build | Workbench Linux/macOS/Windows workflow after final fixes | PASS | run `37176501189`, exact source `03289fb95999462222973e04d8869827fc1a98c7` |
| Hosted native installers | Linux/Windows/macOS installers and container P0 | PASS | run `37176502323`; downloaded evidence under `hosted-p0/` |
| Benchmark | Matched 1.0.0 vs 1.1.1 RF1/RF3 | PASS | 108 RF1 + 6 RF3 successful measured trials |
| Reproducibility | Three alternating fresh-data runs per row | PASS | `standalone-final/summary.json`, `rf3-final/summary.json` |
| Performance regression explanation | No major unexplained regression | PASS WITH DOCUMENTED REGRESSION | 12-trial engine-only diagnostic plus source comparison attribute batch-10 to the expanded collection-lock scope; request lock wait is 25.992 ms at concurrency 8; raw evidence in `batch-lock-diagnostic-final/` |
| Small-batch remediation | Verified ordered-apply optimization or explicit release-owner disposition | ACCEPTED RISK | Root cause is established; the release owner accepted the regression without an unsafe lock-scope change |
| Physical power | Real power interruption/controller-cache test | NOT PERFORMED — OWNER OVERRIDE | Independent external evidence was not supplied; the release owner explicitly accepted this gap for v1.1.1 |
| Independent security | External review required by release policy | NOT PERFORMED — OWNER OVERRIDE | No qualifying signed evidence was supplied; the release owner explicitly accepted this gap for v1.1.1 |
| Registry/public verification | Install public 1.1.1 packages and verify examples | PARTIAL | Native GitHub/GHCR and both npm packages are public and verified; PyPI run `37191728049` failed on an unregistered trusted publisher and Java run `37191730662` lacked Central signing/token credentials |

The raw ledger, trial JSON, method files, system inventory and artifact manifest
are under `benchmarks/results/v1.1.1/`. `PASS` above means the named command was
executed. `ACCEPTED RISK` and `NOT PERFORMED — OWNER OVERRIDE` are not test
passes; they preserve the missing evidence and explicit release decision.
