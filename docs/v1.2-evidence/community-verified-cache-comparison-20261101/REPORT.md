# PacificDB v1.2 Community candidate — benchmark report

**Original primary benchmark: PASS. Production release: INCOMPLETE.**
This is a finalized **self-run project benchmark** prepared for company review.
It is not an official PostgreSQL/pgvector benchmark, independent certification,
release publication or evidence that every PacificDB deployment is production-ready.

**Mixed ANN read availability: FAIL** against
an error-free read target for this measured workload. Explicit freshness errors
protect correctness but still count as unavailable reads. Completed static query
phases passed their stated reference/recall checks; PacificDB did not meet every
availability or performance target. Compare ingestion, ANN
latency, maintenance and recovery costs as well as exact-search latency.

Compared versions: PacificDB **1.2.0-dev**, PostgreSQL **18.6**, pgvector **0.8.7**.
The stable PostgreSQL/pgvector versions were verified on 2026-11-01 against the
[PostgreSQL version policy](https://www.postgresql.org/support/versioning/) and
[pgvector tagged changelog](https://github.com/pgvector/pgvector/blob/v0.8.7/CHANGELOG.md).
PostgreSQL 19 RC1 and pgvector 0.9.0 unreleased are not substituted for stable releases.

## Exact and approximate search

Shared GloVe-25 float32 source values, cosine, top-10, full vector/document payloads.
Each entry is the median of three round statistics; percentiles use NumPy linear
interpolation. Each round uses the same 1000
held-out queries (indices 100–1099), after ten warmups. Tuning uses disjoint queries
0–99. p95/p99 are empirical diagnostics, not production tail-latency guarantees.

| Rows | Public path | p50 ms | p95 ms | p99 ms | Serial QPS | Recall@10 strict / tie-aware |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 10,000 | pacific exact | 3.761 | 6.124 | 8.849 | 248.31 | 1.0000 / 1.0000 |
| 10,000 | pacific ann | 5.010 | 8.159 | 10.259 | 184.24 | 0.9949 / 0.9949 |
| 10,000 | postgres exact | 4.873 | 7.708 | 9.126 | 192.24 | 1.0000 / 1.0000 |
| 10,000 | postgres ann | 1.039 | 2.312 | 3.202 | 840.43 | 0.9919 / 0.9919 |
| 100,000 | pacific exact | 9.115 | 13.087 | 14.927 | 104.27 | 1.0000 / 1.0000 |
| 100,000 | pacific ann | 9.192 | 14.766 | 20.146 | 101.06 | 0.9761 / 0.9761 |
| 100,000 | postgres exact | 25.745 | 33.165 | 40.871 | 37.84 | 1.0000 / 1.0000 |
| 100,000 | postgres ann | 1.355 | 2.860 | 3.657 | 667.93 | 0.9777 / 0.9777 |
| 1,000,000 | pacific exact | 49.727 | 61.181 | 71.255 | 19.46 | 1.0000 / 1.0000 |
| 1,000,000 | pacific ann | 18.243 | 22.482 | 27.683 | 53.34 | 0.9777 / 0.9778 |
| 1,000,000 | postgres exact | 112.228 | 138.051 | 146.206 | 8.74 | 1.0000 / 1.0000 |
| 1,000,000 | postgres ann | 2.260 | 3.155 | 6.500 | 422.99 | 0.9896 / 0.9897 |

Exact is compared with exact; ANN is compared with ANN. The exact ratio for each
size is in [machine-readable results](results.json). No general performance
superiority claim follows from one machine/dataset. Strict IDs and a tie-aware
float64 exact reference, payload values, ranking and result cardinality are
checked outside the timed interval. Float32 score tolerance is 1e-6; raw strict
recall is also retained. PacificDB's packed exact index retains float64 values;
PostgreSQL stores vector float32. This precision/memory difference is disclosed.

### Verified payload-cache reuse by round

The first measured round follows tuning and warmups; it is not a cold-cache run.
Later rounds repeat the same held-out queries. PacificDB reuses full rows only
after a complete storage read has certified them for the current collection
version. Every query still traverses/ranks vectors, checks row visibility and
authorization, and validates the final index/source certificate. This is not a
query-result cache. Any collection mutation conservatively invalidates proofs.
The existing bounded row cache retains at most approximately 50K rows per
collection; proof storage adds approximately 40 bytes per entry on this build,
before allocator/alignment effects. PostgreSQL uses its normal server/OS caches;
no equivalent PostgreSQL row-hit metric is inferred.

| Rows | Pacific path | Round | p50 ms | Verified hits / payload fetches | Hit fraction |
| --- | --- | ---: | ---: | ---: | ---: |
| 10,000 | exact | 1 | 4.773 | 4,452 / 10,000 | 44.5% |
| 10,000 | exact | 2 | 3.761 | 10,000 / 10,000 | 100.0% |
| 10,000 | exact | 3 | 3.491 | 10,000 / 10,000 | 100.0% |
| 10,000 | ann | 1 | 5.010 | 37,464 / 40,000 | 93.7% |
| 10,000 | ann | 2 | 4.622 | 40,000 / 40,000 | 100.0% |
| 10,000 | ann | 3 | 6.047 | 40,000 / 40,000 | 100.0% |
| 100,000 | exact | 1 | 9.553 | 367 / 10,000 | 3.7% |
| 100,000 | exact | 2 | 9.115 | 5,044 / 10,000 | 50.4% |
| 100,000 | exact | 3 | 8.769 | 5,044 / 10,000 | 50.4% |
| 100,000 | ann | 1 | 12.205 | 10,117 / 40,000 | 25.3% |
| 100,000 | ann | 2 | 9.089 | 20,116 / 40,000 | 50.3% |
| 100,000 | ann | 3 | 9.192 | 20,116 / 40,000 | 50.3% |
| 1,000,000 | exact | 1 | 50.004 | 7 / 10,000 | 0.1% |
| 1,000,000 | exact | 2 | 49.080 | 501 / 10,000 | 5.0% |
| 1,000,000 | exact | 3 | 49.727 | 501 / 10,000 | 5.0% |
| 1,000,000 | ann | 1 | 18.328 | 556 / 40,000 | 1.4% |
| 1,000,000 | ann | 2 | 18.243 | 1,997 / 40,000 | 5.0% |
| 1,000,000 | ann | 3 | 17.859 | 1,997 / 40,000 | 5.0% |

## Ingestion, build, memory and storage

There is one fresh load and one initial build per product/dataset; query rounds
are repeated on that loaded database. Ingestion/build results are single-run
measurements, not three independent load trials or confidence intervals.

Fresh durable batch ingestion, 1000 records per acknowledged transaction/batch,
including identical payload construction and final flush/checkpoint. No ANN index
exists during the initial load. Both methods use their native batch protocol:
PacificDB insertMany and PostgreSQL COPY inside a committed transaction.

| Rows | System | Ingestion rows/s | Flat build s | ANN source/build s | Peak sampled PSS MiB | Data+index MiB | WAL MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | pacific | 3,353 | 0.23 | 10.47 | 128.4 | 3.3 | 1.5 |
| 10,000 | postgres | 14,925 | 0.00 | 5.96 | 63.2 | 61.3 | 32.0 |
| 100,000 | pacific | 3,053 | 1.56 | 198.30 | 438.5 | 32.6 | 15.2 |
| 100,000 | postgres | 20,520 | 0.00 | 29.79 | 192.2 | 169.6 | 80.0 |
| 1,000,000 | pacific | 2,548 | 15.45 | 2749.01 | 2343.0 | 325.3 | 152.7 |
| 1,000,000 | postgres | 23,505 | 0.00 | 481.88 | 1308.9 | 1088.7 | 400.0 |

PostgreSQL exact has no packed derived index, so its flat-build entry is zero.
Both ANN builds use M=16 and ef_construction=200; source scanning is included for
PacificDB. ef_search is tuned separately through the same fixed search grid,
using a prespecified tuning threshold 0.97 and held-out acceptance 0.95. All tuning
curves/configurations are retained. HNSW randomized construction can vary by run.

Memory numbers are whole-run sampled PSS maxima across live server processes,
including startup/build/search/recovery, not isolated query RAM or guaranteed
peaks. PostgreSQL RSS is not summed for a fair memory comparison because it
repeats shared mappings. CPU and I/O samples are live-process snapshots, not
complete lifetime counters after workers exit. Storage includes each engine's
actual durable artifacts. PostgreSQL preallocates WAL segments; allocated WAL
size is not equivalent to bytes newly written. No write-amplification claim is
inferred from directory sizes.

PacificDB drops the flat derived index before building ANN to respect existing
staging reservations. Records are retained. Exact and ANN phase memory/index
costs must not be interpreted as simultaneously available under these limits.

## Concurrency, mutation visibility and recovery



| Rows | System | Four-client ANN QPS | Update commit p50 ms | Post-ack ANN-ready wait p50 ms | Mixed successful reads | Restart-to-connection s | Additional ANN rebuild s |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | pacific | 571.98 | 23.01 | 106.51 | 10/360 | 1.03 | 11.67 |
| 10,000 | postgres | 1041.72 | 4.98 | 0.00 | 360/360 | 0.62 | 0.00 |
| 100,000 | pacific | 291.58 | 25.36 | 106.42 | 9/360 | 10.31 | 181.35 |
| 100,000 | postgres | 657.28 | 6.79 | 0.00 | 360/360 | 0.62 | 0.00 |
| 1,000,000 | pacific | 125.17 | 19.38 | 102.99 | 26/360 | 99.04 | 2158.07 |
| 1,000,000 | postgres | 931.86 | 6.86 | 0.00 | 360/360 | 0.61 | 0.00 |

Four-client static throughput includes connection setup and client validation in
wall time; individual query latency excludes those reference checks.

The mixed workload is 360 attempted reads and 40 acknowledged writes across four
clients. PacificDB index lag/freshness errors count as failed reads and are
retained. Successful-only latency must be read together with that availability.
Returned vector/score consistency and deleted-ID exclusion are checked; snapshot
recall during simultaneous mutations is not inferred. PostgreSQL updates its
index synchronously; PacificDB maintenance is asynchronous, so commit latency
and post-ack visibility wait are both shown. Ten controlled updates have
independent post-update reference checks; five deletes have count/exclusion checks.

Only owned disposable servers are SIGKILLed. Restart checks verify all expected
records and ANN recall. PacificDB definitions survive, but derived ANN is ABSENT
until reconstruction; PostgreSQL's persistent index is recovered and available.
Restart-to-connection alone is not PacificDB ANN recovery time. Full rebuild time
is separately reported. These are process-crash tests, not actual power cuts.
The new primary run preserves each owned dirty fixture after SIGKILL and before
restart. The forensic copy is outside the startup timer; raw total stop/restart
wall time includes that copy and records its separate duration. Recovery is warm
cache, including filesystem activity from fixture preservation.

## Filters and supplemental dimensions/metrics

A 1% metadata equality filter is measured through complete exact plans on both
systems; PacificDB uses its declared complete storage-scan fallback. Ten filter
queries are retained per size; no meaningful tails are claimed for that small set.
Other filter semantics have targeted correctness/regression coverage, not a full
selectivity performance matrix.

The supplemental workloads use deterministic normalized synthetic vectors, 10K
rows and 1000 held-out queries, one run per metric/dimension. They are separate
from GloVe, not substitutes for million-vector high-dimensional qualification.

| Rows | Dimensions | Metric | Path | p50 ms | p95 ms | Recall@10 |
| --- | ---: | --- | --- | ---: | ---: | ---: |
| 10,000 | 128 | cosine | pacific exact | 5.614 | 7.800 | 1.0000 |
| 10,000 | 128 | cosine | pacific ann | 7.741 | 9.078 | 0.9957 |
| 10,000 | 128 | cosine | postgres exact | 3.383 | 3.894 | 1.0000 |
| 10,000 | 128 | cosine | postgres ann | 3.936 | 4.362 | 0.9994 |
| 10,000 | 384 | cosine | pacific exact | 10.654 | 11.240 | 1.0000 |
| 10,000 | 384 | cosine | pacific ann | 13.498 | 14.773 | 0.9536 |
| 10,000 | 384 | cosine | postgres exact | 6.614 | 7.027 | 1.0000 |
| 10,000 | 384 | cosine | postgres ann | 6.351 | 6.544 | 0.9865 |
| 10,000 | 128 | l2 | pacific exact | 5.183 | 5.704 | 1.0000 |
| 10,000 | 128 | l2 | pacific ann | 7.718 | 8.974 | 0.9954 |
| 10,000 | 128 | l2 | postgres exact | 3.301 | 3.861 | 1.0000 |
| 10,000 | 128 | l2 | postgres ann | 4.106 | 5.531 | 0.9994 |
| 10,000 | 128 | dot | pacific exact | 5.324 | 6.147 | 1.0000 |
| 10,000 | 128 | dot | pacific ann | 8.880 | 11.139 | 0.9955 |
| 10,000 | 128 | dot | postgres exact | 4.164 | 5.342 | 1.0000 |
| 10,000 | 128 | dot | postgres ann | 5.110 | 6.802 | 0.9994 |

Sparse/named-vector exact and hybrid capability timings are in
[the supplemental evidence](final-supplement/summary.json). Both hybrid examples
use RRF, but PacificDB BM25 and PostgreSQL ts_rank_cd are different lexical
rankers. Their relevance/latency cannot be presented as identical hybrid algorithms.
Supported sparse precision/format and missing-vector behavior differ and are
validated explicitly. This is a capability diagnostic, not a complete feature
parity certification.

## Hardware and measurement protocol

Intel(R) Core(TM) i7-10850H CPU @ 2.70GHz; kernel 6.12.113+deb13-amd64. Host memory and environment
are recorded in [methodology](final-primary/methodology.json). Both servers use
logical CPUs [0, 1, 2, 3]; clients use [4, 5]. Scratch roots
are private directories on local NVMe/ext4. Warm-cache results are reported; OS
caches are not dropped. No concurrent compiler or qualification suite was scheduled
for the final replay. Unrelated host services were left running; their activity,
thermal conditions and filesystem cache state are not controlled like a lab.

Both systems keep fsync and committed acknowledgement enabled. PostgreSQL has
synchronous_commit/full_page_writes on, shared_buffers 512 MiB, work_mem 64 MiB,
maintenance_work_mem 1 GiB, three gather/maintenance workers within four server
CPUs, JIT off. PacificDB WAL/checksum/fsync/commit flushing are on, the standalone
Raft bypass is disabled, and configured worker/memory budgets are retained.
Authentication is disabled only on loopback for both benchmark servers; security
correctness is tested separately. RF3 correctness qualification is separate from
standalone latency. No RF3-versus-single-node speed comparison is made.

PostgreSQL uses a fresh source build and pgvector's portable OPTFLAGS-empty build;
full configure/compiler logs are retained. Query-plan checks prove PostgreSQL
exact scans avoid ANN and ANN uses vectors_hnsw. Prepared generic-plan reuse is
disabled. Fresh connections are opened before each macro measurement phase on
both systems to avoid idle-session expiry during alternating-engine runs;
connection setup is excluded from query timing. PacificDB requires the named index and reports exact/ANN/fallback status
per response. Selective-filter fallback is reported explicitly. Each round
alternates database measurement order. All timed paths include client socket and
payload decoding; reference checking is outside the timer.

## Implementation and qualification

The candidate preserves authoritative WAL/LSM records, schemas and bounded derived
index maintenance. This checkpoint fixes float64 intermediate overflow producing
incorrect finite cosine scores, adds bounded query ef_search, and exposes rebuild
requirements/status. The pre-fix numeric failure and corrected tests are retained.
The follow-on corrects point reads crossing a flush boundary, late cache refill
after concurrent writes, and Raft visibility advancing during lookup. Eighteen
focused real-storage/real-Raft cases cover those interleavings. Update responses
must report actual mutation success, and every changed/deleted record is checked
before and after the benchmark crash; phase evidence is saved incrementally.
This candidate also qualifies reuse of authoritative point-cache entries: older
writer/replay cache rows cannot override newer SST versions, tombstones, restores
or namespace recreation. Native exact/ANN/hybrid queries retain authoritative
validation while avoiding unconditional cache eviction.

PASS: 75 top-level regression checks including 43 CTest cases;
5 current targeted TSAN tests and 4 ASan/UBSan tests; Node/Python/Java units and live Python/Java
TCP/TLS/SIGKILL recovery. Prior checkpoint evidence supplies broader HNSW mutation
scale/sanitizer coverage for its recorded revision, not an invented rerun here.
Document/media/auth/WAL/LSM/RF3 compatibility is exercised by those tests; arbitrary
production workloads and every upgrade/platform are not certified.

## Release and publication decision

**INCOMPLETE** for production release. This scoped Community candidate permits a
validated per-index locking strategy and explicit manual maintenance rather than
requiring immutable graph snapshots/automatic compaction. It does not advertise
clustered vector schemas/catalogs or complete distributed shard search. Those
unsupported operations remain gated. Persistent history capacity, rebuild time,
lag/error behavior and lack of online schema migration are documented limits.

Actual power-loss evidence, complete upgrade/platform packaging, independent
security assessment and sustained production qualification remain unfinished.
Existing release-audit rules are not disabled. The uncommitted tree cannot be
published as a source-bound stable release under that audit. Preparing this local
report does not authorize a commit, push, public upload or deployment.

The benchmark was requested as a company submission. These results may be described
as **PacificDB's self-run candidate benchmark with attached methodology**, not as
an independently certified result or a PostgreSQL/pgvector-endorsed benchmark.

## Reproduction and evidence

Candidate SHA-256: `8691496829b0cafc9e04b793058e5e2e860d4351082d852715348ca797dc4a38`.
Source baseline commit: `8732d299008e6cbc581f76e7c2a1eb2f7dad703f`, branch v1.2 with attached
uncommitted source changes. See [source fingerprints](source-sha256.json),
[source archive](source-checkpoint.tar.gz), [dependency provenance](version-provenance.json),
[regressions](regressions/summary.json), [results](results.json),
[primary raw records](final-primary/summary.json),
[supplemental raw records](final-supplement/summary.json), and
[scoped Community contract](../../V1.2_COMMUNITY_SCOPE.md).

Use a fresh evidence directory, isolated build/data directories, the exact dataset
manifest and pinned PostgreSQL/pgvector source versions. Run all qualification
before timing. The complete scripts and toolchain/configuration are attached.

```bash
/path/to/venv/bin/python scripts/benchmark-community-v12.py \
  --build build-v1.2-verified-cache \
  --pg build-v1.2-official-deps/postgres18/bin \
  --publication /path/to/shared-glove-source \
  --output /path/to/empty-primary-evidence --scratch /path/to/private-ext4-scratch \
  --sizes 10000,100000,1000000 --queries 1000 --rounds 3 --warmups 10 --startup-timeout 900
/path/to/venv/bin/python scripts/benchmark-community-v12-supplement.py \
  --build build-v1.2-verified-cache \
  --pg build-v1.2-official-deps/postgres18/bin \
  --output /path/to/empty-supplement-evidence --scratch /path/to/private-ext4-scratch \
  --queries 1000
```

Earlier runs remain separate in [historical attempts](historical-attempts/README.md).
They include the idle-session fixture failure, the October 31 million-row restart
deadline/changed-row failure and the interrupted November 1 pre-cache comparison.
Their engine hashes differ from this candidate. None is retroactively marked PASS
or pooled into these measurements. The smoke run is harness validation and excluded
from final results. Failed configuration, numeric and cache RED logs are preserved.
No failed run is silently replaced or counted as PASS.

## Additional operational measurements

See [configuration disclosures](METHODOLOGY_ADDENDUM.md) for the fixed-concurrency admission, timeout, memory, authentication and logging settings.

Filtered plans return complete exact results on both systems; PacificDB uses its declared storage-scan fallback. Only ten filter queries per size are measured, so this table makes no tail-latency claim.

| Rows | System | 1% filtered exact p50 ms | Concurrent ANN p95 ms | Delete commit p50 ms | Mixed write p50 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| 10,000 | pacific | 93.798 | 9.073 | 21.699 | 35.633 |
| 10,000 | postgres | 1.455 | 5.666 | 1.832 | 5.600 |
| 100,000 | pacific | 959.205 | 16.185 | 19.030 | 39.176 |
| 100,000 | postgres | 11.004 | 10.604 | 2.044 | 6.642 |
| 1,000,000 | pacific | 7031.488 | 40.124 | 17.895 | 50.429 |
| 1,000,000 | postgres | 76.904 | 9.236 | 1.681 | 6.969 |

| Rows | System | Ingestion elapsed s | Batch acknowledgement p95 ms |
| --- | --- | ---: | ---: |
| 10,000 | pacific | 2.98 | Not reported (<100 batches) |
| 10,000 | postgres | 0.67 | Not reported (<100 batches) |
| 100,000 | pacific | 32.76 | 454.946 |
| 100,000 | postgres | 4.87 | 57.859 |
| 1,000,000 | pacific | 392.47 | 520.307 |
| 1,000,000 | postgres | 42.54 | 44.301 |

The serial query statistics count successful verified static queries. The mixed workload separately counts every attempted strict ANN read, including freshness/unavailability errors. Higher static recall does not erase failed read availability. All raw per-query execution metadata and mutation records remain attached.

These tables were appended by the attached `complete-report.py` from `results.json`; the timed harness, candidate binary and archived engine sources were unchanged.
