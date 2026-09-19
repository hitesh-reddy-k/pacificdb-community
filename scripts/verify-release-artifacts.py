#!/usr/bin/env python3
import argparse
import hashlib
import json
import re
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
        "p0-evidence-linux-amd64-container.json",
    ]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_container_evidence(dist: Path, version: str, revision: str | None) -> dict:
    path = dist / "p0-evidence-linux-amd64-container.json"
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid container evidence: {error}") from error
    digest = evidence.get("registry_digest", "")
    expected_image = f"ghcr.io/hitesh-reddy-k/pacificdb-community@{digest}"
    valid = (
        evidence.get("status") == "PASS"
        and evidence.get("version") == version
        and re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is not None
        and evidence.get("image") == expected_image
        and re.fullmatch(r"[0-9a-f]{40}", evidence.get("revision", "")) is not None
        and (revision is None or evidence.get("revision") == revision)
    )
    if not valid:
        raise ValueError("container evidence does not match the release identity")
    return evidence


def load_evidence(path: Path, label: str) -> dict:
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid {label} evidence: {error}") from error
    if not isinstance(evidence, dict):
        raise ValueError(f"invalid {label} evidence: expected an object")
    return evidence


def native_identity_matches(evidence: dict, version: str, revision: str | None) -> bool:
    recorded_revision = evidence.get("revision", "")
    return (
        evidence.get("status") == "PASS"
        and evidence.get("version") == f"PacificDB {version}"
        and re.fullmatch(r"[0-9a-f]{40}", recorded_revision) is not None
        and (revision is None or recorded_revision == revision)
    )


def validate_native_signing_evidence(
    dist: Path, version: str, revision: str | None
) -> dict:
    windows = load_evidence(
        dist / "p0-evidence-windows-x64.json", "Windows signing"
    )
    windows_signing = windows.get("windows_signing")
    windows_fields = (
        "status",
        "installer",
        "installed_pacificdb",
        "installed_db_engine",
    )
    if (
        not native_identity_matches(windows, version, revision)
        or not isinstance(windows_signing, dict)
        or any(windows_signing.get(field) != "PASS" for field in windows_fields)
    ):
        raise ValueError("Windows signing evidence does not match the release identity")

    summaries = {
        "windows": {field: windows_signing[field] for field in windows_fields}
    }
    for architecture in ("arm64", "x86_64"):
        macos = load_evidence(
            dist / f"p0-evidence-macos-{architecture}.json", "macOS signing"
        )
        macos_signing = macos.get("macos_signing")
        macos_fields = (
            "status",
            "signature",
            "notarization",
            "stapling",
            "gatekeeper",
        )
        if (
            not native_identity_matches(macos, version, revision)
            or macos.get("release_architecture") != architecture
            or not isinstance(macos_signing, dict)
            or any(macos_signing.get(field) != "PASS" for field in macos_fields)
        ):
            raise ValueError("macOS signing evidence does not match the release identity")
        summaries[f"macos-{architecture}"] = {
            field: macos_signing[field] for field in macos_fields
        }
    return summaries


def build_manifest(dist: Path, version: str, revision: str | None = None) -> dict:
    artifacts = []
    for name in sorted(required_names(version)):
        path = dist / name
        if not path.is_file():
            raise ValueError(f"missing required release artifact: {name}")
        size = path.stat().st_size
        if size == 0:
            raise ValueError(f"empty release artifact: {name}")
        artifacts.append({"name": name, "size": size, "sha256": sha256(path)})
    container = validate_container_evidence(dist, version, revision)
    native_signing = validate_native_signing_evidence(
        dist, version, container["revision"]
    )
    return {
        "version": version,
        "revision": container["revision"],
        "oci_image": container["image"],
        "native_signing": native_signing,
        "artifacts": artifacts,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = build_manifest(args.dist, args.version, args.revision)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
