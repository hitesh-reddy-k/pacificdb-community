#!/usr/bin/env python3
"""Static contract for installed Workbench upgrade and release qualification."""

from pathlib import Path


WORKFLOW = (Path(__file__).resolve().parents[1] /
            ".github/workflows/workbench-desktop.yml").read_text(encoding="utf-8")


def section(name: str, next_name: str | None) -> str:
    start = WORKFLOW.index(f"  {name}:\n")
    end = WORKFLOW.index(f"  {next_name}:\n", start) if next_name else len(WORKFLOW)
    return WORKFLOW[start:end]


def ordered(text: str, *needles: str) -> None:
    position = -1
    for needle in needles:
        found = text.find(needle, position + 1)
        if found < 0:
            raise AssertionError(f"missing or out-of-order workflow contract: {needle!r}")
        position = found


linux = section("linux", "windows")
if "if: inputs.release_tag == ''" not in linux:
    raise AssertionError("dispatch retries must reuse the original Linux artifact")
ordered(linux,
    "Package Workbench for Ubuntu 24.04",
    "Download and install previous Workbench Debian",
    "Remove previous Workbench Debian",
    "Install and test candidate Workbench Debian",
    "PACIFICDB_TEST_DESKTOP_PREVIOUS",
    "PACIFICDB_TEST_DESKTOP_DATA",
    "Remove candidate Workbench Debian and verify data preservation",
    "workbench_release_qualification.py platform",
    "name: workbench-linux-x64")

windows = section("windows", "macos")
if "if: inputs.release_tag == ''" not in windows:
    raise AssertionError("dispatch retries must reuse the original Windows artifact")
ordered(windows,
    "Package Workbench for Windows x64",
    "Download and install previous Workbench NSIS",
    "Uninstall previous Workbench NSIS",
    "Install and test candidate Workbench NSIS",
    "PACIFICDB_TEST_DESKTOP_PREVIOUS",
    "PACIFICDB_TEST_DESKTOP_DATA",
    "Uninstall candidate Workbench NSIS and verify data preservation",
    "workbench_release_qualification.py platform",
    "name: workbench-windows-x64")

macos = section("macos", "release")
if "if: inputs.release_tag == ''" not in macos:
    raise AssertionError("dispatch retries must reuse the original macOS artifacts")
ordered(macos,
    "Package Workbench for macOS",
    "Mount and copy previous Workbench DMG",
    "Remove previous Workbench application",
    "Mount and test candidate Workbench DMG",
    "PACIFICDB_TEST_DESKTOP_PREVIOUS",
    "PACIFICDB_TEST_DESKTOP_DATA",
    "Remove candidate Workbench application and verify data preservation",
    "workbench_release_qualification.py platform",
    "name: workbench-macos-${{ matrix.arch }}")

for unpacked in ("linux-unpacked", "win-unpacked"):
    if unpacked in WORKFLOW:
        raise AssertionError(f"release workflow must test installers, not {unpacked}")
if WORKFLOW.count("workbench-data-") < 6 or WORKFLOW.count("/database") < 2 or "\\database" not in WORKFLOW:
    raise AssertionError("each uninstaller must leave the populated database directory")

release = section("release", None)
ordered(release,
    "Download installers from tag build",
    "Download immutable tag-build installers for retry",
    "gh run download",
    "Decode external qualification evidence",
    "npm audit --omit=dev --json",
    "workbench_release_qualification.py aggregate",
    "Upload Workbench qualification report",
    'gh release create "$WORKBENCH_RELEASE_TAG"')
for secret in ("WORKBENCH_PHYSICAL_POWER_EVIDENCE_BASE64",
               "WORKBENCH_SECURITY_REVIEW_EVIDENCE_BASE64",
               "WORKBENCH_LOAD_EVIDENCE_BASE64"):
    if secret not in release:
        raise AssertionError(f"release workflow must consume {secret}")
if "if: always()" not in release:
    raise AssertionError("qualification report must upload when aggregation blocks")

print("WORKBENCH_INSTALLER_CONTRACT_PASS")
