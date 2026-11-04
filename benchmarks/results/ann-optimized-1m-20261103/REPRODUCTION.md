# Reproduce the three-database vector comparison

This protocol measures one standalone database at a time. It does not compare
RF3/clustered deployments or qualify a production release. The workload is the
checksum-frozen ANN-Benchmarks GloVe-25 cosine dataset: 1,000,000 rows, 25 float32
input dimensions, K=10. Modern high-dimensional embeddings need separate trials.

## Inputs and runtime identity

This optimization trial uses an unreleased candidate. Actual runtime SHA-256: `8c293646b8a9ddb54c286268f5e021b660c95e0c2255bb8da1251539b7b1338f`. Base source commit: `aaf12a913087cf50e46504ac31a90319eb7241ce`; apply `provenance/qualified-source.patch.gz` after decompressing it. The patch includes added source files. Qualification/source fingerprints and methodology executable/driver/client hashes identify the measured inputs. Published RC.2 binaries are unchanged.

Comparators: PostgreSQL18.6, pgvector0.8.7, Qdrant1.20.0 native Linux x64.
Qdrant's official release archive SHA-256 is
`2361f3df0dedfef31f5444f9f15bd15d6b4a6cba6ad952daecc5fbe3867c5ee6`.
Obtain it from the [official release](https://github.com/qdrant/qdrant/releases/tag/v1.20.0)
and verify the digest before extracting. Native database dependencies and local
compiler/runtime versions must be recorded. Do not use an existing live cluster.

Prepare/verify the original inputs with `scripts/prepare-community-benchmark.py`.
The [earlier reproduction guide](../docs/releases/v1.2.0-rc.2/BENCHMARK_REPRODUCTION.md)
describes dataset preparation and the PostgreSQL private-prefix layout; its
published RC.2 binary identity and two-system runner do not apply to this trial.
The new driver checks `base.npy`, `queries.npy`, and `truth.npz` against the
prepared manifest before launching any server. Use the same data on every system.

## Environment and command

Use a nonroot Linux x64 account, six available logical CPUs, at least16GiB RAM,
and at least30GiB free NVMe/local block-storage space for retained crash fixtures.
This self-run trial uses a shared workstation; platform, memory and background
load are recorded in methodology.json. Native protocol and ordering effects
remain limitations.
A dedicated idle host is preferable for independent reproduction.

Install the benchmark dependencies in an isolated Python environment (Python3.11+
with NumPy, psutil and psycopg3) and install/use the matching PacificDB Python
client. Keep the driver/helper/client versions with the evidence. The driver
records native executable/plugin/script hashes and actual build-info responses.
For PostgreSQL, supply a private prefix with `bin/initdb`, `bin/postgres` and
`lib/vector.so`, built against that prefix's `pg_config`.

Use empty output and disposable scratch locations. Scratch must be on the storage
being compared; do not use RAM-backed `/tmp` if reporting NVMe results.

```sh
python scripts/test-three-vector-benchmark.py
python scripts/test-community-benchmark.py
python scripts/benchmark-three-vector-databases.py \
  --build /path/to/verified-pacific-build \
  --pg /path/to/postgres18/bin \
  --qdrant /path/to/qdrant \
  --publication /path/to/prepared-inputs \
  --output /path/to/empty-evidence \
  --scratch /path/to/disposable-nvme-scratch \
  --rows 1000000 --queries 1000 --rounds 3 \
  --systems postgres,qdrant,pacific
python scripts/report-three-vector-benchmark.py \
  /path/to/empty-evidence/summary.json --output /path/to/report
```

For this trial, wrap the entire three-database command in `powerprofilesctl
launch --profile performance --reason "Matched database benchmark" --appid
io.pacificdb.benchmark`, using an available performance profile on AC power.
Record the original profile and explicitly restore it afterward. Record actual
profile/frequency observations as the controller does; no fixed-clock or thermal
isolation is claimed. The earlier power-saver partial trial is retained separately.

Run through a controller that retains its PID, logs, heartbeat and **exit code**.
The output directory is never reused. Preserve failed/interrupted attempts;
RUNNING or an old BUILDING log is not a completed benchmark. Engines are stopped
between systems; the runner's crash signal targets only its own process group.
Unrelated services and data are never benchmark targets.

## Common measurement contract

- Four server logical CPUs and two client CPUs; a sampled6GiB PSS guard. This is
  not hard-cgroup enforcement or protection from unrelated host activity.
- Same batch1000, payload fields, vector inputs and full top10 result payloads.
  PostgreSQL uses idiomatic COPY; PacificDB/Qdrant use their batch APIs. Native
  TCP, SQL and HTTP serialization/transport costs remain part of end-to-end timing.
- Ingestion throughput ends at the last synchronous batch ACK for every engine.
  PacificDB storage flush and PostgreSQL CHECKPOINT/ANALYZE are timed separately.
- PacificDB/PostgreSQL fsync and committed acknowledgements remain enabled.
  Pinned Qdrant1.20 `update_worker_internal` explicitly flushes WAL for `wait=true`;
  writes also request `ordering=strong`. Source/configuration and process-crash
  checks are not physical-power-loss certification.
- Same M16/efConstruction200. Tune efSearch on queries0–99 through the same grid
  until mean Recall@10 >=0.97; measure queries100–1099 with target >=0.95.
  Exact search is separate and must match independent reference results.
- Three query rounds reuse one built index and the same held-out inputs. These
  are not three independent ingestion/build experiments. No best-round selection.
- Qdrant normalizes cosine vectors; returned vectors are checked against the
  expected normalized value. Original vector magnitudes are not fabricated.
  PacificDB's compatible legacy float64 storage is preserved.
- One system at a time in a fixed order avoids combined RAM pressure but retains
  order/cache/temperature/background-load effects. No cache dropping is performed.
- 1% selective filters use exact search. Ten filtered/recovery queries do not
  justify tail-latency or broad recall confidence claims.
- Four-client static ANN must meet the recall gate. The mixed workload requires
  all360 reads plus40 acknowledged writes; error latency/counts remain visible.
- Recovery verifies45 surviving changed records, five deletions and exact count
  after SIGKILL. PacificDB manual graph reconstruction/source-validated checkpoint import is timed explicitly and reuse status is recorded. Changed sources reject stale caches.
- Qdrant may pause optimization after restart. If grey, the runner reapplies an
  existing metadata value once with a synchronous ACK, records that action and
  includes it in index-readiness time. No logical payload value is changed.
  Collection counters include tombstones and are approximate; a healthy optimizer
  and reported indexed counts do not prove per-query physical graph coverage.
  `exact=false` allows the engine's planner choice; this limitation is disclosed.
- PostgreSQL post-restart EXPLAIN must select its persistent HNSW index. Every
  system's returned IDs, values, metadata and scores are independently checked.

## Evidence and interpretation

`methodology.json`, `ground-truth.json`, `summary.json`, heartbeat, server logs,
per-query JSONL, tuning curves, per-round records, mixed-worker records,
acknowledged-batch offsets and lifecycle receipts are retained. Validation errors
include their offending response and prior successful attempts. Samples record
RSS/PSS/CPU/I/O; use PSS for cross-process memory comparisons because summed
PostgreSQL RSS can count shared pages repeatedly.

The report distinguishes missing measurements from zero, and failed availability
from successful-query latency. Reported disk size includes preallocated WAL in
live data directories and excludes forensic copies. PacificDB's derived graph is memory-resident during queries. A separate,
checksummed topology checkpoint is reported as derived disk and included in
total disk. Reuse still requires scanning and fingerprinting authoritative
committed records. Incremental edits can invalidate the saved checkpoint;
that case rebuilds safely. Neither graph memory nor its cache establishes
authoritative record durability.

Earlier baseline and diagnostic attempts are retained separately. The initial
backfilled-neighbor optimization passed its 100K correctness/recall checks but
regressed build time and was replaced. Host shutdowns interrupted qualification
attempts before the final supervised run. No partial phase or diagnostic timing
is substituted into this trial. Read the qualification receipts and REPORT.txt
for actual pass/fail outcomes; a reproduction protocol is not a passing result.
