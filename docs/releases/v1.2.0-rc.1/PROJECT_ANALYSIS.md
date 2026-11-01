# Pre-release v1.2: repository assessment

Assessment started2026-11-01 on branchv1.2, baseline 8732d299008e6cbc581f 76e7c2a1eb2f7dad703f.
The checkout already held uncommitted native vector implementation. It was
preserved in an ignored baseline source snapshot before release engineering.

## Actual components

| Component | Implementation and entry point | Release integration |
| --- | --- | --- |
| Engine | C++17 engine/src/main.cpp/server.cpp; JSON-over-TCP/TLS dispatcher, request queues, shared DatabaseEngine/WAL/LSM/Raft | CMake, CPack, native Linux/Windows/macOS workflows |
| Native CLI | engine/src/shell.cpp, local engine discovery/startup and JSON client | Native pacificdb binary beside db_engine |
| npm CLI | cli/bin/pacificdb.js -> cli/src/cli.js, same Node SDK | @pacificdb/cli; managed local engine or explicit remote endpoint |
| Browser Workbench | cli/src/workbench.js + cli/workbench assets | Loopback broker with Host/Origin/token checks |
| Desktop Workbench | Electron desktop/main.mjs/engine.mjs, same broker/UI and bundled native binaries | electron-builder Linux deb/tar, Windows NSIS, macOS arm64/x64 DMG configuration |
| Node SDK | sdk/node/src, asynchronous TCP/TLS pooled client | @pacificdb/client, Node>=18 metadata; actual runtime matrix must be tested |
| Python SDK | sdk/python/pacificdb, pooled socket transport and file transfer | pacificdb wheel/sdist, Python>=3.10 metadata |
| Java SDK | sdk/java, pooled TCP/TLS and Jackson JSON | io.pacificdb:pacificdb-client, Java11 bytecode; JAR/POM/source/Javadoc |
| Deployment | deploy/docker, compose.yaml, Kubernetes/Helm/systemd/monitoring | Source definitions are not proof of deployed production qualification |
| Query intelligence | intelligence deterministic read-only query compiler | Existing tests; no invented remote AI service |

Authoritative vector/document/media records stay in shared durable infrastructure.
ANN/flat/lexical indexes are derived. Native vector preview is standalone/opt-in,
with dense/sparse/named values, schemas, bounded exact search, HNSW lifecycle and
BM25/RRF hybrid. Supported dense incremental indexing is bounded and explicitly
rejects stale/unavailable generations; complete distributed search is absent.
The benchmark and historical checkpoint logs retain their source identities.

## Initial gaps confirmed by inspection or execution

- Root/SDK/CLI/deployment metadata mostly 1.1.2; old default native build actually
  reported 0.1.0-beta.13. New candidate binaries must be rebuilt and inspected.
- Native tag publisher ultimately forced prerelease=false; npm accepted stable
  tags only and always publishedlatest. RC staging/publication needs its own path.
- Python PEP4401.2.0rc1 cannot be compared literally to SemVer 1.2.0-rc.1.
- SDK package tests partly tested source/rebuilt Java rather than the final staged
  artifact bytes; an installed npm counterpart was missing.
- Desktop pipeline recognized only workbench-v* tags and had a separate stable
  publisher/title. Candidate tags must stage artifacts without stable promotion.
- UI supported basic vector search but not every vector administration API.
  Advanced query options can be added while lifecycle/schema/hybrid remain
  explicitly documented SDK/raw-CLI operations.
- Source-validated tenant persistent lockout, tenant database-access self-deadlock
  and two missing password-field redactions need narrow fixes and regression tests.
- Native/desktop existing stable policies require external/platform evidence;
  preview evidence must stay separate and must not manufacture stable PASS.

## Previous release and remote state

Remote origin is https://github.com/hitesh-reddy-k/pacificdb-community.git, public,
with owner push access verified. At initial inspection remote main matched the
baseline; remotev1.2 and tagv 1.2.0-rc.1 were absent. Published engine/SDK prerelease
1.1.2 corresponds to commit 07ffb00a7b224c5ca5cd338c2b0b86723c3b7721, while later
main commits repaired dependencies and Windows Workbench. Release notes derive
both committed changes since that tag and the preserved dirty implementation;
there is no preexisting committedv1.2 feature history to invent.

Local Git contains stale broken Codex capture refs. A regular fetch reports a
missing local object. Existing branches/refs are preserved; source updates and
remote verification must avoid deleting them or overwriting remote work.

## Compatibility and validation boundaries

Engine base version 1.2.0 remains in machine APIs. Additive build metadata identifies
releaseVersion 1.2.0-rc.1/channel/name/tag. npm/Java use 1.2.0-rc.1; Python 1.2.0rc1.
Stable/public website downloads remain on the previous channel until actual
approved publication. Existing data format/protocol versions remain2. No WAL fsync,
Raft acknowledgement, security gate or persistent vector precision is weakened.

Configured platforms are Linux x64, Windows x64, macOS arm64/x64. Linux/GUI/container
and package checks require execution; other platforms remain Not tested or Blocked
until CI supplies actual installer/runtime evidence. Source packages are not an
independent compatibility guarantee. Registry names remain unchanged; GitHub
publication does not establish npm/PyPI/Central availability.
