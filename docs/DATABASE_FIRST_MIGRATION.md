# Database-first migration

This change is included in the public v1.1.1 native, npm, and Workbench release
artifacts. PyPI publication is blocked on trusted-publisher registration, and
Maven Central publication remains an explicit release-owner action after
validation, so use the documented tagged-source/local Maven installs for those
two SDKs.

Create data directly in either updated CLI:

```text
create database app
create collection users
insert users {"id":"1","name":"Ada"}
find users {"id":"1"}
```

Creating a database selects it only after success. To select an existing one,
use `use app`, or start with a URL:

```sh
pacificdb --url 'pacificdb://127.0.0.1:9000/app' --no-start
```

`PACIFICDB_URL` supplies a default; `--url` takes precedence. `pacificdbs://`
requires verified TLS. URL credentials should be supplied through a protected
environment variable, not shared commands, shell history or screenshots.
Conflicting host/port/database flags are rejected before connecting.

Workbench's **New database** button creates and selects a database. Collections
appear directly below it. The connection dialog provides a database URL and
CLI/Node/Python/Java examples; copying is disabled without a selected database.
The latter two examples require the new SDK artifacts after their separate
qualification, not an older package installed from a registry.

Old CLI context files retain only their database. Project IDs, tokens and other
fields are removed on save; the selected database is validated before its first
dependent operation. Failed create/use/drop operations retain the prior
selection. A connection URL overrides a saved selection.

Friendly project commands and Workbench project screens have been removed.
Existing project records, mappings, databases and documents are unchanged.
Explicit Node `createProject`, `useProject` and constructor `projectId` remain
available and retain their opt-in membership behavior. Raw project requests
still work in both shells, for example:

```text
request {"action":"community_project_list"}
```

Use these retained APIs for legacy consumers. No catalog migration, implicit
project creation or storage-format change is performed.

Overview counts are requested only when Overview is visible or explicitly
refreshed, with at most four jobs in flight. Unknown counts display `—`.
Documents/Query/Media retain bounded pagination and scope checks. This removes
unnecessary UI work; it is not a database throughput benchmark.
