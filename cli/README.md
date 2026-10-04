<p align="center">
  <img src="https://raw.githubusercontent.com/hitesh-reddy-k/pacificdb-community/pacificdb-v1.0/site/assets/pacificdb-logo-symbol.png" width="112" alt="PacificDB logo">
</p>

# PacificDB CLI

The Apache-2.0 Community CLI talks directly to a PacificDB engine.

```sh
npm install --global @pacificdb/cli@latest
pacificdb
```

This package source is PacificDB CLI `1.0.1`.

Plain `pacificdb` opens the shell. For a loopback connection, it starts
`db_engine` automatically when the executable is available on `PATH`.
Installing the npm CLI alone does not install the database engine; install a
native PacificDB package first or connect to another host.

Run `pacificdb workbench` to open the local browser Workbench. It prints a
`http://127.0.0.1:PORT/` URL and listens only on the loopback interface.
Use `--ui-port PORT` to select the browser port.
This subcommand belongs to the npm `@pacificdb/cli` executable. The native
`/usr/bin/pacificdb` command from `pacificdb-community` does not provide it.
For the installed desktop GUI, run `pacificdb-workbench` or open PacificDB
Workbench from the Applications menu. From a source checkout, run
`npm run workbench:desktop` for the desktop GUI or `npm run workbench` for
browser mode.

Workbench includes a workspace overview, database/collection navigation,
document filters and editing, vector search, and media upload/download. The
settings button changes appearance and density. Documents use pages of 25, 50,
or 100 records; Ctrl/Cmd+Enter runs the current filter. Press `/` to search the
navigation. Media uploads through Workbench are limited to 64 MiB per file.

Keep the Workbench command running while using the browser. Ctrl+C stops the
Workbench; a locally started database engine continues running.

```sh
pacificdb --version
pacificdb --host 127.0.0.1 --port 9000 ping
pacificdb --host db.example.internal --port 9000 --no-start
pacificdb request '{"action":"ping"}'
```

Create and query data:

```text
create database app
create collection users
insert users {"id":"1","name":"Ada"}
findOne users {"id":"1"}
```

The shell includes databases, document queries,
manual backups, API keys, media, vectors, local context, history, and raw JSON
requests. Run `help` for the complete command list.

Creating a database selects it after success. For an existing database, use
`use app` or connect with `pacificdb --url pacificdb://localhost/app`.
`PACIFICDB_URL` supplies the default URL; `--url` overrides it. Put credentials
in a protected environment variable rather than shell commands/history.
`pacificdbs://` uses verified TLS and never starts a local plaintext engine.
Conflicting URL and host/port/database flags fail before local startup.

Only the database is saved in context. Older context files keep their database
and discard project IDs/tokens; a restored database is validated before use.
Legacy project APIs and stored metadata remain available through SDK methods
and `request` JSON, while friendly project commands are removed.

Media uses bounded, sequential, checksummed chunks:

```text
upload video ./demo.mp4 --collection videos
list media
download media media_... ./downloaded.mp4
```

After a transport interruption, both native and npm shells print a structured
`media_upload_interrupted` response containing the stable `upload_id` and
`next_chunk`. Resume that same upload explicitly:

```text
upload video ./demo.mp4 --collection videos --resume media_...
```

Manual backup export includes every physical backup file:

```text
create backup --name before-upgrade
backup verify backup_...
backup export backup_... ./before-upgrade.json
```

Vector search:

```text
put vector embeddings hero [0.2,0.8]
query vector embeddings [0.2,0.8] --k 5 --metric cosine
```

Local context and history are stored with owner-only permissions. Complete API
keys are excluded from history.
