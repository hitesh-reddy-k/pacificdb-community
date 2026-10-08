# PacificDB Workbench

PacificDB Workbench is a desktop application with its own window, app-menu
launcher, and native file dialogs. The desktop installer includes the graphical
workspace, native database engine, native CLI, and Electron runtime. End users
do not need Node.js, npm, a browser, or a separately installed database server.

## Release and support status

[Workbench v1.1.2](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/1.1.2)
is the published Linux desktop prerelease, with Debian/portable downloads and
SHA-256 checksums. Final Windows/macOS installers are not published. Its
[qualification status](WORKBENCH_PRODUCTION_READINESS.md) must reach `PASS`
before stable promotion or a production-ready claim. It shares Community
1.1.2 with the bundled engine/CLI and SDK source metadata.

The packaged and hosted-test targets are Ubuntu 24.04/Debian-compatible Linux
x64 (Debian package), Windows x64 (NSIS), macOS arm64, and macOS x64 (DMG).
Other distributions and architectures are not release-qualified. The app runs
the bundled engine as the signed-in user, binds it to loopback, and closes it
when Workbench quits. Normal desktop use needs no account or internet access.

### Install and remove

Always verify the release `SHA256SUMS` before installation. Candidate 1.1.2
artifacts built from source are named:

- Linux: `PacificDB-Workbench-1.1.2-linux-amd64.deb`; install with
  `sudo apt install ./PacificDB-Workbench-1.1.2-linux-amd64.deb`, launch with
  `pacificdb-workbench`, and remove with `sudo apt remove pacificdb-workbench`.
- Windows: `PacificDB-Workbench-1.1.2-win-x64.exe`; run the installer, launch
  **PacificDB Workbench** from the Start menu, and remove it from **Installed
  apps** or with the installation directory's `Uninstall PacificDB Workbench.exe`.
- macOS: `PacificDB-Workbench-1.1.2-mac-arm64.dmg` or
  `PacificDB-Workbench-1.1.2-mac-x64.dmg`; mount it, copy **PacificDB
  Workbench.app** to Applications, and remove the app from Applications to
  uninstall it.

Uninstalling removes the application, not its database directory. Delete data
only as a separate, deliberate operation after verifying a backup. Workbench
1.1.2 has no auto-updater: upgrades and rollback are manual.

## Workbench capabilities in this candidate

- Databases appear directly in navigation; creation and selection no longer require a project. Collections sit under their database. Existing project mappings and data remain accessible without migration.
- The connection dialog copies a selected database URL, bundled CLI command and Node.js/Python/Java examples. It omits credentials and reads `PACIFICDB_URL` for authenticated engines. Copy stays disabled until a database is selected.
- Overview counts use one loader with at most four jobs in flight. The candidate reuses cached summaries for the database, invalidates affected summaries after mutations and displays `—` for unavailable totals. Documents, Query and Media views do not schedule a full collection count scan; switching away stops scheduling new count work.

These are source-verifiable changes, not measured startup-speed improvements.
Query durations include UI/server transport and engine work. See the
[1.1.2 candidate notes](releases/workbench-1.1.2.md) and
[package usage guides](../site/docs.html).

## Data and the included CLI

Desktop data lives under the application data directory, in its `database`
subfolder:

| Platform | Default application data | Engine log |
| --- | --- | --- |
| Linux | `~/.config/PacificDB Workbench` | `database/engine.log` |
| Windows | `%APPDATA%\PacificDB Workbench` | `database\engine.log` |
| macOS | `~/Library/Application Support/PacificDB Workbench` | `database/engine.log` |

Use **File → Open data folder** to locate it. This directory is independent of
the command-line package's `~/.local/share/pacificdb` data. Existing CLI databases
are not moved or imported automatically.

The native `pacificdb` CLI ships alongside `db_engine` inside the app's
`resources/engine` directory. While Workbench is running, choose **File → Copy
CLI connection command** and paste it into your terminal. That command connects
the bundled CLI to the same engine and data currently shown in the app. On
Windows the copied command uses PowerShell syntax.

## Build the desktop app from source

Run these commands from the prepared Workbench 1.1.2 candidate checkout.
Cloning the public default branch does not guarantee the candidate. Native
components and SDK source versions align at 1.1.2.

Development requires Node.js 22.12 or newer, CMake, a C++17 compiler, and the
engine build dependencies described in the main README. Build both binaries:

```sh
npm ci
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.1.2
cmake --build build --target db_engine pacificdb -j2
npm run workbench:desktop
```

### Linux sandbox helper

If the source Electron launch reports that the SUID sandbox helper is not configured correctly, set ownership and mode on the locally installed helper:

```sh
sudo chown root:root node_modules/electron/dist/chrome-sandbox
sudo chmod 4755 node_modules/electron/dist/chrome-sandbox
npm run workbench:desktop
```

For a candidate unpacked build, use that build's helper path:

```sh
sudo chown root:root dist/desktop/linux-unpacked/chrome-sandbox
sudo chmod 4755 dist/desktop/linux-unpacked/chrome-sandbox
./dist/desktop/linux-unpacked/pacificdb-workbench
```

The Debian package configures its helper automatically: [electron-builder.cjs](../desktop/electron-builder.cjs) specifies `desktop/after-install.sh`, whose [after-install hook](../desktop/after-install.sh) sets root ownership and mode 4755 on `/opt/PacificDB Workbench/chrome-sandbox`, registers `/usr/bin/pacificdb-workbench` and refreshes the desktop database. The application and engine run as your normal user.

### Create the candidate installer and portable archive


```sh
ELECTRON_BUILDER_COMPRESSION_LEVEL=1 npm run desktop:build -- --linux deb tar.gz --x64
```

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
Windows and macOS installers cannot be validated from a Linux session.

For a future public release, push a `workbench-vVERSION` tag matching the root
Workbench package version. The workflow requires Windows signing secrets
`WINDOWS_CERTIFICATE_BASE64` and `WINDOWS_CERTIFICATE_PASSWORD`; for macOS it
requires `MAC_CSC_LINK` (Developer ID Application certificate),
`MAC_CSC_KEY_PASSWORD`, `APPLE_ID`, `APPLE_APP_SPECIFIC_PASSWORD`, and
`APPLE_TEAM_ID`. It signs the bundled native binaries, notarizes the macOS app,
verifies installed packages, aggregates exact-revision qualification, publishes
four installer variants only on `PASS`, and adds SHA-256 checksums to a GitHub
Release. Normal branch and pull request runs only upload workflow artifacts.
Candidate 1.1.2 has not been published.

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

Open the printed `http://127.0.0.1:PORT/` address in your browser. A `pacificdb://host:port/database` URL addresses the database protocol and cannot be opened as a web page. To choose the browser port:

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
Workbench HTTP server listens only on loopback. Its browser interface has no account login or team management. An authenticated engine can be selected using a privately configured `PACIFICDB_URL`; Workbench does not manage those credentials.

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
npm install --global ./pacificdb-client-1.1.2.tgz ./pacificdb-cli-1.1.2.tgz
pacificdb workbench
```

The CLI archive contains its browser assets; running Workbench does not require
a clone of this repository. Publishing packages or creating signed native
installers is a separate release step.

## Working with data

1. Choose **New database**, enter a name, then create a collection.
   Creating or clicking a database selects it automatically. Collections are
   nested directly under their database. Existing project-mapped databases
   remain accessible; this workflow does not move or migrate stored data.
2. Open **Data Explorer → Documents** and enter a JSON filter, for example
   `{"status":"active"}`. Use **Query Workbench** for a focused query and results
   view. Its **Edit in Documents** action opens the document editor.
3. Run the query. Choose 25, 50, or 100 records per page and use Previous/Next.
4. Choose Edit on a record, or New document to insert one. Updating and deleting
   require a nonempty filter; deletion asks for confirmation.
   Document cards and the editor show application fields; internal engine
   bookkeeping remains in storage and is hidden from the editing view.
5. Use Vectors for numeric embeddings and nearest-neighbor queries.
6. Use Media to upload files up to 64 MiB and download files in the collection. Use the candidate SDK file APIs for larger files; their limits are disk, network and request bounds rather than this UI limit.

The query duration displayed is browser-to-Workbench elapsed time, including
transport and engine work. It is not an isolated engine benchmark. Documents
are rendered one page at a time; new document/media queries abort obsolete
browser requests. Navigation ignores responses from older selections. Overview
counts describe accessible databases and the selected database's collections. The dashboard's document total and distribution use
actual counts for the selected database; unavailable counts display a dash.
Engine health, memory ratio, Raft role, term, and commit index come from the
running engine. Recent activity lists actions in the current Workbench window,
not a database audit log. Counts load only while Overview is visible, with at
most four jobs in flight. Documents, Query and Media do not scan all collection
counts. Mutations invalidate affected summaries; unknown values display `—`.

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

Database and collection deletion ask for confirmation. Legacy project APIs
remain in the SDK and raw protocol; project screens and friendly CLI commands
are removed.

## Preferences and keyboard controls

- The settings button selects light, dark, or system appearance and comfortable
  or compact density. Row count and document view also persist in this browser.
- `/` focuses workspace navigation search. It searches the loaded navigation
  databases and the selected database’s collections.
- Ctrl+K or Cmd+K opens the command search for navigation and collections.
- Ctrl+Enter or Cmd+Enter runs the document filter.
- Arrow keys navigate the collection tabs in narrow windows. Escape closes
  dialogs/navigation.

Only display preferences are stored in browser local storage. Query text,
documents, and credentials are not saved there by Workbench.

The default appearance is dark blue. Choose **Local engine** to copy a selected
**database URL**, bundled CLI command, or Node.js, Python or Java example.
Copy is disabled until a database is selected. Examples connect directly using
`pacificdb://host:port/database` (or `pacificdbs://` for verified TLS) without a
separate database selection step. Authenticated examples read `PACIFICDB_URL`
from the environment; Workbench never includes credentials or tokens in the
example. The Java/Python examples require the updated SDK artifacts described
in the package qualification evidence, not an older published package.

The desktop engine listens on loopback and runs only while Workbench is open;
its port can change on the next launch. Browser mode accepts `--url` too:

```sh
npm run workbench -- --url 'pacificdb://127.0.0.1:9000/app' --no-start
```

## Upgrade and troubleshooting

Before every upgrade:

1. Keep the previous installer and its `SHA256SUMS`.
2. Open Workbench and choose **File → Copy CLI connection command**. Run the
   copied command in a terminal, then create, verify, and export a backup to a
   location outside the Workbench data directory:

   ```text
   create backup --name before-workbench-1.1.2
   backup verify backup_...
   backup export backup_... /external/path/workbench-before-1.1.2.json
   quit
   ```

3. Quit Workbench and confirm its engine has stopped. Make an offline copy of
   the whole platform application-data directory. Do not copy it while the app
   is running and never open one data directory from two engine processes.
4. Install the candidate over the application. Launch it, verify important
   records, create another backup, and keep the external export until the
   upgrade has been accepted.

`restore backup backup_...` verifies an internal backup and writes a separate
restore directory recorded by `list restores`; it does not overwrite the live
database. For a full rollback, quit Workbench, uninstall the candidate without
deleting application data, reinstall the retained 1.1.1 installer, and reopen
the unchanged data. If 1.1.1 cannot open it, quit immediately and restore the
offline pre-upgrade directory copy before retrying. Never merge two data
directories.

If a candidate fails to launch, leave the data directory untouched. Check
`database/engine.log` under the platform data path above; startup dialogs also
name the active log file. Reinstall 1.1.1 and use the offline copy if necessary.
For Linux sandbox errors, use the helper setup above. On any platform, confirm
that antivirus or filesystem permissions did not quarantine the bundled
`resources/engine` binaries.

Set `PACIFICDB_WORKBENCH_DATA` to an absolute alternate directory only for an
isolated source or qualification run. If the native command rejects
`workbench`, launch the installed desktop app instead; browser Workbench is the
npm CLI feature. A desktop engine port can change on relaunch, so copy a fresh
CLI connection command. Desktop quit stops its owned engine. Browser Ctrl+C
stops the HTTP service but may leave an automatically started engine available
to other clients.

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
creates a database/collection and document, uploads media, verifies the
sandbox, quits, then reopens to verify data and preferences. To test an unpacked
Linux package instead of the development entry point:

```sh
PACIFICDB_TEST_DESKTOP="$PWD/dist/desktop/linux-unpacked/pacificdb-workbench" npm run test:workbench:desktop
```

For an upgrade check, provide the previous installed executable separately and
use a caller-owned temporary data directory:

```sh
PACIFICDB_TEST_DESKTOP_PREVIOUS=/path/to/previous/pacificdb-workbench \
PACIFICDB_TEST_DESKTOP=/path/to/candidate/pacificdb-workbench \
PACIFICDB_TEST_DESKTOP_DATA=/absolute/temporary/workbench-data \
npm run test:workbench:desktop
```

The previous executable creates the fixture and the candidate verifies it,
restarts, and exercises the bundled CLI's backup verification and restore
history. Restore writes a separately validated restore directory; it does not
replace the live desktop database. A caller-owned test directory is never
removed by the test, so installer-uninstall checks can assert that data remains.
