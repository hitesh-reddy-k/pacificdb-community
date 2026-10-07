# Workbench 1.1.2 production-readiness candidate — 6 October 2026

**Verdict: candidate only. Public production release is blocked until every
hosted, external, and signing row below has evidence for the exact release
revision and installer digests.** No release was published by this work.

Workbench, engine, CLI and SDK source versions now align at 1.1.2 for the
Community security patch. Earlier local installer results below describe
their original candidate, not newly qualified 1.1.2 engine artifacts. See the
[patch notes and current readiness statement](releases/1.1.2.md).

## Locally verified on this branch

| Check | Result | Boundary |
| --- | --- | --- |
| npm SDK and CLI suites | PASS | 58 tests |
| Desktop persistence, sandbox, bundled CLI backup verification, and restore history | PASS | Local Linux Electron run with caller-owned temporary data |
| Exact-revision qualification validators | PASS | 47 release, power, security, and Workbench evidence tests |
| Installer workflow contracts | PASS | Debian, NSIS, and both DMG architectures are required; unpacked directories are rejected as release evidence |
| Runtime dependency audit | PASS | `npm audit --omit=dev` reports zero advisories |
| Linux candidate packaging | PASS | Local 1.1.2 Debian and tar.gz build; Debian metadata and installed executable path inspected |

These results prove repository-owned behavior on this Linux host. They are not
substitutes for hosted native installation or external certification.

## Required release evidence

| Gate | Current state | PASS requirement |
| --- | --- | --- |
| Linux x64 Debian install/upgrade/uninstall | PENDING HOSTED RUN | Install public 1.1.1, retain caller-owned data, install 1.1.2, pass desktop/backup/restore checks, uninstall, and retain the populated database directory |
| Windows x64 NSIS install/upgrade/uninstall | PENDING HOSTED RUN | Same sequence on `windows-2025`, using the actual NSIS installer and uninstaller |
| macOS arm64 and x64 DMG install/upgrade/remove | PENDING HOSTED RUN | Mount each DMG, copy the app, run the upgrade check, remove the app, and retain data |
| Physical power-loss recovery | BLOCKED | Valid dedicated-host evidence for the exact candidate revision |
| Independent security review | BLOCKED | Independent review bundle for the exact revision covering every candidate installer digest, with no unresolved critical/high finding |
| Long-duration load | BLOCKED | At least 8 hours, 500,000 records, positive operation and peak-RSS measurements, and zero errors |
| Windows signing and macOS signing/notarization | BLOCKED / OUT OF IMPLEMENTATION SCOPE | Release-owner credentials and successful existing workflow checks; this work deliberately did not implement or weaken signing |
| Aggregated release qualification | BLOCKED | Clean tree, all four platform results, zero runtime advisories, and every external gate above returns `PASS` |

The release workflow uploads installer candidates and the qualification report
even when external evidence is absent, but it does not run `gh release create`
unless aggregation returns `PASS`. A blocked report is expected and must never
be relabeled as production approval.

## Operational boundary

Workbench is a single-user, offline-first local desktop application. Its owned
engine and private HTTP bridge bind to loopback. Public network exposure,
multi-user service operation, accounts, cloud sync, telemetry, and automatic
updates are unsupported. The verified 500,000-record workload is a tested
point, not an unlimited-capacity claim.

Installation, backup, rollback, data locations, logs, and failed-launch
recovery are documented in the [Workbench operations guide](WORKBENCH.md).
Evidence formats and fail-closed status meanings are in
[Release evidence](RELEASE_EVIDENCE.md).
