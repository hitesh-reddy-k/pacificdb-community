# Additional configuration disclosures

This addendum belongs to the same frozen candidate and public-path workload as
`final-primary/methodology.json`; it does not change timed settings or results.
The full executable harness is attached under `executed-harnesses/`.

- PacificDB uses `ADAPTIVE_ADMISSION=0` for fixed-concurrency measurement,
  `MAX_MEMORY_MB=8192`, and a separate sampler terminates either product above
  6 GiB whole-server sampled PSS. This is not qualification of default adaptive
  admission, scheduler behavior, arbitrary collection counts or production quotas.
- PacificDB query timeout is 120 seconds, maximum scan rows is dataset size plus
  100000, and maximum result documents is 10000. Returned K is 10. Limits remain
  explicit and failures are not silently converted to partial results.
- PostgreSQL statement timeout is 120 seconds. Connection readiness after startup
  has a separately declared 900-second budget; elapsed recovery is measured.
- Benchmark authentication, TLS, audit logging and metrics are disabled on owned
  loopback-only servers to isolate this workload. Authentication/TLS and document/
  media isolation/recovery are exercised separately, not timed by this comparison.
- PacificDB's existing single-member Raft path remains enabled, standalone bypass
  disabled, WAL/checksums/fsync/commit flushing enabled. PostgreSQL fsync,
  synchronous_commit and full_page_writes remain enabled.
- CPU affinities bound both server products to the same four logical CPUs, and
  benchmark clients to two additional CPUs. The host is shared with unrelated
  services. No compiler or regression job runs during final timings. OS caches
  are warm and are not dropped. These are workstation measurements, not lab SLOs.
- Per-query `payload_cache_hits` counts verified full-row reuse, not cached result
  sets. Repeated query rounds can warm those rows. Graph traversal, ranking,
  payload verification, tenant checks and final source certificates still run.

Source/binary hashes, compiler flags, full PostgreSQL/pgvector build logs and
versions are retained. This is a self-run candidate benchmark prepared for review;
it is neither PostgreSQL endorsement nor production-release certification.
