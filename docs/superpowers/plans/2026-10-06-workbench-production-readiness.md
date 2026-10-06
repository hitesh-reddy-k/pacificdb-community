# Workbench Production Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a fail-closed PacificDB Workbench 1.1.2 candidate pipeline with real-installer, upgrade, recovery, dependency, and exact-revision evidence checks.

**Architecture:** Keep the released database components at 1.1.1 and version the private desktop build independently as 1.1.2. Extend the existing desktop acceptance flow so every operating system exercises the same persisted-data contract, then aggregate small per-platform evidence records with existing physical-power and security validators before release publication.

**Tech Stack:** Node.js 24, Electron/Playwright, Python 3 standard library, CMake, GitHub Actions, electron-builder.

**Spec:** `docs/superpowers/specs/2026-10-06-workbench-production-readiness-design.md`

## Global Constraints

- Base commit is `b4c0ad3c7bec1a3d7594986f59cd25d94a6e31f6` on branch `codex/workbench-production-readiness`.
- The original `/mnt/pacificdb-backup/home-hitesh/Downloads/pacificdb-community` checkout must not be modified.
- Workbench version is 1.1.2; bundled engine, Node SDK, and CLI component versions remain 1.1.1.
- Do not configure or weaken signing, notarization, or certificate gates.
- Do not publish, push, tag, deploy, or mutate the existing 1.1.1 GitHub release.
- Do not mark physical-power, independent-security, or long-load evidence passing unless matching external JSON is supplied.
- Preserve the loopback-only desktop engine and HTTP bridge boundary.
- Use only existing dependencies and standard-library code.

## Review Focus

- A previous-version application exits during seeding: candidate verification must not start and evidence must fail.
- A candidate installer uninstalls successfully but removes its caller-owned database directory: the platform job must fail.
- External evidence names a different revision or omits an installer digest: qualification must be `BLOCKED` or `FAIL`, never `PASS`.
- A tag or workflow retry names a version other than the root Workbench manifest: publication must stop before artifact upload.
- A platform test passes against `*-unpacked` while the real installer is broken: only installed-artifact evidence may satisfy the release gate.

---

### Task 1: Independent Workbench Version and Dependency Gate

**Files:**
- Modify: `package.json`
- Modify: `package-lock.json`
- Modify: `scripts/prepare-workbench-desktop.mjs`
- Modify: `.github/workflows/workbench-desktop.yml`
- Modify: `scripts/test-workflow-contract.py`
- Create: `scripts/test-workbench-package-contract.mjs`

**Interfaces:**
- Produces: root `package.json.version` as the Workbench version; `cli/package.json.version` remains the bundled component version.
- Produces: staged `desktop/stage/package.json` with `version === root.version` and `dependencies['@pacificdb/client'] === cli.dependencies['@pacificdb/client']`.
- Produces: `WORKBENCH_VERSION` and `PACIFICDB_COMPONENT_VERSION` workflow environment values derived from manifests.

- [ ] **Step 1: Build the unchanged native packaging fixtures**

Run: `cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.1.1 && cmake --build build --target db_engine pacificdb -j2`

Expected: the current 1.1.1 engine and CLI binaries exist for the preparation contract without changing source behavior.

- [ ] **Step 2: Write the failing package-contract check**

Create `scripts/test-workbench-package-contract.mjs`. It reads the root, CLI, SDK, and generated stage manifests and asserts literal expectations: Workbench `1.1.2`, components `1.1.1`, exact CLI-to-SDK dependency, and stage output using the Workbench version while bundling the 1.1.1 client.

- [ ] **Step 3: Run the contract and verify RED**

Run: `PACIFICDB_WORKBENCH_ENGINE="$PWD/build/db_engine" node scripts/prepare-workbench-desktop.mjs && node scripts/test-workbench-package-contract.mjs`

Expected: FAIL because the root manifest has no 1.1.2 Workbench version and stage still inherits 1.1.1 from the CLI.

- [ ] **Step 4: Implement manifest-derived versions**

Set root `version` to `1.1.2`; update only root/CLI/SDK lockfile metadata needed by that decision. Change `prepare-workbench-desktop.mjs` to read both root and CLI manifests, use the root version for the staged app, and retain the exact CLI client dependency.

In the desktop workflow, derive both versions once and replace hard-coded CMake `1.1.1` arguments and release-tag comparison with the corresponding manifest value. Keep the 1.1.1 signing exception unchanged and scoped only to that historical tag.

- [ ] **Step 5: Update workflow behavior checks**

Change `scripts/test-workflow-contract.py` so a future hard-coded candidate version or tag mismatch fails, while the explicit historical 1.1.1 exception remains required.

- [ ] **Step 6: Refresh the permitted transitive dependency**

Run: `npm update http-cache-semantics --package-lock-only --ignore-scripts`

Expected: lockfile resolves `http-cache-semantics` to a non-vulnerable version in its existing semver range; no dependency is added.

- [ ] **Step 7: Verify GREEN and audit behavior**

Run: `PACIFICDB_WORKBENCH_ENGINE="$PWD/build/db_engine" node scripts/prepare-workbench-desktop.mjs && node scripts/test-workbench-package-contract.mjs && python3 scripts/test-workflow-contract.py && npm audit --omit=dev --audit-level=high`

Expected: package contract and workflow contract print PASS; production audit reports zero vulnerabilities.

- [ ] **Step 8: Run the repository version contract**

Run: `python3 scripts/test-release-consistency.py`

Expected: `RELEASE_CONSISTENCY_PASS`, proving the independent desktop version did not alter product component identity.

- [ ] **Step 9: Commit**

```bash
git add package.json package-lock.json scripts/prepare-workbench-desktop.mjs \
  .github/workflows/workbench-desktop.yml scripts/test-workflow-contract.py \
  scripts/test-workbench-package-contract.mjs
git commit -m "build: separate Workbench release version"
```

### Task 2: Upgrade, Backup, Restore, and Caller-Owned Data

**Files:**
- Modify: `scripts/test-workbench-desktop.mjs`
- Create: `scripts/test-workbench-upgrade-contract.mjs`
- Modify: `docs/WORKBENCH.md`

**Interfaces:**
- Consumes: `PACIFICDB_TEST_DESKTOP` candidate executable.
- Produces: optional `PACIFICDB_TEST_DESKTOP_PREVIOUS` previous executable.
- Produces: optional `PACIFICDB_TEST_DESKTOP_DATA` caller-owned user-data directory, which the test must never remove.
- Produces: `WORKBENCH_DESKTOP_UPGRADE_PASS` only after previous-version seeding, candidate verification, CLI backup verification/restore history, and candidate restart all succeed.

- [ ] **Step 1: Add the failing upgrade harness contract**

Create `scripts/test-workbench-upgrade-contract.mjs` with two temporary executable wrappers. The previous wrapper records that it launched before delegating to Electron; the candidate wrapper refuses to start unless that marker exists. Run the real desktop test with both executable variables and a caller-owned data directory, then assert the directory and database files remain after the test process exits.

- [ ] **Step 2: Verify RED**

Run: `xvfb-run -a node scripts/test-workbench-upgrade-contract.mjs`

Expected: FAIL because `test-workbench-desktop.mjs` ignores the previous executable and deletes its internally selected directory.

- [ ] **Step 3: Implement phased executable selection and data ownership**

Refactor `launch(executablePath)` in `scripts/test-workbench-desktop.mjs`. When a previous executable is supplied, use it for the creation phase, cleanly quit it, then use the candidate for all verification phases. Use the caller-owned directory verbatim and remove only directories created by the test itself.

After the initial document/media/preferences assertions, use the bundled CLI against the running candidate to create a named backup, list and verify it, request a restore, and confirm a completed restore appears in `list restores`. Preserve the current same-executable behavior when no previous executable is supplied.

- [ ] **Step 4: Verify GREEN**

Run: `xvfb-run -a node scripts/test-workbench-upgrade-contract.mjs`

Expected: `WORKBENCH_DESKTOP_UPGRADE_PASS`, with the caller-owned database directory still present.

- [ ] **Step 5: Run existing desktop behavior**

Run: `PACIFICDB_WORKBENCH_ENGINE="$PWD/build/db_engine" xvfb-run -a npm run test:workbench:desktop`

Expected: existing `WORKBENCH_DESKTOP_PASS` remains present and no stderr error is collected.

- [ ] **Step 6: Document the test inputs**

Update the verification section of `docs/WORKBENCH.md` with the three environment variables and explain that restore creates a validated restore record/directory rather than overwriting the live database.

- [ ] **Step 7: Commit**

```bash
git add scripts/test-workbench-desktop.mjs scripts/test-workbench-upgrade-contract.mjs docs/WORKBENCH.md
git commit -m "test: verify Workbench upgrade and recovery"
```

### Task 3: Exact-Revision Workbench Qualification

**Files:**
- Create: `scripts/workbench_release_qualification.py`
- Create: `scripts/test_workbench_release_qualification.py`
- Create: `docs/schemas/workbench-load-result.schema.json`
- Modify: `docs/RELEASE_EVIDENCE.md`

**Interfaces:**
- Consumes: `power_loss_harness.validate_evidence(evidence: dict) -> dict`.
- Consumes: `security_review.validate_review(bundle: Path | str, review: dict | None) -> dict`.
- Produces: `validate_load_evidence(evidence: dict, revision: str) -> dict`.
- Produces CLI subcommand: `platform --name NAME --version VERSION --artifact PATH --output PATH`.
- Produces CLI subcommand: `aggregate --version VERSION --artifact PATH... --platform-result PATH... --physical-power-evidence PATH --security-bundle PATH --security-review PATH --load-evidence PATH --audit-report PATH --output PATH`.
- Produces JSON decision `PASS`, `BLOCKED`, or `FAIL` bound to `revision`, `dirty`, `version`, and exact artifact digests.

- [ ] **Step 1: Write failing validator tests**

In `scripts/test_workbench_release_qualification.py`, use temporary Git repositories and literal JSON fixtures to cover: clean and dirty revisions, complete/missing platform matrices, artifact digest mismatch, physical evidence for another revision, missing security digest, unresolved high finding, load evidence with nonzero errors, load evidence shorter than the declared duration, runtime audit advisory, and a complete PASS case.

- [ ] **Step 2: Verify RED**

Run: `python3 -m unittest scripts/test_workbench_release_qualification.py -v`

Expected: import failure because `scripts.workbench_release_qualification` does not exist.

- [ ] **Step 3: Implement the minimal qualification module**

Reuse `collect_repository_state`, `collect_artifact`, and atomic JSON writing from `release_qualification.py`; reuse the existing power and security validators. Add only targeted standard-library validation for the load record and npm-audit JSON. Require platform names `linux-x64`, `windows-x64`, `macos-arm64`, and `macos-x64`, matching revision/version, status `PASS`, and matching artifact digest.

Define the load schema with literal required fields: schema version 1, revision, status `PASS`, duration seconds at least 28,800, records at least 500,000, operations greater than zero, errors exactly zero, and positive peak resident bytes.

- [ ] **Step 4: Verify GREEN**

Run: `python3 -m unittest scripts/test_workbench_release_qualification.py -v`

Expected: all qualification tests pass.

- [ ] **Step 5: Verify existing evidence tools**

Run: `python3 -m unittest scripts/test_release_qualification.py scripts/test_power_loss_harness.py scripts/test_security_review.py -v`

Expected: all existing release, power, and security evidence tests pass.

- [ ] **Step 6: Document the external-evidence contract**

Update `docs/RELEASE_EVIDENCE.md` with the Workbench qualification commands, 8-hour/500k load floor, exact-revision rule, and the fact that absent external evidence intentionally yields `BLOCKED`.

- [ ] **Step 7: Commit**

```bash
git add scripts/workbench_release_qualification.py \
  scripts/test_workbench_release_qualification.py \
  docs/schemas/workbench-load-result.schema.json docs/RELEASE_EVIDENCE.md
git commit -m "build: add Workbench release qualification"
```

### Task 4: Real Installer and Upgrade Jobs

**Files:**
- Modify: `.github/workflows/workbench-desktop.yml`
- Modify: `scripts/test-workflow-contract.py`
- Create: `scripts/test-workbench-installer-contract.py`

**Interfaces:**
- Consumes: Task 1 manifest-derived versions.
- Consumes: Task 2 desktop upgrade environment variables.
- Produces: one platform-evidence JSON per matrix entry through Task 3 `platform` command.
- Produces: release job input artifacts only from installed-package tests, never `*-unpacked` tests.

- [ ] **Step 1: Write failing installer workflow contract**

Create `scripts/test-workbench-installer-contract.py` to parse the workflow job steps and assert each platform has this observable sequence: build installer, install/mount/copy actual artifact, run previous-to-candidate desktop test using a caller-owned data directory, uninstall/remove candidate, assert data remains, write platform evidence, upload installer plus evidence. Assert the release job downloads all evidence and invokes Task 3 aggregation before `gh release create`.

- [ ] **Step 2: Verify RED**

Run: `python3 scripts/test-workbench-installer-contract.py`

Expected: FAIL because jobs currently run only unpacked applications and upload no platform evidence.

- [ ] **Step 3: Implement Linux installed-artifact flow**

Download the public 1.1.1 Debian fixture, install it, seed the caller-owned data directory, remove it without purging data, install the newly built 1.1.2 Debian package, run candidate verification under `xvfb`, uninstall it, assert the data directory remains, then emit `linux-x64` evidence.

- [ ] **Step 4: Implement Windows installed-artifact flow**

Download the public 1.1.1 NSIS fixture, install it silently into a runner-local directory, seed data, uninstall it, install the candidate NSIS artifact into a separate runner-local directory, verify the same data, uninstall it, assert preservation, then emit `windows-x64` evidence. Keep signing behavior untouched.

- [ ] **Step 5: Implement macOS installed-artifact flow**

For each architecture, download/mount the 1.1.1 DMG fixture, copy the app to a temporary Applications directory, seed data, remove it, mount/copy the candidate DMG, verify the same data, remove it, assert preservation, then emit the matching macOS evidence. Do not add Gatekeeper/signing assertions.

- [ ] **Step 6: Gate publication on aggregated qualification**

Upload each installer with its platform result. In the release job, download flat artifacts; decode `WORKBENCH_PHYSICAL_POWER_EVIDENCE_BASE64`, `WORKBENCH_SECURITY_REVIEW_EVIDENCE_BASE64`, and `WORKBENCH_LOAD_EVIDENCE_BASE64` without echoing their values; run `npm audit --omit=dev --json`; build the security bundle; aggregate qualification; and invoke `gh release create` only when the report decision is `PASS`. Upload the qualification report even when aggregation blocks. A first tag run without external evidence is expected to retain candidate artifacts and block publication; `workflow_dispatch.release_tag` retries the immutable tag after evidence is supplied.

- [ ] **Step 7: Verify workflow contracts GREEN**

Run: `python3 scripts/test-workbench-installer-contract.py && python3 scripts/test-workflow-contract.py`

Expected: both print PASS.

- [ ] **Step 8: Run a non-publishing local Linux package rehearsal**

Run: `PACIFICDB_WORKBENCH_ENGINE="$PWD/build/db_engine" npm run desktop:build -- --linux deb tar.gz --x64 --publish never`

Expected: Debian and tar.gz candidates named with Workbench version 1.1.2 are created.

- [ ] **Step 9: Commit**

```bash
git add .github/workflows/workbench-desktop.yml scripts/test-workflow-contract.py \
  scripts/test-workbench-installer-contract.py
git commit -m "ci: qualify installed Workbench artifacts"
```

### Task 5: Production Operations Documentation

**Files:**
- Modify: `docs/WORKBENCH.md`
- Modify: `docs/WORKBENCH_PRODUCTION_READINESS.md`
- Modify: `README.md`
- Create: `docs/releases/workbench-1.1.2.md`

**Interfaces:**
- Consumes: exact commands and limits implemented in Tasks 1-4.
- Produces: user-facing installation, backup, upgrade, rollback, support, and security boundaries for Workbench 1.1.2.

- [ ] **Step 1: Update the Workbench operations guide**

Document supported targets, platform-specific install/uninstall paths, data directories, external backup before upgrade, bundled-CLI backup/verify/restore commands, previous-installer rollback, manual update policy, logs, and recovery from a failed candidate launch.

- [ ] **Step 2: Replace stale readiness status**

Rewrite `docs/WORKBENCH_PRODUCTION_READINESS.md` as a candidate checklist that distinguishes locally verified results from hosted/platform/external gates. State that missing physical-power, independent-security, long-load, or signing evidence blocks a production claim.

- [ ] **Step 3: Add candidate release notes**

Create `docs/releases/workbench-1.1.2.md` describing Workbench-only versioning, bundled component 1.1.1, manual updates, supported targets, data preservation, and the no-public-network boundary. Do not claim that unpublished artifacts exist publicly.

- [ ] **Step 4: Update the README pointer**

Link the Workbench guide and candidate readiness status without changing the main PacificDB 1.1.1 product release identity.

- [ ] **Step 5: Verify documentation contracts**

Run: `node scripts/test-site-docs.mjs && node scripts/test-site-docs-contract.mjs && python3 scripts/test-release-consistency.py`

Expected: site/doc contracts and release consistency pass.

- [ ] **Step 6: Commit**

```bash
git add docs/WORKBENCH.md docs/WORKBENCH_PRODUCTION_READINESS.md \
  docs/releases/workbench-1.1.2.md README.md
git commit -m "docs: define Workbench production operations"
```

### Task 6: Integrated Verification and Handoff

**Files:**
- Modify only files required by failures demonstrated in this task.

**Interfaces:**
- Consumes: all previous task outputs.
- Produces: a clean candidate branch with fresh local evidence and an explicit list of hosted/external checks still blocked.

- [ ] **Step 1: Build fresh native binaries**

Run: `cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.1.1 && cmake --build build --target db_engine pacificdb -j2`

Expected: both binaries build and report component version 1.1.1.

- [ ] **Step 2: Run affected unit and contract suites**

Run: `npm run test:npm && python3 -m unittest scripts/test_workbench_release_qualification.py scripts/test_release_qualification.py scripts/test_power_loss_harness.py scripts/test_security_review.py -v && python3 scripts/test-workflow-contract.py && python3 scripts/test-workbench-installer-contract.py`

Expected: all tests pass with zero failures.

- [ ] **Step 3: Run Workbench behavior suites**

Run: `node scripts/test-workbench-e2e.mjs build && node scripts/test-desktop-engine.mjs build && node scripts/test-index-low-cardinality.mjs build 500000 && npm run test:workbench:browser -- build && PACIFICDB_WORKBENCH_ENGINE="$PWD/build/db_engine" xvfb-run -a npm run test:workbench:desktop && xvfb-run -a node scripts/test-workbench-upgrade-contract.mjs`

Expected: every named PASS marker is present, including the 500,000-record index result and upgrade pass.

- [ ] **Step 4: Build and exercise the local Linux installer**

Run: `PACIFICDB_WORKBENCH_ENGINE="$PWD/build/db_engine" npm run desktop:build -- --linux deb tar.gz --x64 --publish never`

Then run the same install/test/uninstall/data-preservation sequence used by the Linux workflow on the generated Debian package.

Expected: installed application passes and the caller-owned database directory remains after uninstall.

- [ ] **Step 5: Verify dependency and repository state**

Run: `npm audit --omit=dev --audit-level=high && npm audit --audit-level=high; git diff --check; git status --short`

Expected: runtime audit clean; known high build advisory absent; no whitespace errors; only intended committed changes exist.

- [ ] **Step 6: Generate deliberately blocked development qualification**

Run the aggregate command without external evidence against local Linux artifacts.

Expected: report decision `BLOCKED` naming missing physical-power, security, load, and non-Linux platform evidence. This is the correct local outcome and must not be rewritten as PASS.

- [ ] **Step 7: Final whole-branch review**

Review the complete base-to-head diff against the spec, emphasizing installer command quoting, cleanup ordering, release fail-closed behavior, evidence/revision binding, and preservation of signing gates. Fix Critical or Important findings with RED-GREEN tests, then rerun the integrated suite.

- [ ] **Step 8: Commit verified review fixes, if any**

If review fixes were required, use the exact `git add` paths and commit command
from the task that owns those files, with commit message
`fix: address Workbench readiness review`. If no fix was required, create no
empty commit.
