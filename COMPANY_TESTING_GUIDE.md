# Company testing guide — Pre-release v1.2

PacificDB is a self-hosted unified document/vector/media database. This candidate
is for professional evaluation and feedback, not a production-readiness claim.
Base version `1.2.0`, candidate/tag `v1.2.0-rc.1`, branch `v1.2`, title
**Pre-release v1.2**. Check the exact artifact/publication/platform outcomes in
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) before using a download or registry.

Publication/platform status belongs to the final release manifest and checklist.
The original disk-full and desktop dependency failures are retained with the fixes. Review
[the storage findings](docs/releases/v1.2.0-rc.1/STORAGE_FAILURE_FINDINGS.md) before
planning evaluation; use disposable data only.

## Requirements and first installation

Use a supported tested host with sufficient memory/disk and an isolated data root.
The recorded local test host is Linux x64/Debian 13; platform artifacts are verified
only where explicit install/runtime evidence exists. Native packages require the
matching OpenSSL/LZ4/RE2/C++ runtime dependencies. Workbench needs a graphical
session and Electron dependencies; SDK clients require Node>=18, Python>=3.10 or
Java>=11 metadata, with actually tested versions listed separately.

1. Obtain the selected candidate assets, `SHA256SUMS`, release notes and manifest
   from the verified staging location or the actual published GitHub prerelease.
2. Verify `sha256sum -c SHA256SUMS` before installing. The manifest binds exact
   bytes, candidate version, source revision and verification status.
3. Follow [INSTALLATION.md](INSTALLATION.md). Prefer extracted/staged packages and
   new test directories first; do not upgrade a live database for this evaluation.
4. Record `db_engine --build-info` and `pacificdb --version`. Expect base 1.2.0 with
   explicit 1.2.0-rc.1 prerelease metadata. npm/Java use 1.2.0-rc.1; Python 1.2.0rc1.

## Native CLI quickstart

Set an absolute private home before starting the CLI-managed local engine:

```sh
export PACIFICDB_HOME="$PWD/pacificdb-company-evaluation"
pacificdb ping
pacificdb --help
pacificdb
```

In the interactive shell:

```text
create database company_eval
create collection documents
insert documents {"id":"one","name":"Ada","team":"engineering"}
find documents {"id":"one"}
help
quit
```

Expected: ping returns pong; create/insert/find succeed and find returns the
recorded name. Exit the shell, reconnect and verify persistence. `quit` exits the
client; `pacificdb stop` stops its owned engine. Follow documented commands rather
than assuming SQL syntax. Use only test passwords/tokens, never customer secrets.

## Workbench scenarios

Start the packaged desktop Workbench, or install the npm CLI and run
`pacificdb workbench` with the engine available. The native CLI does not implement
that npm browser subcommand. Confirm the displayed base/candidate prerelease
identity, select/create a database and collection, insert/query a document, explore
media upload/download and run vector queries. For media, compare downloaded bytes
with the input (`sha256sum`) rather than only viewing a thumbnail.

Expected: engine connection/configuration is visible, invalid input produces an
understandable error, documents remain scoped to the selected database, and no
credential value is displayed in public connection metadata. Desktop owns its
private engine and stops it on exit; npm CLI-managed engine behavior is documented
separately. The GUI exposes advanced vector search options, while vector schemas,
index lifecycle and hybrid administration use SDK/raw CLI operations.

## Language integrations and vectors

Install the actual SDK packages and execute the copy-paste examples in
[SDK_GUIDE.md](SDK_GUIDE.md). Test one successful CRUD roundtrip, a missing/invalid
input error, explicit database selection, vector query and binary media roundtrip
in each language relevant to your company. APIs use TCP/TLS, not an invented REST
endpoint. A library install alone does not install/start the native engine.

For opt-in standalone native vector evaluation, start the managed engine with
`PACIFICDB_VECTOR_PREVIEW_ENABLED=1` before its first startup. Desktop instead
requires `PACIFICDB_WORKBENCH_VECTOR_PREVIEW=1`. Validate dense, sparse and named
values against their collection rules; intentionally submit mismatched dimensions
and malformed sparse indices. Run exact search, then explicitly create/build a
supported ANN index through documented SDK/raw requests, inspect readiness and
query with explicit mode/fallback policy. Use the actual lifecycle examples in
[the vector preview guide](docs/V1.2_NATIVE_VECTOR_PREVIEW.md) and
[schema/incremental guide](docs/V1.2_SCHEMAS_AND_INCREMENTAL_ANN.md).

Expected: exact results match a small independently calculated set; ANN scores
correspond to returned records and the declared metric. ABSENT/STALE/unavailable
errors are visible. Filters must not silently return a falsely complete prefix.
Use missing/null/array filters only where documented semantics support them.
Do not assume sparse/flat/lexical indexes update incrementally or every index
supports every vector type.

## Recovery, authorization and stress evaluation

On disposable data only, record acknowledged IDs/values, restart normally and
verify each record/media checksum. ANN definitions remain but graphs must be
rebuilt explicitly after restart; exact fallback follows policy/budgets. This is
a documented limitation, not vector data loss. Crash/fault tests must never target
production roots or a shared customer service.

Where authentication is configured, test an authorized database operation and an
unauthorized cross-owner request; do not disable access controls to force success.
Local auth-off development mode is not a production deployment. Never expose it
to an untrusted network. Production mTLS/RF3/audit/encrypted-storage requirements
remain separate and are not certified by this preview.

Run bounded concurrent writes/searches and record every error/timeout/cancellation,
not just successful latencies. The known strict ANN freshness behavior can reject
many reads while indexing catches up. Stop before exhausting host disk/memory;
resource failures should be explicit. No unlimited ingestion, history capacity or
production SLO is promised.

## Known results and limits

The new post-publication run uses the exact `v1.2.0-rc.1` Linux release archive
against PostgreSQL18.6 + pgvector0.8.7. The 10K/100K workloads and supplemental
dense/sparse/named/hybrid cases completed; 1M ANN/lifecycle qualification remains
in progress. Three completed 1M exact rounds matched ground truth (1,000 held-out
queries per round), with median round p50 37.685ms PacificDB versus82.535ms
PostgreSQL on this host. These partial results are not a completed 1M comparison.
Follow [the frozen reproduction protocol](docs/releases/v1.2.0-rc.1/BENCHMARK_REPRODUCTION.md).
Exact timing uses a separately built PacificDB packed derived index; include its
setup cost. Smaller completed workloads show slower ANN, ingestion and lifecycle
costs than pgvector; strict ANN reads can be unavailable during concurrent writes.
Company hardware/settings/cache state will change timings.

The previous frozen dev comparison passed static correctness/recall at 10K/100K/1M
GloVe25, but ANN latency, ingestion, selective filtering and mixed-read availability
had material gaps. Initial 1M ANN construction took 45.82 minutes; reconstruction
35.97 minutes. Those are build/recovery costs, not per-query delays. They are not
newRC-artifact performance certification. See the full [benchmark report](docs/v1.2-evidence/community-verified-cache-comparison-20261101/REPORT.md).
Distributed vector completeness, online schema migration, advanced compression/
disk indexes and full release/platform/security qualification remain incomplete.

## Feedback format

Submit product feedback using the repository issue template or the testing contact
agreed by your company. If no private company destination has been designated,
GitHub issues at https://github.com/hitesh-reddy-k/pacificdb-community/issues are
appropriate for non-sensitive bugs. Security vulnerabilities use private GitHub
reporting as described in [SECURITY.md](SECURITY.md); never a public issue.

```text
Title:
Candidate version and build-info (sanitized):
Artifact filename and SHA-256:
OS / architecture / runtime versions:
Install method:
Preview flags and safe configuration:
Dataset size / dimensions / metric (no customer records):
Steps to reproduce:
Expected outcome:
Actual outcome and exact error:
Reproducibility / concurrent workload:
Sanitized logs, request shape and timing method:
Data durability or isolation impact observed:
```

Keep logs free of credentials, tokens, keys, private addresses and customer data.
Report incomplete/blocked scenarios honestly. A screenshot alone cannot prove
vector correctness, persistence or authorization; include a minimal safe example.
