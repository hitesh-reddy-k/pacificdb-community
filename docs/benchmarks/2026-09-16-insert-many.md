# Durable `insertMany`: PacificDB beta.14 versus MongoDB 8.3.8

Date: 2026-09-16

This comparison measures sustained, durably acknowledged batch ingestion. It is
not comparable to a single-document YCSB result or to a benchmark that allows an
engine to acknowledge before journal persistence.

## Environment

- CPU: Intel Core i7-10850H, 6 cores / 12 threads
- Memory: 15 GiB
- OS/filesystem: Linux, ext4
- PacificDB: `0.1.0-beta.14` release candidate, standalone Raft bypass,
  WAL fsync enabled
- MongoDB server: 8.3.8
- MongoDB Node.js driver: 7.6.0
- Both data directories: same ext4 filesystem and host

## Workload

- 5,000 warmup documents per sample
- 50,000 measured documents per sample
- 500 documents per `insertMany`
- 4 concurrent producers
- Client pool limit: 16
- 5 samples, alternating which engine ran first
- PacificDB durability: default WAL fsync
- MongoDB durability: write concern `{ w: 1, j: true }`
- Final document count verified after every sample

Documents contained a caller-provided ID, integer sequence, boolean, one of 32
categories, and a 96-character payload.

## Results

| Engine | Throughput median | Batch latency p50 median | Batch latency p95 median |
| --- | ---: | ---: | ---: |
| PacificDB | 3,135 records/sec | 450.6 ms | 1,432.5 ms |
| MongoDB | 7,298 records/sec | 185.4 ms | 869.4 ms |

PacificDB/MongoDB throughput ratio: **0.430**. MongoDB was **2.33x faster** in
this sustained durable-ingestion test.

### Raw PacificDB samples

| Sample | Records/sec | Elapsed | Batch p50 | Batch p95 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 3,139 | 15.928 s | 458.9 ms | 1,432.5 ms |
| 2 | 3,291 | 15.194 s | 463.3 ms | 1,394.3 ms |
| 3 | 2,955 | 16.922 s | 409.4 ms | 1,465.3 ms |
| 4 | 3,129 | 15.982 s | 450.6 ms | 1,354.8 ms |
| 5 | 3,135 | 15.949 s | 355.3 ms | 1,551.2 ms |

### Raw MongoDB samples

| Sample | Records/sec | Elapsed | Batch p50 | Batch p95 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 7,738 | 6.461 s | 169.2 ms | 869.4 ms |
| 2 | 8,196 | 6.101 s | 185.4 ms | 692.4 ms |
| 3 | 7,298 | 6.851 s | 194.9 ms | 721.1 ms |
| 4 | 7,016 | 7.127 s | 193.1 ms | 959.6 ms |
| 5 | 6,979 | 7.165 s | 168.6 ms | 1,102.9 ms |

## Interpretation

Persistent pooling removes per-request TCP teardown from the Node.js path, and
`insertMany` amortizes protocol and engine dispatch overhead across 500 records.
Those improvements are functional and measurable, but they do not make this
release faster than MongoDB for sustained durable ingestion.

A focused 500-document profile measured 139.0 ms inside PacificDB, of which
131.1 ms was the WAL append/fsync stage. That durable WAL path, plus sustained
memtable flushing, is the next optimization target. Disabling durability would
produce a different benchmark contract and was not used for the published result.

## Reproduce

Install the benchmark dependency and start both engines with data directories on
the same filesystem:

```bash
cd benchmarks/insert-many
npm ci
BENCH_RECORDS=50000 \
BENCH_WARMUP_RECORDS=5000 \
BENCH_BATCH_SIZE=500 \
BENCH_CONCURRENCY=4 \
BENCH_POOL_SIZE=16 \
BENCH_SAMPLES=5 \
PACIFICDB_PORT=9000 \
MONGODB_URI=mongodb://127.0.0.1:27017 \
node benchmark.mjs
```

The harness alternates execution order and exits on a count mismatch.
