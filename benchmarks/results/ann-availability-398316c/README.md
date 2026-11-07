# ANN availability and recovery qualification

Frozen engine: `398316ce68bbff73a4446b4fe0f531a24f4601fd`.
Driver: `scripts/test-vector-ann-availability.py`, introduced at `9de39d9`.

The compressed JSON receipts retain input/binary/script hashes, per-query
checks, every mixed-read result, mutation verification, recovery and sampled
resource usage. Local test-directory paths are removed; measured values are
unchanged. `inventory.json` identifies each compressed file's SHA-256.
Original local logs and summaries remain preserved separately.

10K, 100K and 1M cases passed: GloVe-25 cosine, 100 held-out initial reference
queries, Recall@10 at least 0.95, all 360 strict ANN reads during 40 concurrent writes,
five verified deletions, SIGKILL, acknowledged-record recovery and ANN
reconstruction. Initial/recovery Recall@10 was 1.0/1.0 at 10K and 100K,
and 0.993/0.96 at 1M. No exact fallback or
accepted freshness errors are counted as a successful ANN read.

All runs use ef_search=600. The published RC.2 performance comparison selected
ef_search=200 for PacificDB and used three 1,000-query rounds; these follow-ups
use 100 initial reference queries and ten recovery reference queries. Do not
interpret the different configurations as a controlled latency comparison.
Startup allows 240 seconds; observed 1M restart readiness was 126.7 seconds.
The graph construction budget remains 3,600 seconds.

The terminal 1M summary and complete final metrics were retained after driver
shutdown; the launching parent did not retain a separate OS exit-code receipt.
These results are a self-run
single-system qualification, not a paired PostgreSQL latency benchmark or
production certification. They do not replace the published RC.2 result.

Prepare the checksum-frozen inputs using
[the reproduction guide](../../../docs/releases/v1.2.0-rc.2/BENCHMARK_REPRODUCTION.md),
then run the driver with the appropriate build, input and empty evidence paths.
The driver requires six available CPU threads and the benchmark Python environment.
