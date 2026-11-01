# Storage failure qualification: release blocked

The complete Community runner failed its genuine ENOSPC test on 2026-11-01.
These results supplement the narrower successful vector/regression checkpoints;
they must not be replaced by those PASS results. Public release/tag creation is
blocked. All reproductions use disposable 128 MiB tmpfs mount namespaces.

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

## Unresolved: failed response with a mutation recovered after restart

The unchanged full ENOSPC gate still fails:

- `database-drop`: the request receives an error; after restart the explicitly
  targeted database is absent, while the test requires its baseline to remain.
- `document-update`: the request receives an error; after restart the explicitly
  requested value `2` is visible, while the test requires the old value `1`.
  This also reproduces against the unmodified packaged `f1d48ad` binary, before
  the catalog fix. The catalog fix did not introduce this failure.

The single-node Raft path durably appends, advances commit, applies, and then
persists progress. It catches a later failure and returns false; some response
paths describe that as not committed. This ordering is a confirmed integration
point, not a complete diagnosis of every failed operation. Resolving the gate
requires storage/Raft error-outcome and recovery validation. Neither pretending
success without the required durable progress nor assuming rollback after commit
is acceptable. No consensus/durability change was made as a release shortcut.

Reproduce without changing the existing assertions:

```sh
PACIFICDB_ENOSPC_CATEGORIES=database-drop \
  scripts/test-community-disk-full.sh build-rc
PACIFICDB_ENOSPC_CATEGORIES=document-update \
  scripts/test-community-disk-full.sh build-rc
scripts/test-community-disk-full.sh build-rc
```

The full runner aborts at the first failure, so later stages of that invocation
are unexecuted. Individual successful RF3/SDK/checkpoint runs retain their own
scope. A diagnostic subset is not a passing full disk-full matrix.

The independent desktop build-tool advisory gate also remains Failed. Windows/
macOS installers, mixed-version upgrade, power-loss, external security review and
production readiness are not qualified by the local Linux checks. Draft staged
packages are not a published or release-qualified candidate.
