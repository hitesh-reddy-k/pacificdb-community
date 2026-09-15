#!/usr/bin/env python3
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
        rf"^      - uses: actions/upload-artifact@v4\n"
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
    "source-linux:",
    "packages:",
    "uses: ./.github/workflows/release.yml",
    "version: 0.1.0-ci.${{ github.run_number }}",
)
forbid(".github/workflows/ci.yml", "secrets: inherit")
require_flat_release_uploads()
print("WORKFLOW_CONTRACT_PASS")
