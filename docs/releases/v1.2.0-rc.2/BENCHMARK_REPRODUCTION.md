# RC.2 benchmark reproduction: current artifact identity

RC.2 is independently measured. Do not substitute the historical RC.1 numbers.
The source, parameters, frozen GloVe25 inputs and methodology below are shared;
the installed artifact/compiler/runtime identities differ and are recorded.

| RC.2 input | Identity |
| --- | --- |
| Git tag | `v1.2.0-rc.2` |
| Source | `e3d70761f03341acc13130d17b75fed2bc758a29` |
| Linux archive SHA-256 | `2b6700a205865c0b2d1af2bddc51fccb59cc9de5d0506894ee2059dd4af765d5` |
| Engine SHA-256 | `1faed8f998e9410101816643659e04ca1230fad033ae54ba0c59cefdc61d83a0` |
| Compiler/native target | GCC13.3, Ubuntu24.04 x64 |
| Test host | Debian13 x64, with privately extracted Ubuntu RE2 runtime |
| Comparator | PostgreSQL18.6 / pgvector0.8.7, GCC14 source build |
| Harness | Unchanged main SHA-256 `f416a11c1f6d6a4cd6f6dd71ce90f82b8c0cdfcb22f704b05e91c6196310850d` |

The RC.2 run uses the exact tested CI archive cached locally, checked against the
now-public release asset digest before starting; no local development executable
was substituted. Independent public installation tests separately download and
verify the released packages on all desktop targets. All declared study phases
completed with exit0 and summaryPASS. [Final report](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/BENCHMARK_REPORT_1.2.0-rc.2.txt) and
[public evidence](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/pacificdb-benchmarks-1.2.0-rc.2-20261101.tar.gz) retain measured failures and limitations.
Study execution completion does not mean every request succeeded or production
qualification passed. Mixed strict ANN availability at1M was0/360 versus360/360.

On Ubuntu24, use its normal package-provided RE2 dependency. The Debian13 benchmark
host extracted Ubuntu's `libre2-10_20230301-3build1_amd64.deb` into a private runtime
root and set LD_LIBRARY_PATH for the benchmark only. Its verified SHA-256 is
`4ce429c1254f57c96feb80536fe4418ea326603fa797d8311a50f946273fd29b`;
[official package/checksum](https://packages.ubuntu.com/noble/amd64/libre2-10/download).
No system package was replaced. This compatibility execution is not a Debian13
native-package support claim. The runtime/compiler difference prevents treating
RC.1 timings as RC.2 measurements.

Run long workloads in a detached controller that records PID, heartbeat, phase
exit code and final result. Check that both controller and phase processes are
alive; an old BUILDING log line alone is not proof. Preserve interrupted attempts
as INCOMPLETE. Do not continue a controllerless server and infer a reconstruction
time or successful final qualification. RC.1's interrupted attempt is retained.

The adapted protocol follows. Its frozen dataset hashes, durability, payload,
precision, resource, warm-cache and tail-percentile limits still apply. The
original preparation needs Python3.12+ for NumPy2.5.4; execution uses Python3.11+
and recorded NumPy2.2.4/psutil7.0.0/psycopg3.3.6 with the released1.2.0rc2 SDK.

---

# Reproducing the published v1.2 candidate comparison

This is a self-run, disclosed benchmark of **Pre-release v1.2**, not independent
certification. Report every failure, index build and recovery cost. Do not compare
only successful requests or describe this candidate as production-ready.

## Frozen identities

| Input | Identity |
| --- | --- |
| PacificDB release | `v1.2.0-rc.2`, base engine `1.2.0` |
| Engine source | `e3d70761f03341acc13130d17b75fed2bc758a29` |
| Linux native archive SHA-256 | `2b6700a205865c0b2d1af2bddc51fccb59cc9de5d0506894ee2059dd4af765d5` |
| Benchmarked engine SHA-256 | `1faed8f998e9410101816643659e04ca1230fad033ae54ba0c59cefdc61d83a0` |
| PostgreSQL | `18.6` |
| pgvector | `0.8.7` |
| Main harness SHA-256 | `f416a11c1f6d6a4cd6f6dd71ce90f82b8c0cdfcb22f704b05e91c6196310850d` |

Download the engine archive, Python wheel, source archive, manifest and checksums
from the [actual prerelease](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/v1.2.0-rc.2).
Verify the files against `SHA256SUMS` before extracting/installing. The recorded
post-publication run used the exact tested CI archive, checked against public
release asset digests before execution. Independent public-install verification
is recorded separately.

The benchmark is Linux-only: it uses `taskset`, process groups and Linux process
memory accounting. Windows/macOS installer tests are separate evidence. This
workload is standalone versus standalone, not RF3 or distributed search.

## Environment

Use a normal nonroot account on a dedicated Linux x64 host with at least six
available logical CPUs, 16 GiB RAM and enough free scratch space for both
databases plus retained crash fixtures. Allow at least 30 GiB free space and
monitor it. The original host was an Intel Core i7-10850H with 16 GB RAM on
Debian 13, kernel `6.12.113+deb13-amd64`. Four logical CPUs are assigned to the
servers; two to the Python client. This does not prevent unrelated host activity.

Install PostgreSQL **18.6** and pgvector **0.8.7** into a private prefix whose
`bin/` contains `initdb` and `postgres` and whose `lib/vector.so` is installed by
pgvector. Distribution packages with a different directory layout need a matching
private installation for this harness; do not point it at a production cluster.
The recorded PostgreSQL source build used GCC 14.2, the normal Release defaults,
`--without-icu --without-readline`, and its own absolute prefix. Build pgvector
against that prefix's `pg_config`. Source versions and hashes, rather than locally
recompiled binary hashes, identify the equivalent comparator on another host.

Use Python **3.11+** for benchmark tooling (`hashlib.file_digest` is required).
The published Python SDK supports its separately tested runtime range. Install
the downloaded wheel into a private environment:

```sh
python3 -m venv benchmark-venv
benchmark-venv/bin/python -m pip install \
  numpy==2.2.4 psutil==7.0.0 'psycopg[binary]==3.3.6' \
  ./pacificdb-1.2.0rc2-py3-none-any.whl
benchmark-venv/bin/python -m pip freeze > benchmark-python-environment.txt
```

## Dataset and independent truth

Download the original [ANN-Benchmarks GloVe25 angular HDF5](https://ann-benchmarks.com/glove-25-angular.hdf5).
Its SHA-256 must be
`51004cb0ae962159f0db507a51fec2b395de14b166f55976c89f16bd2f8b6391`.
The first 1,000,000 training vectors are the base. NumPy's
`default_rng(20261029).permutation(10000)[:1100]` selects the query source rows.
The first 100 queries are tuning-only; the next 1,000 are held out. Base/query
rows are normalized in float64, then stored in float32 `.npy` files.
Ground truth uses exhaustive float64 cosine calculations on those stored vectors.

The portable preparation helper is available on the `v1.2` branch at
`scripts/prepare-community-benchmark.py`; it is a transcription of the original
preparer, added after the engine tag. It refuses to replace an existing dataset
and checks all five frozen input hashes. Use a separate preparation environment
with the original versions and Python **3.12+** (NumPy2.5.4 requires it); do this **before** starting measured servers:

```sh
python3 -m venv preparation-venv
preparation-venv/bin/python -m pip install \
  numpy==2.5.4 h5py==3.16.0 threadpoolctl==3.7.0
preparation-venv/bin/python scripts/prepare-community-benchmark.py \
  --source "$PWD/glove-25-angular.hdf5" \
  --publication "$PWD/published-workload"
python3 scripts/prepare-community-benchmark.py \
  --publication "$PWD/published-workload" --verify-only
```

Do not edit checksums to accept different inputs. If a different BLAS/runtime
produces different ground-truth bytes, retain the failed output and report that
reproduction difference; do not call it the byte-identical frozen study. The main
harness also independently verifies input hashes and compares returned IDs,
scores and payloads against truth, including explicit tie tolerance.

## Run the comparison

Use the main/supplement/test harnesses from the released source archive. Record
their SHA-256 values before running. Set the following paths to the **extracted
published native archive** and the installed comparator; no source build of
PacificDB is required:

```sh
export PACIFICDB_BENCH_BIN="$PWD/pacificdb-community-1.2.0-rc.2-linux-x64/bin"
export PG_BENCH_BIN="$PWD/postgres18/bin"
"$PACIFICDB_BENCH_BIN/db_engine" --build-info
"$PG_BENCH_BIN/postgres" --version
benchmark-venv/bin/python scripts/test-community-benchmark.py

benchmark-venv/bin/python scripts/benchmark-community-v12.py \
  --build "$PACIFICDB_BENCH_BIN" --pg "$PG_BENCH_BIN" \
  --publication "$PWD/published-workload" \
  --output "$PWD/results-10k100k" --scratch "$PWD/scratch-10k100k" \
  --sizes 10000,100000 --queries 1000 --rounds 3 --warmups 10 \
  --batch-size 1000 --startup-timeout 900

benchmark-venv/bin/python scripts/benchmark-community-v12-supplement.py \
  --build "$PACIFICDB_BENCH_BIN" --pg "$PG_BENCH_BIN" \
  --output "$PWD/results-supplement" --scratch "$PWD/scratch-supplement" \
  --queries 100

benchmark-venv/bin/python scripts/benchmark-community-v12.py \
  --build "$PACIFICDB_BENCH_BIN" --pg "$PG_BENCH_BIN" \
  --publication "$PWD/published-workload" \
  --output "$PWD/results-1m" --scratch "$PWD/scratch-1m" \
  --sizes 1000000 --queries 1000 --rounds 3 --warmups 10 \
  --batch-size 1000 --startup-timeout 900
```

Each output directory must be empty. Preserve failed attempts in their original
directories and choose a new directory for retries. Run stages sequentially and
avoid other measured work on the host. The harness starts isolated loopback-only,
auth-disabled test servers with durability enabled. Never use these settings on
an exposed or production server. It deliberately SIGKILLs its own disposable
servers during recovery tests and retains copies of the crashed data roots.

## What must accompany the results

- All `summary.json`, `methodology.json`, ground-truth, server logs and failure
  files; retain per-query data and sampled memory observations.
- All three per-round p50/p95/p99 values, mean latency, serial throughput and
  held-out Recall@10. A median of round medians is a summary, not a pooled p99.
- Ingestion including request preparation/final flush, batch sizes, exact packed
  index setup, HNSW build, update/delete costs, 1% filter fallbacks, four-reader
  throughput, mixed read/write errors, and crash/rebuild timings.
- Peak **sampled** RSS/PSS, storage/WAL bytes and retention caveats. These are not
  guaranteed instantaneous peaks or normalized index-only disk usage.
- Machine, filesystem, CPU affinity, power/thermal state, other host activity,
  dependency versions, binary/source hashes and exact commands.

Both systems use durable acknowledgements; fsync is not disabled. PacificDB
ingests JSON batches, PostgreSQL uses transactional COPY, each 1,000 records.
This measures their supported ingestion paths, not an identical wire format.
PacificDB preserves JSON numeric precision; pgvector dense values are float32.
Exact timing uses PacificDB's separately built packed flat derived index versus
PostgreSQL's verified sequential/parallel exact scan. Disclose that setup cost.
Full vector/document payloads are fetched; PacificDB also returns engine metadata.
Warm-cache results are not cold-start results; operating-system caches are not
dropped. HNSW uses M=16/ef_construction=200, with ef_search selected separately on
100 tuning queries to reach 0.97 recall; held-out acceptance is 0.95 Recall@10.
The supplemental hybrid test uses BM25 on PacificDB and ts_rank_cd on PostgreSQL,
so it is a capability/latency diagnostic, not equal relevance certification.

Measured latencies depend on hardware, caches and workload. A company rerun
should reproduce the documented inputs and correctness checks; exact timings
cannot be guaranteed. A benchmark PASS is scoped to exercised scenarios and
does not complete distributed search, physical power-loss, security, upgrade or
production-readiness qualification.
