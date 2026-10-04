# SDK Package Qualification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Build complete Java/Python packages, verify their installed behavior and prepare explicitly gated publication workflows.

**Architecture:** Package the completed SDK APIs using current setuptools/Maven ecosystems and no additional runtime libraries. Build once, inspect artifacts and run smoke/tests from clean installed contexts; publication jobs consume tested artifacts rather than rebuild silently. Local Java bundle checks skip remote upload; test-only signing is distinguished from a release-owner signature.

**Tech Stack:** Python build/twine development tools, setuptools, Maven source/Javadoc/GPG/Central plugins, GitHub Actions OIDC/protected environments and existing Node/Playwright/Electron checks.

**Spec:** [Database-first design](../specs/2026-10-03-database-first-cli-workbench-design.md). Execute after [workflow](2026-10-03-database-first-workflow.md) and [SDK](2026-10-03-java-python-sdk-capabilities.md) plans.

## Global Constraints

- “No package is published, no release tag is created, and no installer is deployed during implementation.”
- “Local artifacts and tag-gated publication workflows are prepared and validated.”
- “Python uses only the standard library at runtime; Java retains Jackson as its only runtime dependency.”
- “Registry project ownership, `io.pacificdb` namespace verification, PyPI trusted-publisher registration, signing keys, protected environments, and final human approval remain external prerequisites.”
- “Record platform/runtime limitations instead of claiming untested Windows/macOS or registry validation.”
- Retain current package versions until an authorized coordinated release-version change; local `1.0.1` artifacts are candidates, not permission to overwrite registry versions. Use existing release consistency check for eventual version changes.

## Review Focus

- Wheel/sdist contains source but no usable import or license: inspect both archives and test clean installed artifact (task 1).
- Java compiles on current JDK but breaks declared Java 11 consumers: compiler `release=11`, inspect bytecode, CI exercises supported floor (task 2).
- Test signing or local validation mistaken for Central publication approval: evidence separates test key, release signature, local bundle and remote validation (task 2).
- Tag/workflow dispatch bypasses package tests or protected publication approval: workflow contract tests require exact tag/version/artifact and environment checks (task 3).
- Broad production claim attached to SDK tests while engine external release gates remain blocked: final report links existing release evidence without waiving it (task 4).

---

### Task 1: Complete Python metadata and installed-artifact verification

**Files:** Modify `sdk/python/pyproject.toml`, `sdk/python/README.md`, SDK public exports; create `sdk/python/MANIFEST.in` only if discovery needs it, `scripts/test-sdk-packages.py`, `scripts/verify-sdk-packages.py`; update release consistency tests where new fields require it.

**Interfaces:** Distribution remains `pacificdb`; wheel imports `pacificdb.PacificDB`, `PacificDBClient`, public errors and operation accessors. Verifier CLI `python3 scripts/verify-sdk-packages.py --python-dist sdk/python/dist --java-target sdk/java/target --output <json>` inspects packages without publishing. Missing artifact/signature is reported honestly, never silently passing.

- [x] Write verifier tests with temporary good/bad archives: missing README/license/public modules, credentials/build-cache inclusion, malformed metadata, incorrect version/Requires-Python, missing expected API export and altered hash fail. Run `python3 scripts/test-sdk-packages.py`; expect targeted missing verifier/metadata behavior to fail before implementation.
- [x] Fill description/readme/license files/author/project URLs and supported Python classifiers, explicit package discovery and included files using setuptools. URLs point to this repository; do not claim a registry listing exists. Use development-only build/twine tooling in an isolated environment and build `python -m build sdk/python`; run `python -m twine check sdk/python/dist/*`.
- [x] Inspect both sdist and wheel; rebuild wheel from sdist to detect missing sources. Install exact wheel in a clean venv without source-tree PYTHONPATH, confirm import location and zero runtime dependencies, then run unit/installed-real-engine smoke from a temporary directory. Pin development tool versions in CI setup, not runtime requirements. Hash artifacts and record archive inventories.
- [x] Run package verifier tests and installed smoke; commit as `build: qualify Python distribution metadata and installed client`.

### Task 2: Java Maven metadata, complete artifacts and local Central bundle

**Files:** Modify `sdk/java/pom.xml`, `sdk/java/README.md`; extend verifier/tests from task 1; create `scripts/test-java-installed-client.sh` and artifact consumer test fixture under `sdk/contracts/installed-java/`.

**Interfaces:** Coordinates remain `io.pacificdb:pacificdb-client:<version>`. Attach binary, `-sources.jar`, `-javadoc.jar` and complete POM. Release profile `central` configures `org.sonatype.central:central-publishing-maven-plugin:0.11.0`, `autoPublish=false`, `waitUntil=validated`, `checksums=all`, and property-controlled `skipPublishing` default **true** for local safety; only explicit authorized publication workflow overrides it. Ordinary `mvn test/package/install` never uploads.

- [x] Add POM/archive checks for metadata (name, description, URL, Apache license URL/distribution, developer identity, SCM), Java 11 bytecode, public API in binary/sources/Javadoc, checksums and detached signatures. Fail missing Javadoc/signature or unexpected dependency; do not treat an unsigned package as a valid Central bundle.
- [x] Configure pinned compiler (`release=11`), source, Javadoc and GPG plugins in appropriate build/release profiles; ensure docs cover the entire public API and Javadoc generation fails real errors. Keep Jackson the sole direct runtime dependency (its transitive Jackson modules remain expected). Use release-owner signing via configured key references, never committed private keys.
- [x] Build/test/install to a temporary local Maven repository; compile/run separate consumer from that repository with no source/class-directory shortcuts. Use the task 1 verifier to inspect JARs/POM. For local signature/bundle pipeline test, generate a disposable explicitly labelled test-only GPG key in a temporary keyring and run `mvn -B -f sdk/java/pom.xml -Pcentral -Dcentral.skipPublishing=true deploy` with that key and temporary repository configuration. Verify bundle signatures/checksums against the test public key. Delete only that temporary private keyring; mark resulting bundle test-signed, unsuitable for release-owner publication.
- [x] Run archive/verifier/installed-consumer tests and record hashes/signature identity/local-only status. Commit as `build: prepare complete Java SDK artifacts and safe Central profile`.

### Task 3: SDK CI and manually gated publishing workflows

**Files:** Create `.github/workflows/sdk-packages.yml`, `.github/workflows/python-publish.yml`, `.github/workflows/java-publish.yml`; modify existing CI to invoke shared SDK contracts; create `scripts/test-sdk-workflows.py`, `docs/SDK_PUBLISHING.md`.

**Interfaces:** Build/test workflow runs on SDK/contracts/scripts changes and manual invocation, uploads exact inspected artifacts. Python publication accepts release tags `v<major>.<minor>.<patch>` only, verifies package/tag versions, downloads/builds tested distribution artifact, uses protected `pypi` environment and job-scoped `id-token: write`; `pypa/gh-action-pypi-publish` OIDC, no long-lived PyPI secret. Java release workflow verifies same tag/coordinates, uses protected `maven-central`, imports externally configured release signing key privately, produces/verifies signed bundle, uploads for validation only (`autoPublish=false`); final Central publish remains a separate explicit owner action. Public workflows are prepared, not triggered here.

- [x] Write workflow contract tests rejecting missing version/tag guard, missing test/installed smoke/inspection dependency, unsafe permissions outside publish job, absence of protected environment, long-lived PyPI token, automatic Central publication, disabled TLS/signature verification or secret printing. Run `python3 scripts/test-sdk-workflows.py`; expect missing workflows/guards to fail before configuration.
- [x] Implement supported Python-floor/current and Java 11/17/21 test matrix using current installed dependencies. Add Linux and Windows transport/path tests where runner support exists; local results are still Linux-only. Build Python wheel/sdist and Java full JARs, run installed smoke then hash artifact before upload. Separate credentials into protected publishing jobs; no registry writes in ordinary CI/builds. Pin plugin/action versions to official verified releases, using commit hashes for third-party publish actions.
- [x] Document one-page release commands, registry ownership/namespace verification, PyPI trusted publisher registration, protected environments and signing setup; record existing version occupancy as unresolved until release owner verifies it. Link primary PyPI/Central docs. Verify the local-only Java command cannot contact publishing endpoint even when credentials happen to exist.
- [x] Run workflow contract tests, release consistency checks and artifact verifier; commit as `ci: add tested and approval-gated SDK package releases`.

### Task 4: Fresh whole-program qualification and release evidence

**Files:** Create `docs/evidence/2026-10-03-database-first-package-readiness.md`, extend `docs/RELEASE_EVIDENCE.md` only to link scoped SDK/UI evidence, update relevant docs/tests for final changes; retain generated artifacts in ignored output directories.

**Interfaces:** Evidence names exact clean commit, versions/platform, test commands/results, package SHA-256 and per-gate status. External prerequisites remain `BLOCKED`/not run, not implied pass. No migration or performance claim.

- [x] Run fresh full `scripts/test-community.sh build`, both SDK suites/matrix, native/npm database-first integration, Workbench API/browser/Linux-desktop smoke, wheel/sdist/JAR/Javadoc/signature/bundle inspection and installed-artifact smoke. Confirm workspace resolution and binary embedded revision refer to this isolated branch. No test silently skipped as a pass.
- [x] Use native execution's one fresh independent whole-branch reviewer after implementations; include imported baseline, all new code, package/release contracts and retained compatibility. Address actionable findings and rerun affected checks, keeping review/evidence traceable.
- [x] Commit final docs, rebuild immutable distributions from clean final revision and rerun installed-artifact smoke if packaging inputs changed. Record hashes and a runnable beginner example for both languages. Distinguish locally tested signatures from release-owner keys and local validation from remote Central acceptance.
- [x] Report concrete result and remaining registry/platform/engine certification gates. Ask for publication only after artifacts and checks are complete and reviewable; do not publish/tag/deploy in this plan.

## Primary publication references checked during planning

- [PyPI trusted publishing](https://docs.pypi.org/trusted-publishers/using-a-publisher/) and [publisher registration](https://docs.pypi.org/trusted-publishers/adding-a-publisher/): OIDC with a workflow identity and environment.
- [Central Maven plugin](https://central.sonatype.org/publish/publish-portal-maven/): `skipPublishing` creates only a bundle, `autoPublish=false` requires manual publication, and sources/Javadoc/signatures are separate build prerequisites. Plugin release 0.11.0 is listed by the official documentation.

These references guide configuration; reading them does not validate this repository's registry ownership or remote deployment.

Execution note: installed Central plugin 0.11.0 skip mode required a dummy settings server and skipped staging, contrary to the documented bundle-only description. The safe profile remains unchanged; the local/release packer builds a ZIP from cryptographically verified existing bytes without uploading. Prepared publication workflows use the official validation-only API after protected-environment approval, preserving tested bytes. No registry endpoint was contacted.

Final execution note: qualification is frozen to clean runtime revision `e6781c9`; the final evidence-only commit changes no runtime or package input and intentionally does not replace exact installed/crash-tested bytes. Its seven-artifact manifest records that revision honestly. Publication workflows separately rebuild and qualify the authorized release tag; no tag/hash guard is relaxed.
