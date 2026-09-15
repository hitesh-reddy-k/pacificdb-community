#!/usr/bin/env python3
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
print("WORKFLOW_CONTRACT_PASS")
