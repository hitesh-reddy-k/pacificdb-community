# v1.2 Release

Version **1.2.0**. PacificDB combines document, vector and media storage using
shared durable WAL/LSM infrastructure. Use matching engine, CLI, Workbench and SDK
packages, and verify checksums before installation.

## Added

- Persistent dense, sparse and named vectors within the existing WAL/LSM storage.
- Creation-time vector schemas and dimension/metric validation.
- Bounded exact top-K and explicit exact/ANN/automatic search policies.
- Opt-in standalone HNSW lifecycle and bounded incremental dense maintenance.
- Lexical/vector hybrid retrieval with reciprocal rank fusion.
- Consistent engine, Workbench, CLI and SDK version reporting.

## Improved

- Bounded candidate retention and less unnecessary document materialization.
- Request-owned query limits, cancellation and read visibility.
- Point reads around flushes, caches and committed changes.
- Workbench document/vector/media navigation and vector query options.
- SDK operation coverage and installation verification.

## Fixed

- Short-window Workbench navigation keeps the database tree accessible.
- Linux desktop packaging retains application-scoped installer integration.
- Historical release packages and tags remain unchanged.

## Further improvements


- Incremental ANN maintains complete coverage during concurrent and unrelated
  commits, preserving source and applied-commit visibility checks.
- Existing maintenance is signalled for freshness-bound queries.
- Python and Java replace observably expired idle sockets before sending a new
  command, without replaying commands already sent.
- Cross-platform private credential-file validation and startup cleanup.
- Standard-size Linux launcher icons and installer integration.
- Workbench terminal command registration, with Windows installer PATH setup
  and a File-menu setup action for bash/zsh, so `pacificdb --version` is accessible.
- Lower storage-iterator overhead for visibility-correct vector scans.
- Prepare Maven Central coordinates `in.pacificdb:pacificdb-client` for the
  `pacificdb.in` domain; Java API packages and published RC.2 assets are retained.

- Align source package identities at 1.2.0 and select CI packaging versions from
  the canonical release identity.
- Allow unsigned Windows/macOS 1.2.0 installers by explicit owner decision;
  checksums and installation/runtime validation remain required.
- Keep npm prereleases on `next`; stable publication to `latest` requires a
  matching public stable GitHub release. Maven Central publication is deferred.

## Installation and packages

Use [INSTALLATION.md](INSTALLATION.md) for the engine/Workbench,
[SDK_GUIDE.md](SDK_GUIDE.md) for clients and
[COMPANY_TESTING_GUIDE.md](COMPANY_TESTING_GUIDE.md) for evaluation scenarios.

| Component | Release identifier |
|---|---|
| Native engine / CLI / Workbench | `1.2.0` (engine base `1.2.0`) |
| Node.js SDK | `@pacificdb/client@1.2.0` |
| npm CLI | `@pacificdb/cli@1.2.0` |
| Python SDK | `pacificdb==1.2.0` |
| Java SDK | `in.pacificdb:pacificdb-client:1.2.0` JAR/POM from GitHub |

Maven Central publication is separate from the downloadable GitHub JAR/POM.
Check artifact checksums and `db_engine --build-info` to identify the exact build.

## Compatibility and operation

Document/media APIs, existing vector records, authoritative WAL/LSM storage and
the existing replication lifecycle are retained. Storage-format and Raft protocol
versions remain 2. Back up data and evaluate upgrades in a separate directory.

Native vector/index/hybrid administration is an explicit standalone opt-in:
`PACIFICDB_VECTOR_PREVIEW_ENABLED=1` before engine startup, or
`PACIFICDB_WORKBENCH_VECTOR_PREVIEW=1` for the desktop-owned engine. Schema/index
administration and hybrid requests are SDK/raw-protocol operations; not every
operation has a graphical control.

Derived indexes need readiness to be re-established after restart. Strict ANN
can report unavailability during maintenance; exact fallback depends on the
requested policy. Large builds and reconstruction take time.

Collection schemas are immutable at creation. Sparse/flat/lexical incremental
maintenance, automatic graph compaction and distributed sharded vector search
are outside the current supported vector scope. Physical index history is
bounded and requires manual maintenance at configured limits.

## Testing, benchmarks and feedback

[TESTING.md](TESTING.md) provides reproducible checks.
[Benchmark methodology](benchmarks/THREE_DATABASE_REPRODUCTION.md) explains
matched datasets, durability settings, recall and query-mode comparisons.
Historical measurements retain their original artifact identities; source
improvements do not retroactively change published RC.2 results.

Report version, OS/architecture, sanitized reproduction and expected/actual
behavior through [GitHub issues](https://github.com/hitesh-reddy-k/pacificdb-community/issues).
Use [private security reporting](SECURITY.md) for vulnerabilities. Exclude
credentials, customer data and private deployment details.
