# PacificDB versus MongoDB batch-ingestion benchmark

This benchmark compares durably acknowledged `insertMany` calls with identical
documents, batch size, concurrency, warmup, and client pool limits. PacificDB uses
its default WAL fsync and MongoDB uses write concern `{ w: 1, j: true }`. It
alternates engine order across samples and verifies the final record count after
every sample.

Run against already-started engines:

```bash
cd benchmarks/insert-many
npm ci
PACIFICDB_PORT=9000 MONGODB_URI=mongodb://127.0.0.1:27017 npm exec -- node benchmark.mjs
```

The defaults are 50,000 measured records, 5,000 warmup records, batches of 500,
four concurrent batch producers, a 16-connection pool, and five samples. Override
them with `BENCH_RECORDS`, `BENCH_WARMUP_RECORDS`, `BENCH_BATCH_SIZE`,
`BENCH_CONCURRENCY`, `BENCH_POOL_SIZE`, and `BENCH_SAMPLES`.

Run both engines on the same Linux host and ext4 device. Record CPU model, memory,
PacificDB commit, MongoDB version, filesystem, and durability settings with the
result. Do not compare this batch test with single-document YCSB numbers.
