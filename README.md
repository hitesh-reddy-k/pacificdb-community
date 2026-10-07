<p align="center">
  <img src="site/assets/pacificdb-logo-symbol.png" width="96" alt="PacificDB logo">
</p>

<h1 align="center">PacificDB Community v1.1.2</h1>

<p align="center">
  Open-source, self-hosted database for documents, vectors, and media.<br>
  Security and reliability patch — source prepared; publication pending.
</p>

<p align="center">
  <a href="#quick-start">Quick Start</a> ·
  <a href="https://pacificdb.in/docs.html">Documentation</a> ·
  <a href="https://discord.gg/67w8ET9Sf2">Discord</a>
</p>

## Quick Start

Build this checkout using the [source instructions](#build-and-test), then run `./build/pacificdb`. In the shell:

```text
create database app
create collection users
insert users {"id":"1","name":"Ada"}
find users {"id":"1"}
```

Database creation selects it automatically. Projects are optional legacy metadata. See [database-first migration](docs/DATABASE_FIRST_MIGRATION.md), the [full install options](#install), and the [1.1.2 security patch notes and upgrade requirements](docs/releases/1.1.2.md).

### 1.1.2 security and real-data readiness

This open-source Community patch adds database ownership and explicit grants,
resource-derived media authorization, local-only managed listeners, bounded
protocol/regex work, persisted lockout, attributable audit events and verified
audit alerts. The maintainer reports completing verification. Retained tests
have explicit revision/platform boundaries; that statement is not independent
certification or a guarantee against vulnerabilities or data loss. Before
using real data, follow the [readiness and backup checklist](docs/releases/1.1.2.md#production-readiness-statement).

The download and registry instructions below still identify published 1.1.1
artifacts. They do not contain the 1.1.2 security fixes. Build this patch from
source until matching 1.1.2 artifacts are published and verified.

## What is included

- JSON document CRUD, filters, projection, pagination, bounded aggregation, and explain
- WAL and LSM storage, crash recovery, compaction, checksums, and snapshots
- Secondary indexes and vector similarity search
- RF3 Raft replication, elections, follower recovery, and snapshot catch-up
- Resumable checksummed storage for images, audio, video, and other files
- Manual full backups, verification, complete JSON export, restore, and restore history
- Password authentication, API keys, roles, TLS, tenant boundaries, and audit logs
- Native and npm shells plus Node.js, Python, and Java clients
- Debian, Windows, macOS, Docker, Kubernetes, and Helm packaging

See [COMMUNITY_SCOPE.md](COMMUNITY_SCOPE.md) for the exact boundary.

## v1.0 transport and ingestion

- The Node.js SDK reuses a persistent connection pool (16 sockets by default,
  configurable from 1 through 32) and provides explicit `connect()` and `close()`
  lifecycle methods.
- `insertMany(collection, documents)` sends one batch through the engine's
  grouped WAL and LSM path.
- The retained end-to-end suite verifies a 500-document batch before and after
  graceful and abrupt engine restarts.

## Install

### Native package

Download the package for your platform from
[GitHub Releases](https://github.com/hitesh-reddy-k/pacificdb-community/releases).
The native package contains the database engine and CLI.

Ubuntu or Debian:

```sh
sudo apt install ./pacificdb-community-*-linux-amd64.deb
pacificdb
```

Windows:

1. Open the downloaded `.exe` installer.
2. Open a new Command Prompt.
3. Run `pacificdb`.

macOS:

1. Install the package matching Apple silicon or Intel.
2. Open Terminal.
3. Run `pacificdb`.

Plain `pacificdb` starts the local engine when it is not already running and
opens the interactive shell. The engine continues in the background and is
reused by later CLI and application connections. Use `--no-start` when the CLI
must only connect to an already-running engine. Check the installed release
without starting the engine with `pacificdb --version`.

Windows and macOS 1.1.1 installers are unsigned under a version-specific release-owner exception. Verify downloads
against `SHA256SUMS` and review the
[certification status](docs/COMMUNITY_P0_CERTIFICATION.md) before installation.

### npm

Node.js 18 or newer:

```sh
npm install --global @pacificdb/cli@1.1.1
npm install @pacificdb/client@1.1.1
```

These commands install the previous published release. The source versions of
the CLI and Node.js client in this patch are `1.1.2`; matching registry packages
must be published before using `@1.1.2` install commands.

The npm CLI is a client. It can automatically start `db_engine` when a native
PacificDB server package is installed and available on `PATH`. Installing only
the npm package does not install the database engine.

### Desktop Workbench

[Workbench 1.1.1](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/workbench-v1.1.1) includes Linux, Windows, and macOS installers with the bundled engine and CLI.

Workbench, engine, CLI and SDK source versions are aligned at 1.1.2 for this
Community security patch. Public 1.1.2 publication is still pending. See the
[Workbench operations guide](docs/WORKBENCH.md),
[candidate readiness checklist](docs/WORKBENCH_PRODUCTION_READINESS.md), and
[candidate notes](docs/releases/workbench-1.1.2.md). Missing hosted or external
evidence blocks publication.

The desktop app bundles the Workbench GUI, native database engine, CLI, and
runtime. Users can install it and open **PacificDB Workbench** from their app
menu. It starts and stops its own local engine and retains data between launches.

Build the engine and native CLI, then run `npm run workbench:desktop` during
development. Run `npm run desktop:build -- --linux deb --x64` to create the
Debian installer in `dist/desktop`. See [the Workbench guide](docs/WORKBENCH.md)
for installation, platform requirements, data locations, and verification.

The optional browser mode remains available with
`PATH="$PWD/build:$PATH" npm run workbench`.

### Docker

```sh
git clone https://github.com/hitesh-reddy-k/pacificdb-community.git
cd pacificdb-community
docker compose up -d --build database
docker compose run --rm shell
```

Data remains in the `pacificdb-data` volume. Run `docker compose down` to
stop the containers. Add `-v` only when you intend to delete the volume.

The prompt displays the selected database:

```text
pacificdb:app>
```

Run `help` for the complete categorized command list and `quit` to leave the
shell. Leaving the shell does not stop the background engine.

`create database` selects the database after success; `list databases` lists
accessible databases and `use <name>` switches to an existing one. Connect
directly with `pacificdb --url 'pacificdb://127.0.0.1:9000/app'`.
`pacificdbs://` uses verified TLS. Authenticated connections can read a privately
configured `PACIFICDB_URL`; credentials never belong in shared commands.

Local mode listens only on `127.0.0.1:9000` and starts with authentication
disabled. Configure authentication and TLS before exposing the engine to a
network.

## Shell command reference

| Area | Commands |
|---|---|
| Databases | `create database`, `list databases`, `use`, `show database`, `drop database` |
| Collections | `create collection`, `list collections` |
| Documents | `insert`, `find`, `findOne`, `update`, `delete`, `count` |
| Queries | `aggregate`, `explain` |
| Backups | `create backup`, `list backups`, `show backup`, `backup verify`, `backup export`, `restore backup`, `list restores`, `delete backup` |
| API keys | `create api-key`, `list api-keys`, `show api-key`, `revoke api-key` |
| Media | `upload image\|video\|media`, `download media`, `list media`, `find media`, `show media`, `delete media`, `media cleanup` |
| Vectors | `put vector`, `query vector` |
| Shell | `help`, `status`, `context show`, `context clear`, `history`, `clear`, `request`, `exit` |

Examples:

```text
aggregate users [{"$match":{"active":true}},{"$project":{"name":1}},{"$limit":10}]
explain users {"id":"1"}

create backup --name before-upgrade
list backups
backup verify backup_...
backup export backup_... ./before-upgrade.json
restore backup backup_...

create api-key --name application --role readwrite
list api-keys
revoke api-key key_...

upload video ./demo.mp4 --collection videos
list media
download media media_... ./downloaded.mp4

put vector embeddings item-1 [0.2,0.8]
query vector embeddings [0.2,0.8] --k 5 --metric cosine
```

API-key roles are `read`, `readwrite`, and `admin`. A full key is shown
once at creation and is never persisted in plaintext.

Media transfers use bounded, sequential, checksummed chunks. PacificDB sets no
application-level total file-size cap; available disk, network time, and machine
resources remain limits.

Backup export writes a self-contained JSON document containing every physical
backup file in bounded, checksummed chunks.

## Connect an application

These examples require matching 1.1.1 clients. Start `pacificdb` once before running an application. Install the Node client with `npm install @pacificdb/client@1.1.1`.

For complete install, authenticated connection, CRUD, vector/media, close, upgrade, and troubleshooting examples, see [Node.js](site/docs.html#nodejs), [Python](site/docs.html#python), [Java](site/docs.html#java), [CLI](site/docs.html#shell-reference), and [Workbench](site/docs.html#workbench). A `pacificdb://` database URL uses the engine protocol; open the separately printed `http://` URL for browser Workbench. Keep private credentials in `PACIFICDB_URL` and use `pacificdbs://` for verified TLS.

### Node.js

```js
import { PacificDB } from '@pacificdb/client';

const db = await PacificDB.connect('pacificdb://127.0.0.1:9000/app');
try {
  await db.createDatabase(); // Omit this if the URL database already exists.
  await db.createCollection('events');
  await db.insert('events', { id: 'event-1', type: 'signup' });
  console.log(await db.find('events', { type: 'signup' }));
} finally {
  db.close();
}
```

See [sdk/node/README.md](sdk/node/README.md) for media, vectors, and backup
export.

### Python

Install into a virtual environment:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --no-deps \
  'git+https://github.com/hitesh-reddy-k/pacificdb-community.git@v1.1.1#subdirectory=sdk/python'
```

PyPI publication is pending trusted-publisher registration. The tagged-source
command above was verified from a clean virtual environment.

```python
import os
from pacificdb import PacificDB

url = os.environ.get("PACIFICDB_URL", "pacificdb://127.0.0.1:9000/app")
with PacificDB.connect(url) as db:
    print(db.find("events", {"type": "signup"}))
```

### Java

Maven Central ownership is not configured. The release tag predates the final
Jackson 2.18.11 security update, so install the audited release-branch revision:

```sh
git checkout 25fb81d413973b6779eaf71a26bedb42f6d79be3
mvn -f sdk/java/pom.xml install
```

Use `io.pacificdb:pacificdb-client:1.1.1` in your Maven application with Java 11+:

```java
import io.pacificdb.PacificDB;
import java.util.Map;

String url = System.getenv().getOrDefault("PACIFICDB_URL", "pacificdb://127.0.0.1:9000/app");
try (var db = PacificDB.connect(url)) {
    System.out.println(db.find("events", Map.of("type", "signup")));
}
```

Create databases and collections explicitly for new data; URL connection alone does not create them. Existing clients can still use the original constructors. See [SDK capabilities](docs/SDK_CAPABILITIES.md) for named operation families and raw protocol access.

## Local files

| Platform | Engine data |
|---|---|
| Linux | `${XDG_DATA_HOME:-$HOME/.local/share}/pacificdb` |
| macOS | `~/Library/Application Support/PacificDB` |
| Windows | `%LOCALAPPDATA%\PacificDB` |

Each directory contains `data`, `backup`, `restore`, `engine.log`, and
`engine.pid`. Set `PACIFICDB_HOME` before the first launch to use another
absolute location.

CLI context and history use the platform state directory. The CLI does not
store credentials; complete API keys are excluded from history.

## Connect to another engine

```sh
pacificdb --host db.example.internal --port 9000 --no-start
pacificdb --host db.example.internal --port 9000 ping --no-start
```

The CLI automatically starts an engine only for loopback hosts.

## Build and test

Run the following from the 1.1.2 patch checkout. The public `v1.1.2` tag is not
claimed to exist until the release workflow publishes the reviewed revision.

Requirements: CMake 3.20+, a C++17 compiler, OpenSSL development headers and
libraries, LZ4, RE2, Node.js 22.12+ for repository development, Python 3.10+, Java 11+, and Maven. On Debian or
Ubuntu, install `libssl-dev`, `liblz4-dev` and `libre2-dev` before configuring the engine; the `openssl`
command alone does not include the files CMake needs.

```sh
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.1.2
cmake --build build -j2
scripts/test-community.sh build
```

After building, `./build/pacificdb` starts `./build/db_engine` automatically.

Focused SDK checks:

```sh
npm run test:npm
PYTHONPATH=sdk/python python -m pytest sdk/python/tests
mvn -f sdk/java/pom.xml test
```

## Release readiness and support

Version 1.1.2 is the prepared Community security patch; public artifact publication and exact-release external qualification are separate gates. Retained engineering reports describe their exact revisions, validation scope, known regressions, and release-owner exceptions. See the [changelog](CHANGELOG.md), [patch notes and readiness statement](docs/releases/1.1.2.md), and [upgrade guide](site/docs.html#upgrade). Historical 1.1.1 exceptions do not automatically authorize this patch.

- [Certification report](docs/COMMUNITY_P0_CERTIFICATION.md)
- [Production release procedure](docs/PRODUCTION_RELEASE.md)
- [Production operations contract](docs/OPERATIONS.md)
- [Container and Kubernetes deployment](deploy/helm/pacificdb/README.md)
- [Documentation](https://pacificdb.in/docs.html)
- [Security policy](SECURITY.md)
- [Contributing guide](CONTRIBUTING.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Issue tracker](https://github.com/hitesh-reddy-k/pacificdb-community/issues)
- [Discord community](https://discord.gg/67w8ET9Sf2)

## Licensing

- Engine, query intelligence, and deployments: AGPL-3.0
- Node.js, Python, Java clients and CLI: Apache-2.0
- Bundled dependencies: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

Commercial use is allowed under these licenses. Network users of modified
AGPL-covered server code must be offered the corresponding source. Obtain legal
advice if your company requires a different licensing model.
