# Workbench Production Readiness Design

## Goal

Turn the existing PacificDB Workbench 1.1.1 source into a 1.1.2 release
candidate whose installers, upgrade path, data recovery, dependency state, and
release evidence are reproducibly checked before publication.

This work starts from commit `b4c0ad3c7bec1a3d7594986f59cd25d94a6e31f6`
on `pacificdb-v1.0`. It does not modify or republish the existing 1.1.1
release, push a branch, create a tag, configure signing credentials, or claim
external evidence that has not been supplied.

## Scope

The candidate will:

- use a dedicated Workbench 1.1.2 version in the root desktop-build manifest,
  generated desktop package, documentation, and lockfile while preserving the
  released 1.1.1 engine, SDK, and CLI versions bundled by the app;
- derive workflow build versions from the package manifest instead of keeping
  separate hard-coded values;
- test installed Linux, Windows, and macOS artifacts rather than treating an
  unpacked build directory as installer evidence;
- verify that uninstalling an application leaves its database directory
  intact;
- run the previous Workbench release against a temporary data directory,
  upgrade to the candidate, and verify documents, media, preferences, and the
  bundled CLI still work;
- exercise backup creation, verification, mutation, restore, and recovered
  data through public CLI behavior;
- update the dependency lock to remove the known high-severity build-tool
  advisory and fail CI on production dependency advisories;
- generate release evidence bound to an exact clean revision and candidate
  artifact digests;
- validate externally supplied physical-power, independent-security, and
  long-duration-load evidence without manufacturing those results; and
- document supported systems, tested workload limits, backup, update,
  rollback, data locations, and the loopback-only security boundary.

Signing, notarization, certificate procurement, and signing-secret setup are
explicitly out of scope. Existing signing behavior remains unchanged.

## Source and Release Identity

Development happens only on `codex/workbench-production-readiness` in an
isolated clone. The original dirty checkout remains untouched.

The private root package manifest is the Workbench version authority. The CLI
manifest remains the bundled component-version authority. CI passes the CLI
version to CMake, passes the root version to desktop packaging, and records
both. A release tag must be exactly `workbench-v<root manifest version>` and
resolve to the checked-out commit. Branch and pull-request runs may build
artifacts but cannot publish a GitHub release.

The existing 1.1.1 GitHub release is historical input for upgrade testing. The
candidate uses a new 1.1.2 tag when the release owner later chooses to publish;
assets under an existing release are never replaced by this work.

## Installer and Upgrade Verification

The existing Electron test remains the single UI acceptance flow. It gains
three optional inputs:

- a caller-owned data directory that is not deleted by the test;
- a previous-version executable used for the initial data-creation phase; and
- a candidate executable used for the verification and recovery phase.

Without those inputs the current development and same-version restart behavior
is preserved. With them, the previous executable creates the database,
document, media item, and persisted preference, then exits cleanly. The
candidate opens the same directory and verifies those values before exercising
the bundled CLI backup/verify/restore flow.

Each hosted operating-system job installs its real candidate artifact into a
temporary or disposable system location, runs the shared acceptance flow,
uninstalls it, and asserts that the caller-owned database directory remains.

- Linux installs the generated Debian package with `apt`, launches the
  installed `pacificdb-workbench`, and removes the package afterward.
- Windows silently installs the generated NSIS executable into a runner-local
  directory, launches the installed executable, and runs its uninstaller.
- macOS mounts the generated DMG, copies the application to a temporary
  Applications directory, launches the copied application, and removes it.

The workflow downloads 1.1.1 only as the previous-version fixture. Candidate
artifacts are always built from the current checked-out revision.

## Release Evidence

A small Workbench-specific qualification command will reuse repository state
and artifact hashing conventions from `scripts/release_qualification.py`
without changing the server-wide release policy. Its machine-readable report
contains:

- source revision, dirty state, and manifest version;
- every installer path, size, and SHA-256 digest;
- platform installer and upgrade-test results supplied by CI;
- production dependency audit status;
- 500,000-record index result;
- external physical-power and independent-security reports;
- long-duration-load evidence; and
- a final `PASS` or `BLOCKED` decision.

External reports must be JSON, report `PASS`, identify the exact candidate
revision, and satisfy their repository schemas. Security evidence must cover
the candidate artifact digests. Load evidence must include duration, operation
count, error count, peak resident memory, and the declared 500,000-record
workload. Missing, mismatched, or failed evidence blocks publication.

The GitHub release job consumes the qualification report produced for the same
workflow run. It cannot publish merely because build jobs succeeded.

## Dependency Policy

`npm audit --omit=dev` is the release-blocking runtime check. The lockfile is
updated within the existing semver range to remove the high-severity
`http-cache-semantics` advisory. Remaining build-only advisories are recorded
in the qualification report and may block only when high or critical; no new
override or dependency is added solely to silence moderate transitive findings.

## Documentation and Support Boundary

Workbench remains an offline-first, single-user local application. Its engine
and HTTP bridge must stay bound to loopback and are not supported as network
services. Production documentation will state:

- Ubuntu 24.04/Debian-compatible Linux x64, Windows x64, macOS arm64, and
  macOS x64 as the packaged targets;
- the exact platforms exercised by the release workflow;
- 500,000 records as a verified workload, not an absolute capacity ceiling;
- the manual backup, restore, upgrade, and rollback procedures;
- that uninstall preserves data and users must retain an external backup
  before upgrades; and
- that updates are manual for 1.1.2.

Auto-update, telemetry, accounts, cloud sync, team features, container/Helm
qualification, and public network exposure are not added. They are unrelated
to a production-ready local desktop release.

## Failure Behavior

Release checks fail closed on a dirty revision, tag/version mismatch, missing
installer, checksum mismatch, failed installation, upgrade data loss, failed
restore, production dependency advisory, absent external report, evidence for
another revision, or a nonzero load-test error count.

Test data always uses temporary directories. Installer cleanup may remove the
application but never the test database until the preservation assertion has
completed.

## Acceptance Criteria

1. The original checkout has no new changes from this work.
2. The Workbench package resolves to 1.1.2 from the root manifest, while the
   bundled engine, SDK, and CLI consistently report their preserved 1.1.1
   component version.
3. Existing npm, browser, API, desktop, and index checks remain green.
4. The real Debian, NSIS, and DMG installation paths are exercised in CI.
5. The 1.1.1-to-1.1.2 test preserves application data and proves backup restore.
6. Uninstall tests preserve the database directory on every platform.
7. Runtime dependency audit has no advisory and the known high build-tool
   advisory is absent.
8. Qualification is bound to a clean revision and exact artifact digests.
9. Missing external physical-power, security, or load evidence yields
   `BLOCKED`; valid matching evidence permits `PASS`.
10. No publishing, signing, public-release mutation, or fabricated evidence is
    performed by this implementation branch.
