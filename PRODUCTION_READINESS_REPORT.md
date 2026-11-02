# PacificDB v1.2 production-readiness report

**Release decision: INCOMPLETE. Do not describe v1.2 as production-qualified.**

The public company-testing release remains `v1.2.0-rc.2`, source
`e3d70761f03341acc13130d17b75fed2bc758a29`. The unreleased ANN/launcher fix is
committed on `v1.2` at `398316ce68bbff73a4446b4fe0f531a24f4601fd`.
Its scale-test driver is committed separately at `9de39d9`.
The default branch and the dependency integration PR do not yet promote this
vector subsystem to a stable release.

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
- Dependency updates and publication-guard repairs are reviewed together in
  [PR #53](https://github.com/hitesh-reddy-k/pacificdb-community/pull/53).
  Existing PRs are retained until that integration is verified and merged.

## Evidence and acceptance status

PASS below applies only to the stated test scope.

| Scope | Status | Evidence and limits |
| --- | --- | --- |
| Exact search, schemas, sparse/named records and existing vector API regressions | PASS | Local 78-check checkpoint including 50 CTest cases; exact source fingerprints and logs retained. Fresh frozen-source [qualification](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41454844350) includes dedicated failpoints and installed SDK checks. |
| Document/media/auth/WAL/LSM/snapshot/RF3 regressions | PASS | [Frozen-source full Community CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41455594933) and local suite: 21 genuine ENOSPC categories, sustained 64/128-client smoke, Python/Java TCP/TLS and SIGKILL recovery. This does not establish an unlimited workload capacity. |
| Request/cancellation/scan-budget isolation | PASS | Targeted real-storage and public-API tests; three instrumented ThreadSanitizer tests; no deadline extension or budget reset across retries. |
| Exact-artifact RF3 rolling upgrade | PASS | Published RC.2 to candidate: follower replacement, interrupted follower resume, leader replacement, writes during upgrade and full restart; 29 acknowledged records verified. Other version pairs/downgrades remain unqualified. |
| 500-record mixed ANN availability | PASS | 30 overlapping write/read checks and 360/360 strict mixed reads; no exact fallback. |
| 10K GloVe-25 cosine ANN lifecycle | PASS | 100 reference queries, Recall@10 1.0; all 360 mixed reads, acknowledged mutations/deletions, SIGKILL and reconstruction; sampled recovery recall 1.0. |
| 100K GloVe-25 cosine ANN lifecycle | PASS | Same acceptance conditions as 10K; Recall@10 1.0 in initial and sampled recovery checks. |
| New 1M ANN lifecycle | INCOMPLETE | Full qualification is executing. Smaller passing datasets do not supersede the published RC.2 failure of 0/360 mixed reads. |
| Linux launcher packaging | PASS | Local extracted-package/GTK lookups at eight sizes and packaged sandbox/CRUD/media/backup/restart checks. [Frozen-source desktop CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41454844337) records platform-specific outcomes. |
| New four-target desktop candidate packages | PASS | [398316c desktop CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41454844337): Linux x64, Windows x64, macOS Intel/Apple Silicon fresh/upgrade/runtime checks passed. These candidate packages are not published as a new release. |
| PyPI RC.2 package | PASS | [Publication](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41336261556); independently downloaded wheel installed outside the source tree: 134 tests plus TCP/TLS/SIGKILL recovery passed. |
| Maven Central publication | BLOCKED | Owner namespace/signing setup remains incomplete; public GitHub JAR/POM distribution is available. |
| Stable production release | INCOMPLETE | Remaining implementation and qualification gates below have not been satisfied. |

PyPI and GitHub wheel/sdist contents were compared and match. Archive hashes
are different because of container timestamps; use the selected channel's
checksum. Registry installation does not install the native engine.

## Remaining release blockers

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
- No fresh paired PostgreSQL performance result is claimed for the ANN wait
  fix. The published RC.2 report retains its latency, ingestion, filtering and
  availability failures.

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
