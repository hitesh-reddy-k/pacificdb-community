# RC implementation and qualification follow-through

This change continues the existing v1.2 checkpoint. It does not introduce a
separate vector engine or claim stable production qualification.

## Implemented changes

- `engine/src/raft_core.cpp`, `engine/include/raft_core.hpp`: standalone append,
  checked log sync and durable commit publication share existing node-local log
  ownership. Application/read work remains outside that lock. Unpublished tails
  are rolled back and synchronized before later appenders proceed. Late uncertain
  publication fences the node; its error includes `write_outcome_unknown`.
- Consensus checkpoint updates keep election intent monotonic and update cached
  durable progress only after publication. Directory-open/sync errors are checked;
  Windows uses write-through replacement. Group-sync errors release waiters
  without falsely advancing the durable sequence.
- Recovery-incomplete nodes refuse heartbeat/read-barrier acknowledgements and
  matching-tail advertisements. Synchronous log I/O failure fences election,
  replication acknowledgement and strong-read eligibility until restart.
- `raft_commit_publication_test.cpp` exercises real filesystem and Linux syscall
  errors through executable-local interposition. No fault injector is linked into
  the shipped engine. The existing genuine ENOSPC assertions are unchanged; new
  optional diagnostics retain failure category and engine logs.
- `patch-build-tool-compat.mjs` pins patched minimatch11.0.3 and adapts five exact
  reviewed source files in legacy build-only consumers. Version/source hashes
  fail closed; no advisory exception or audit suppression is added. Eight package/
  guard tests and all 30 upstream glob7 test files pass. Actual packaged Linux GUI,
  backup/restore and archive contents were exercised before final source binding.
- `qualify-prerelease.py` consumes reviewed exact-artifact receipts; changed or
  missing evidence, mismatched source/package identity, incomplete mandatory
  checks and artifact substitutions block sealing. The disk-full gate is required.
- The manual GitHub publisher validates the owner-qualified draft's exact assets
  rather than trying to publish automatically blocked staging. The explicit RC
  container workflow verifies a public qualified prerelease before publishing an
  RC-only image and testing its pulled digest. Stable/latest flows stay separate.
- `PUBLISHING_OWNER_GUIDE.md` provides real PyPI/Maven ownership, credential setup,
  workflow and public installation steps. Missing owner credentials stay Blocked.

## Validation scope

The full Community runner passed on the integrated safety/dependency source,
including document/query/index/media/auth flows, genuine ENOSPC, RF3, partition,
recovery and sustained concurrency. All 50 CTest cases passed. Final error-outcome
normalization and final immutable package rebuilding receive their own checkpoint
receipts and binary hashes; historical failed attempts are retained. Source tests
are not substituted for final package installation or public accessibility.

Final release receipts must map every required gate to its actual logs, exact
source revision and artifact bytes. Earlier targeted sanitizer results do not
qualify the complete new patch. Complete platform/mixed-version/power/security/
production gates remain separate and must not be labeled Passed without evidence.

## Compatibility and tradeoffs

Public success responses, JSON-vector APIs, durable vector records, numeric
precision, document/media formats, storage format2 and Raft protocol2 are retained.
No fsync/quorum/tenant/auth checks are weakened. Standalone append publication is
more serialized; ingestion/write concurrency must be remeasured. Fail-closed I/O
handling can reduce availability and require restart, while preserving trustworthy
acknowledgement and recovery boundaries. A distributed timeout after quorum is
not a rollback guarantee; verify outcomes before retrying non-idempotent writes.

The matcher bridge is temporary maintenance responsibility until supported
upstream build clients accept the patched matcher API. New dependency versions
must be reviewed and tested before updating its source guards. No claim of native
Windows/macOS installer qualification follows from Linux compatibility tests.

## Native runtime distribution fix

The Workbench Debian package now explicitly declares its bundled engine's RE2
runtime dependency for Debian13 and Ubuntu24 builds. A populated development host
can hide missing shared-library declarations; final qualification includes a clean
Debian installation and direct bundled-engine/CLI execution, separately from GUI
checks. Portable archives require the documented matching shared libraries.
