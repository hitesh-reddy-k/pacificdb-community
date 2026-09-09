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
- Resumable, checksummed image, GIF, audio, video, and arbitrary file storage
- Read-only natural-language query compilation and query explanations
- TLS, password/token authentication, API keys, RBAC, tenant isolation, and local audit logging
- Manual snapshot backup, verification, deletion, and restore
- Health, logs, engine metrics, and Prometheus output
- JSON-over-TCP API, CLI/shell, Node.js, Java, and Python clients
- Dockerfile, Docker Compose, Kubernetes manifests, Helm chart, and reproducible YCSB binding

The exact implemented scope and current gaps are in [COMMUNITY_SCOPE.md](COMMUNITY_SCOPE.md).

Install the Node.js client or command-line package after a beta is published:

```sh
npm install @pacificdb/client@beta
npm install --global @pacificdb/cli@beta
```

## Run locally

Install Docker with the Compose plugin, download this repository, and run:

```sh
docker compose up -d --build database
docker compose run --rm shell
```

The shell connects to `database:9000`. Friendly commands are available:

```text
create project demo
use project project_...
create database app
use app
create collection users
insert users {"id":"1","name":"Ada"}
findOne users {"id":"1"}
```

Type `help` for the categorized command list. Raw protocol requests remain
available as `request {"action":"ping"}`. Type `quit` to leave the shell.
Data remains in the `pacificdb-data` Docker
volume. Stop the database with `docker compose down`; add `-v` only when you
also want to delete the local database and backups.

To use the host CLI when Node.js 18+ is installed:

```sh
node cli/bin/pacificdb.js ping
node cli/bin/pacificdb.js shell --database app
```

Run the complete local startup and CRUD check with
`scripts/test-local-compose.sh`. PacificDB Community is self-hosted; an
Atlas-style managed service requires the separate PacificDB Cloud control plane.

## Native downloads

The release workflow builds separate packages for Linux (`.deb`), Windows
(`.exe` installer), macOS Apple silicon (`arm64.pkg`), and macOS Intel
(`x86_64.pkg`). The server and native `pacificdb` shell are included; Node.js
is not required.

On Ubuntu or Debian:

```sh
sudo apt install ./pacificdb-community-*-linux-amd64.deb
pacificdb-local
```

On Windows, run the `.exe` installer and then start `pacificdb-local.cmd` from
a new Command Prompt. On macOS, choose the package matching `uname -m`, install
it, and run `pacificdb-local`. In a second terminal on any platform:

```sh
pacificdb ping
pacificdb shell
pacificdb put-media assets hero ./hero.gif --content-type image/gif --database app
pacificdb get-media assets hero ./downloaded.gif --database app
pacificdb put-vector embeddings hero-vector '[0.2,0.8]' --database app
pacificdb query-vector embeddings '[0.2,0.8]' --k 5 --database app
```

Inside `pacificdb shell`, `upload video ./movie.mp4` uses sequential,
checksummed chunks and applies no PacificDB total file-size cap. Available disk
space, per-request limits, and other machine resources remain real limits.

Community projects are local organizational metadata. They do not add billing,
quotas, organizations, fleet management, or a new authorization boundary.
Backups are manual, restores complete synchronously, and `backup export` writes
only the portable backup manifest. API keys have `read`, `readwrite`, or
`admin` roles; their full secrets are returned once and are never stored.

`pacificdb_meta` is reserved for these Community records. Ordinary database
requests cannot read or write it. Raw access requires an authenticated admin
and the explicit `internalAdmin: true` flag.

The local launcher binds to `127.0.0.1`, stores data under the current user's
application-data directory, and runs in the foreground. Release candidates
must be code-signed and notarized before they are presented as trusted public
installers.

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
