# ANN availability and recovery qualification

Frozen engine: `398316ce68bbff73a4446b4fe0f531a24f4601fd`.
Driver: `scripts/test-vector-ann-availability.py`, introduced at `9de39d9`.

The compressed JSON receipts retain input/binary/script hashes, per-query
checks, every mixed-read result, mutation verification, recovery and sampled
resource usage. Local test-directory paths are removed; measured values are
unchanged. `inventory.json` identifies each compressed file's SHA-256.
Original local logs and summaries remain preserved separately.

10K and 100K cases passed: GloVe-25 cosine, 100 held-out initial reference
queries, Recall@10 1.0, all 360 strict ANN reads during 40 concurrent writes,
five verified deletions, SIGKILL, acknowledged-record recovery and ANN
reconstruction. The sampled recovery Recall@10 was 1.0. No exact fallback or
accepted freshness errors are counted as a successful ANN read.

The 1M case is executing and remains INCOMPLETE. These results are a self-run
single-system qualification, not a paired PostgreSQL latency benchmark or
production certification. They do not replace the published RC.2 result.

Prepare the checksum-frozen inputs using
[the reproduction guide](../../../docs/releases/v1.2.0-rc.2/BENCHMARK_REPRODUCTION.md),
then run the driver with the appropriate build, input and empty evidence paths.
The driver requires six available CPU threads and the benchmark Python environment.
