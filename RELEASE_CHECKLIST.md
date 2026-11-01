# Pre-release v1.2 release checklist

Base 1.2.0; candidate/tag v1.2.0-rc.1; branch v1.2; title Pre-release v1.2.
Statuses: **Passed / Failed / Blocked / Not applicable / Not tested**.
This live checklist is revised from executed evidence. Production readiness stays
INCOMPLETE; scoped preview staging never substitutes for strict stable qualification.

| Activity | Status | Evidence / remaining action |
| --- | --- | --- |
| Preserve current source/dirty changes before release edits | Passed | Ignored baseline source/archive/diff snapshot retained |
| Inspect local/remote branch/tag and previous releases | Passed | Remote main baseline 8732d29; candidate branch/tag absent initially; previous 1.1.2 identity recorded |
| Canonical release/ecosystem mapping tests | Passed | release.json; three initial identity tests and focused SDK mapping checks |
| Fresh native 1.2.0/RC binaries | Passed | Clean-source RC build at 4955ab5 inspected; engine and native CLI report base/candidate identity |
| Tenant lockout/access/logging fixes | Passed | RED/GREEN real storage/auth tests; bounded expiry/reset, access no hang, credentials redacted; independent static patch review |
| Current native/vector/document/media/RF3 checkpoint | Passed | 76 top-level checks / 45 CTest on clean-source 4955ab5 RC binary; original timestamp-test failure retained and fixed |
| SDK source units and initial staged packages | Passed | Current Node34/Python 134/Java30 units; isolated artifact tests and clean-source installed consumers passed |
| Initial exact staged SDK live TCP/TLS/recovery | Passed | All three installed SDKs, CLI, media, schemas/typed vectors/hybrid; clean-source 4955ab5 rerun passed TCP/TLS/SIGKILL recovery with exact staged bytes |
| Workbench/CLI source contracts | Passed | Agent source regression/lifecycle/preview checks; real browser, desktop and bundled-engine runtime smoke passed |
| Fresh browser/Electron/native integration | Passed | Real browser, isolated engine, explicit ANN opt-in and Electron sandbox-enforced GUI |
| Native Linux package extraction/runtime | Passed | Extracted payload CRUD/media/vector checks; actual Debian 13 container installation and nonroot managed-engine ping/stop passed |
| Initial Workbench Linux package/extracted runtime | Passed | Debian/tar payload and extracted GUI tested; Electron 44.10.1/source-bound 4955ab5 Debian payload passed sandbox-enforced GUI, persistence and backup/restore |
| Container build/nonroot/runtime | Not tested | Docker available; candidate build/smoke pending |
| Windows/macOS/ARM64 actual install/runtime | Not tested | Remote Windows/macOS/Linux Workbench jobs stop at the unchanged dependency gate; no installer/runtime qualification claimed |
| Runtime npm advisory check | Passed | npm audit --omit=dev: 0; Electron updated to 44.10.1 and compatible undici/fast-uri patches |
| Full build-tool dependency audit | Failed | 15 moderate build-only transitive advisories remain; no new waiver or unsafe major override applied |
| Static security audit | Passed | Bounded offline review completed with explicit partial coverage/two validated issues; narrow fixes exercised separately |
| Resolve confirmed engine findings | Passed | Focused regressions and independent patch review; targeted TSAN/ASan pass |
| Full independent security certification | Blocked | Requires external exact-artifact review; not established by local audit |
| Physical power-loss qualification | Blocked | External hardware evidence unavailable; process crash tests are separate |
| Full mixed-version/platform upgrade matrix | Not tested | Requires exact previous artifacts and executed scenarios |
| Source-bound final artifact checksums/SBOM/license inventory | Not tested | Stage and final review pending |
| Company examples/docs link checks | Passed | Three language examples and media roundtrips executed from staged SDKs; root document targets checked |
| Commit and remote v1.2 push | Passed | 296aa2a and 4955ab5 pushed and independently resolved on origin/v1.2; no force push or unrelated-ref deletion |
| Candidate tag source binding | Blocked | Tag remains uncreated until candidate qualification passes; do not tag a failed release gate |
| GitHub prerelease publication | Blocked | Concrete artifacts/destination/visibility approval required before publication |
| npm publication (@pacificdb/client, @pacificdb/cli; next) | Blocked | Owner credentials/permissions and exact-artifact publication approval required |
| PyPI publication (pacificdb 1.2.0rc1) | Blocked | Registered OIDC/environment and publication approval required |
| Maven Central publication | Blocked | Namespace/signing/token/environment and publication approval required; staged JAR/POM is alternative |
| GHCR candidate publication | Blocked | Exact-image verification and publication approval required; no stable/latest promotion |
| Independently verify public accessibility/installed examples | Not tested | Only after actual approved publication |
| Stable production release | Not applicable | This task is an external-testing prerelease; known ANN/performance limits remain |

The previous [benchmark/recovery report](docs/v1.2-evidence/community-verified-cache-comparison-20261101/REPORT.md)
remains revision-scoped. Its successful static checks do not erase measured ANN
availability/performance failures. Full raw local logs remain under ignored build
roots; portable final summaries/manifests will be attached to staged artifacts.

## Validation scope and publication blocker

The clean-source 4955ab5 staging attempt installs the exact eight SDK artifacts
against the RC engine; all three languages pass authenticated TCP, TLS and abrupt
restart recovery (160 documents and 700,000 media bytes each). The native Debian
package was also installed in a clean Debian 13 container. GUI validation uses
Electron 44.10.1 with the Chromium sandbox enabled. Final delivery manifests bind
any subsequent documentation/package rebuild to its own revision and checksums.

The full dependency gate remains **Failed**: npm reports 15 moderate entries
through minimatch in the desktop build dependency tree. The upstream
[advisory](https://github.com/isaacs/minimatch/security/advisories/GHSA-g3hv-75wg-m249)
identifies 11.0.1 as patched; the older callable dependency consumers prevent a
blind major override. No new exception, disabled check or forced downgrade was
used. Runtime npm audit has zero findings; that does not override the full gate.
Workbench CI runs 41290663590 and 41290952849 retain the failures. Public release,
tag and registry publication remain blocked rather than reported complete.
