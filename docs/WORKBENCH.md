# PacificDB Workbench

PacificDB Workbench is a desktop application with its own window, app-menu
launcher, and native file dialogs. The desktop installer includes the graphical
workspace, native database engine, native CLI, and Electron runtime. End users
do not need Node.js, npm, a browser, or a separately installed database server.

## Install the Linux desktop preview (x86-64)

Download the public 1.0.1 Linux installer and its checksum:

- [Workbench Linux .deb](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/workbench-linux-v1.0.1/PacificDB-Workbench-1.0.1-linux-amd64.deb)
- [SHA256SUMS](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/workbench-linux-v1.0.1/SHA256SUMS)

Open the downloaded package in your software installer, or run:

```sh
sha256sum --ignore-missing -c SHA256SUMS
sudo apt install ./PacificDB-Workbench-1.0.1-linux-amd64.deb
```

Then launch **PacificDB Workbench** from the Applications menu. You can also run
`pacificdb-workbench` from a terminal. The app starts its bundled engine and
closes that engine when you quit. Documents persist between launches. No
external database account or internet connection is required for normal use.

The public x86-64 installer is built and tested on Ubuntu 24.04. A locally
built Debian 13 package was tested separately; other distributions need
compatible native libraries.
This public download is a preview, not a production certification. The package
is unsigned; verify its SHA-256 checksum before installation.

A portable `.tar.gz` is also produced in `dist/desktop`. Extract it and run its
`pacificdb-workbench` executable. The `.deb` is recommended on Debian because it
installs the desktop entry and configures the Chromium sandbox helper.

## Data and the included CLI

Desktop data lives under the application data directory, in its `database`
subfolder. On Linux this is normally:

`~/.config/PacificDB Workbench/database`

Use **File → Open data folder** to locate it. This directory is independent of
the command-line package's `~/.local/share/pacificdb` data. Existing CLI projects
are not moved or imported automatically.

The native `pacificdb` CLI ships alongside `db_engine` inside the app's
`resources/engine` directory. While Workbench is running, choose **File → Copy
CLI connection command** and paste it into your terminal. That command connects
the bundled CLI to the same engine and data currently shown in the app. On
Windows the copied command uses PowerShell syntax.

## Build the desktop app from source

Development requires Node.js 22.12 or newer, CMake, a C++17 compiler, and the
engine build dependencies described in the main README. Build both binaries:

```sh
npm ci
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.0.1
cmake --build build --target db_engine pacificdb -j2
npm run workbench:desktop
```

Create the Linux installer and portable archive:

```sh
ELECTRON_BUILDER_COMPRESSION_LEVEL=1 npm run desktop:build -- --linux deb tar.gz --x64
```

For Ubuntu 24.04, set `PACIFICDB_DESKTOP_UBUNTU_24=1` for that build. If an
earlier copy of version 1.0.1 is already installed, install the rebuilt package
with `sudo dpkg -i dist/desktop/PacificDB-Workbench-1.0.1-linux-amd64.deb`.
The package installs under `/opt/PacificDB-Workbench`; the app menu still shows
**PacificDB Workbench**. Reinstalling does not remove the existing database in
`~/.config/PacificDB Workbench`.

The preparation script copies only the app sources, SDK, licenses, and native
binaries into a staging directory. It does not include local database files,
credentials, development dependencies, or the repository's working files.
Set `PACIFICDB_WORKBENCH_ENGINE` to an alternate absolute `db_engine` path; the
matching `pacificdb` executable must be next to it.

Windows NSIS and macOS DMG packaging configurations are included. Build on the
target operating system with matching native binaries and use `--win nsis` or
`--mac dmg`. For Windows, set `PACIFICDB_WORKBENCH_ENGINE` to the Release build's
`db_engine.exe` path. The CLI executable must be beside it. If the native build
uses DLLs, set `PACIFICDB_WORKBENCH_ENGINE_LIBS` to their directory. The desktop
workflow in `.github/workflows/workbench-desktop.yml` builds and launches Linux
x64, Windows x64, macOS Apple silicon, and macOS Intel packages on their native
runners, then uploads installers as workflow artifacts. Its Linux CI package
targets Ubuntu 24.04; the local package documented above targets Debian 13.
Windows and macOS installers have not been built or validated in this Linux
session.

For a future multi-platform release, push a `workbench-vVERSION` tag matching the CLI
package version. The workflow requires Windows signing secrets
`WINDOWS_CERTIFICATE_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD`; for macOS it
requires `MAC_CSC_LINK` (Developer ID Application certificate),
`MAC_CSC_KEY_PASSWORD`, `APPLE_ID`, `APPLE_APP_SPECIFIC_PASSWORD`, and
`APPLE_TEAM_ID`. It signs the bundled native binaries, notarizes the macOS app,
verifies the packages, publishes four installer variants, and adds SHA-256
checksums to a GitHub Release. Normal branch and pull request runs only upload
workflow artifacts. The release step has not run yet.

The renderer runs sandboxed with Node integration disabled. A private local
service connects it to the bundled engine. The app denies outside navigation,
new windows, and permission requests. Display preferences use a stable app
origin so they survive restarts.

## Optional browser mode from source

`pacificdb workbench` is an npm CLI subcommand. The native `pacificdb` in
`/usr/bin` does not recognize it. Use `pacificdb-workbench` for the installed
desktop app, or the npm scripts below from this repository.

Use Node.js 22.12 or newer for this repository. Build the engine using the repository's source build
instructions, then run these commands from the repository root:

```sh
npm ci
PATH="$PWD/build:$PATH" npm run workbench
```

Open the printed `http://127.0.0.1:PORT/` address. To choose the browser port:

```sh
PATH="$PWD/build:$PATH" npm run workbench -- --ui-port 3001
```

`--port` selects the engine port (9000 by default); `--ui-port` selects the
browser port. Connect to an already running local engine with:

```sh
npm run workbench -- --host 127.0.0.1 --port 9000 --ui-port 3001 --no-start
```

The npm package does not contain the native engine. Automatic local startup
requires `db_engine` on PATH, or an absolute `PACIFICDB_ENGINE` path. The
Workbench HTTP server listens only on loopback. Its current browser interface
has no account login or team management.

Keep the terminal running while using Workbench. Ctrl+C closes the Workbench
server. An engine started automatically continues running for other clients.

## Share the optional CLI-only npm build

Workbench changes in this checkout are not automatically available from npm's
published `latest` version. Build local packages for another user:

```sh
mkdir -p dist
npm pack --workspace @pacificdb/client --workspace @pacificdb/cli --pack-destination dist
```

Give them both generated `.tgz` files and a compatible native PacificDB engine
package. After installing the engine, they can install the two npm packages:

```sh
npm install --global ./pacificdb-client-1.0.1.tgz ./pacificdb-cli-1.0.1.tgz
pacificdb workbench
```

The CLI archive contains its browser assets; running Workbench does not require
a clone of this repository. Publishing packages or creating signed native
installers is a separate release step.

## Working with data

1. Create or select a project, then a database, then a collection.
   The left explorer nests databases under their project and collections under
   their database. The Documents page shows the selected project ID in a compact
   context strip; connection examples also include it.
2. Open **Data Explorer → Documents** and enter a JSON filter, for example
   `{"status":"active"}`. Use **Query Workbench** for a focused query and results
   view. Its **Edit in Documents** action opens the document editor.
3. Run the query. Choose 25, 50, or 100 records per page and use Previous/Next.
4. Choose Edit on a record, or New document to insert one. Updating and deleting
   require a nonempty filter; deletion asks for confirmation.
   Document cards and the editor show application fields; internal engine
   bookkeeping remains in storage and is hidden from the editing view.
5. Use Vectors for numeric embeddings and nearest-neighbor queries.
6. Use Media to upload files up to 64 MiB and download files in the collection.

The query duration displayed is browser-to-Workbench elapsed time, including
transport and engine work. It is not an isolated engine benchmark. Documents
are rendered one page at a time; new document/media queries abort obsolete
browser requests. Navigation ignores responses from older selections. Overview
counts describe the loaded projects, selected project's databases, and selected
database's collections. The dashboard's document total and distribution use
actual counts for the selected database; unavailable counts display a dash.
Engine health, memory ratio, Raft role, term, and commit index come from the
running engine. Recent activity lists actions in the current Workbench window,
not a database audit log. Project lists with more pages show a `+` count.

## Collection indexes and document tools

Open a collection and choose **Indexes** to list its real engine indexes. The
engine manages the primary `id` index. Create, rebuild, and delete single-field,
non-unique B-tree indexes; ascending, descending, and sparse options use the
existing engine API. **Validate** checks the collection's indexes and exposes
the engine report. Automatic indexing budgets, unique, compound, text, TTL,
and vector index creation are unavailable in this API.

Documents support JSON cards, a table of the current page, nested field
inspection, copy controls, and confirmed deletion. The JSON editor includes
validation, line numbers, formatting, and a syntax preview. Query history
stores up to 20 successful filters in this session and separates collection
scopes. Ctrl/Cmd+Enter runs the filter in Data Explorer and Query Workbench.

Vector searches expose cosine, L2, and dot-product metrics plus metadata
filters. Media search filters the loaded page; previews support images and
text files up to 1 MiB. Downloads and confirmed deletion use the same engine
storage. Monitoring includes the live engine Prometheus report and existing
health/Raft values; no historical charts or estimated storage metrics are
shown.

Project, database, and collection deletion asks for confirmation. The engine
requires a project's databases to be removed before deleting that project.

## Preferences and keyboard controls

- The settings button selects light, dark, or system appearance and comfortable
  or compact density. Row count and document view also persist in this browser.
- `/` focuses workspace navigation search. It searches the loaded navigation
  items; load more projects to include later pages.
- Ctrl+K or Cmd+K opens the command search for navigation and collections.
- Ctrl+Enter or Cmd+Enter runs the document filter.
- Arrow keys navigate the collection tabs in narrow windows. Escape closes
  dialogs/navigation.

Only display preferences are stored in browser local storage. Query text,
documents, and credentials are not saved there by Workbench.

The default appearance is dark blue. Choose **Local engine** to see the current TCP
host and port and copy examples for the bundled CLI, Node.js client, or Java
client. Examples include the selected project ID and database. The desktop
engine listens on loopback and runs only while Workbench is open; the port can
change on the next launch. PacificDB clients use a host and port rather than a
MongoDB connection URI.

## Verify changes

```sh
npm run test:workbench:desktop
npm run test:npm
node scripts/test-workbench-e2e.mjs build
npx playwright install chromium
npm run test:workbench:browser -- build
```

The browser test uses a temporary engine and data directory, exercises creation,
CRUD, bounded pagination, stale query cancellation, vectors, file transfer,
preferences, and mobile layout, then cleans up. Set `WORKBENCH_SCREENSHOTS` to a
directory to retain desktop and mobile screenshots. Playwright is a development
dependency and is not included in the distributable CLI package.

The desktop test opens a real Electron window with a temporary data directory,
creates a project/database/collection and document, uploads media, verifies the
sandbox, quits, then reopens to verify data and preferences. To test an unpacked
Linux package instead of the development entry point:

```sh
PACIFICDB_TEST_DESKTOP="$PWD/dist/desktop/linux-unpacked/pacificdb-workbench" npm run test:workbench:desktop
```
