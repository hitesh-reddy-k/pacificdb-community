<p align="center"><img src="site/assets/pacificdb-logo-symbol.png" width="96" alt="PacificDB logo"></p>

# PacificDB Community v1.2.0 — Pre-release v1.2

A self-hosted C++17 database for documents, vectors and media.
Current company-testing candidate: **1.2.0-rc.2**, base engine **1.2.0**.
This is a prerelease; full production qualification remains incomplete.

[Download RC.2](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/v1.2.0-rc.2) · [Installation](INSTALLATION.md) · [Documentation](docs/README.md) · [Changes](RELEASE_NOTES_v1.2.md)

## Install

Download the matching checksummed package from the release. Desktop packages include
Workbench, engine and native CLI. Verified targets are Ubuntu 24.04 x64, Windows
x64, and macOS 15 Intel/Apple Silicon. Other OS versions and architectures are
unverified. See [installation](INSTALLATION.md) for dependencies and signing limits.

Node.js client and CLI:

```sh
npm install @pacificdb/client@1.2.0-rc.2 @pacificdb/cli@1.2.0-rc.2
```

SDKs require a running engine. Python wheel and Java JAR/POM are downloadable
from GitHub; PyPI and Maven Central publication await owner setup.
Use [SDK_GUIDE.md](SDK_GUIDE.md) for installation and working examples.

## First database

Start the installed native `pacificdb` shell, then:

```text
create database app
create collection users
insert users {"id":"1","name":"Ada"}
find users {"id":"1"}
```

The local CLI starts a loopback engine. Set an absolute `PACIFICDB_HOME` before
startup to use a separate evaluation directory. Configure authentication and TLS
before network access. Desktop owns a separate local engine/data directory.

## What changed in v1.2

- Dense, sparse and named vectors with creation-time collection schemas.
- Bounded exact top-K, explicit search modes, request-owned limits and cancellation.
- Opt-in standalone HNSW lifecycle, bounded dense incremental maintenance and BM25/RRF hybrid retrieval.
- Storage/recovery, authorization and credential-log handling corrections.
- Consistent prerelease packages and installed-package verification.
- RC.2 repairs short-window Workbench navigation and Linux installer integration.

Documents, media and vectors use the shared WAL/LSM storage infrastructure.
See [release notes](RELEASE_NOTES_v1.2.md) for scope and compatibility.

## Vector evaluation

Native vector features require explicit preview configuration. Follow
[the vector guide](docs/VECTORS.md) for dense/sparse/named records, schemas,
exact/ANN queries, index lifecycle and hybrid retrieval.
ANN can be unavailable during writes and requires graph reconstruction after
restart. Distributed vector completeness and full production qualification remain
incomplete. Do not use the prerelease as a production-readiness guarantee.

## Testing and feedback

Company testers should start with [COMPANY_TESTING_GUIDE.md](COMPANY_TESTING_GUIDE.md).
[Release status](RELEASE_CHECKLIST.md) records actual scoped verification.
[Benchmark reproduction](docs/releases/v1.2.0-rc.2/BENCHMARK_REPRODUCTION.md) identifies
inputs, settings and limitations. The complete RC.2 1M comparison is still running;
no final result is claimed. Timings vary across machines.

Report bugs through the [issue tracker](https://github.com/hitesh-reddy-k/pacificdb-community/issues)
with version, sanitized reproduction and expected/actual behavior. Send suspected
vulnerabilities through [private security reporting](SECURITY.md).
Never attach credentials or customer data.

Developers: [architecture](ARCHITECTURE.md), [tests](TESTING.md),
[contributing](CONTRIBUTING.md), [license](LICENSE),
[third-party notices](THIRD_PARTY_NOTICES.md).
