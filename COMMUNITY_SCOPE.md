# Community beta scope

## Implemented in this repository

| Area | Included capability |
|---|---|
| Data | CRUD, filters, pagination, projection, basic aggregation, MVCC/consistency controls |
| Durability | WAL, LSM, recovery, compaction, checksums, snapshots |
| Indexes | secondary/B-tree, vector index, validation and statistics |
| Availability | RF3 Raft, election, failover, catch-up snapshots and storage repair diagnostics |
| Sharding | explicit create/place/split/merge/migrate/rebalance operations |
| Query intelligence | deterministic read-only English-query compiler and explain metadata |
| Search | vector storage, similarity search, metadata filters |
| Media | replicated images, GIFs, audio, video and arbitrary bytes within the request-size limit |
| Security | TLS/mTLS transport, passwords/tokens, API keys, RBAC, tenant isolation, audit logs |
| Backup | operator-triggered full snapshot, list, verify, delete and restore |
| Operations | ping/health, metrics, Prometheus, logs and storage diagnostics |
| Clients | JSON-over-TCP, native CLI/shell, Node.js, Java and Python |
| Deployment | Docker, Docker Compose, Kubernetes, Helm, and native release packaging |

## Not implemented in this beta

- REST gateway
- Go client
- Dynamic plugin loader, packaged plugin SDK, installer, or marketplace; the in-process C++ extension interfaces and hooks are included
- `pdb.ask()`, `pdb.watch()`, Semantic Layer, or Autopilot product APIs
- Automatic index creation/removal, index budgets, and named index policy modes
- Hybrid text/vector search
- Chunked large-object and external object-storage media workflows
- A fully integrated periodic replica comparison service

These items are omitted rather than represented by placeholders. Add them only
with executable behavior, documentation, and a focused correctness test.

## Release gates still open

- Three clean RF3 sustained runs at both 64 and 128 clients
- Stable terms during measurement and zero request errors
- Acknowledged-write history check with no loss or duplicate logical writes
- Replica convergence and apply/catch-up lag returning to zero
- Failover, manual backup/restore, and mixed-version rolling tests on release artifacts
- Security configuration review and signed release artifacts
