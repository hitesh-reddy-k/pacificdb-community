# Pre-release v1.2 release checklist

Base `1.2.0`; candidate/tag `v1.2.0-rc.1`; branch `v1.2`; title **Pre-release v1.2**.
Statuses: **Passed / Failed / Blocked / Not applicable / Not tested**.
Final artifact manifest/receipts bind immutable bytes and revision; this source
checklist records executed preparation scopes. Stable production readiness remains
INCOMPLETE. Original failed attempts are retained and are not relabeled PASS.

| Activity | Status | Evidence / scope |
| --- | --- | --- |
| Preserve original checkpoint and unrelated work | Passed | Baseline archive/diff retained; existing v1.2 implementation preserved |
| Canonical engine/Workbench/CLI/SDK identity | Passed | release.json, SemVer/PEP440 mapping, source and package checks |
| Tenant authentication/access/logging corrections | Passed | Lockout/reset/reload, access deadlock, secret redaction regressions; earlier targeted sanitizer runs remain revision-scoped |
| Native core and vector CTest | Passed | Integrated source: 50 CTest cases including new commit/syscall/heartbeat failure regressions |
| Complete Community runner | Passed | community-complete.log: CRUD, indexes/aggregation, media/authentication, automatic startup, crash/checkpoint recovery, ENOSPC, RF3/partition/restart, sustained concurrency, SDK units/live TLS/recovery |
| Full real disk-full matrix | Passed | All 21 operation categories; rejected baseline state retained after restart |
| Synchronous log and checkpoint publication failure handling | Passed | Early failure rollback, ordinary/group sync errors, uncertain-directory publication fencing, heartbeat rejection; RED/GREEN tests |
| Installed SDK packages | Passed | Eight exact npm/wheel/sdist/JAR/POM artifacts; clean installed consumers; authenticated TCP/TLS/SIGKILL recovery; final rebuilt artifacts require their own receipts |
| SDK runtime CI | Passed | 16 jobs on source-equivalent SDK revision f1d48ad: Node18/22/24, Python3.10/3.12 and Java11/17/21 on Linux/Windows; repeat final tag CI before publishing |
| Linux Workbench runtime | Passed | Normal Chromium sandbox, document/media, restart and bundled CLI backup/restore; final source-bound packaging repeats these checks |
| Native Debian installation and container runtime | Passed | Earlier clean-source Debian13 installation and exported/reloaded nonroot read-only container checks; final artifact receipts required |
| Full/runtime npm advisory gate | Passed | Zero findings; patched minimatch11.0.3 plus five exact-source guarded build-tool adaptations; no new waiver or suppressed audit |
| Matcher compatibility | Passed | Eight package/guard tests, all 30 upstream glob7 test files, actual Linux deb/archive packaging and GUI |
| Windows/macOS native installers | Not tested | Final platform CI build/install/runtime evidence required; source/runtime matrix alone is insufficient |
| Mixed-version/platform upgrade matrix | Not tested | Complete Community runner explicitly skips without exact previous artifact |
| Physical power-loss validation | Blocked | Requires external hardware evidence; process crash/failpoint tests are separate |
| Independent external security certification | Blocked | Bounded source review/fixes are not external certification |
| New storage patch sanitizer qualification | Not tested | Earlier sanitizer evidence does not cover the complete new storage patch |
| RC source commit/remote references | Passed | Earlier source checkpoints pushed without force; final staged manifest records latest exact revision and clean source |
| Exact final package checksums / receipt qualification | Not tested | Run qualify-prerelease.py and verify-prerelease-staging.py --require-qualified on the final immutable staging directory |
| RC Git tag | Not tested | Create once after final source-bound qualification and remote conflict check; never move an existing tag |
| GitHub prerelease / public download checks | Not tested | Explicitly authorized by user; publish only qualified assets as prerelease, never latest; independently download/verify |
| npm client and CLI publication to next | Not tested | Repository NPM_TOKEN configured; exact tag workflow and registry install verification still required |
| PyPI publication | Blocked | Owner must configure matching trusted publisher, then publish/verify; see owner guide |
| Maven Central publication | Blocked | Owner namespace, signing key and Portal token setup required; validation-only workflow then owner Publish |
| GHCR RC publication | Not tested | Explicit manual RC workflow; requires public qualified prerelease; verify pushed digest and anonymous accessibility |
| Post-publication benchmarks | Not tested | Run only after publication from downloaded native/SDK artifacts; keep original dev evidence separate |
| Stable production release | Not applicable | This is an external-testing prerelease; vector/distributed/upgrade/power/security limits remain disclosed |

Read [storage fixes and original failures](docs/releases/v1.2.0-rc.1/STORAGE_FAILURE_FINDINGS.md),
[owner publishing steps](PUBLISHING_OWNER_GUIDE.md), [company guide](COMPANY_TESTING_GUIDE.md)
and [release notes](RELEASE_NOTES_v1.2.md). Missing/failed receipts block publication;
qualification cannot be obtained by changing a status string or dropping a test.

The old 1M [benchmark report](docs/v1.2-evidence/community-verified-cache-comparison-20261101/REPORT.md)
retains its original dev revision, limitations and failures. A new report must use
actual published RC bytes with equivalent PostgreSQL/pgvector settings.
