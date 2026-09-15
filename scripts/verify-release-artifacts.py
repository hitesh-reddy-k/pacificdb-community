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
