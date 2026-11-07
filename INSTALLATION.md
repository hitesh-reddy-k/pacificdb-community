# Install Pre-release v1.2

Candidate: `v1.2.0-rc.2`, base `1.2.0`. This is an external-testing release.
Check [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) before downloading: Not tested
or Blocked is not verified support, and an intended URL is not proof of publication.
Use an isolated data directory and copies of test data. Keep production unchanged.

## Upcoming 1.2.0 packages

This source tree prepares **1.2.0**. The published installation commands below
continue to select RC.2 until a new release is available; they do not install the
new source changes. Do not substitute an unpublished version into registry commands.

The owner has chosen unsigned Windows/macOS installers for 1.2.0. Verify the
release's `SHA256SUMS` before installation (`Get-FileHash -Algorithm SHA256` on
Windows, `shasum -a 256` on macOS). Windows can show an unknown-publisher warning;
macOS installers are not notarized and may require approval through System
Settings → Privacy & Security after an attempted open. Follow your organization's
software policy; do not disable OS protections globally. Checksums detect altered
downloads when compared with a trusted release page; they are not a publisher
signature. Java JAR/POM downloads do not require an owner GPG key; Maven Central
publication is deferred.

## Native Linux x64 staging

The [public RC.2 prerelease](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/v1.2.0-rc.2)
contains the native engine/CLI, Linux/Windows/macOS desktop installers and SDK
packages. RC.1 assets remain unchanged. **RC.2 Linux native packages target
Ubuntu24.04 x64.** Do not install them as Debian13 packages: their RE2 runtime
requirements differ. Use the public container for other Linux hosts with a
compatible container runtime; other distribution-native packages are unverified.
Python is published on PyPI: `python -m pip install pacificdb==1.2.0rc2`.
Maven Central still requires [owner setup](PUBLISHING_OWNER_GUIDE.md). npm client/CLI and
the independent fresh public-install matrix have Passed evidence in the release
checklist. The initial npm registry propagation failure is preserved.

The Ubuntu Debian-format package includes `db_engine` and native `pacificdb` plus notices.
A Linux archive contains the same native commands under `bin/`. Use the artifact
built for your actual distribution/runtime. RC.2's Ubuntu24 x64 native/desktop
packages have CI install/runtime evidence. Windows x64 and macOS15 Intel/Apple
Silicon desktop installers have separate fresh/upgrade runtime evidence.
Native shared dependencies include OpenSSL3, LZ4, RE2 and the platform C++ runtime.
For `.deb`, inspect its declared dependencies before installation:

```sh
sha256sum -c SHA256SUMS
dpkg-deb --info pacificdb-community-1.2.0-rc.2-linux-x64.deb
sudo apt install ./pacificdb-community-1.2.0-rc.2-linux-x64.deb
pacificdb --version
db_engine --build-info
```

For testing without modifying system packages, extract into a private directory:

```sh
mkdir -p "$PWD/pacificdb-rc-extracted"
dpkg-deb -x pacificdb-community-1.2.0-rc.2-linux-x64.deb "$PWD/pacificdb-rc-extracted"
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

The Linux desktop archive includes Electron, the SDK, browser assets and the
native engine/CLI. It still needs matching native shared libraries: OpenSSL3, LZ4
and RE2 plus the C++ runtime. The Debian package declares the host ABI (Debian13
RE2/libstdc++14, or Ubuntu24 RE2/libstdc++13 for its separately built package).
A Debian13 binary is not an Ubuntu24 binary; inspect the manifest and dependencies. Extract it and run `pacificdb-workbench`; a `.deb` creates the
same launcher. Graphical runtime dependencies and Chromium sandbox behavior must
be available. Do not run the GUI as root or disable its sandbox for normal use.

```sh
sha256sum -c SHA256SUMS
tar -xzf PacificDB-Workbench-1.2.0-rc.2-linux-x64.tar.gz
./PacificDB-Workbench-1.2.0-rc.2-linux-x64/pacificdb-workbench
```

Actual archive root names are recorded in the distribution manifest. If using
an extracted `.deb`, the executable is under its `opt/PacificDB Workbench/`
payload; inspect with `dpkg-deb -c`. Use an absolute optional
`PACIFICDB_WORKBENCH_DATA` to isolate desktop data. Desktop owns its engine
process and uses a different default data directory from the CLI.

To evaluate native vector preview in desktop, explicitly set
`PACIFICDB_WORKBENCH_VECTOR_PREVIEW=1` before starting it. This maps only to the
owned local engine and stays disabled by default. Browser Workbench uses the
selected engine's operator configuration; `pacificdb workbench` is provided by
the npm CLI, not the native executable.

Windows/macOS installers are included only when a platform build and actual
installation/runtime test supply Passed evidence. Installer names or configured
build targets alone do not qualify them. Follow platform-specific artifact
instructions/checklist; do not use a Linux binary on another OS.

## npm, Python and Java

The packages are transport clients and need a running engine, or an installed
native engine for CLI-managed startup. Their staged installation examples are in
[SDK_GUIDE.md](SDK_GUIDE.md). npm packages use `1.2.0-rc.2`, Python uses equivalent
`1.2.0rc2`, Java uses `1.2.0-rc.2`. Installing one does not install the engine.
Do not use `npm install ...@latest` to obtain this prerelease. npm/PyPI/Central
availability must be independently verified; GitHub assets provide an alternative
staged distribution when registry publication is Blocked.

## Explicit engine startup and preview

```sh
export PACIFICDB_HOME="$PWD/pacificdb-company-data"
export PACIFICDB_VECTOR_PREVIEW_ENABLED=1
pacificdb ping
```

Preview settings must be supplied before a managed engine starts. If an owned
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
cmake -S engine -B build-rc -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.2.0
cmake --build build-rc -j2
ctest --test-dir build-rc --output-on-failure
./build-rc/db_engine --build-info
./build-rc/pacificdb --version
npm ci --ignore-scripts --no-audit --no-fund
```

Run these from the exact released source including `release.json`, rather than
copying only `engine/`. Use a fresh build directory. OS packages provide native
dependencies; no developer `/home/...` toolchain path is required by distributed
source. Desktop builds need Electron's supported host dependencies and network
access to build tooling; follow `scripts/prepare-workbench-desktop.mjs` using the
fresh binary path. Package output is not verified until its own install smoke passes.

## Containers and troubleshooting

The public RC.2 container is
`ghcr.io/hitesh-reddy-k/pacificdb-community:1.2.0-rc.2`, with verified digest
`sha256:bda3cc787d33ff93d34f5545d9c4bd77dcc3f25d051fd8eda4c954733438ad26`.
The publishing workflow pulled and smoke-tested that digest; anonymous manifest
access was independently checked. It is a Linux x64 prerelease image. Use the
existing isolated-volume/authentication configuration in `deploy/docker/`; a
public image does not qualify an exposed or production deployment.

`deploy/docker/Dockerfile` is a source build definition. The exact candidate image
and smoke evidence must be Passed before calling it verified. Local builds do
not publish an image. Preserve nonroot UID10001, data volumes and loopback host
port bindings for development. Production deployment has additional existing
mTLS/RF3/auth/audit/encrypted-storage requirements and is outside this preview claim.

If startup fails, collect `db_engine --build-info`, OS/runtime versions and
sanitized engine log. Check absolute writable roots, disk space, listener ports,
owned-process/root identity and missing shared libraries (`ldd` on Linux).
Do not delete root locks or repair production files to bypass refusal. ANN ABSENT
or STALE is not lost vector data: inspect index status and the documented fallback
policy. Rebuild explicitly after restart; do not expect persisted ANN graphs.
