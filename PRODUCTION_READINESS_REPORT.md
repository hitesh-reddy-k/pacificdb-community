# PacificDB v1.2 production-readiness report

**Release decision: INCOMPLETE. Do not describe v1.2 as production-qualified.**

The public company-testing release remains `v1.2.0-rc.2`, source
`e3d70761f03341acc13130d17b75fed2bc758a29`. The unreleased ANN/launcher fix is
committed on `v1.2` at `398316ce68bbff73a4446b4fe0f531a24f4601fd`.
Its scale-test driver is committed separately at `9de39d9`.
The default branch and the dependency integration PR do not yet promote this
vector subsystem to a stable release.

The fresh three-database 1M trial completed with exit code 0 on 2026-11-02 UTC.
[Report and portable raw receipts](benchmarks/results/three-db-1m-20261102/README.md)
compare the verified development runtime against PostgreSQL 18.6/pgvector0.8.7
and Qdrant 1.20.0. All three passed the stated correctness, mixed-read and
process-crash workload. This does not establish performance parity: PacificDB
ANN, ingestion, filtered search, build and reconstruction remain materially
slower in this trial. Published RC.2 program assets and its historical report
are unchanged.

## Implemented follow-up

- Strict ANN queries against initialized incremental indexes can wait briefly
  for freshness. `index_wait_ms` defaults to 1000, accepts 0–5000, and zero
  retains fail-fast behavior. The original deadline, cancellation and scan
  budget span attempts. Exhausted budgets and permanent failures remain errors.
- Queries still revalidate authoritative visibility; no stale results or
  implicit exact fallback are introduced. Mutation acknowledgements, WAL/LSM
  formats and RF3 consensus are unchanged by this fix.
- The Linux Workbench package selects its existing standard-size logo assets.
  A real GTK resolver test now checks the installed launcher resource in CI.
- Dependency updates and publication-guard repairs passed combined qualification
  and merged through [PR #53](https://github.com/hitesh-reddy-k/pacificdb-community/pull/53),
  commit `2d7493a8...` on `main`. PRs #38–#52 were closed as superseded, with
  their branches/history preserved. That cleanup found no open PRs before the
  separate security follow-up PR #54 was opened. The
  [cleanup receipt](benchmarks/results/ann-availability-398316c/pr-cleanup.json)
  links the exact-head tests. The updates are synchronized into `v1.2` at
  `0c86f579...`, with engine/storage code unchanged; new combined branch CI
  is qualified separately from the frozen-engine scale run.

## Evidence and acceptance status

PASS below applies only to the stated test scope.

| Scope | Status | Evidence and limits |
| --- | --- | --- |
| Fresh 1M three-database workload correctness/lifecycle | PASS | [Complete report](benchmarks/results/three-db-1m-20261102/REPORT.txt): three 1,000-query rounds, exact reference agreement, held-out ANN Recall@10 0.9787/0.9900/0.9868 for PacificDB/PostgreSQL/Qdrant, all 360 mixed reads per engine, 45 changed records/five deletions recovered and controller exit 0. Shared active workstation, 25-dimensional inputs and native protocol differences limit conclusions. |
| ANN/ingestion/filter/build performance parity | FAIL | PacificDB ANN19.83ms versus2.19/0.91ms p50; load3860 versus32140/25282rows/s;1% filtered exact6644.87 versus63.53/867.01ms. Initial graph2040.66s; reconstruction2441.07s. These are measured workload comparisons, not universal rankings. |
| Latest exact-source hosted qualification | PASS | [Seven completed workflows](docs/v1.2-evidence/security-hardening-20261102/hosted-workflows.json): `v1.2` at `79fdf775...` and PR #54 at `836bd87...`; source, SDK/artifact, CodeQL, four native installers and four desktop targets. [Fresh checkpoint](docs/v1.2-evidence/security-hardening-20261102/ci-checkpoint.json): 79 checks / 52 CTest cases. |
| Security PR merge and repository protections | PASS | PR #54 merged normally as `ea771868...` after exact-head checks; fresh PR query found none open. [Effective controls](docs/v1.2-evidence/security-hardening-20261102/repository-controls.json): one approval, stale-review dismissal, named cross-platform checks and both secret protections. This is not independent security signoff. |
| Rebuilt security candidate checkpoint | PASS | [79 top-level checks / 52 CTest cases](docs/v1.2-evidence/security-hardening-20261102/checkpoint.json), including dedicated crash tests. The retained 266 source fingerprints match the later QA-only source changes. |
| Rebuilt security candidate full Community regression | PASS | [Scoped receipt](docs/v1.2-evidence/security-hardening-20261102/community-regression.json): all 21 ENOSPC categories, RF3/64–128-client smoke, Node/CLI, 134 Python tests and Java/live TCP/TLS/SIGKILL checks. First attempt failed final website branding; the corrected full rerun passed. |
| Security candidate monitoring and exact-artifact rolling upgrade | PASS | [Live mTLS alert recovery](docs/v1.2-evidence/security-hardening-20261102/monitor-live-e2e.json) and [RC.2-to-candidate RF3 upgrade](docs/v1.2-evidence/security-hardening-20261102/rf3-upgrade.json), including isolated restore and all 29 acknowledged records. Each receipt retains its actual revision/hash. |
| Exact search, schemas, sparse/named records and existing vector API regressions | PASS | Local 78-check checkpoint including 50 CTest cases; exact source fingerprints and logs retained. Fresh frozen-source [qualification](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41454844350) includes dedicated failpoints and installed SDK checks. |
| Document/media/auth/WAL/LSM/snapshot/RF3 regressions | PASS | [Frozen-source full Community CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41455594933) and local suite: 21 genuine ENOSPC categories, sustained 64/128-client smoke, Python/Java TCP/TLS and SIGKILL recovery. This does not establish an unlimited workload capacity. |
| Request/cancellation/scan-budget isolation | PASS | Targeted real-storage and public-API tests; three instrumented ThreadSanitizer tests; no deadline extension or budget reset across retries. |
| Exact-artifact RF3 rolling upgrade | PASS | Published RC.2 to candidate: follower replacement, interrupted follower resume, leader replacement, writes during upgrade and full restart; 29 acknowledged records verified. Other version pairs/downgrades remain unqualified. |
| 500-record mixed ANN availability | PASS | 30 overlapping write/read checks and 360/360 strict mixed reads; no exact fallback. |
| 10K GloVe-25 cosine ANN lifecycle | PASS | 100 reference queries, Recall@10 1.0; all 360 mixed reads, acknowledged mutations/deletions, SIGKILL and reconstruction; sampled recovery recall 1.0. |
| 100K GloVe-25 cosine ANN lifecycle | PASS | Same acceptance conditions as 10K; Recall@10 1.0 in initial and sampled recovery checks. |
| 1M GloVe-25 ANN lifecycle on frozen 398316c | PASS | [Portable raw evidence](benchmarks/results/ann-availability-398316c/1m.json.gz): all 360 strict mixed reads; initial Recall@10 0.993; 45 surviving acknowledged changes and five deletions verified after SIGKILL; count 999995; reconstructed ANN Recall@10 0.96. This uses ef_search=600 and is not a controlled performance comparison with published RC.2 ef_search=200. |
| Linux launcher packaging | PASS | Local extracted-package/GTK lookups at eight sizes and packaged sandbox/CRUD/media/backup/restart checks. [Frozen-source desktop CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41454844337) records platform-specific outcomes. |
| New four-target desktop candidate packages | PASS | [398316c desktop CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41454844337): Linux x64, Windows x64, macOS Intel/Apple Silicon fresh/upgrade/runtime checks passed. These candidate packages are not published as a new release. |
| PyPI RC.2 package | PASS | [Publication](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41336261556); independently downloaded wheel installed outside the source tree: 134 tests plus TCP/TLS/SIGKILL recovery passed. |
| Maven Central publication | BLOCKED | Owner namespace/signing setup remains incomplete; public GitHub JAR/POM distribution is available. |
| Stable production release | INCOMPLETE | Remaining implementation and qualification gates below have not been satisfied. |

PyPI and GitHub wheel/sdist contents were compared and match. Archive hashes
are different because of container timestamps; use the selected channel's
checksum. Registry installation does not install the native engine.

## Remaining release blockers

Security follow-up: repository scanning findings are being reviewed separately
from successful CI execution. Confirmed logging/private-I/O and source-evidence
issues have targeted fixes in
[PR #54](https://github.com/hitesh-reddy-k/pacificdb-community/pull/54).
The rebuilt-engine checkpoint, complete Community rerun, live mTLS monitoring
and exact-artifact rolling upgrade passed for their stated scopes. The first
Community run failed a final website wording check; its logs are retained. A
Windows CI test peer also needed explicit handling of its expected reset;
queued-reconnect assertions remain intact and a deterministic failing control
is retained. Hosted exact-head platform and scan qualification is tracked
separately and all seven final workflows passed for the recorded heads. The full
`v1.2` scan cleared ten findings; six remain for contextual and independent
review. Findings are not dismissed merely to obtain a passing status. PR #54
has merged; published RC.2 program assets remain unchanged. The source-review
bundle still requires an independent result available to this workflow. The
owner reports a completed review on another system; that report and its exact
revision-bound result have not been supplied here, so local validation remains
unverified rather than an invented PASS.

- Physically immutable incremental ANN generations and automatic graph
  compaction are incomplete. The current graph uses per-index synchronization
  and final freshness validation; configured history/capacity limits can require
  explicit rebuilding.
- Durable ANN graph checkpoints/replay are incomplete. Authoritative vectors
  and definitions are durable, but graphs require reconstruction after restart.
- Complete distributed vector routing, shard record transfer and result
  completeness are not qualified; affected operations remain gated.
- Full sustained, high-dimensional, resource-limit, platform/upgrade and
  independent security/physical-power-loss qualification remains incomplete.
  Crash injection and unit tests do not replace those gates.
- Release signing/notarization and managed-device endpoint acceptance are not
  fully qualified. Current prerelease installation results are scoped evidence.
- The new three-database comparison is development-runtime evidence. It does
  not replace the published RC.2 results or qualify every architecture/release
  gate. PacificDB's 360/360 mixed-read result passes availability for this
  workload; the remaining performance gaps are retained in the report.

## Reproduce the scale qualification

Prepare the checksum-frozen inputs using the
[company reproduction guide](docs/releases/v1.2.0-rc.2/BENCHMARK_REPRODUCTION.md).
Use the benchmark Python environment with NumPy, psutil, psycopg and this
checkout's Python client. Build the selected engine revision, then run:

```sh
python scripts/test-vector-ann-availability.py \
  --build build-v1.2-critical-fixes \
  --publication /path/to/prepared-publication \
  --output /path/to/empty-evidence-directory \
  --scratch /path/to/disposable-scratch \
  --rows 1000000
```

The runner records input/binary/script hashes, actual engine revision,
per-query checks, every mixed-read error, mutation verification, recovery and
resource samples. Any failed mixed read makes qualification FAIL. It is a
single-system availability/correctness test, not a paired latency benchmark.

See [detailed validation](docs/V1.2_VECTOR_VALIDATION.md) and the
[release checklist](RELEASE_CHECKLIST.md). Unexecuted checks remain INCOMPLETE;
no stable tag or replacement RC.2 program assets have been published here.
