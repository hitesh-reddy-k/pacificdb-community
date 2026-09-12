<p align="center">
  <img src="https://raw.githubusercontent.com/hitesh-reddy-k/pacificdb-community/main/site/pacificdb-logo.png" width="112" alt="PacificDB logo">
</p>

# PacificDB CLI

The Apache-2.0 Community CLI talks directly to a PacificDB engine.

```sh
npm install --global @pacificdb/cli@beta
pacificdb
```

The npm `beta` tag currently installs `0.1.0-beta.7`. The `0.1.0-beta.8`
package is prepared but has not been published yet.

Plain `pacificdb` opens the shell. For a loopback connection, it starts
`db_engine` automatically when the executable is available on `PATH`.
Installing the npm CLI alone does not install the database engine; install a
native PacificDB package first or connect to another host.

```sh
pacificdb --host 127.0.0.1 --port 9000 ping
pacificdb --host db.example.internal --port 9000 --no-start
pacificdb request '{"action":"ping"}'
```

Create and query data:

```text
create project demo
list projects
use project project_...
create database app
use app
create collection users
insert users {"id":"1","name":"Ada"}
findOne users {"id":"1"}
```

The shell includes authentication, projects, databases, document queries,
manual backups, API keys, media, vectors, local context, history, and raw JSON
requests. Run `help` for the complete command list.

Media uses bounded, sequential, checksummed chunks:

```text
upload video ./demo.mp4 --collection videos
list media
download media media_... ./downloaded.mp4
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

Local context and history are stored with owner-only permissions. Passwords and
complete API keys are excluded from history.
