#!/usr/bin/env python3
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CANONICAL_LOGO = "site/assets/pacificdb-logo-symbol.png"


def require_text(relative_path: str, expected: str) -> None:
    text = (ROOT / relative_path).read_text(encoding="utf-8")
    if expected not in text:
        raise AssertionError(f"{relative_path} must contain {expected!r}")


def match_version(relative_path: str, pattern: str) -> str:
    text = (ROOT / relative_path).read_text(encoding="utf-8")
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"cannot read version from {relative_path}")
    return match.group(1)


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
        "pacificdb-community/pacificdb-v1.0/" + CANONICAL_LOGO
    )
    for readme in (
        "cli/README.md",
        "sdk/node/README.md",
        "sdk/python/README.md",
        "sdk/java/README.md",
    ):
        require_text(readme, raw_logo)

    engine_version = match_version(
        "engine/CMakeLists.txt",
        r'set\(PACIFICDB_ENGINE_VERSION "([^"]+)"',
    )
    cli = json.loads((ROOT / "cli/package.json").read_text(encoding="utf-8"))
    node = json.loads(
        (ROOT / "sdk/node/package.json").read_text(encoding="utf-8")
    )

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
            "sdk/java/pom.xml",
            r"<artifactId>pacificdb-client</artifactId>\s*<version>([^<]+)</version>",
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
            "version mismatch: sdk/python/pyproject.toml has "
            f"{python_version}, expected {pep440}"
        )
    if cli["dependencies"]["@pacificdb/client"] != engine_version:
        raise AssertionError("CLI dependency must exactly match the Node client version")

    print("RELEASE_CONSISTENCY_PASS")


if __name__ == "__main__":
    main()
