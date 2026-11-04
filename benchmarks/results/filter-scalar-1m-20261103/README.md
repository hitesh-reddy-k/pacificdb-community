# Scalar metadata decoder: recovered 1M filtered exact experiment

This is an unreleased Linux x64 reader optimization, not a new three-database
comparison or production approval. The previous published-style 1M comparison
retains its original executable, results and limitations.

| Run order | Runtime | Median of two round medians | Calls | Exact recall |
| --- | --- | ---: | ---: | ---: |
| 1 | Frozen baseline | 7,712.05 ms | 20 | 1.0 |
| 2 | Candidate | 5,745.14 ms | 20 | 1.0 |
| 3 | Same frozen baseline | 7,247.24 ms | 20 | 1.0 |

Across four baseline round medians: 7,432.28 ms. Candidate: 5,745.14 ms.
Observed latency reduction: **22.7%** (20.7% against the later control alone).
The earlier baseline control differs by 6.4% from the later one; run order,
dynamic clocks, caches and active background work remain limitations.

Method: same recovered fixture from the completed 1M GloVe25/cosine/top10 trial,
999,995 surviving records, filter group_id=1 with 9,999 eligible rows; two warmups
and two rounds over the same ten unique queries per variant. Four server logical
CPUs and two client CPUs, temporary performance profile, local unauthenticated
TCP/no TLS. Native fsync/WAL and query limits retain the prior runner settings.
Forty-five changed records, five deletions and total count are checked, along
with independent filtered ground truth for every query. No metadata index is
created and every authoritative row remains part of the exact scan.

All 80 checkpoint checks, decoder ASan/UBSan and TSAN, and authenticated
Python/Java TCP/TLS/SIGKILL checks through the Node fixture passed. Sanitizers
instrument the storage-format/test translation units, not every dependency or
the complete engine. No unexecuted platform or production gate is called PASS.

[Implementation, compatibility, tests and remaining work](IMPLEMENTATION.md)
contains the detailed scope. comparison.json and all per-query/resource receipts
are retained. These 20 candidate calls repeat ten queries; do not infer broad
p95/p99 or independent-build confidence. No fresh pgvector/Qdrant, ingestion or
ANN latency measurement is made for this candidate. Do not replace the earlier
6.29-second fresh-workload cell with this recovered-fixture result.

Source identity: provenance.json, sources/candidate-source.patch.gz and
sources/baseline-source.patch.gz apply to their recorded base commit. The
measurement/controller sources and input manifest are provided. Their local
fixture paths are redacted; adapt them to a new disposable reproduction of the
prior 1M trial, preserving the recovered record state and isolation checks.
Use scripts/test-vector-checkpoint.py with separate production/failpoint builds
and expected version 1.2.0 to repeat qualification. Review fixture paths before
starting any engine; the harness deliberately restricts use to benchmark scratch.

Verify files with SHA256SUMS. Runtime/server/test logs remain private; only their
hashes are distributed. There are no credentials, customer records or private
security-review materials in this bundle. No tag, package or release was
published. Production status remains INCOMPLETE.
