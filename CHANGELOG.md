# Changelog

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
