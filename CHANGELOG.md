# Changelog

## 1.1.1 — unpublished candidate

Publication and release qualification are pending. Public engine/native installers and npm client/CLI remain 1.0.0; the Workbench Linux preview is separately published as 1.0.1.

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
- Fixed Node.js pool scheduling after sequential warmup: completed slots return to the pool before caller promises settle, so a concurrent burst can use the configured connections instead of queuing behind one warm slot. The peer regression verifies four concurrent requests with a four-connection pool.
- Verified media downloads replace destinations after checksum validation; interrupted writes are never automatically retried.
- Workbench Overview count loading uses a single loader with at most four jobs in flight, cached database scope, mutation invalidation and unknown-value placeholders. Other views do not schedule full count scans.

Project metadata and legacy SDK/raw APIs remain compatible; no data migration is required. Candidate qualification must verify package installs, upgrade/restore, authentication, security, retained tests and release artifacts. No candidate benchmark ranking or measured startup-speed improvement is claimed.

See [candidate release notes](site/release-1.1.1.html), [usage and upgrade documentation](site/docs.html), and [historical 1.0.1 source notes](site/release-1.0.1.html).
