# RC.2 context: unchanged RC.1 engine/storage implementation

The following historical findings retain their original revision and test scope.
They are not relabeled as new RC.2 qualification.

# Storage failure qualification and fixes

The complete Community runner failed its genuine ENOSPC test on 2026-11-01.
The original failures remain evidence; later scoped PASS results below describe
the fixes. Public release/tag creation still requires final exact-artifact receipts. All reproductions use disposable 128 MiB tmpfs mount namespaces.

## Fixed: partial database and collection catalogs

Creating a database wrote directly to `db.meta` and did not check write/close
failure. ENOSPC could leave an empty catalog. The schema discovery performed at
restart then refused recovery. Ordinary collection creation and orphan-marker
repair also wrote the live catalog directly, and could report an ID after a
failed catalog write. Replicated apply could accept an orphan marker as success.

`DatabaseEngine::ensureUserRoot`, `createDatabase` and the ordinary collection
creation/repair paths now reuse the existing checked temporary-file replacement
helper. The helper checks close errors before publication. Replicated collection
apply requires a successfully returned catalog ID. Existing schema-specific
durable publication, record formats, WAL fsync, Raft commit rules and document/
media mutation paths remain unchanged. Database directories must be writable for
atomic replacement; a writable catalog file alone is insufficient.

The new `db_engine_database_metadata_failure_test` failed before the fix and now
passes. It checks unchanged existing metadata, no partial new catalog, orphan
marker repair, retry, and failed replicated apply. Genuine ENOSPC tests pass for
database creation, collection creation and collection drop after the complete fix.
Raw RED/GREEN logs are retained in the local release evidence directory.

## Fixed: pre-apply commit publication and rejected-tail isolation

The unchanged full ENOSPC gate still fails:

- `database-drop`: the request receives an error; after restart the explicitly
  targeted database is absent, while the test requires its baseline to remain.
- `document-update`: the request receives an error; after restart the explicitly
  requested value `2` is visible, while the test requires the old value `1`.
  This also reproduces against the unmodified packaged `f1d48ad` binary, before
  the catalog fix. The catalog fix did not introduce this failure.

The original standalone path appended and applied before its checksummed commit
record was known durable. ENOSPC could still permit an append into an existing
log page while preventing allocation of the checkpoint temporary file. In addition,
a later audit write could commit across a rejected tail. Both are now addressed:

- The existing node-local Raft log lock covers append, checked log sync and durable
  commit publication. Application and reads remain outside that lock. This trades
  standalone append concurrency for an atomic publication boundary; new benchmarks
  must include the ingestion/concurrent-write cost.
- Commit publication precedes visible commit/apply. Pre-publication failure rolls
  the rejected/partial tail back under the same ownership, synchronizes the rollback,
  and leaves the previous commit marker intact. A later append cannot commit it.
- Log-sync failure is propagated; group waiters are released without marking the
  failed sync durable. A node with uncertain synchronous I/O is fenced from writes,
  elections, strong reads and heartbeat/tail acknowledgements until restart.
- A post-rename directory-sync failure is an **unknown outcome**, not guaranteed
  rollback. The node refuses further persistence and work until recovery. The tail
  is retained because the new commit record may already be visible/durable. The
  error includes `write_outcome_unknown`; callers must inspect state after
  recovery rather than automatically retrying a non-idempotent mutation.
- Election term/vote intent is monotonic, while failed pre-publication progress does
  not enter the cached durable commit/apply watermark. Windows replacement uses
  write-through atomic replacement instead of deleting the live checkpoint first.

The real unchanged ENOSPC suite passes all **21 categories** after the integrated
fix. Four additional focused CTest cases pass: failed checkpoint publication,
ordinary/group fdatasync errors, and post-rename directory-sync failure; actual
heartbeat requests verify that fenced nodes do not advertise a matching tail.
All 50 CTest cases passed on the integrated source before the final source-bound
artifact rebuild. Final release receipts separately identify the shipped binary.

A general distributed timeout/error after quorum is not proof of rollback. No
new unconditional rollback promise is made. The ENOSPC test specifically verifies
rejection before publication and retained baseline state under that scenario.

Reproduce without changing the existing assertions:

```sh
PACIFICDB_ENOSPC_CATEGORIES=database-drop \
  scripts/test-community-disk-full.sh build-rc
PACIFICDB_ENOSPC_CATEGORIES=document-update \
  scripts/test-community-disk-full.sh build-rc
scripts/test-community-disk-full.sh build-rc
```

The original full runner aborted at its first failure; later stages of that
invocation were unexecuted. Its raw failure is retained. The new complete runner
and final checkpoint must independently pass; a diagnostic subset cannot replace them.

The desktop matcher dependency is also repaired without an advisory exception;
full/runtime npm audits contain zero findings. Native platform installers require
their own runtime receipts. Mixed-version, physical power-loss, external security
and stable production qualification remain separate unqualified gates.
