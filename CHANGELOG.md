# Changelog

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
