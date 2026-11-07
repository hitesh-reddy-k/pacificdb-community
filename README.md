<p align="center"><img src="site/assets/pacificdb-logo-symbol.png" width="96" alt="PacificDB logo"></p>

# PacificDB Community v1.2.0

A self-hosted C++17 database for documents, vectors and media, using shared
WAL/LSM storage and the existing replication infrastructure.

Published v1.2 packages: **Pre-release v1.2 (`1.2.0-rc.2`)**. This branch also
prepares version **1.2.0** with follow-up improvements; published packages retain
their original source and version identity. See the installation guide for the
1.2.0 unsigned Windows/macOS installer policy.

[Downloads](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/v1.2.0-rc.2) · [Installation](INSTALLATION.md) · [SDK guide](SDK_GUIDE.md) · [Documentation](docs/README.md) · [v1.2 changes](RELEASE_NOTES_v1.2.md)

## Install

Download the package and checksums for your platform from the release page.
Workbench packages include the desktop application, engine and native CLI.
Targets are Ubuntu 24.04 x64, Windows x64, macOS Intel and macOS Apple Silicon.
Read [INSTALLATION.md](INSTALLATION.md) for dependencies and platform instructions.

Node.js client and CLI:

```sh
npm install @pacificdb/client@1.2.0-rc.2 @pacificdb/cli@1.2.0-rc.2
```

Python client:

```sh
python -m pip install pacificdb==1.2.0rc2
```

SDKs connect to a running engine; installing an SDK does not install the engine.
Java JAR/POM and SDK archives are available from the release page. Follow
[SDK_GUIDE.md](SDK_GUIDE.md) for examples and local artifact installation.
Maven Central distribution is separate from the downloadable GitHub artifacts.

## Run a local database

Use a separate evaluation directory, then start the installed native shell:

```sh
export PACIFICDB_HOME="$PWD/pacificdb-evaluation"
pacificdb
```

In the shell:

```text
create database app
create collection users
insert users {"id":"1","name":"Ada"}
find users {"id":"1"}
quit
```

Creating a database selects it automatically. The CLI starts its own loopback
engine; `quit` exits the shell, and `pacificdb stop` stops that owned engine.
Desktop Workbench uses a separate local engine/data directory. Configure
TLS, authentication and access controls before providing network access.

The npm CLI also provides a browser Workbench:

```sh
npx @pacificdb/cli@1.2.0-rc.2 workbench
```

This needs an installed native engine for managed startup, or connection settings
for an existing engine. See the installation guide for those options.

## Features

- Document CRUD, filters, secondary indexes and aggregation.
- Dense, sparse and named vector records with collection schemas.
- Exact cosine, L2 and dot-product similarity search.
- Opt-in standalone HNSW, bounded dense incremental maintenance and hybrid retrieval.
- Resumable, checksummed image, audio, video and file storage.
- WAL/LSM persistence, snapshots, backup/restore and existing RF3 replication.
- Authentication, API keys, roles, TLS and tenant access controls.
- Desktop Workbench, native/npm CLIs, and Node.js, Python and Java clients.

Follow [the vector guide](docs/VECTORS.md) for configuration, query policies,
metadata filtering, index lifecycle and hybrid examples. Native vector
administration is an explicit standalone opt-in. Strict ANN can report index
unavailability during maintenance; distributed sharded vector search is outside
its supported scope. Index history has configured bounds and manual maintenance.

## Improvements on this branch

- More reliable incremental ANN visibility during concurrent writes.
- Lower exact-search overhead while preserving visibility and query limits.
- Idle SDK connection handling before command dispatch.
- Cross-platform credential-file validation and launcher integration.
- Compatible document/media APIs and shared durable mutation paths.

Read [the release notes](RELEASE_NOTES_v1.2.md) to distinguish published RC.2
changes from subsequent source improvements.

## Build and test

Install dependencies from [INSTALLATION.md](INSTALLATION.md), then:

```sh
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.2.0
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/db_engine --build-info
./build/pacificdb
```

`--build-info` identifies the exact source commit and release channel. See
[TESTING.md](TESTING.md) for isolated test commands and
[the company guide](COMPANY_TESTING_GUIDE.md) for evaluation scenarios.

[Benchmark reproduction](benchmarks/THREE_DATABASE_REPRODUCTION.md) describes
matched datasets and settings. Performance depends on hardware, workload,
durability, recall and index configuration.

## Support and licensing

Report bugs through [GitHub issues](https://github.com/hitesh-reddy-k/pacificdb-community/issues)
with version, sanitized reproduction and expected/actual behavior. Follow
[SECURITY.md](SECURITY.md) for private vulnerability reporting. Never include
credentials or customer data.

[License](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md) ·
[Contributing](CONTRIBUTING.md) · [Architecture](ARCHITECTURE.md)
