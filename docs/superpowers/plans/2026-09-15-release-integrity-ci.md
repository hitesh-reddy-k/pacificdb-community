# Release Integrity and Continuous Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore native packaging on current `main` and establish reproducible, mandatory cross-platform build and release gates tied to one source commit.

**Architecture:** A dependency-free Python consistency check protects release metadata and packaging inputs. The existing cross-platform release workflow becomes reusable by pull-request CI, while a separate Linux source job runs the retained repository suite. Tag publication validates a complete artifact set, publishes through a draft release, downloads the result, verifies it, and only then exposes the prerelease.

**Tech Stack:** Python 3 standard library, CMake/CPack, Bash, PowerShell, GitHub Actions, `gh` CLI, Node.js 22, Python 3.12, Java 21/Maven.

**Spec:** `docs/superpowers/specs/2026-09-15-production-readiness-program-design.md`

## Global Constraints

- Supported native matrix: Linux AMD64 Debian, Windows x64 NSIS, macOS 15 arm64, and macOS 15 x86_64.
- Supported OCI matrix in this program: Linux AMD64.
- Every artifact and evidence file must identify one version and Git commit.
- Stable publication must fail on any failed, cancelled, skipped, or unavailable required job.
- The canonical repository logo source is `site/assets/pacificdb-logo-symbol.png`; the installed compatibility filename is `pacificdb-logo.png`.
- Development flows remain available, but no development configuration may be presented as production-safe.
- Do not publish releases, change repository settings, or add secrets while executing this plan.
- Preserve user-owned files and unrelated worktree changes.

---

### Task 1: Canonicalize the packaging logo

**Files:**
- Create: `scripts/test-release-consistency.py`
- Modify: `engine/CMakeLists.txt:398`
- Modify: `scripts/test-community.sh:29`
- Modify: `README.md:2`
- Modify: `cli/README.md:2`
- Modify: `sdk/node/README.md:2`
- Modify: `sdk/python/README.md:2`
- Modify: `sdk/java/README.md:2`

**Interfaces:**
- Consumes: canonical asset `site/assets/pacificdb-logo-symbol.png`.
- Produces: installed package asset `share/pacificdb/pacificdb-logo.png` and command `python3 scripts/test-release-consistency.py`.

- [ ] **Step 1: Write the failing release consistency check**

Create `scripts/test-release-consistency.py` with this initial behavior:

```python
#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CANONICAL_LOGO = "site/assets/pacificdb-logo-symbol.png"


def require_text(relative_path: str, expected: str) -> None:
    text = (ROOT / relative_path).read_text(encoding="utf-8")
    if expected not in text:
        raise AssertionError(f"{relative_path} must contain {expected!r}")


def main() -> None:
    logo = ROOT / CANONICAL_LOGO
    if not logo.is_file() or logo.stat().st_size == 0:
        raise AssertionError(f"missing canonical logo: {CANONICAL_LOGO}")

    require_text("engine/CMakeLists.txt", CANONICAL_LOGO)
    require_text("engine/CMakeLists.txt", "RENAME pacificdb-logo.png")
    require_text("scripts/test-community.sh", f"test -s {CANONICAL_LOGO}")
    require_text("README.md", f'src="{CANONICAL_LOGO}"')

    raw_logo = (
        "https://raw.githubusercontent.com/hitesh-reddy-k/"
        "pacificdb-community/main/" + CANONICAL_LOGO
    )
    for readme in (
        "cli/README.md",
        "sdk/node/README.md",
        "sdk/python/README.md",
        "sdk/java/README.md",
    ):
        require_text(readme, raw_logo)

    print("RELEASE_CONSISTENCY_PASS")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the check and confirm the packaging reference fails**

Run:

```bash
python3 scripts/test-release-consistency.py
```

Expected: FAIL with `engine/CMakeLists.txt must contain 'site/assets/pacificdb-logo-symbol.png'`.

- [ ] **Step 3: Point packaging and documentation at the canonical asset**

Replace the CMake install rule with:

```cmake
install(FILES ${CMAKE_SOURCE_DIR}/../site/assets/pacificdb-logo-symbol.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/pacificdb
    RENAME pacificdb-logo.png)
```

Change `scripts/test-community.sh` to:

```bash
test -s site/assets/pacificdb-logo-symbol.png
```

Change the root README image source to
`site/assets/pacificdb-logo-symbol.png`. Change all four raw GitHub README image
URLs to end in `site/assets/pacificdb-logo-symbol.png`.

- [ ] **Step 4: Verify the focused check and native package behavior**

Run:

```bash
python3 scripts/test-release-consistency.py
cmake -S engine -B build-release-integrity -DCMAKE_BUILD_TYPE=Release
cmake --build build-release-integrity --target db_engine pacificdb -j2
cpack --config build-release-integrity/CPackConfig.cmake -G DEB -B build-release-integrity/dist
scripts/test-native-package.sh \
  build-release-integrity/dist/pacificdb-community-0.1.0-beta.13-Linux.deb \
  0.1.0-beta.13
```

Expected: consistency check prints `RELEASE_CONSISTENCY_PASS`; CPack and package smoke exit 0; the installed package contains a non-empty `share/pacificdb/pacificdb-logo.png`.

- [ ] **Step 5: Add the consistency check to the retained suite**

Insert this immediately before the canonical-logo file assertion in
`scripts/test-community.sh`:

```bash
python3 scripts/test-release-consistency.py
```

Run:

```bash
python3 scripts/test-release-consistency.py
git diff --check
```

Expected: both exit 0.

- [ ] **Step 6: Commit the packaging repair**

```bash
git add engine/CMakeLists.txt scripts/test-community.sh scripts/test-release-consistency.py \
  README.md cli/README.md sdk/node/README.md sdk/python/README.md sdk/java/README.md
git commit -m "fix: restore release packaging asset"
```

---

### Task 2: Enforce one release version across packages

**Files:**
- Modify: `scripts/test-release-consistency.py`
- Modify: `sdk/python/pyproject.toml:7`
- Modify: `sdk/java/pom.xml:7`

**Interfaces:**
- Consumes: release version `0.1.0-beta.13` from the CMake project.
- Produces: exact release-version consistency across CMake, npm workspaces, vcpkg, Helm, Java, Python, and the release workflow.

- [ ] **Step 1: Extend the consistency check with independent version parsing**

Add imports and helpers:

```python
import json
import re


def match_version(relative_path: str, pattern: str) -> str:
    text = (ROOT / relative_path).read_text(encoding="utf-8")
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"cannot read version from {relative_path}")
    return match.group(1)
```

Add these checks to `main()`:

```python
engine_version = match_version(
    "engine/CMakeLists.txt",
    r'set\(PACIFICDB_ENGINE_VERSION "([^"]+)"',
)
cli = json.loads((ROOT / "cli/package.json").read_text(encoding="utf-8"))
node = json.loads((ROOT / "sdk/node/package.json").read_text(encoding="utf-8"))

expected = {
    "cli/package.json": cli["version"],
    "sdk/node/package.json": node["version"],
    "vcpkg.json": json.loads(
        (ROOT / "vcpkg.json").read_text(encoding="utf-8")
    )["version-string"],
    "deploy/helm/pacificdb/Chart.yaml": match_version(
        "deploy/helm/pacificdb/Chart.yaml", r'^appVersion: "([^"]+)"$'
    ),
    ".github/workflows/release.yml": match_version(
        ".github/workflows/release.yml", r"^\s+default: ([^\s]+)$"
    ),
    "sdk/java/pom.xml": match_version(
        "sdk/java/pom.xml", r"<artifactId>pacificdb-client</artifactId>\s*<version>([^<]+)</version>"
    ),
}
for source, actual in expected.items():
    if actual != engine_version:
        raise AssertionError(
            f"version mismatch: {source} has {actual}, expected {engine_version}"
        )

python_version = match_version(
    "sdk/python/pyproject.toml", r'^version = "([^"]+)"$'
)
pep440 = engine_version.replace("-beta.", "b")
if python_version != pep440:
    raise AssertionError(
        f"version mismatch: sdk/python/pyproject.toml has {python_version}, expected {pep440}"
    )
if cli["dependencies"]["@pacificdb/client"] != engine_version:
    raise AssertionError("CLI dependency must exactly match the Node client version")
```

- [ ] **Step 2: Confirm the stale Python or Java version fails**

Run:

```bash
python3 scripts/test-release-consistency.py
```

Expected: FAIL reporting the Java `0.1.0-beta.9` or Python `0.1.0b8` mismatch.

- [ ] **Step 3: Align the source SDK metadata**

Set Java to:

```xml
<version>0.1.0-beta.13</version>
```

Set Python to:

```toml
version = "0.1.0b13"
```

- [ ] **Step 4: Verify package metadata and SDK tests**

Run:

```bash
python3 scripts/test-release-consistency.py
PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests
mvn -q -f sdk/java/pom.xml test
```

Expected: consistency prints `RELEASE_CONSISTENCY_PASS`; Python reports one passing test; Maven exits 0.

- [ ] **Step 5: Commit version consistency**

```bash
git add scripts/test-release-consistency.py sdk/python/pyproject.toml sdk/java/pom.xml
git commit -m "test: enforce release metadata consistency"
```

---

### Task 3: Make the cross-platform package workflow reusable from CI

**Files:**
- Create: `scripts/test-workflow-contract.py`
- Create: `.github/workflows/ci.yml`
- Modify: `.github/workflows/release.yml:3`

**Interfaces:**
- Consumes: reusable workflow input `version: string`.
- Produces: stable required-check jobs `source-linux` and `packages`, with `packages` invoking all four native package jobs.

- [ ] **Step 1: Write a failing workflow contract check**

Create `scripts/test-workflow-contract.py`:

```python
#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise AssertionError(f"{path} must contain {needle!r}")


require(
    ".github/workflows/release.yml",
    "workflow_call:",
    "required: true",
    "type: string",
)
require(
    ".github/workflows/ci.yml",
    "pull_request:",
    "branches: [main]",
    "source-linux:",
    "packages:",
    "uses: ./.github/workflows/release.yml",
    "version: 0.1.0-ci.${{ github.run_number }}",
)
print("WORKFLOW_CONTRACT_PASS")
```

- [ ] **Step 2: Run the check and confirm CI is absent**

Run:

```bash
python3 scripts/test-workflow-contract.py
```

Expected: FAIL because `.github/workflows/ci.yml` does not exist or because `workflow_call:` is absent.

- [ ] **Step 3: Add the reusable workflow interface**

Under `on:` in `.github/workflows/release.yml`, add:

```yaml
  workflow_call:
    inputs:
      version:
        description: Version embedded in non-publishing validation artifacts
        required: true
        type: string
```

Keep `workflow_dispatch` and tag triggers. Existing `${{ inputs.version }}`
expressions consume the same input for both manual and reusable runs.

- [ ] **Step 4: Add pull-request and main CI**

Create `.github/workflows/ci.yml`:

```yaml
name: Required production checks

on:
  pull_request:
  push:
    branches: [main]

permissions:
  contents: read

concurrency:
  group: production-ci-${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  source-linux:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with:
          node-version: 22
          cache: npm
      - uses: actions/setup-python@v5
        with:
          python-version: "3.12"
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: "21"
          cache: maven
      - name: Install native and Python test dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake g++ liblz4-dev libssl-dev
          python -m pip install pytest
      - run: npm ci --ignore-scripts --no-audit --no-fund
      - run: python3 scripts/test-release-consistency.py
      - run: python3 scripts/test-workflow-contract.py
      - run: scripts/test-community.sh build

  packages:
    uses: ./.github/workflows/release.yml
    with:
      version: 0.1.0-ci.${{ github.run_number }}
    secrets: inherit
```

- [ ] **Step 5: Include the workflow contract in the retained suite**

Add immediately after the release consistency command in
`scripts/test-community.sh`:

```bash
python3 scripts/test-workflow-contract.py
```

- [ ] **Step 6: Verify workflow syntax and contracts**

Run:

```bash
python3 scripts/test-workflow-contract.py
python3 scripts/test-release-consistency.py
git diff --check
```

Expected: both Python checks print their PASS marker and all commands exit 0.

- [ ] **Step 7: Commit continuous integration**

```bash
git add .github/workflows/ci.yml .github/workflows/release.yml \
  scripts/test-workflow-contract.py scripts/test-community.sh
git commit -m "ci: require source and platform package validation"
```

---

### Task 4: Validate the complete artifact set before publication

**Files:**
- Create: `scripts/verify-release-artifacts.py`
- Create: `scripts/test-verify-release-artifacts.py`
- Modify: `.github/workflows/release.yml:277`

**Interfaces:**
- Consumes: `--dist PATH`, `--version VERSION`, and `--output PATH`.
- Produces: a JSON manifest with `version`, `artifacts[]`, `name`, `size`, and `sha256`; exits nonzero if a required artifact or evidence file is absent or empty.

- [ ] **Step 1: Write failing artifact-verifier tests**

Create `scripts/test-verify-release-artifacts.py` using `unittest` and a temporary directory. Import `verify-release-artifacts.py` with `importlib.util`. Define:

```python
INSTALLERS = [
    "pacificdb-community-1.0.0-linux-amd64.deb",
    "pacificdb-community-1.0.0-windows-x64.exe",
    "pacificdb-community-1.0.0-macos-arm64.pkg",
    "pacificdb-community-1.0.0-macos-x86_64.pkg",
]
EVIDENCE = [
    "p0-evidence-linux-amd64.json",
    "p0-evidence-windows-x64.json",
    "p0-evidence-macos-arm64.json",
    "p0-evidence-macos-x86_64.json",
]
```

Add three tests:

```python
def test_missing_required_artifact_fails(self):
    self.populate(INSTALLERS[:-1] + EVIDENCE)
    with self.assertRaisesRegex(ValueError, "missing required release artifact"):
        verifier.build_manifest(self.root, "1.0.0")

def test_empty_required_artifact_fails(self):
    self.populate(INSTALLERS + EVIDENCE)
    (self.root / INSTALLERS[0]).write_bytes(b"")
    with self.assertRaisesRegex(ValueError, "empty release artifact"):
        verifier.build_manifest(self.root, "1.0.0")

def test_complete_artifacts_have_stable_hashes(self):
    self.populate(INSTALLERS + EVIDENCE)
    manifest = verifier.build_manifest(self.root, "1.0.0")
    self.assertEqual(manifest["version"], "1.0.0")
    self.assertEqual([item["name"] for item in manifest["artifacts"]], sorted(INSTALLERS + EVIDENCE))
    self.assertTrue(all(len(item["sha256"]) == 64 for item in manifest["artifacts"]))
```

The fixture helper writes each filename as its own UTF-8 content so hashes are
deterministic and non-empty.

- [ ] **Step 2: Run tests and confirm the module is absent**

Run:

```bash
python3 scripts/test-verify-release-artifacts.py
```

Expected: FAIL because `scripts/verify-release-artifacts.py` does not exist.

- [ ] **Step 3: Implement the dependency-free artifact verifier**

Create `scripts/verify-release-artifacts.py` with:

```python
#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path


def required_names(version: str) -> list[str]:
    return [
        f"pacificdb-community-{version}-linux-amd64.deb",
        f"pacificdb-community-{version}-windows-x64.exe",
        f"pacificdb-community-{version}-macos-arm64.pkg",
        f"pacificdb-community-{version}-macos-x86_64.pkg",
        "p0-evidence-linux-amd64.json",
        "p0-evidence-windows-x64.json",
        "p0-evidence-macos-arm64.json",
        "p0-evidence-macos-x86_64.json",
    ]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_manifest(dist: Path, version: str) -> dict:
    artifacts = []
    for name in sorted(required_names(version)):
        path = dist / name
        if not path.is_file():
            raise ValueError(f"missing required release artifact: {name}")
        size = path.stat().st_size
        if size == 0:
            raise ValueError(f"empty release artifact: {name}")
        artifacts.append({"name": name, "size": size, "sha256": sha256(path)})
    return {"version": version, "artifacts": artifacts}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = build_manifest(args.dist, args.version)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Verify unit tests**

Run:

```bash
python3 scripts/test-verify-release-artifacts.py
```

Expected: three tests pass.

- [ ] **Step 5: Make tag publication draft-first and self-verifying**

Add `actions/checkout@v4` before artifact download in the `release` job. Replace
the publication shell body with:

```bash
set -euo pipefail
cd dist
python3 ../scripts/verify-release-artifacts.py \
  --dist . --version "${GITHUB_REF_NAME#v}" --output RELEASE-MANIFEST.json
sha256sum pacificdb-community-* p0-evidence-*.json RELEASE-MANIFEST.json > SHA256SUMS
gh release create "$GITHUB_REF_NAME" * --repo "$GITHUB_REPOSITORY" \
  --draft --prerelease --generate-notes
verify_dir="$RUNNER_TEMP/published-release"
mkdir -p "$verify_dir"
gh release download "$GITHUB_REF_NAME" --repo "$GITHUB_REPOSITORY" --dir "$verify_dir"
(cd "$verify_dir" && sha256sum -c SHA256SUMS)
gh release edit "$GITHUB_REF_NAME" --repo "$GITHUB_REPOSITORY" --draft=false --prerelease
```

The draft intentionally remains available for diagnosis if verification fails;
it must not be exposed as a published release.

- [ ] **Step 6: Verify artifact and workflow contracts**

Run:

```bash
python3 scripts/test-verify-release-artifacts.py
python3 scripts/test-workflow-contract.py
git diff --check
```

Expected: verifier reports three passing tests, workflow check prints
`WORKFLOW_CONTRACT_PASS`, and diff check exits 0.

- [ ] **Step 7: Commit atomic release validation**

```bash
git add scripts/verify-release-artifacts.py scripts/test-verify-release-artifacts.py \
  .github/workflows/release.yml
git commit -m "ci: verify releases before publication"
```

---

### Task 5: Document and inspect required repository controls

**Files:**
- Create: `docs/PRODUCTION_RELEASE.md`
- Create: `scripts/check-repository-controls.sh`
- Modify: `README.md:305`

**Interfaces:**
- Consumes: authenticated read-only `gh api` access and repository name from `GITHUB_REPOSITORY` or `origin`.
- Produces: a non-mutating control audit that exits nonzero when `main` protection, rulesets, CodeQL, Dependabot, or secret scanning are absent.

- [ ] **Step 1: Write the read-only control audit**

Create `scripts/check-repository-controls.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
repo=${GITHUB_REPOSITORY:-hitesh-reddy-k/pacificdb-community}

gh api "repos/$repo/branches/main/protection" >/dev/null
test "$(gh api "repos/$repo/rulesets" --jq 'length')" -gt 0
gh api "repos/$repo/code-scanning/analyses?per_page=1" --jq 'length > 0' | grep -qx true
gh api "repos/$repo" --jq '.security_and_analysis.secret_scanning.status' | grep -qx enabled
gh api "repos/$repo" --jq '.security_and_analysis.secret_scanning_push_protection.status' | grep -qx enabled
gh api "repos/$repo/dependabot/alerts?per_page=1" >/dev/null
printf 'REPOSITORY_CONTROLS_PASS\n'
```

- [ ] **Step 2: Run it and confirm current controls fail**

Run:

```bash
scripts/check-repository-controls.sh
```

Expected: FAIL because `main` is not protected and security analysis is disabled.

- [ ] **Step 3: Document the exact release-owner checklist**

Create `docs/PRODUCTION_RELEASE.md` with these required sections and commands:

```markdown
# Production release procedure

## Required repository controls

- Pull requests required for `main`, with one approval and stale approvals dismissed.
- Required checks: `source-linux` and the reusable cross-platform `packages` workflow.
- Force pushes and branch deletion disabled.
- CodeQL, Dependabot alerts, secret scanning, and push protection enabled.

Verify without changing settings:

```sh
scripts/check-repository-controls.sh
```

## Candidate qualification

Run `scripts/test-community.sh build`, all installed P0 package jobs, the OCI
digest smoke test, mixed-version upgrade test, restore drill, and external
certification probes. Every result must be attached to the release commit.

## Tag publication

Create an annotated release tag only from a clean, reviewed commit. The tag
workflow must remain the sole stable publication path. Do not publish or promote
a draft when any required job is failed, cancelled, skipped, or blocked.

## External writes

Repository controls, credentials, public tags, packages, images, and releases
must be previewed and explicitly approved before mutation.
```

- [ ] **Step 4: Link the release procedure from the README**

Add `- [Production release procedure](docs/PRODUCTION_RELEASE.md)` under “Beta
status and support”.

- [ ] **Step 5: Verify documentation and shell syntax**

Run:

```bash
bash -n scripts/check-repository-controls.sh
python3 scripts/test-release-consistency.py
git diff --check
```

Expected: syntax and consistency checks exit 0. The live control audit remains
expected to fail until the separately approved GitHub settings change occurs.

- [ ] **Step 6: Commit the control contract**

```bash
git add docs/PRODUCTION_RELEASE.md scripts/check-repository-controls.sh README.md
git commit -m "docs: define production release controls"
```

---

### Task 6: Run the integrated release-integrity gate

**Files:**
- Verify only; do not modify files unless a failing check identifies an in-scope defect and a new failing regression test is added first.

**Interfaces:**
- Consumes: Tasks 1–5.
- Produces: fresh local evidence and a clean release-integrity branch state.

- [ ] **Step 1: Run deterministic static and unit checks**

```bash
python3 scripts/test-release-consistency.py
python3 scripts/test-workflow-contract.py
python3 scripts/test-verify-release-artifacts.py
bash -n scripts/check-repository-controls.sh
git diff --check
```

Expected: all local checks pass. Do not treat the deliberately failing live
repository-control audit as part of this local command group.

- [ ] **Step 2: Run SDK tests**

```bash
npm ci --ignore-scripts --no-audit --no-fund
npm run test:npm
PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests
mvn -q -f sdk/java/pom.xml test
```

Expected: all SDK suites pass.

- [ ] **Step 3: Run the retained source suite**

```bash
scripts/test-community.sh build-release-integrity
```

Expected: exit 0 and final line `PacificDB Community checks passed`.

- [ ] **Step 4: Rebuild and smoke-test the Debian artifact from a clean build directory**

```bash
cmake -S engine -B build-package-final -DCMAKE_BUILD_TYPE=Release
cmake --build build-package-final --target package -j2
scripts/test-native-package.sh \
  build-package-final/pacificdb-community-0.1.0-beta.13-Linux.deb \
  0.1.0-beta.13
```

Expected: build and installed-package smoke exit 0.

- [ ] **Step 5: Inspect branch state and commit any plan progress marks separately**

```bash
git status --short --branch
git log --oneline --decorate -8
```

Expected: only intentional plan progress changes, if tracked, remain. Do not
commit generated build, package, node_modules, or Maven target files.
