#!/usr/bin/env python3
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise AssertionError(f"{path} must contain {needle!r}")


def forbid(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle in text:
            raise AssertionError(f"{path} must not contain {needle!r}")


def upload_paths(workflow: str, artifact_name: str) -> list[str]:
    match = re.search(
        rf"^      - uses: actions/upload-artifact@v6\n"
        rf"        with:\n"
        rf"          name: {re.escape(artifact_name)}\n"
        rf"          path: \|\n"
        rf"((?:            .+\n)+)",
        workflow,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"release artifact upload {artifact_name!r} is missing")
    return [line.strip() for line in match.group(1).splitlines()]


def require_flat_release_uploads() -> None:
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    expected_uploads = {
        "linux-amd64": [
            "dist/pacificdb-community-*-linux-amd64.deb",
            "dist/p0-evidence-linux-amd64.json",
        ],
        "windows-x64": [
            "dist/pacificdb-community-*-windows-x64.exe",
            "dist/p0-evidence-windows-x64.json",
        ],
        "macos-${{ matrix.arch }}": [
            "dist/pacificdb-community-*-macos-${{ matrix.arch }}.pkg",
            "dist/p0-evidence-macos-${{ matrix.arch }}.json",
        ],
        "linux-amd64-container": [
            "dist/p0-evidence-linux-amd64-container.json",
        ],
    }
    for artifact_name, expected_paths in expected_uploads.items():
        actual_paths = upload_paths(workflow, artifact_name)
        if any(not path.startswith("dist/") for path in actual_paths):
            raise AssertionError(
                f"release artifact upload {artifact_name!r} must not mix upload roots"
            )
        if actual_paths != expected_paths:
            raise AssertionError(
                f"release artifact upload {artifact_name!r} paths must be {expected_paths!r}"
            )
    require(
        ".github/workflows/release.yml",
        "arch: arm64",
        "arch: x86_64",
    )
    macos_paths = upload_paths(workflow, "macos-${{ matrix.arch }}")
    for arch in ("arm64", "x86_64"):
        resolved_paths = [path.replace("${{ matrix.arch }}", arch) for path in macos_paths]
        expected_paths = [
            f"dist/pacificdb-community-*-macos-{arch}.pkg",
            f"dist/p0-evidence-macos-{arch}.json",
        ]
        if resolved_paths != expected_paths:
            raise AssertionError(
                f"macOS {arch} upload paths must be {expected_paths!r}"
            )


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
    "contents: write",
    "packages: write",
    "source-linux:",
    "contents: read",
    "packages:",
    "uses: ./.github/workflows/prerelease.yml",
)
# The protected branch requires packages / linux, windows and both macOS jobs.
# Keep the native reusable workflow under this caller ID; SDK/Linux-only
# prerelease qualification cannot supply the missing native platform checks.
require(
    ".github/workflows/ci.yml",
    "  packages:\n    needs: identity\n    uses: ./.github/workflows/release.yml",
    "  prerelease:\n    needs: identity\n    if: needs.identity.outputs.channel == 'prerelease'\n    uses: ./.github/workflows/prerelease.yml",
)
release_identity = json.loads((ROOT / "release.json").read_text())
require(".github/workflows/ci.yml", "      version: ${{ needs.identity.outputs.version }}\n",
        'python3 scripts/release_identity.py --field releaseVersion')
require(
    ".github/workflows/release.yml",
    '$baseVersion = $version.Split(\'-\')[0]',
    '-DPACIFICDB_ENGINE_VERSION="${version%%-*}"',
    '-DPACIFICDB_ENGINE_VERSION="$baseVersion"',
    'PACIFICDB_RELEASE_VERSION=${{ env.IMAGE_VERSION }}',
)
forbid(".github/workflows/ci.yml", "secrets: inherit")
require(
    ".github/workflows/npm-publish.yml",
    "workflow_dispatch:",
    "environment:",
    "check-sdk-release-tag.py",
    '--tag "$dist_tag"',
    "npm_publication_policy.py",
)
forbid(".github/workflows/npm-publish.yml", "--tag latest")
require(
    ".github/workflows/workbench-desktop.yml",
    "release_tag:",
    "WORKBENCH_RELEASE_TAG:",
    "WORKBENCH_VERSION=$(node -p",
    "PACIFICDB_COMPONENT_VERSION=$(python3 scripts/release_identity.py --field version)",
    '-DPACIFICDB_ENGINE_VERSION="$PACIFICDB_COMPONENT_VERSION"',
    '-DPACIFICDB_ENGINE_VERSION="$env:PACIFICDB_COMPONENT_VERSION"',
    '--x-install-root="$pwd/build/vcpkg_installed"',
    '-DVCPKG_INSTALLED_DIR="$pwd/build/vcpkg_installed"',
    'test "$version" = "$WORKBENCH_VERSION"',
    "ref: ${{ env.WORKBENCH_RELEASE_TAG || inputs.source_ref || github.ref }}",
    "if ($env:WORKBENCH_RELEASE_TAG -in @('workbench-v1.1.1', 'workbench-v1.2.0'))",
    "$env:PACIFICDB_DESKTOP_RELEASE = '0'",
    'if [[ "$WORKBENCH_RELEASE_TAG" == workbench-v1.1.1 || "$WORKBENCH_RELEASE_TAG" == workbench-v1.2.0 ]]',
    "export PACIFICDB_DESKTOP_RELEASE=0",
    "workbench_release_qualification.py platform",
    "workbench_release_qualification.py aggregate",
    "WORKBENCH_PHYSICAL_POWER_EVIDENCE_BASE64",
    "WORKBENCH_SECURITY_REVIEW_EVIDENCE_BASE64",
    "WORKBENCH_LOAD_EVIDENCE_BASE64",
    "if: always()",
    'gh release create "$WORKBENCH_RELEASE_TAG"',
)
# Every platform must preserve explicit frozen-source qualification, with an
# existing release tag taking precedence over a manually selected source ref.
assert (ROOT / ".github/workflows/workbench-desktop.yml").read_text().count(
    "ref: ${{ env.WORKBENCH_RELEASE_TAG || inputs.source_ref || github.ref }}") == 3
forbid(
    ".github/workflows/workbench-desktop.yml",
    "-DPACIFICDB_ENGINE_VERSION=1.1.1",
    'test "$version" = "$(node -p "require(\'./cli/package.json\').version")"',
    "linux-unpacked",
    "win-unpacked",
    "dist/desktop/PacificDB-Workbench-*.tar.gz",
    '-DVCPKG_INSTALLED_DIR="$pwd/vcpkg_installed"',
)
require_flat_release_uploads()
require(
    ".github/workflows/release.yml",
    "container:",
    "docker/setup-buildx-action@v3",
    "docker/login-action@v4",
    "docker/build-push-action@v7",
    "push-by-digest=true",
    "p0-evidence-linux-amd64-container.json",
    "container_publish:",
    "needs: [container]",
    "needs: [linux, windows, macos, container_publish]",
    'pattern: "{linux-amd64,windows-x64,macos-*,linux-amd64-container}"',
    "docker buildx imagetools create",
    'test "$alias_digest" = "${image_ref#*@}"',
    "packages: write",
    '--revision "$GITHUB_SHA"',
    "--allow-unsigned",
    "--prerelease=false",
)
forbid(
    ".github/workflows/release.yml",
    "ghcr.io/hitesh-reddy-k/pacificdb-community:latest",
    "ghcr.io/hitesh-reddy-k/pacificdb-community:beta",
)
print("WORKFLOW_CONTRACT_PASS")

# Unsigned publishing is an explicit owner decision for named historical tags.
# Never turn it into an arbitrary stable-version or wildcard exemption.
release_workflow = (ROOT / '.github/workflows/release.yml').read_text()
require('.github/workflows/release.yml',
        "$env:GITHUB_REF_NAME -in @('v1.0.0', 'v1.1.1', 'v1.2.0')")
assert release_workflow.count('"$GITHUB_REF_NAME" == v1.2.0') == 3
require('.github/workflows/release.yml', "--title 'v1.2 Release' --draft=true --prerelease=false")
workbench_workflow = (ROOT / '.github/workflows/workbench-desktop.yml').read_text()
assert workbench_workflow.count("env.WORKBENCH_RELEASE_TAG != 'workbench-v1.2.0'") == 3
require('.github/workflows/workbench-desktop.yml',
        '"$WORKBENCH_RELEASE_TAG" != workbench-v1.2.0')
forbid('.github/workflows/release.yml', "'v1.2.*'", '== v1.2.*')
forbid('.github/workflows/workbench-desktop.yml', '== workbench-v1.2.*')
print('UNSIGNED_VERSION_EXCEPTION_PASS')

# Stable native Linux artifacts share the announced Ubuntu24 ABI with Workbench.
require('.github/workflows/release.yml',
        "  linux:\n    if: ${{ !startsWith(github.ref, 'refs/tags/v') || !contains(github.ref_name, '-') }}\n    runs-on: ubuntu-24.04")
print('LINUX_PACKAGE_ABI_CONTRACT_PASS')
