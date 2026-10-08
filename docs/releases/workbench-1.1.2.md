# PacificDB Workbench v1.1.2 prerelease

Workbench 1.1.2 is published as part of the [Community prerelease](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/1.1.2).
The Linux Debian installer and portable archive are available with verified
SHA-256 checksums. Final Windows/macOS installers are not published; this is
not production certification.

## Version boundary

Workbench now shares the Community 1.1.2 patch version with its bundled engine,
native/npm CLI and SDKs. These are the security-remediated source components,
not unchanged public 1.1.1 binaries. See the [complete Community patch notes](1.1.2.md).

## Candidate changes

- Release CI installs and exercises the real Debian, NSIS, and DMG artifacts,
  including a 1.1.1-to-1.1.2 caller-owned-data upgrade and uninstall check.
- The desktop verification exercises bundled-CLI backup creation, verification,
  restore history, restart persistence, and sandbox boundaries.
- Release qualification binds platform results to a clean Git revision,
  Workbench version, installer paths, and SHA-256 digests. Stable promotion remains
  blocked when platform, runtime audit, physical-power, independent-security,
  or long-duration-load evidence is missing or invalid.
- Runtime dependencies are audited independently from development tooling.

## Supported package targets

- Ubuntu 24.04/Debian-compatible Linux x64: Debian installer
- Windows x64: NSIS installer
- macOS arm64 and macOS x64: DMG images

The published Linux portable tarball passes scoped sandbox/runtime checks but
is not an independently qualified native installer. Other operating systems and architectures are unsupported.

## Upgrade and security boundary

Updates are manual; 1.1.2 has no auto-updater. Keep the 1.1.1 installer, make
and verify an external backup, quit Workbench, and retain an offline copy of
the entire application-data directory before installation. Uninstalling the app
must preserve that directory. See the [operations guide](../WORKBENCH.md) for
platform paths, backup commands, rollback, logs, and failed-launch recovery.

Workbench is an offline-first, single-user local application. Its bundled
engine and HTTP bridge bind to loopback. It does not add accounts, telemetry,
cloud sync, team operation, or supported public-network exposure.

Signing and notarization were deliberately outside this implementation scope;
the existing release checks remain fail-closed. Physical-power recovery,
independent security review covering candidate installer digests, an eight-hour
500,000-record load run, native hosted package runs, and signing evidence must
all pass before this candidate can be called production-ready.
