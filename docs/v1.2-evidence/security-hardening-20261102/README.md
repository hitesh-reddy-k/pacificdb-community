# Security and reliability follow-up — 2026-11-02

Status: **INCOMPLETE for production release**. This directory records scoped
qualification, not a stable release or an independent security assessment.
Published RC.2 artifacts and their failed mixed-ANN benchmark are unchanged.

## Implemented

- Request-bounded ANN freshness waits and launcher packaging are qualified
  separately in the [10K/100K/1M receipts](../../../benchmarks/results/ann-availability-398316c/README.md).
- Caller-owned timestamp conversion; private creation of new POSIX WAL-integrity
  files; descriptor-based monitor credential validation and exclusive temporary
  output publication. Existing WAL serialization/fsync and file modes remain.
  POSIX integrity writes now reject final-component symlinks. The monitor's
  descriptor-based credential reader is POSIX-only and fails explicitly on
  unsupported systems; Windows native engine/SDK behavior is preserved.
- Source-review dependency evidence comes from the archived revision; unsafe
  inputs and nonempty destinations are rejected. Source-only assessment does
  not qualify unrelated binaries.
- Effective default-branch protection checks replace an obsolete hardcoded
  branch; missing rules/access fail closed. Negative policy tests are included.

## Retained evidence

- [79-check checkpoint](checkpoint.json), including all 52 CTest cases and
  dedicated media/schema/Raft-apply/incremental-ANN crash tests, passed on
  `b114a31d57769af98eef8638b312bee8700ae803`.
- [Source fingerprints](source-sha256.json) identify all 266 checked C++ sources,
  tests, CMake and checkpoint-driver inputs. Later documentation/QA-only commits
  are not the same executable identity; each receipt retains its actual hash.
- [Live monitor receipt](monitor-live-e2e.json): real mTLS, least privilege,
  readiness refusal/liveness continuity during audit failure, and Alertmanager
  receiver failure/recovery. The test uses an exact pinned container digest.
- [RF3 upgrade receipt](rf3-upgrade.json): published RC.2 to the security candidate,
  interrupted follower resume, leader replacement, isolated backup restore and
  full restart; all 29 acknowledged fixture records survived.

The [broader Community rerun](community-regression.json) and all
[seven exact-head hosted workflows](hosted-workflows.json) passed. The fresh
[`79fdf775...` checkpoint](ci-checkpoint.json) passed 79 checks / 52 CTest cases.
PR #54 merged as `ea771868...` after its 29 exact-head checks completed;
[effective repository protections](repository-controls.json) then passed.
The PR inventory was empty at the final check.
[Targeted timestamp ThreadSanitizer, ten private-monitor tests, seventeen
source-review tests and nine repository-control tests](targeted-qa.json) passed.
Three normal-sandbox browser layout cases also passed. The first full Community attempt reached the final
branding check and failed a future-edition website sentence; functional tests
had passed. The wording was corrected and the whole suite passed on rerun. A Windows
accepted-peer reset in the Node timeout test was then handled explicitly; the
request-rejection and both queued-reconnect assertions remain intact, and an
injected peer-reset control fails without the fixture handler. Original
failed logs remain in local/CI evidence; they are not overwritten or counted PASS.

## Remaining gates

A successful scan is not an independent review. Remaining findings need
contextual disposition, and external security and physical power-loss evidence
are absent. Distributed completeness, immutable incremental graph generations,
automatic graph compaction, durable graph checkpoints and full sustained/high-
dimensional qualification remain incomplete. Signing/notarization and Maven
Central retain their own gates. See the
[current readiness report](../../../PRODUCTION_READINESS_REPORT.md) and
[review handoff procedure](../../SECURITY_REVIEW.md).

Only sanitized fixture receipts are distributed here. Local absolute paths,
credentials, private finding details and production data are omitted.
