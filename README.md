# PacificDB Community

PacificDB Community is the open-source beta of the PacificDB distributed
document database. The database engine is licensed under AGPL-3.0; the Node.js,
Java, and Python clients and CLI are licensed under Apache-2.0.

This repository is a **beta candidate**. It compiles and its retained local
correctness tests pass, but this filtered tree has not completed fresh sustained
three-node release testing. Do not describe it as production-certified yet.

## Included

- Document CRUD, filters, pagination, projection, aggregation helpers, and consistency controls
- WAL, LSM storage, crash recovery, compaction, snapshots, and integrity checks
- Secondary/B-tree and vector indexes, validation, and runtime statistics
- RF3 Raft consensus, elections, failover, snapshot catch-up, and repair diagnostics
- Manual shard creation, placement, migration, splitting, merging, and rebalancing
- Basic vector similarity search and metadata filtering
- Read-only natural-language query compilation and query explanations
- TLS, password/token authentication, API keys, RBAC, tenant isolation, and local audit logging
- Manual snapshot backup, verification, deletion, and restore
- Health, logs, engine metrics, and Prometheus output
- JSON-over-TCP API, CLI/shell, Node.js, Java, and Python clients
- Dockerfile, Kubernetes manifests, Helm chart, and reproducible YCSB binding

The exact implemented scope and current gaps are in [COMMUNITY_SCOPE.md](COMMUNITY_SCOPE.md).

## Build and test

Requirements: CMake 3.20+, a C++17 compiler, OpenSSL, LZ4, Node.js 18+, Python
3.10+, and Java 11+/Maven for all SDK checks.

```sh
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
scripts/test-community.sh build
```

Run a development node after creating absolute storage directories and adapting
`engine/.env.example`:

```sh
set -a; . engine/.env; set +a
./build/db_engine
```

Production mode refuses unsafe TLS, authentication, Raft, WAL, bind-address,
and encrypted-storage settings at startup.

## Licensing

- Engine, query intelligence, deployments, and benchmarks: AGPL-3.0 (`LICENSE`)
- `sdk/` and `cli/`: Apache-2.0 (license file in each package)
- Bundled third-party notices: `THIRD_PARTY_NOTICES.md`

Commercial use is allowed under these licenses. Network users of modified
AGPL-covered server code must be offered the corresponding source as required
by AGPL-3.0. Obtain legal advice before release if you need a dual-license model.
