#!/usr/bin/env python3
import json
import re
from release_identity import load_release_identity
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
        "pacificdb-community/main/" + CANONICAL_LOGO
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
    identity = load_release_identity(ROOT)
    if engine_version != identity["version"]:
        raise AssertionError("engine base version does not match release.json")
    component_version = identity["releaseVersion"]
    # The public website stays on its published channel until explicit publication.
    published_version = "1.1.2" if identity["channel"] == "prerelease" else engine_version
    release_tag = f"v{published_version}"
    landing_tag = match_version("site/index.html", r'<span class="badge">(v[0-9.]+)(?: candidate[^<]*)?</span>')
    if landing_tag != release_tag:
        raise AssertionError("landing version does not match engine")
    require_text("site/index.html", f'href="release-{published_version}.html"')
    docs_tag = match_version("site/docs.html", r'<span class="version-badge">(v[0-9.]+)(?: candidate[^<]*)?</span>')
    if docs_tag != release_tag:
        raise AssertionError("documentation version does not match engine")
    require_text(
        f"site/release-{published_version}.html", f"PacificDB Community · {release_tag}"
    )

    # Published prerelease assets use tag 1.1.2, not the stable v* pattern.
    require_text("site/index.html", f"releases/download/{published_version}/pacificdb-community-{published_version}-linux-amd64.deb")
    require_text("site/index.html", f"releases/download/{published_version}/SHA256SUMS")
    require_text("site/docs.html", f"pacificdb-community-{published_version}-linux-amd64.deb")
    for workflow in ("pages", "ci", "codeql", "sdk-packages", "workbench-desktop"):
        workflow_text = (ROOT/f".github/workflows/{workflow}.yml").read_text()
        if "branches:" not in workflow_text or "main" not in workflow_text:
            raise AssertionError(f"{workflow} must preserve main validation")

    cli = json.loads((ROOT / "cli/package.json").read_text(encoding="utf-8"))
    node = json.loads(
        (ROOT / "sdk/node/package.json").read_text(encoding="utf-8")
    )

    expected = {
        "package.json": json.loads((ROOT / "package.json").read_text())["version"],
        "cli/package.json": cli["version"],
        "sdk/node/package.json": node["version"],
        "vcpkg.json": json.loads(
            (ROOT / "vcpkg.json").read_text(encoding="utf-8")
        )["version-string"],
        "deploy/helm/pacificdb/Chart.yaml": match_version(
            "deploy/helm/pacificdb/Chart.yaml", r'^appVersion: "([^"]+)"$'
        ),
        "sdk/java/pom.xml": match_version(
            "sdk/java/pom.xml",
            r"<artifactId>pacificdb-client</artifactId>\s*<version>([^<]+)</version>",
        ),
    }
    for source, actual in expected.items():
        if actual != component_version:
            raise AssertionError(
                f"version mismatch: {source} has {actual}, expected {component_version}"
            )

    python_version = match_version(
        "sdk/python/pyproject.toml", r'^version = "([^"]+)"$'
    )
    pep440 = identity["pythonVersion"]
    if python_version != pep440:
        raise AssertionError(
            "version mismatch: sdk/python/pyproject.toml has "
            f"{python_version}, expected {pep440}"
        )
    if cli["dependencies"]["@pacificdb/client"] != component_version:
        raise AssertionError("CLI dependency must exactly match the Node client version")

    lock = json.loads((ROOT / "package-lock.json").read_text())
    for workspace in ("", "cli", "sdk/node"):
        if lock["packages"][workspace]["version"] != component_version:
            raise AssertionError(f"lockfile workspace version mismatch: {workspace}")
    if lock["packages"]["cli"]["dependencies"]["@pacificdb/client"] != component_version:
        raise AssertionError("lockfile CLI dependency must match the patch")
    require_text("README.md", f"PacificDB Community v{engine_version}")
    require_text("cli/src/shell.js", "releaseIdentity")
    require_text("engine/src/shell.cpp", '"               v" PACIFICDB_ENGINE_VERSION')

    print("RELEASE_CONSISTENCY_PASS")


if __name__ == "__main__":
    main()
