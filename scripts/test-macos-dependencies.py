#!/usr/bin/env python3
"""Exercise macOS dependency setup without downloading or building dependencies."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

COMMAND = """#!/bin/bash
set -euo pipefail
case "${0##*/}" in
  brew)
    if [[ "$*" == *vcpkg* && "$EXPECTED_TRIPLET" == x64-osx ]]; then
      echo 'Homebrew vcpkg conflicts with the runner installation' >&2
      exit 1
    fi
    ;;
  vcpkg)
    if [[ "$0" != "$VCPKG_INSTALLATION_ROOT/vcpkg" ]]; then
      echo 'Could not detect vcpkg-root' >&2
      exit 1
    fi
    [[ "$1" == install && "$2" == --triplet && "$3" == "$EXPECTED_TRIPLET" ]]
    [[ "$4" == "--x-install-root=$PWD/build/vcpkg_installed" ]]
    mkdir -p build/vcpkg_installed
    ;;
  cmake)
    found_toolchain=false
    found_triplet=false
    found_install=false
    for argument in "$@"; do
      case "$argument" in
        -DCMAKE_TOOLCHAIN_FILE=*)
          [[ "${argument#*=}" == "$VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" ]]
          test -f "${argument#*=}"
          found_toolchain=true
          ;;
        -DVCPKG_TARGET_TRIPLET=*)
          [[ "${argument#*=}" == "$EXPECTED_TRIPLET" ]]
          found_triplet=true
          ;;
        -DVCPKG_INSTALLED_DIR=*)
          [[ "${argument#*=}" == "$PWD/build/vcpkg_installed" ]]
          test -d "${argument#*=}"
          found_install=true
          ;;
      esac
    done
    [[ "$found_toolchain" == true && "$found_triplet" == true && "$found_install" == true ]]
    touch configured
    ;;
esac
"""

failures = []
with tempfile.TemporaryDirectory(prefix="macos-dependencies-") as temporary:
    root = Path(temporary)
    binaries = root / "bin"
    binaries.mkdir()
    installation = root / "runner-vcpkg"
    toolchain = installation / "scripts/buildsystems/vcpkg.cmake"
    toolchain.parent.mkdir(parents=True)
    toolchain.touch()
    for command in (binaries / "brew", binaries / "vcpkg", binaries / "cmake", installation / "vcpkg"):
        command.write_text(COMMAND, encoding="utf-8")
        command.chmod(0o755)
    for workflow, architectures in (
        ("workbench-desktop.yml", ("arm64", "x64")),
        ("release.yml", ("arm64", "x86_64")),
    ):
        text = (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8")
        macos = text.split("\n  macos:\n", 1)[1].split("\n  release:", 1)[0]
        setup = re.findall(r"^      - run: (brew .+)$", macos, re.MULTILINE)
        build = macos.split("      - name: Build ", 1)[1]
        block = build.split("        run: |\n", 1)[1]
        block = "\n".join(line[10:] for line in block.splitlines())
        dependencies = block.split("signing_p12=", 1)[0].split("cmake -S", 1)[0]
        configure = "cmake -S" + block.split("cmake -S", 1)[1].split("cmake --build", 1)[0]
        for architecture in architectures:
            case = root / f"{workflow}-{architecture}"
            case.mkdir()
            environment = dict(os.environ, PATH=f"{binaries}:{os.environ['PATH']}",
                VCPKG_INSTALLATION_ROOT=str(installation),
                EXPECTED_TRIPLET="arm64-osx" if architecture == "arm64" else "x64-osx",
                PACIFICDB_COMPONENT_VERSION="1.1.2", version="1.1.2")
            script = "\n".join(setup + [dependencies, configure])
            script = script.replace("${{ matrix.arch }}", architecture)
            result = subprocess.run(["bash", "-euo", "pipefail", "-c", script],
                cwd=case, env=environment, capture_output=True, text=True)
            if result.returncode or not (case / "configured").exists():
                failures.append(f"{workflow} {architecture}: {result.stderr or 'configure was skipped'}")
            else:
                print(f"MACOS_DEPENDENCIES_PASS {workflow} {architecture}")
assert not failures, "\n".join(failures)
