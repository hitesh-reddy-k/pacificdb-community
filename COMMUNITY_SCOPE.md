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
| Media | resumable replicated chunks for images, GIFs, audio, video and arbitrary files; per-chunk request limits, no PacificDB total-size cap |
| Security | TLS/mTLS transport, passwords/tokens, API keys, RBAC, tenant isolation, audit logs |
| Backup | operator-triggered full snapshot, show/list, verify, complete checksummed JSON export, delete, synchronous restore and restore journal |
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
- External object-storage media workflows
- A fully integrated periodic replica comparison service

These items are omitted rather than represented by placeholders. Add them only
with executable behavior, documentation, and a focused correctness test.

## Beta certification status

The Linux beta passed three sustained RF3 runs at both 64 and 128 clients,
acknowledged-write verification, replica convergence, failover, manual
backup/restore, genuine disk-full injection, and Debian package installation.
See [docs/COMMUNITY_P0_CERTIFICATION.md](docs/COMMUNITY_P0_CERTIFICATION.md) for
the measured results.

Physical power-controller testing, mixed-version rolling upgrades, an
independent security review, and signed native Windows/macOS artifacts remain
open before a broader production-readiness claim.

## Community operational boundary

Local projects organize database names and do not grant access or represent
SaaS organizations. Database authorization remains the security boundary.
`pacificdb_meta` and the bootstrap `system` database are reserved by the engine
and omitted from normal database listings. Ordinary requests cannot query or
modify their internal records.

Media uploads use sequential Base64-safe chunks sized from the engine's actual
request limit. A manifest becomes visible as ready only after all chunks and
the complete SHA-256 checksum verify. Interrupted uploads require an explicit
resume ID or manual cleanup. There is no application-level total video or file
size check, though disk space, network time, and other resources still limit
what a machine can store.
