# Unreleased v1.2 follow-up

- Validate operator accounts files through the opened descriptor/Windows handle
  with native ownership and access checks and a 1 MiB limit. Reject broad access
  and links; join the memory monitor on startup refusal. Keep account roles,
  authentication and storage formats unchanged.
- Keep complete incremental ANN construction eligible during unrelated writes;
  fence real source edits and withhold eligibility until journal coverage is
  installed. Report lifecycle status during paused apply without bypassing the
  public query fence. Add controlled build/apply regressions and Windows fixture
  startup diagnostics; platform qualification remains pending.
- Keep a complete, covered incremental ANN generation eligible across unrelated
  Raft commits. Preserve applied-index, source/configuration, journal and deadline
  fences; add controlled scheduling-stall regressions and native/sanitizer CI
  coverage. See [validation and scope](docs/V1.2_ANN_COMMIT_VISIBILITY.md).
- Bound strict incremental ANN freshness waits to each request's original
  deadline, cancellation and cumulative scan budget; explicit fail-fast remains
  available. The separate 10K/100K/1M lifecycle qualification passed all 360
  mixed reads per size. These are not new paired PostgreSQL latency results.
- Correct Linux Workbench launcher icon packaging using existing logo assets.
- Use caller-owned timestamp conversion and private creation of new POSIX
  WAL-integrity files, retaining existing serialized data and fsync behavior.
  POSIX integrity writes now reject final-component symlinks; regular existing
  file modes are preserved. The POSIX monitor credential reader reports an
  explicit unsupported-platform error elsewhere; Windows engine/SDK paths remain.
- Validate monitor credentials through the descriptor actually read and publish
  monitor output with exclusive private temporary files.
- Bind source-review dependency evidence to the archived revision; add negative
  regressions, wider branch scanning and effective repository-control checks.
- Integrate reviewed dependency updates through PR #53. Publication and complete
  production qualification remain separate; RC.2 assets are unchanged.

# 1.2.0-rc.2 — Pre-release v1.2

## Fixed

- Keep database-tree and collection controls reachable in short Workbench windows;
  reproduce the original sidebar overlap and verify real pointer clicks at650,
  653 and900px heights.
- Retain electron-builder's upstream Linux post-install behavior, including the
  application-specific Ubuntu AppArmor profile and launcher registration.

## Changed

- Advance compatible engine/CLI/Workbench/SDK prerelease identities toRC.2 without
  overwriting publishedRC.1 tags or packages. Database/storage/vector implementation
  source is unchanged; release metadata and Workbench layout/packaging differ.
- Add independent public-package installation tests and GUI failure diagnostics.

## Known issues

- RC.2 is published with scoped installation/test evidence and a completed
  10K/100K/1M benchmark. Its strict mixed ANN availability failed at 1M; the
  follow-up fix above is not in published RC.2. ANN/distributed/production limits
  remain. Python RC.2 is published on PyPI; Maven Central needs owner setup.

---

# Changelog

## Pre-release v1.2 reliability follow-through

- Fixed standalone pre-apply durable commit publication and rejected Raft tail
  cleanup under ENOSPC; propagated log-sync errors and fenced uncertain nodes.
- Prevented recovery-incomplete replicas from acknowledging heartbeat/read-barrier
  requests or advertising an untrusted tail. Added syscall-failure regressions.
- Replaced vulnerable desktop matcher versions with pinned patched minimatch and
  reviewed, hash-guarded compatibility adaptations; full npm audit is clean.
- Added receipt-backed prerelease qualification, RC-only container publication and
  owner PyPI/Maven publishing instructions. Stable qualification remains separate.


## 1.2.0-rc.1 — Pre-release v1.2 (external testing)

Base version:1.2.0. Production qualification remains incomplete. Publication and
verified-platform status: [release checklist](RELEASE_CHECKLIST.md).

### Added

- Durable dense, sparse and named vector values through existing WAL/LSM/Raft;
  creation-time vector schemas and matching SDK operations.
- Opt-in standalone HNSW lifecycle with bounded dense incremental maintenance,
  packed float64 exact indexes and native BM25/RRF hybrid retrieval.
- Explicit search-mode/fallback/index selection, bounded per-query ef_search,
  readiness/generation/freshness/resource errors and payload-cache counters.
- Canonical prerelease identity and consistent npm/Python/Java candidate mapping;
  actual staged package installation/live examples and company testing guides.
- Workbench advanced vector search controls, prerelease identity and explicit
  desktop vector-preview opt-in while preserving default isolation.

### Changed

- Safe auto/ANN routing removes the legacy all-pairs graph builder. Strict ANN
  requires a fresh eligible index; configured exact fallback is explicit.
- Query limit, cancellation and read-context state is request-owned and propagated
  through workers. Native vector records remain authoritative in existing storage.
- Release candidates stage separately from stable publication. npm candidates use
  next, Python PEP440rc syntax, and GitHub prerelease/latest flags are explicit.

### Improved

- Exact top-K retains bounded candidates, selectively decodes vector/metadata and
  avoids full-document materialization for every candidate.
- Verified authoritative row-cache reuse avoids forced rereads without caching
  search result sets or removing final source/tenant/score checks.
- SDK verifiers compare exact staged JAR and POM bytes; package import tests use
  installed distributions rather than source checkouts.

### Fixed

- Float64 distance intermediate overflow; point-read version visibility across
  flush, late cache fill, recovery replay and advancing Raft commit bounds.
- Secondary-index queue/locking correctness and bounded apply retry diagnostics,
  with evidence retained in earlier implementation checkpoints.
- Tenant locks now have persisted expiry and authorized password-reset recovery;
  limited-role database access avoids recursive mutex deadlock.
- Deterministic logs redact owner/reset passwords on accepted and rejected
  credential operations; existing configuration-secret redaction remains.
- Structured-event test now compares event fields and validates timestamps
  separately instead of assuming two distinct events share one millisecond.

### Deprecated

No additional public API deprecation is introduced by this candidate.

### Known Issues

- ANN graph persistence/replay is incomplete: explicit reconstruction after restart.
- Strict ANN queries can be unavailable during writes. Sparse/flat/lexical indexes
  do not support incremental maintenance. Bound history requires manual rebuilds.
- ANN, ingestion and selective-filter performance have material PostgreSQL/pgvector
  gaps in the measured workload. No general performance-superiority claim.
- Cluster vector schemas/catalogs and complete distributed vector search remain
  gated; online schema migration, automatic graph compaction and advanced indexes
  are incomplete/deferred. Full production/platform/upgrade/security gates remain.

### Compatibility and breaking behavior

No new storage-format or Raft protocol bump (both remain2), silent float32
migration, or document/media API removal. Explicit limit errors replace formerly
ambiguous partial vector answers, and strict ANN eligibility can reject requests.
These compatibility changes and fallback/maintenance rules are documented; a
universal mixed-version/downgrade guarantee is not established.

The [completed benchmark](docs/v1.2-evidence/community-verified-cache-comparison-20261101/REPORT.md)
remains bound to its frozen1.2.0-dev candidate. It is not relabeled as anRC build.

### Historical development implementation record

The earlier checkpoint notes below retain their original scopes and counts.


- Correct point reads across memtable flushes, late cache refill and Raft visibility changes; add 18 deterministic interleaving cases.
- Reuse authoritative point-cache rows only with matching source proofs; preserve native result/tenant/freshness validation and expose payload fetch/cache-hit counts.
- Verify 75 top-level checks /43 CTest, five targeted TSAN, four ASan/UBSan, 34 Node/134 Python/30 Java units, live SDK recovery and a fresh 1M dirty restart.
- Complete the [PostgreSQL 18.6 + pgvector 0.8.7 comparison](docs/v1.2-evidence/community-verified-cache-comparison-20261101/REPORT.md): 1M indexed exact 49.73 ms versus 112.23 ms, ANN 18.24 ms versus 2.26 ms p50. ANN latency, ingestion, selective filtering and mixed-read availability remain gaps; graph checkpoints and full release qualification remain incomplete.

- Correct dense-score intermediate overflow for large finite float64 values, with a wide-precision fallback and regression coverage.
- Add bounded per-query HNSW `ef_search` / Node `efSearch` and expose index rebuild requirements and maintenance policy.
- Qualify the current candidate with 72 top-level checks (40 CTest), three targeted TSAN tests, flat ASan/UBSan and SDK TCP/TLS/crash recovery; retain the numeric pre-fix failure.
- Run a fresh PostgreSQL 18.6 / pgvector 0.8.7 comparison with held-out recall, repeated query rounds, durable ingestion, concurrency, mutation visibility and process recovery. See [live evidence and limitations](docs/v1.2-evidence/community-official-20261031/STATUS.md); this is a self-run candidate benchmark, not a release or independent certification.

- Add immutable creation-time dense/sparse/named collection schemas and CRUD, batch, update-post-image and replay enforcement.
- Add opt-in schema-constrained incremental HNSW maintenance using bounded LSM change journals and the existing index worker; preserve explicit freshness/coverage errors and rebuild behavior.
- Preserve schema metadata during later DDL/ACL edits; fence standalone durable tails before serving clients; bound failed apply retries and diagnostics.
- Pass the 72-check/40-CTest checkpoint, five targeted TSAN tests, HNSW ASan/UBSan and SDK TCP/TLS/SIGKILL recovery. Cluster capability, automatic compaction, distributed completeness and complete release qualification remain unfinished.


- Integrate packed float64 exact generations and opt-in standalone public index lifecycle, ANN/exact routing and BM25/RRF hybrid APIs; default preview remains disabled.
- Add Node/Python/Java lifecycle/hybrid methods and verify all 141 action contracts.
- Propagate request-local vector deadlines; reject invalid global timeouts before recovery. Cancel dropped-collection index workers without joining them in committed apply.
- Pass 65 top-level regressions (35 CTest), five targeted TSAN tests, flat ASan/UBSan and live TCP/TLS/SIGKILL SDK integration.
- Record a paired NVMe/ext4 1M GloVe-25 public exact replay: PacificDB 36.61 ms p50 versus PostgreSQL exact 73.65 ms, both exact-reference matches. pgvector ANN is 2.11 ms at Recall@10 0.981; PacificDB public ANN was not measured. These development measurements do not establish production readiness.


- Add versioned float64 sparse records and multiple named vector spaces to existing vector writes/exact search, with WAL/SST/snapshot/RF3 recovery tests.
- Add a bounded trusted C++ native ANN service, composite generation publication, authoritative revalidation and source/build cancellation. Public ANN remains gated.
- Add a derived PMR BM25 inverted index and tested RRF hybrid retrieval behind trusted C++ integration.
- Fix secondary-index rebuild lock ordering and obsolete queued updates resurrecting deleted index entries; validate mixed document/media/vector traffic normally and under ThreadSanitizer.
- Record a 62-check regression checkpoint and separate 1M graph recall diagnostic; retain unfinished public/distributed release gates.
- Add Node sparse-value validation and `vectorName` mapping while preserving legacy dense requests.

- Begin the native vector subsystem audit and staged acceptance plan on `v1.2`.
- Bound exact vector ranking to top-K candidates; apply document tenant/read visibility checks.
- Decode selected vector/metadata fields in one pass and avoid an extra numeric-vector allocation without changing stored bytes or legacy precision.
- Add explicit `search_mode` / Node `searchMode`; remove unsafe legacy ANN construction. Auto uses safe exact fallback, and strict ANN explicitly reports index unavailability.
- Isolate query limits and cancellation per RPC, propagate owned contexts through queues, and clear worker read contexts on success/error.
- Preserve standalone version floors and expose bounded aggregate/document cancellation partial results.
- Add concurrent tenant/API/worker tests and ThreadSanitizer evidence; checkpoint 1 passed 52 top-level checks.
- Begin experimental HNSW core repairs and background generation primitives; public ANN remains unavailable pending full lifecycle qualification.
- Cap graph/internal scratch allocation bytes with shared accounting across retained generations; reject failed graphs and preserve healthy graphs after read-budget exhaustion.
- Track derived-index freshness across LSM mutations, replay, snapshots and namespace drops; add a versioned internal vector index catalog with standalone recovery tests.
- Repair collection-WAL recovery lock ordering using per-log idle admission, preserving existing formats and fsync settings.
- Record an additive replicated vector incarnation on newly committed collection creation while preserving existing collection IDs and APIs.
- Reject exhausted exact scans/cancellation rather than returning an incomplete ranking as exact; apply the output-document limit to K rather than all candidates.
- Enforce query time budgets without relying on a worker token and reject oversized integer K values before conversion.
- Add deterministic score/ID ties, retained-document byte checks, independent-reference tests, and a baseline-compatible exact benchmark.
- Preserve existing authoritative document storage, mutation, WAL and Raft formats. No compact-format migration is enabled.

See [development status and limitations](docs/releases/1.2.0-development.md).
Standalone public ANN/hybrid lifecycle, incremental HNSW and creation-time schemas
are implemented as opt-in previews. Production qualification, distributed vector
fan-out and advanced indexes remain incomplete. This entry does not announce a release.

## 1.1.2 — 2026-10-07 (security and reliability prerelease)

- Align Community engine, native/npm CLI, Node/Python/Java SDKs, Workbench and Helm source metadata at 1.1.2.
- Enforce database owners and explicit read-only/read-write/co-owner grants, role ceilings, namespace/database identity, nested bulk scope and audited superadmin recovery.
- Authorize media by the stored owning database; bound cleanup/reconciliation by actual stored chunks rather than sparse declared counts.
- Bind managed-local engine and Raft listeners to loopback; reject invalid binds and use the same resolved configuration for validation and listeners.
- Reject invalid/oversized Raft frames before allocation and bound Node response buffering before decoding.
- Persist account lockout expiry; use bounded RE2 query matching instead of uninterruptible backtracking.
- Attribute authorization denials and superadmin actions; expose audit persistence failure, eviction and recovery without leaking credentials.
- Add persisted metrics-only API keys that cannot read/write documents; verified-mTLS pod-IP readiness rejects disabled/unhealthy audit logging without creating liveness restart loops.
- Add a restricted audit-monitor timer, alert rules and real engine-to-Alertmanager-to-receiver failure/retry/recovery verification.
- Pin the container base digest; preserve fail-closed deployment and publication controls.
- Verify packaged native revision/hash manifests, sandbox, CRUD/media, backup/restore and restart persistence; reject reviewed build-tool dependencies in runtime payloads.
- Document the exact-version build-only advisory exception through 2026-11-06; CI rejects new/runtime/unreviewed/expired findings. These advisories are accepted, not patched.
- Retain post-1.1.1 log-descriptor and lifecycle diagnostics fixes described below.

See [complete patch notes, migration requirements and readiness limits](docs/releases/1.1.2.md).
Maintainer-reported verification is not an independent certificate; historical
eight-hour/power results keep their original revision identities.

## Post-release branch follow-up — not in the published v1.1.1 artifacts

- Startup-failure diagnostics read a bounded Unicode tail from the engine's original log descriptor instead of reopening a replaceable pathname. Existing write-only logs leave their contents untouched and use a new, exclusive readable log whose path is reported to the caller.
- Website and mixed-version upgrade validation remove redundant filesystem preflights while retaining missing-file, fragment, nonexecutable-artifact and traverse-only-directory checks.
- Added deterministic file/symlink-replacement, descriptor-cleanup and permission-compatibility regression coverage. Published release tags and artifacts are unchanged; hosted requalification is tracked in PR #31.
- Native lifecycle validation captures stdout and stderr separately and waits for pipe drain, preventing unrelated stdout from corrupting JSON event assertions on Windows. Event parsing remains strict; no engine behavior or validation gate is weakened.

## 1.1.1 — 2026-10-04

Released from branch `v-1.1.1`. The release owner explicitly accepted the documented external-review and small-batch performance risks and authorized unsigned Windows/macOS artifacts for this version only.

### Added

- Direct database URL connections across Node.js, Python, Java, CLI and Workbench, with authenticated URLs and verified TLS.
- Python/Java document lifecycle, CRUD/bulk/query methods and named engine operation families.
- Python/Java checksummed bounded file uploads/downloads, explicit upload resume, and streaming full backup export.
- Workbench database URL and matching CLI/SDK connection examples; package-specific source installation and usage documentation.

### Removed

- Mandatory project selection from normal database/collection workflows.
- Friendly project commands in native/npm shells and Workbench project screens; project IDs/tokens in updated CLI context.

### Improved

- Creation selects a database only after success; switching validates the accessible database catalog.
- Java/Python pooled connection lifecycle, request scope capture, typed errors and explicit close behavior.
- Verified media downloads replace destinations after checksum validation; interrupted writes are never automatically retried.

### Fixed

- Preserved request ownership after refused connections and TLS-handshake timeouts so an old socket callback cannot reject a queued successor.
- Preserved explicit source identity in archive/container builds instead of silently embedding `unknown`.
- Corrected native CLI socket-error handling for Windows and normalized executable names ending in `.exe`.
- Updated the Java SDK's Jackson runtime from 2.18.9 to 2.18.11. Post-tag dependency review found two additional high-severity advisories affecting 2.18.10; 2.18.11 is the first patched 2.18.x release for all five advisories recorded against this branch.

### Performance

- Fixed Node.js pool scheduling after sequential warmup: completed slots return before caller promises settle, allowing a concurrent burst to use the configured bounded pool.
- In three matched runs against public 1.0.0, read median throughput changed from 3,850.9 to 19,990.6 calls/s and low-cardinality index rebuild from 4.9 to 21.6 calls/s.
- Retained regressions are explicit: batch-10 changed from 756.8 to 237.5 calls/s, batch-1,000 from 35.7 to 32.4 calls/s, and mixed/delete tail latency increased. This release is not uniformly faster.
- A separate 12-trial engine-only diagnostic attributes the batch-10 regression to the v1.1.1 per-collection lock spanning durable WAL completion: at concurrency 8, median mean request lock wait was 25.992 ms versus 0.480 ms in 1.0.0. The ordering guard was not removed merely to improve a benchmark.

### Workbench

- Replaced mandatory project selection with direct database and collection navigation while retaining legacy stored project metadata.
- Overview count loading uses one loader with at most four jobs in flight, cached database scope, mutation invalidation and unknown-value placeholders. Other views do not schedule full count scans.
- Local Linux browser/packaged-desktop checks and hosted Linux/Windows/macOS Workbench builds pass; 1.1.1 desktop artifacts use the explicit unsigned-release exception.

### SDKs

- Added database-first Node.js, Python and Java connection helpers while retaining raw/legacy APIs.
- Verified exact document and 700,000-byte media recovery across authenticated TCP/TLS Python and Java clients.
- Rebuilt and retested the Java main, sources and javadoc JARs with Jackson 2.18.11; all three Jackson components resolve to that version.

### Documentation

- Added tested package examples, Workbench operation/troubleshooting guidance, upgrade notes, reproducible benchmark scripts/raw evidence and the two v1.1.1 release reports.

### Known Issues

- Independent physical-power/security evidence remains unavailable; the release owner explicitly accepted that risk for 1.1.1.
- Windows and macOS artifacts are unsigned. The exception is restricted to the 1.1.1 engine and Workbench tags.
- Maven Central publisher ownership remains unavailable; use the verified source-install path for the Java SDK.
- The Electron build dependency chain retains a recorded `http-cache-semantics` advisory; runtime-only npm audit reports zero advisories.
- GitHub's default branch can continue reporting Jackson alerts until the 2.18.11 release fix is merged. The `v1.1.1` tag contains the earlier 2.18.10 Java source, so the public Java source-install guide pins audited commit `25fb81d413973b6779eaf71a26bedb42f6d79be3`; no Java artifact was published from the tag.

Project metadata and legacy SDK/raw APIs remain compatible; no storage-format migration is required. The measurements are finite same-host PacificDB comparisons, not cross-product rankings or service-level guarantees.

See [release notes](site/release-1.1.1.html), [usage and upgrade documentation](site/docs.html), and [historical 1.0.1 source notes](site/release-1.0.1.html).
