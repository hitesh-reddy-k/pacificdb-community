# Pre-release v1.2

**Base version 1.2.0 · candidate/tag v1.2.0-rc.2 · branch v1.2 · GitHub type: Pre-release.**
Intended for external company testers and developers. Production qualification
remains INCOMPLETE. Publication and platform status are recorded in
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md); a candidate name alone does not prove
an artifact is published, installed or tested.

## Changes in this candidate

RC.2 corrects two installer/Workbench findings from RC.1: short desktop windows
could collapse the database tree behind the sidebar actions, and the custom
Linux installer omitted electron-builder's application-scoped AppArmor setup.
Navigation now scrolls while preserving an accessible database-tree area, and
Linux packaging retains the upstream installer script. A short-window regression
reproduces the original click failure and passes at650/653/900px heights after
the change. Fresh installed-desktop tests are separate from upgrade tests.

RC.1 assets/tags/npm packages remain unchanged. RC.2 is public on GitHub at
`e3d70761f03341acc13130d17b75fed2bc758a29`, with scoped native/SDK/platform and
complete Community/actual ENOSPC qualification. Public desktop/SDK installations,
npm publication/consumers and the public GHCR image passed independent checks.
Python is published on PyPI as `pacificdb==1.2.0rc2`; Maven Central remains owner-blocked. Engine/storage/vector source remains unchanged from RC.1;
component release metadata is advanced consistently. RC.1 benchmark results
keep their original artifact identity and are not called RC.2 measurements.

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
reports base 1.2.0 plus explicit release metadata. npm/Java identify 1.2.0-rc.2;
Python's equivalent is 1.2.0rc2. Storage-format and Raft protocol versions remain2.
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

## Known limitations

- The published Linux Workbench launcher icon can be missing because of a
  non-standard theme size. An unreleased packaging fix uses the existing
  standard-size logo assets; local package/GTK lookup checks passed.
- Native vector/index/hybrid operations are an explicit standalone preview.
- ANN graphs require rebuilding after restart; strict ANN may be unavailable
  during writes. Large builds and recovery reconstruction can take many minutes.
- Sparse/flat/lexical incremental maintenance, online schema evolution,
  automatic graph compaction, advanced compression/disk ANN and complete
  distributed vector search remain incomplete. Physical history needs manual
  maintenance at configured bounds.
- Full production, physical power-loss, independent security and complete
  mixed-version/platform upgrade qualification is incomplete.
- Maven Central publication awaits owner setup. Python is available on PyPI,
  and GitHub wheel/JAR/POM distribution remains available. Platform signing/notarization limits are documented.

## Testing and benchmarks

Frozen RC.2 passed 78 checkpoint checks/50 CTest cases, 16 SDK jobs, complete
Community/21 real disk-full categories, and scoped installed-desktop/public SDK
checks. Separate targeted TSAN and ASan/UBSan jobs passed. Exact scopes and CI
receipts are in [the release checklist](RELEASE_CHECKLIST.md).

The post-publication RC.2 study completed at 10K/100K/1M plus smaller metric,
dimension, sparse/named/hybrid diagnostics. At 1M GloVe25 cosine, median round p50
was 50.641ms versus PostgreSQL94.017ms for packed exact, and21.139ms versus1.521ms
for ANN, with Recall@10 0.9782 versus0.9899. Initial ANN builds took2045.841s
versus350.554s. **Mixed strict ANN reads succeeded0/360 versus360/360**; this is a
failed availability result, not hidden by the study execution PASS.

[Published report](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/BENCHMARK_REPORT_1.2.0-rc.2.txt) and
[per-query evidence](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/pacificdb-benchmarks-1.2.0-rc.2-20261101.tar.gz) include
filter/concurrency/ingestion/resource costs and SIGKILL/rebuild results.
[Reproduction](docs/releases/v1.2.0-rc.2/BENCHMARK_REPRODUCTION.md) discloses precision,
protocol and packed-index setup differences. This is a project-run comparison,
not independent certification, a general speed claim or a promise of identical
company timings. Full production qualification remains incomplete.

## Feedback

Report product bugs with version/build-info, platform, installation method,
reproduction, expected/actual outcome, sanitized logs and whether preview was on.
Use the [company feedback format](COMPANY_TESTING_GUIDE.md). Never include private
keys, credentials, tokens or customer data. Suspected security vulnerabilities use
private reporting described in [SECURITY.md](SECURITY.md), not public issues.
