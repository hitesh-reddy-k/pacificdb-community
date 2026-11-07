# Warm metadata-index exact vector search: recovered 1M trial

Query execution: PASS for the stated workload. Index creation request ACK: FAIL
in the retained preparation attempt. Production readiness: INCOMPLETE.
This is an unreleased Linux x64 optimization, not a new three-database comparison.

| Execution order | Median of two round medians | Calls | Exact reference recall |
| --- | ---: | ---: | ---: |
| Frozen prior executable | 6,032.20 ms | 20 | 1.0 |
| Candidate, warm indexed path | 120.50 ms | 20 | 1.0 |
| Same frozen prior executable | 6,127.38 ms | 20 | 1.0 |

Median across all four baseline round medians: 6,127.38 ms. Candidate: 120.50 ms.
Observed warm speedup: 50.85 times; warm latency 98.03% lower. All measured
candidate requests ranked all 9,999 eligible candidates. Every one of the 60
measured calls matched both strict and tie-aware independent ground truth.

The candidate first payload-cold query took **5,966.57 ms**. Its next warmup took
111.77 ms. A warm existing metadata cache is required; cold/busy/incomplete or
pending indexes retain the full scan. These numbers do not promise an immediate
first-query speedup. Do not substitute them for prior unindexed pgvector/Qdrant
cells or claim a new comparison with those systems.

Same recovered GloVe25/cosine/top10 fixture, 1M original records and 999,995
survivors; group_id=1 selects 9,999 rows. Both executables use the same complete
18.87 MiB metadata index and same document-index warmup (0.69–0.72 s). Two
warmups and two rounds repeat ten unique queries per variant. Four server/two
client logical CPUs, temporary performance profile, local TCP without TLS/auth.
Native WAL/fsync/query budgets retain the prior runner settings. Shared active
host, dynamic clocks, order/caches and the small repeated query sample limit
interpretation. No meaningful tail-latency or independent-build claim is made.
45 changed records, five deletions and total count are checked per process.

**Preparation failure:** the public createIndex request returned
write_outcome_unknown. The index later reached ready/complete before shutdown;
catalog creation-to-ready timestamps span 32.209 s, not a successful client ACK.
Peak sampled PSS in that failed attempt was 5,463.52 MiB. No write was retried and
no assertion, timeout or durability setting was relaxed. Failure receipts and
the independently observed definition are retained under prior-attempt/. The
following matched read-only trial reused that same complete index on both
runtimes. Large synchronous metadata-index creation remains a release concern.

All 81 checkpoint checks, separate production/failpoint builds, authenticated
Python/Java TCP/TLS and SIGKILL through the Node fixture passed. ASan/UBSan and
TSAN passed query and paused-publication tests with LSM, DatabaseEngine and the
test translation units instrumented; linked dependencies are not a fully
sanitized engine. An independent static review found the publication-to-pending
coverage gap and uncancellable cold-loader risk. Both were corrected before
final validation; their reproductions and dispositions are retained. Broader
indexed named/sparse/platform/distributed/sustained qualification is INCOMPLETE.

[Implementation, compatibility, tests and limitations](IMPLEMENTATION.md).
comparison.json, per-query/warmup/resource records, source fingerprints,
provenance and checksums identify the actual inputs and outcomes. Private
runtime/test logs are retained with hashes, not distributed. No credentials,
customer data or private security-review material is included. No commit,
tag, registry package or release was published by this checkpoint.

Source patches in sources/ apply to provenance.json's base commit. Measurement
and sanitizer scripts are provided with local paths redacted. Reproduction
requires a new disposable recovered fixture matching the previous 1M trial,
including its documented mutations/deletions, and the same ready metadata index.
Replace $REPOSITORY/$PUBLICATION/$NODE_PREFIX placeholders with your isolated
paths; retain path-containment guards. Do not run fixture scripts against live
data. Use separate build/failpoint directories and the checkpoint runner with
expected version 1.2.0 to repeat qualification. The initial failed setup remains
part of the evidence, never a passing preparation result.

Verify all files with SHA256SUMS.
