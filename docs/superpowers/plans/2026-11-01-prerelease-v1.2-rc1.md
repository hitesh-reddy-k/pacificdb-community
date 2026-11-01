# Pre-release v1.2 release-engineering implementation plan

> For agentic workers: execute this user-supplied specification continuously with
> bounded independent ownership. Use code-work, executing-plans, TDD and fresh
> verification. The user explicitly authorized ordinary edits/tests/commits and
> branch/tag preparation; public publication approval is the final step.

**Goal:** Deliver a coherent externally testable Pre-release v1.2, base1.2.0,
branchv1.2, tagv1.2.0-rc.1, with independently verified staged artifacts and truthful
platform/publication status.

**Architecture:** Preserve existing C++17 WAL/LSM/Raft engine, native CLI, Node
browser/Electron Workbench and TCP/TLS Node/Python/Java SDKs. Use release.json for
base/candidate/ecosystem identity. Add an explicit prerelease pipeline and evidence
profile; leave stable qualification strict and unchanged. Stage and test actual
package bytes before any publication.

**Tech stack:** CMake/CTest/CPack; npm/Electron/Playwright; Python wheel/sdist;
Maven JAR/POM/source/Javadoc; Docker; existing GitHub Actions matrices.

**Specification:** User's Master Prompt: Pre-release v1.2, phases1–14. The user
requires autonomous execution; no repeated design/ordinary-action approvals.

## Global constraints

- Exact target: version1.2.0, titlePre-release v1.2, branchv1.2,
  tagv1.2.0-rc.1, GitHub prerelease. npm/Java1.2.0-rc.1, Python1.2.0rc1.
- Preserve all current dirty source, commits, historical frozen benchmarks and data.
  Work on requested existing branch; baseline snapshot is retained under ignored
  build-v1.2-release/evidence/baseline. Do not reset unrelated state or force-push.
- No public release, registry upload, stable/latest promotion, deployment or
  credential disclosure before concrete destination/artifact approval as required.
- Keep ANN/hybrid opt-in and known performance/availability/distributed limits.
  Do not rebrand old1.2.0-dev benchmark artifacts as RC measurements.
- Test failpoints only on disposable roots; never relax WAL/LSM/RF3/auth guarantees.
- Do not claim Windows/macOS/ARM64 or installed distributions pass from source alone.
- Stable release policy remains intact. RC evidence explicitly records untested
  physical-power/external-security/sustained qualification; no invented PASS.
- Commit only reviewed source/documentation/portable evidence. Keep generated data,
  credentials, private scan artifacts, binaries and large local traces out of Git.

## Review focus

- RC tags must never publish stable/latest releases, images or npm versions.
- Every packaged Workbench/native/SDK byte must match its declared source and
  version mapping; Python PEP440 must not be compared literally to SemVer.
- Staged-artifact imports/installations must not accidentally use source modules.
- Tenant lock expiry/reset/legacy state, limited-role access and credential logging
  must preserve legitimate behavior and survive restart without leaking secrets.
- Company installation must work without local developer paths or secret fixtures;
  absent platforms/credentials must remain Blocked/Not tested.

## Task1: Repository and security audit

Owner root, independent read-only architecture/SDK/Workbench/security workers.
Files: docs/releases/v1.2.0-rc.1/PROJECT_ANALYSIS.md; private managed security scan.

- [x] Inventory architecture, metadata, dirty source, runtime/tooling and workflows.
- [x] Verify remote main/branch/tag state without overwriting refs: remote main
  matches8732d299..., remotev1.2 andv1.2.0-rc.1 absent at inspection.
- [x] Audit existing Workbench/SDK packaging; record stale native artifacts and
  stable-only RC guards. Preserve exact independent reports.
- [x] Finish bounded static security scan; retain two validated medium findings
  and explicit incomplete coverage. It is not independent production certification.
- [ ] Write public-safe architecture/gap report from verified source/history.

## Task2: Canonical identity and prerelease metadata

Root owns release.json, scripts/release_identity.py + tests, engine/CMakeLists.txt,
engine/include/build_identity.hpp.in, engine/src/main.cpp, native shell reporting,
release-consistency tests, Docker/CPack metadata, unified release workflows.
SDK owner owns root/CLI/Node manifests + lockfile, Python/Java versions and SDK
package version guards. Workbench owner consumes identity without editing them.

Interfaces: release.json flat fields schemaVersion,version,releaseVersion,name,
channel,branch,tag,npmVersion,pythonVersion,javaVersion. Root helper
load_release_identity(root: Path|str|None=None)->dict validates these fields;
CLI `python3 scripts/release_identity.py --field FIELD` prints one value.
Engine engineVersion remains base1.2.0 with explicit releaseVersion/channel/name/tag
in build-info. Existing machine fields retain compatibility; human version output
identifies the prerelease. CPack platform package versions/names map correctly.

- [ ] Add failing tests for candidate/base/PEP440 mismatch, RC/stable policy,
  invalid tags and inconsistent component metadata.
- [ ] Implement identity reader/checker and compatible native release metadata.
- [ ] Align SDK/Workbench/CLI/package/deployment metadata through explicit mapping.
- [ ] Validate focused tests, fresh native build-info and version output.

## Task3: Confirmed security/release-quality fixes

Root owns engine/src/tenant_manager.cpp, engine/include/tenant_manager.hpp,
server credential sanitizer and targeted tests; narrow tests/CMake registration.

- [ ] Complete independent boundary investigation before patching.
- [ ] Reproduce persistent tenant lockout; implement persisted expiry with backward
  compatibility and authorized password-reset clearing; verify lock/reload/reset.
- [ ] Reproduce lower-role database-access deadlock; use lookup under existing lock,
  preserving role/database scope; verify success, denial and no hang.
- [ ] Reproduce owner/new password logging through real logging boundary; fix shared
  sanitizer without suppressing required diagnostics; verify accepted/rejected and
  alternate credential inputs plus existing config redaction.
- [ ] One independent bypass/regression review, focused checks then full regressions.

## Task4: Workbench + CLI release

Workbench owner: cli/src/{workbench,cli,shell}.js as needed, cli/workbench assets,
desktop/, scripts/prepare-workbench-desktop.mjs, Workbench/browser/desktop tests,
.github/workflows/workbench-desktop.yml. No SDK manifest/lock edits.

- [ ] Add failing version/stale-engine/preview workflow contracts.
- [ ] Display base1.2.0 and Pre-release v1.2; align packaged candidate identity.
- [ ] Expose documented major vector lifecycle/search controls using supported SDK
  methods where safely practical; otherwise clearly document SDK-only operations.
- [ ] Preserve broker Host/Origin/token controls and Electron isolation/lifecycle.
- [ ] Run real browser and desktop/native communication on fresh native binaries.
- [ ] Package Linux Workbench; perform staged/extracted installation/execution smoke.
- [ ] Enable nonpublishing CI for Windows/macOS; do not claim unsupported platforms.

## Task5: Installed SDK artifacts and compatibility

SDK owner: sdk/, package manifests/lockfile, scripts/check-sdk-release-tag.py,
verify-sdk-packages.py, pack-java-sdk.py, installed SDK tests/examples,
.github/workflows/{sdk-packages,npm-publish,python-publish,java-publish}.yml,
SDK_GUIDE.md. Root coordinates helper/schema and aggregate workflows.

- [ ] Add failing RC/PEP440/exact-byte staged artifact tests; preserve stable cases.
- [ ] Build npm client/CLI tarballs, Python wheel/sdist, Java JAR/POM/sources/Javadoc.
- [ ] Install those exact bytes into clean consumers and run real CRUD/vector/media,
  authentication/error/TCP/TLS and relevant recovery tests; identify source imports.
- [ ] Correct RC npm next tag and version guards; registry publishing remains gated.
- [ ] Provide tested equivalent language examples and Maven-compatible local/release
  repository distribution without claiming Central publication before it occurs.

## Task6: Unified build/evidence/distribution workflow

Root: .github/workflows/release.yml, ci.yml, new prerelease orchestration,
scripts/prerelease_qualification.py + focused tests, distribution/staging helpers,
root documentation and platform matrix. Workbench/SDK workflows stay owned above.

- [ ] Fail tests on automatic stable/latest promotion, mismatched candidate bytes,
  unresolved release-blocking findings or missing mandatory staged checks.
- [ ] Implement prerelease staging/publishing separately from strict stable policy;
  require manual publish intent/environment and checksum/source verification.
- [ ] Build/test native Linux distributions and Docker nonroot image locally.
- [ ] Push reviewed release source to remotev1.2 without force; use available CI to
  validate configured Windows/macOS/SDK/runtime matrices, retain exact artifacts.
- [ ] Add only verified platform assets; mark remaining platforms Blocked/Not tested.
- [ ] Generate checksums and practical dependency/license inventory/SBOM.

## Task7: Documentation, final validation and publication handoff

Root owns CHANGELOG.md, RELEASE_NOTES_v1.2.md, INSTALLATION.md, ARCHITECTURE.md,
TESTING.md, COMPANY_TESTING_GUIDE.md, RELEASE_CHECKLIST.md, SDK_GUIDE.md integration.

- [ ] Derive Added/Changed/Improved/Fixed/Deprecated/Known Issues/Breaking Changes
  from current source diff and real previous-release history; no fictional changes.
- [ ] Execute every company quickstart/example on staged distributions; validate
  documentation links, cleanup and runtime floors.
- [ ] Complete fresh engine/vector/failpoint/document/media/auth/RF3/SDK/Workbench
  regression evidence for exact candidate; retain failures and blocked checks.
- [ ] Independently review combined diff, secret/artifact contents, versions/hashes,
  license notices, platform claims and release identity; correct confirmed issues.
- [ ] Commit reviewed work, build source-bound artifacts, prepare annotated local
  v1.2.0-rc.1 tag only after validation and check remote conflicts again.
- [ ] Present exact intended GitHub/GHCR/npm/PyPI/Maven destinations, identifiers,
  assets and visibility for final required publication approval.
- [ ] Publish only approved destinations; verify remote branch/tag/release prerelease
  flags, assets, checksums and installations independently. Report other channels
  Blocked with exact missing credentials/actions, never fabricated URLs/success.
- [ ] Deliver structured final report A–G requested by user, with Passed/Failed/
  Blocked/Not applicable/Not tested per item and company testing instructions.

## Initial rulings

- Use existingv1.2 checkout as explicitly requested; preserve its entire uncommitted
  checkpoint through snapshot and focused commits, not a reset/new implementation.
- Current published1.1.2/workbench artifacts differ from latest main. Previous tags
  are reference evidence, not renamed or overwritten.
- All historical benchmark artifacts remain1.2.0-dev; new RC packaging requires
  fresh artifact tests but no unrequested multi-hour benchmark rerun.
- Security review is bounded and partial. Two source-validated medium findings and
  one tenant deadlock require fixes; full external qualification is not claimed.
