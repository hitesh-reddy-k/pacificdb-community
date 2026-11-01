# Pre-release v1.2

**Base version 1.2.0 · candidate/tag v1.2.0-rc.1 · branch v1.2 · GitHub type: Pre-release.**
Intended for external company testers and developers. Production qualification
remains INCOMPLETE. Publication and platform status are recorded in
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md); a candidate name alone does not prove
an artifact is published, installed or tested.

## Changes in this candidate

The existing unified C++ engine now includes durable dense, sparse and named
vectors; creation-time schemas; bounded exact top-K; explicit search modes and
resource errors; derived HNSW lifecycle with opt-in bounded dense incremental
maintenance; packed exact plans; and BM25/RRF hybrid retrieval. Authoritative
records retain shared WAL/LSM/Raft infrastructure and legacy numeric compatibility.
Unsafe legacy all-pairs ANN routing was removed. Query limits, cancellation and
read context are request-owned; point reads were corrected around flush, cache
publication and advancing commit visibility. Verified source proofs permit safe
row-cache reuse without a result cache or weakened acknowledgements.

Release engineering aligns native, npm, Python, Java and Workbench candidate
identity, staged-artifact verification and prerelease-only publishing. The engine
reports base 1.2.0 plus explicit release metadata. npm/Java identify 1.2.0-rc.1;
Python's equivalent is 1.2.0rc1. Storage-format and Raft protocol versions remain2.
New tenant authentication/logging fixes have targeted tests; final execution
status belongs to the release checklist, not this narrative.

Workbench retains its document/vector/media explorer and isolated local-engine
lifecycle. It displays prerelease identity and supports advanced vector query
options. Vector schema/index administration and hybrid commands remain documented
SDK/raw-CLI operations; do not assume every SDK feature has a graphical control.
The native and npm CLIs retain command/help/JSON workflows. Node/Python/Java SDKs
provide native preview operations over the existing protocol; see tested examples
and exact package identities in [SDK_GUIDE.md](SDK_GUIDE.md).

## Installation and compatibility

Follow [INSTALLATION.md](INSTALLATION.md) and [COMPANY_TESTING_GUIDE.md](COMPANY_TESTING_GUIDE.md).
Use only the artifacts whose installed/runtime evidence is Passed for the actual
platform. Configured Linux, Windows and macOS build targets do not by themselves
constitute verified support. Engine and Workbench can use different data roots;
back up existing data before evaluation and test in a separate directory.
No automatic major-data migration or native graph persistence is promised.

Vector preview stays default-off. Standalone operators explicitly enable
`PACIFICDB_VECTOR_PREVIEW_ENABLED=1`. Desktop's separate opt-in is
`PACIFICDB_WORKBENCH_VECTOR_PREVIEW=1`. Unsupported clustered vector operations
remain gated. Existing document/media APIs are preserved in exercised regressions;
this is not a universal mixed-version or downgrade guarantee.

## Known issues

- Standalone commit publication and rejected-tail cleanup now fail closed under
  ENOSPC and synchronous I/O errors. Uncertain post-rename outcomes fence the node
  until restart. See [the storage findings](docs/releases/v1.2.0-rc.1/STORAGE_FAILURE_FINDINGS.md).

- ANN graphs rebuild after restart. Measured 1M initial construction 45.82 minutes,
  post-crash reconstruction 35.97 minutes on the recorded 25D workstation workload.
- Strict ANN queries can be unavailable during writes; measured 1M mixed reads
  succeeded 26/360 times against an error-free target. Explicit freshness errors
  protect correctness but do not meet high-availability expectations.
- ANN, ingestion and selective filtering remain slower than PostgreSQL/pgvector
  on the documented workload. Performance is dataset/configuration dependent.
- Sparse/flat/lexical incremental maintenance, online schema evolution, automatic
  graph compaction, advanced compression/disk indexes and complete distributed
  vector search are incomplete/deferred. Bound physical history requires manual
  maintenance; no unlimited-update promise is made.
- Production/platform/upgrade/security/power-loss/sustained qualification is not
  complete. Existing strict stable-release policy is preserved; prerelease
  staging evidence is separately scoped.

## Testing and benchmarks

The previous frozen 1.2.0-dev checkpoint passed 75 top-level/43CTest, fiveTSAN,
fourASan/UBSan,34Node/134Python/30Java units and live SDK recovery. The completed
self-run comparison covers 10K/100K/1M GloVe25 plus smaller dimensional/metric and
sparse/named/hybrid supplements. At 1M, indexed exact p50 was 49.73ms versus 112.23ms
PostgreSQL; ANN 18.24ms versus 2.26ms, Recall@10 0.9778 versus 0.9897. Exact packed
index build cost 15.45s is disclosed. This is not independent certification or a
benchmark of renamedRC bytes. See [original report](docs/v1.2-evidence/community-verified-cache-comparison-20261101/REPORT.md)
and [current tests/checklist](TESTING.md). Fresh RC tests remain separately recorded.

Current RC validation additionally passed 76 top-level checks/45 CTest on the
initial RC build, source/installed Node, Python and Java checks, and sandboxed
Linux Workbench package tests. Clean-source delivery receipts identify their own
binary hashes. The matcher compatibility repair clears the full/runtime desktop dependency
audits without an exception. Publication and platform support still require exact
final artifact receipts. No new benchmark run is attributed to RC artifacts.

## Feedback

Report product bugs with version/build-info, platform, installation method,
reproduction, expected/actual outcome, sanitized logs and whether preview was on.
Use the [company feedback format](COMPANY_TESTING_GUIDE.md). Never include private
keys, credentials, tokens or customer data. Suspected security vulnerabilities use
private reporting described in [SECURITY.md](SECURITY.md), not public issues.
