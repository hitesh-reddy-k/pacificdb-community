# Install PacificDB v1.2

Version **1.2.0**. Download the package for your platform from
[GitHub Releases](https://github.com/hitesh-reddy-k/pacificdb-community/releases).
Verify its checksum and back up your data before an upgrade.

Windows/macOS installers are unsigned; macOS packages are not notarized. Follow
your organization's installation policy. Windows can show an unknown-publisher
warning. On macOS, approve the individual application in System Settings → Privacy
& Security after attempting to open it. Do not disable OS protections globally.
Checksums verify download integrity against the trusted release page; they are
not a publisher's code-signing certificate. Use `Get-FileHash -Algorithm SHA256`
on Windows or `shasum -a 256` on macOS.

## Native Linux x64

The release contains native engine/CLI and Workbench packages for Ubuntu24.04
x64, Windows x64 and macOS15 Intel/Apple Silicon, plus SDK distributions. Use the
public container for other Linux hosts with a compatible container runtime;
other distribution-native packages are unverified. Do not treat Ubuntu24 packages
as Debian13 packages: their native dependency versions differ.

Node and Python registry commands use explicit version **1.2.0**. Java downloads
include the JAR and matching POM; Maven Central publication is separate and deferred.

The Ubuntu Debian-format package includes `db_engine` and native `pacificdb` plus notices.
Use the Debian-format package built for Ubuntu24.04 x64. Ubuntu24 x64 native/desktop
packages have CI install/runtime evidence. Windows x64 and macOS15 Intel/Apple
Silicon desktop installers have separate fresh/upgrade runtime evidence.
Native shared dependencies include OpenSSL3, LZ4, RE2 and the platform C++ runtime.
For `.deb`, inspect its declared dependencies before installation:

```sh
sha256sum -c SHA256SUMS
dpkg-deb --info pacificdb-community-1.2.0-linux-amd64.deb
sudo apt install ./pacificdb-community-1.2.0-linux-amd64.deb
pacificdb --version
db_engine --build-info
```

For testing without modifying system packages, extract into a private directory:

```sh
mkdir -p "$PWD/pacificdb-rc-extracted"
dpkg-deb -x pacificdb-community-1.2.0-linux-amd64.deb "$PWD/pacificdb-rc-extracted"
export PATH="$PWD/pacificdb-rc-extracted/usr/bin:$PATH"
export PACIFICDB_HOME="$PWD/pacificdb-company-data"
pacificdb --version
pacificdb ping
```

Extraction verifies payload behavior but does not qualify installer scripts or
system-wide package management. The native CLI manages a local loopback engine;
`PACIFICDB_HOME` must be absolute. `pacificdb stop` stops that owned engine.
Use `pacificdb --help` and the [company guide](COMPANY_TESTING_GUIDE.md).

## Workbench

The Ubuntu24.04 desktop `.deb` includes Electron, the SDK, browser assets and the
native engine/CLI. Its declared dependencies include OpenSSL3, LZ4, RE2 and the
C++/desktop runtimes. Use the actual installer so its application-specific
AppArmor profile and launcher are registered; extracting the payload alone does
not perform those installation steps. Do not run the GUI as root or disable its
sandbox for normal use.

```sh
sha256sum -c SHA256SUMS
sudo apt install ./PacificDB-Workbench-1.2.0-linux-amd64.deb
pacificdb-workbench
```

Use an absolute optional `PACIFICDB_WORKBENCH_DATA` to isolate desktop data.
Desktop owns its engine process and uses a different default data directory from
the standalone CLI.

To evaluate native vector preview in desktop, explicitly set
`PACIFICDB_WORKBENCH_VECTOR_PREVIEW=1` before starting it. This maps only to the
owned local engine and stays disabled by default. Browser Workbench uses the
selected engine's operator configuration; `pacificdb workbench` is provided by
the npm CLI, not the native executable.

Windows/macOS installers are included only when a platform build and actual
installation/runtime test supply Passed evidence. Installer names or configured
build targets alone do not qualify them. Follow platform-specific artifact
instructions/checklist; do not use a Linux binary on another OS.

## Use the terminal command bundled with Workbench

Windows installers register their bundled native CLI in your user PATH. Close
all existing terminal windows (including Windows Terminal) and open a fresh CMD:

```bat
where pacificdb
pacificdb --version
```

On macOS and Linux, open Workbench and choose **File → Install pacificdb terminal
command…**, then open a new terminal:

```sh
command -v pacificdb
pacificdb --version
```

This creates `~/.local/bin/pacificdb` pointing to Workbench's bundled CLI and adds
that directory to your bash/zsh startup files. Existing commands at that path and
symlinked profile files are preserved; setup reports a conflict instead of
replacing them. For another shell, add Workbench's bundled engine directory to
its PATH using your shell's configuration. The setup action is also available on
Windows to repair PATH registration. Version reporting works offline.

The standalone native CLI has its own default database directory. To operate on
the currently open Workbench database, use **File → Copy CLI connection command**.
If you later remove Workbench on macOS/Linux, remove its owned `~/.local/bin/pacificdb`
link; the profile PATH entry is harmless and can be removed as well. Older installers do not contain this terminal setup action.

## npm, Python and Java

The packages are transport clients and need a running engine, or an installed
native engine for CLI-managed startup. Their installation examples are in
[SDK_GUIDE.md](SDK_GUIDE.md). npm packages use `1.2.0`, Python uses equivalent
`1.2.0`, Java uses `1.2.0`. Installing one does not install the engine.
Pin the package version when reproducing a deployment. npm/PyPI/Central
availability is separate from GitHub downloads. The GitHub SDK archives provide
an alternative when registry access is unavailable.

## Explicit engine startup and vector configuration

```sh
export PACIFICDB_HOME="$PWD/pacificdb-company-data"
export PACIFICDB_VECTOR_PREVIEW_ENABLED=1
pacificdb ping
```

Vector settings must be supplied before a managed engine starts. If an owned
local engine is already running, stop it first and restart with the setting.
Advanced operators may run `db_engine` directly with absolute `DATA_ROOT`,
`BACKUP_ROOT`, `RESTORE_DIR`, loopback engine/Raft binds, node/cluster identity
and appropriate authentication/TLS settings; see `engine/.env.example` and the
development guide. Local development settings are not a production-security profile.
Do not expose an auth-disabled listener to other hosts.

## Source build

Prerequisites: CMake>=3.20, C++17 toolchain, OpenSSL/LZ4/RE2 development libraries,
Node/npm for CLI/Workbench/tests, Python>=3.10 and Java>=11/Maven for SDKs.
Individual minimum runtime/platform claims require test evidence.

```sh
cmake -S engine -B build-v1.2 -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.2.0
cmake --build build-v1.2 -j2
ctest --test-dir build-v1.2 --output-on-failure
./build-v1.2/db_engine --build-info
./build-v1.2/pacificdb --version
npm ci --ignore-scripts --no-audit --no-fund
```

Run these from the exact released source including `release.json`, rather than
copying only `engine/`. Use a fresh build directory. OS packages provide native
dependencies; no developer `/home/...` toolchain path is required by distributed
source. Desktop builds need Electron's supported host dependencies and network
access to build tooling; follow `scripts/prepare-workbench-desktop.mjs` using the
fresh binary path. Package output is not verified until its own install smoke passes.

## Containers and troubleshooting

Use `ghcr.io/hitesh-reddy-k/pacificdb-community:1.2.0` for the release container.
For a reproducible deployment, pin the image digest recorded in that release's
`RELEASE-MANIFEST.json`, rather than copying a digest from an older release.
The source build definition and volume/authentication examples are in
`deploy/docker/`. The image runs as nonroot UID10001. Persist the documented data
volumes and keep local trial ports on loopback; configure authentication and TLS
before remote access. An image does not configure a secure deployment by itself.

If startup fails, collect `db_engine --build-info`, OS/runtime versions and
sanitized engine log. Check absolute writable roots, disk space, listener ports,
owned-process/root identity and missing shared libraries (`ldd` on Linux).
Do not delete root locks or repair production files to bypass refusal. ANN ABSENT
or STALE is not lost vector data: inspect index status and the documented fallback
policy. Rebuild explicitly after restart; do not expect persisted ANN graphs.
