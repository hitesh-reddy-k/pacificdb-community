# Pre-release v1.2 release checklist

Base 1.2.0; candidate/tagv 1.2.0-rc.1; branch v1.2; title Pre-release v 1.2.
Statuses: **Passed / Failed / Blocked / Not applicable / Not tested**.
This live checklist is revised from executed evidence. Production readiness stays
INCOMPLETE; scoped preview staging never substitutes for strict stable qualification.

| Activity | Status | Evidence / remaining action |
| --- | --- | --- |
| Preserve current source/dirty changes before release edits | Passed | Ignored baseline source/archive/diff snapshot retained |
| Inspect local/remote branch/tag and previous releases | Passed | Remote main baseline 8732d29; candidate branch/tag absent initially; previous 1.1.2 identity recorded |
| Canonical release/ecosystem mapping tests | Passed | release.json; three initial identity tests and focused SDK mapping checks |
| Fresh native 1.2.0/RC binaries | Not tested | Fresh build currently running; stale default build was rejected |
| Tenant lockout/access/logging fixes | Passed | RED/GREEN real storage/auth tests; bounded expiry/reset, access no hang, credentials redacted; independent static patch review |
| Current native/vector/document/media/RF3 checkpoint | Passed | 76 top-level checks / 45 CTest on initial RC build; original timestamp-test failure retained and fixed |
| SDK source units and initial staged packages | Passed | Current Node34/Python 134/Java30 units; initial isolated artifact tests; final live RC tests pending separately |
| Initial exact staged SDK live TCP/TLS/recovery | Passed | All three installed SDKs, CLI, media, schemas/typed vectors/hybrid; clean source-bound rerun pending |
| Workbench/CLI source contracts | Passed | Agent source regression/lifecycle/preview checks; runtime smoke pending |
| Fresh browser/Electron/native integration | Passed | Real browser, isolated engine, explicit ANN opt-in and Electron sandbox-enforced GUI |
| Native Linux package extraction/runtime | Not tested | Build/stage/install smoke pending |
| Initial Workbench Linux package/extracted runtime | Passed | Debian/tar payload and extracted GUI tested; patched Electron and final source-bound rebuild required |
| Container build/nonroot/runtime | Not tested | Docker available; candidate build/smoke pending |
| Windows/macOS/ARM64 actual install/runtime | Not tested | Configured CI targets require executed installer/runtime evidence |
| Runtime npm advisory check | Passed | npm audit --omit=dev: 0; Electron updated to 44.10.1 and compatible undici/fast-uri patches |
| Full build-tool dependency audit | Failed | 15 moderate build-only transitive advisories remain; no new waiver or unsafe major override applied |
| Static security audit | Passed | Bounded offline review completed with explicit partial coverage/two validated issues; fixes pending |
| Resolve confirmed engine findings | Passed | Focused regressions and independent patch review; targeted TSAN/ASan pass |
| Full independent security certification | Blocked | Requires external exact-artifact review; not established by local audit |
| Physical power-loss qualification | Blocked | External hardware evidence unavailable; process crash tests are separate |
| Full mixed-version/platform upgrade matrix | Not tested | Requires exact previous artifacts and executed scenarios |
| Source-bound final artifact checksums/SBOM/license inventory | Not tested | Stage and final review pending |
| Company examples/docs link checks | Not tested | Documents being finalized, actual staged examples pending |
| Commit and remote v1.2 push | Not tested | Authorized; preserve broken local capture refs and avoid force/overwrite |
| Candidate tag source binding | Not tested | Create only after validation and renewed remote conflict check |
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
