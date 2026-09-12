<p align="center">
  <img src="site/pacificdb-logo.png" width="144" alt="PacificDB logo">
</p>

<h1 align="center">PacificDB Community</h1>

<p align="center">
  Self-hosted documents, vectors, media, backups, and RF3 replication.
</p>

> **Beta:** `0.1.0-beta.8` is a tested Community release candidate for evaluation,
> development, staging, and controlled early-adopter deployments. Read
> [the certification report](docs/COMMUNITY_P0_CERTIFICATION.md) before storing
> critical data.

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

## Install

### Native package

Download the beta package for your platform from
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
must only connect to an already-running engine.

Windows and macOS beta installers are currently unsigned. Review the
[certification status](docs/COMMUNITY_P0_CERTIFICATION.md) before installation.

### npm

Node.js 18 or newer:

```sh
npm install --global @pacificdb/cli@beta
npm install @pacificdb/client@beta
```

The npm CLI is a client. It can automatically start `db_engine` when a native
PacificDB server package is installed and available on `PATH`. Installing only
the npm package does not install the database engine.

### Docker

```sh
git clone https://github.com/hitesh-reddy-k/pacificdb-community.git
cd pacificdb-community
docker compose up -d --build database
docker compose run --rm shell
```

Data remains in the `pacificdb-data` volume. Run `docker compose down` to
stop the containers. Add `-v` only when you intend to delete the volume.

## First database

Run:

```sh
pacificdb
```

Then enter:

```text
create project demo
list projects
use project project_...

create database app
use app
create collection users
insert users {"id":"1","name":"Ada","active":true}
find users {"active":true}
findOne users {"id":"1"}
update users {"id":"1"} {"name":"Ada Lovelace"}
count users {}
```

The prompt displays the selected database:

```text
pacificdb:app>
```

Run `help` for the complete categorized command list and `quit` to leave the
shell. Leaving the shell does not stop the background engine.

Local mode listens only on `127.0.0.1:9000` and starts with authentication
disabled. Configure authentication and TLS before exposing the engine to a
network.

## Shell command reference

| Area | Commands |
|---|---|
| Authentication | `login`, `whoami`, `logout` |
| Projects | `create project`, `list projects`, `use project`, `show project`, `delete project` |
| Databases | `create database`, `list databases`, `use`, `show database`, `drop database` |
| Collections | `create collection`, `list collections` |
| Documents | `insert`, `find`, `findOne`, `update`, `delete`, `count` |
| Queries | `aggregate`, `explain` |
| Backups | `create backup`, `list backups`, `show backup`, `backup verify`, `backup export`, `restore backup`, `list restores`, `delete backup` |
| API keys | `create api-key`, `list api-keys`, `show api-key`, `revoke api-key` |
| Media | `upload image|video|media`, `download media`, `list media`, `find media`, `show media`, `delete media`, `media cleanup` |
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

Start the native `pacificdb` command once before running an application.

### Node.js

```js
import { PacificDBClient } from '@pacificdb/client';

const db = new PacificDBClient({
  host: '127.0.0.1',
  port: 9000,
  database: 'app'
});

await db.createCollection('events');
await db.insert('events', { id: 'event-1', type: 'signup' });
console.log(await db.find('events', { type: 'signup' }));
```

See [sdk/node/README.md](sdk/node/README.md) for media, vectors, and backup
export.

### Python

Install the current source client:

```sh
python -m pip install ./sdk/python
```

```python
from pacificdb import PacificDBClient

db = PacificDBClient(database="app")
db.insert("events", {"id": "event-2", "type": "purchase"})
print(db.find("events", {"type": "purchase"}))
```

### Java

Build the current source client:

```sh
mvn -f sdk/java/pom.xml package
```

```java
var db = new PacificDBClient("127.0.0.1", 9000, "app");
var result = db.request(Map.of(
    "action", "find",
    "collection", "events",
    "filter", Map.of("type", "signup")
));
```

## Authentication

Authentication is disabled only for the loopback local-development launcher.
When the engine is configured with authentication:

```text
login admin
Password:
whoami
```

The password is not stored in history. Session tokens and CLI context are
written with owner-only permissions. Applications can authenticate with a
username/password or use an API key.

## Local files

| Platform | Engine data |
|---|---|
| Linux | `${XDG_DATA_HOME:-$HOME/.local/share}/pacificdb` |
| macOS | `~/Library/Application Support/PacificDB` |
| Windows | `%LOCALAPPDATA%\PacificDB` |

Each directory contains `data`, `backup`, `restore`, `engine.log`, and
`engine.pid`. Set `PACIFICDB_HOME` before the first launch to use another
absolute location.

CLI context and history use the platform state directory and never store
plaintext passwords or complete API keys.

## Connect to another engine

```sh
pacificdb --host db.example.internal --port 9000 --no-start
pacificdb --host db.example.internal --port 9000 ping --no-start
```

The CLI automatically starts an engine only for loopback hosts.

## Build and test

Requirements: CMake 3.20+, a C++17 compiler, OpenSSL, LZ4, Node.js 18+,
Python 3.10+, Java 11+, and Maven.

```sh
git clone https://github.com/hitesh-reddy-k/pacificdb-community.git
cd pacificdb-community
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release
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

## Beta status and support

The Linux candidate passed 57 Linux test units, genuine ENOSPC coverage across
21 write categories, six 10-minute RF3 load rounds, partition/election checks,
and real Debian package installation. Physical power-controller testing and
signed native Windows/macOS certification remain open.

- [Certification report](docs/COMMUNITY_P0_CERTIFICATION.md)
- [Security policy](SECURITY.md)
- [Issue tracker](https://github.com/hitesh-reddy-k/pacificdb-community/issues)

## Licensing

- Engine, query intelligence, deployments, and benchmarks: AGPL-3.0
- Node.js, Python, Java clients and CLI: Apache-2.0
- Bundled dependencies: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

Commercial use is allowed under these licenses. Network users of modified
AGPL-covered server code must be offered the corresponding source. Obtain legal
advice if your company requires a different licensing model.
