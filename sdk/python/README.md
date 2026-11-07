![PacificDB](https://raw.githubusercontent.com/hitesh-reddy-k/pacificdb-community/main/site/assets/pacificdb-logo-symbol.png)

# PacificDB Python client

This source package is **1.2.0**. Public RC.2 packages remain `1.2.0rc2`;
they do not contain all current source changes. Use the wheel/source distribution
from your selected release. Install `pacificdb==1.2.0` from PyPI only after that
version is confirmed published.

A database-first client for the Community engine. Requires Python 3.10 or later;
no runtime dependencies. The examples use public APIs available in this version.

Start a PacificDB server, then connect to a database directly:

```python
import os
from pacificdb import PacificDB, PacificDBError

url = os.environ.get('PACIFICDB_URL', 'pacificdb://localhost:9000/app')
with PacificDB.connect(url) as db:
    db.create_database()             # create the URL's database and select it
    db.create_collection('users')
    db.insert('users', {'id': '1', 'name': 'Ada'})
    print(db.find_one('users', {'id': '1'}))
    db.update_one('users', {'id': '1'}, {'$set': {'name': 'Grace'}})
    print(db.count('users'))
```

Create calls are not implicit or idempotent: omit them when the database or
collection already exists. No project or separate selection call is required.
The existing `PacificDBClient(host, port, user_id, database, use_tls, ca_file,
timeout)` constructor still works. `PacificDB` is its shorter alias.

`connect(url, **options)` eagerly authenticates URL credentials and checks the
connection with `ping`; `from_url(url, **options)` connects lazily on the first
request. URL credentials must contain both username and password. Use an
environment variable for private URLs, not a literal in source or logs. Use
`pacificdbs://` for TLS with verified hostname and default trust, or provide
`ca_file='/path/to/ca.pem'`. Authentication on plain TCP does not encrypt traffic.

URL defaults: port 9000, user `system`, 30-second deadline, pool size 16. Query
keys are `userId`, `timeoutMs`, `poolSize`, `caFile`; duplicate/unknown keys and
conflicting explicit options fail before connecting. Python options use
`user_id`, `timeout` (seconds), `pool_size`, `ca_file`. Pools are limited to 32;
responses default to a 64 MiB limit (`max_response_bytes` can lower it). Idle
connections are reused only when the engine explicitly advertises keepalive.

Database methods: `create_database(name=None, db_type='binary')`,
`list_databases()`, `use_database(name)`, `drop_database(name=None)`. Creation
selects on success; switching checks the engine's authorized listing. Failed
create/switch/drop calls preserve selection; dropping the selected database
clears it. `create_collection`, `list_collections`, and `drop_collection` operate
on the selected database.

Document methods: `insert`, `insert_many`, `find`, `find_one`, `count`,
`aggregate`, `explain`, `update_one`, `update_many`, `delete_one`, `delete_many`,
`bulk_write`. `find(collection, filter=None, limit=-1, offset=0)` returns the
original engine response; `find_one` returns the first document or `None`.
`list_databases` returns a list of names. Other methods preserve protocol
responses, including partial bulk results. Use `capabilities()` for the server's
feature report, and `request({...})` for raw protocol access. The engine remains
responsible for namespace rules and authorization.

Every operation snapshots selected scope and caller JSON before network I/O.
Do not mutate input while the method itself is copying it. Selection is shared
client state; use separate clients when independent concurrent selection is
needed. A completed drop cannot clear a newer selection of another database.

`PacificDBError` preserves `.code` and a sanitized `.response`. Known
credentials are redacted from error diagnostics. Successful document values are
unchanged. A disconnect/timeout after sending a write can have an unknown
outcome: **writes are never silently retried**. The deadline covers pool waiting
and socket I/O; OS DNS resolution can exceed it. `close()` is idempotent and
wakes queued calls; always use a context manager or close explicitly.

Before reusing an idle connection, the pool retires sockets with buffered input
or an observable close. This happens before dispatch, without replaying a sent
command. A close racing the dispatch still returns an explicit unknown-outcome
error. Healthy TCP/TLS connections remain reusable.


## Media and backups

```python
from pathlib import Path

# Small in-memory attachments; normal collection documents.
db.media.put('assets', 'logo', b'\x00\xff', {'contentType': 'image/png'})
attachment = db.media.get('assets', 'logo')  # data: bytes, metadata: dict

# Files are hashed and transferred in bounded chunks.
manifest = db.media.upload_file('assets', Path('video.mp4'))
db.media.download_file(manifest['id'], Path('download.mp4'), collection='assets')
backup = db.backups.create(description='before maintenance')
db.backups.export(backup['backup_id'], Path('backup.json'))
```

On interrupted uploads, catch `MediaUploadError` and inspect `upload_id`,
`next_chunk`, `received_chunks`, `received_bytes` and `resumable`. Resume
explicitly with the same file, collection and chunk setting:
`db.media.upload_file('assets', path, resume=error.upload_id)`.
A lost acknowledgement can mean a chunk was stored; resume asks the server
which chunks exist and skips them. It never replays a write automatically.

Uploads reject empty/nonregular files and changed content. File downloads
require a selected database and validate the returned database/optional
collection, chunk sizes/indexes/base64/SHA-256 and whole file. Downloads and
backup exports write a private unique sibling temporary file, then replace
the destination only after verification. The parent directory must exist.
Backup export streams the existing `pacificdb-full-backup-v1` JSON format;
it is not an engine restore API. Filesystem errors are typed errors.
Each network call has a deadline; an entire multi-chunk transfer has no single
overall deadline. In-memory attachments allocate the supplied bytes; use the
file APIs for large media.

Apache-2.0. Stable-release and registry qualification remain separate.

Advanced named operations and wire aliases are documented in [SDK capabilities](https://github.com/hitesh-reddy-k/pacificdb-community/blob/main/docs/SDK_CAPABILITIES.md).
