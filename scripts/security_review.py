#!/usr/bin/env python3
"""Build and validate exact-revision independent security-review evidence."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tomllib
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Sequence


SECRET_PATTERNS = (
    ("private_key_header", re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")),
    ("aws_access_key", re.compile(r"AKIA[0-9A-Z]{16}")),
    ("github_token", re.compile(r"gh[pousr]_[A-Za-z0-9]{20,}")),
)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_record(root: Path, path: Path) -> dict:
    resolved = path.resolve()
    relative = resolved.relative_to(root.resolve()).as_posix()
    return {
        "path": relative,
        "sha256": sha256_file(resolved),
        "size_bytes": resolved.stat().st_size,
    }


def scan_text_for_secrets(path: str, text: str) -> list[dict]:
    findings = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        for name, pattern in SECRET_PATTERNS:
            if pattern.search(line):
                findings.append({"path": path, "line": line_number, "pattern": name})
    return findings


def scan_archive_for_secrets(archive: Path) -> list[dict]:
    findings: list[dict] = []
    with tarfile.open(archive, "r") as source:
        for member in source.getmembers():
            if not member.isfile() or member.size > 2 * 1024 * 1024:
                continue
            stream = source.extractfile(member)
            if stream is None:
                continue
            text = stream.read().decode("utf-8", errors="ignore")
            findings.extend(scan_text_for_secrets(member.name, text))
    return findings


def _component(ecosystem: str, name: str, version: str = "unknown") -> dict:
    return {
        "type": "library",
        "name": name,
        "version": version or "unknown",
        "properties": [{"name": "pacificdb:ecosystem", "value": ecosystem}],
    }


def inventory_components(repo: Path) -> list[dict]:
    components: dict[tuple[str, str, str], dict] = {}

    vcpkg = repo / "vcpkg.json"
    if vcpkg.is_file():
        document = json.loads(vcpkg.read_text(encoding="utf-8"))
        for dependency in document.get("dependencies", []):
            name = dependency if isinstance(dependency, str) else dependency.get("name", "")
            version = "unknown" if isinstance(dependency, str) else dependency.get("version>=", "unknown")
            if name:
                components[("vcpkg", name, version)] = _component("vcpkg", name, version)

    lockfile = repo / "package-lock.json"
    if lockfile.is_file():
        document = json.loads(lockfile.read_text(encoding="utf-8"))
        for package_path, package in document.get("packages", {}).items():
            if not package_path.startswith("node_modules/"):
                continue
            name = package.get("name") or package_path.removeprefix("node_modules/")
            version = package.get("version", "unknown")
            components[("npm", name, version)] = _component("npm", name, version)

    pom = repo / "sdk/java/pom.xml"
    if pom.is_file():
        root = ET.parse(pom).getroot()
        namespace = {"m": "http://maven.apache.org/POM/4.0.0"}
        properties = {
            child.tag.split("}")[-1]: (child.text or "").strip()
            for child in root.findall("m:properties/*", namespace)
        }
        for dependency in root.findall("m:dependencies/m:dependency", namespace):
            group = dependency.findtext("m:groupId", default="", namespaces=namespace)
            artifact = dependency.findtext("m:artifactId", default="", namespaces=namespace)
            version = dependency.findtext("m:version", default="unknown", namespaces=namespace)
            property_match = re.fullmatch(r"\$\{([^}]+)\}", version)
            if property_match:
                version = properties.get(property_match.group(1), version)
            name = f"{group}:{artifact}" if group else artifact
            if name:
                components[("maven", name, version)] = _component("maven", name, version)

    pyproject = repo / "sdk/python/pyproject.toml"
    if pyproject.is_file():
        document = tomllib.loads(pyproject.read_text(encoding="utf-8"))
        for dependency in document.get("project", {}).get("dependencies", []):
            name = re.split(r"[<>=!~ ;\[]", dependency, maxsplit=1)[0]
            components[("pypi", name, dependency)] = _component("pypi", name, dependency)
        for requirement in document.get("build-system", {}).get("requires", []):
            name = re.split(r"[<>=!~ ;\[]", requirement, maxsplit=1)[0]
            components[("pypi-build", name, requirement)] = _component(
                "pypi-build", name, requirement
            )

    workflow_dir = repo / ".github/workflows"
    if workflow_dir.is_dir():
        for workflow in sorted(workflow_dir.glob("*.yml")):
            for match in re.finditer(
                r"^\s*-?\s*uses:\s*([^@\s]+)@([^\s]+)",
                workflow.read_text(encoding="utf-8"),
                re.MULTILINE,
            ):
                name, version = match.groups()
                components[("github-actions", name, version)] = _component(
                    "github-actions", name, version
                )

    return [components[key] for key in sorted(components)]


def dependency_inputs(repo: Path) -> list[dict]:
    candidates = [
        repo / "vcpkg.json",
        repo / "package-lock.json",
        repo / "sdk/java/pom.xml",
        repo / "sdk/python/pyproject.toml",
        repo / "engine/CMakeLists.txt",
    ]
    candidates.extend(sorted((repo / ".github/workflows").glob("*.yml")))
    return [file_record(repo, path) for path in candidates if path.is_file()]


def build_bundle(repo: Path, output: Path, revision: str) -> dict:
    repo = repo.resolve()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    resolved_revision = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", f"{revision}^{{commit}}"],
        check=True,
        capture_output=True,
        text=True,
        shell=False,
    ).stdout.strip()

    source_archive = output / "source.tar"
    subprocess.run(
        [
            "git",
            "-C",
            str(repo),
            "archive",
            "--format=tar",
            f"--output={source_archive}",
            resolved_revision,
        ],
        check=True,
        shell=False,
    )

    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": utc_now(),
            "component": {
                "type": "application",
                "name": "pacificdb-community",
                "version": resolved_revision,
            },
        },
        "components": inventory_components(repo),
    }
    sbom_path = output / "sbom.cdx.json"
    sbom_path.write_text(json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    artifacts = [file_record(output, source_archive), file_record(output, sbom_path)]
    dist = repo / "dist"
    if dist.is_dir():
        artifact_root = output / "artifacts"
        artifact_root.mkdir(exist_ok=True)
        for source in sorted(path for path in dist.rglob("*") if path.is_file()):
            relative = source.relative_to(dist)
            destination = artifact_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            artifacts.append(file_record(output, destination))

    secret_findings = scan_archive_for_secrets(source_archive)
    manifest = {
        "schema_version": 1,
        "revision": resolved_revision,
        "created_at": utc_now(),
        "source_archive": file_record(output, source_archive),
        "sbom": file_record(output, sbom_path),
        "inputs": dependency_inputs(repo),
        "artifacts": artifacts,
        "secret_scan": {
            "status": "PASS" if not secret_findings else "FAIL",
            "findings": secret_findings,
            "redacted": True,
        },
    }
    manifest_path = output / "bundle-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def _contained_file(bundle: Path, relative: str) -> Path | None:
    try:
        candidate = (bundle / relative).resolve()
        candidate.relative_to(bundle.resolve())
    except (ValueError, OSError):
        return None
    return candidate if candidate.is_file() else None


def validate_review(bundle: Path | str, review: dict | None) -> dict:
    bundle = Path(bundle).resolve()
    manifest_path = bundle / "bundle-manifest.json"
    if not manifest_path.is_file():
        return {"status": "BLOCKED", "reasons": ["bundle_manifest_missing"]}
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if review is None:
        return {"status": "BLOCKED", "reasons": ["external_review_result_missing"]}

    blocked: list[str] = []
    failed: list[str] = []
    reviewer = review.get("reviewer") if isinstance(review.get("reviewer"), dict) else {}
    if not reviewer.get("name"):
        blocked.append("missing_reviewer_name")
    if not reviewer.get("organization"):
        blocked.append("missing_reviewer_organization")
    if reviewer.get("independent") is not True:
        blocked.append("reviewer_not_independent")
    if not review.get("review_date"):
        blocked.append("missing_review_date")
    if review.get("reviewed_revision") != manifest.get("revision"):
        failed.append("reviewed_revision_mismatch")
    if manifest.get("secret_scan", {}).get("status") == "FAIL":
        failed.append("bundle_secret_scan_failed")

    report = review.get("report") if isinstance(review.get("report"), dict) else {}
    report_path = _contained_file(bundle, report.get("path", ""))
    if report_path is None:
        blocked.append("review_report_missing")
    elif report.get("sha256") != sha256_file(report_path):
        failed.append("report_digest_mismatch")

    supplied_artifacts = review.get("artifact_digests")
    if not isinstance(supplied_artifacts, dict):
        supplied_artifacts = {}
    for artifact in manifest.get("artifacts", []):
        path = artifact.get("path", "")
        if path not in supplied_artifacts:
            blocked.append(f"missing_artifact_digest:{path}")
            continue
        actual_path = _contained_file(bundle, path)
        if actual_path is None:
            failed.append(f"bundle_artifact_missing:{path}")
            continue
        actual_digest = sha256_file(actual_path)
        if artifact.get("sha256") != actual_digest or supplied_artifacts[path] != actual_digest:
            failed.append(f"artifact_digest_mismatch:{path}")

    findings = review.get("findings")
    if not isinstance(findings, list):
        blocked.append("findings_inventory_missing")
        findings = []
    for finding in findings:
        severity = str(finding.get("severity", "")).lower()
        if severity not in {"critical", "high"}:
            continue
        disposition = finding.get("disposition")
        if disposition not in {"resolved", "accepted_risk"}:
            blocked.append(f"unresolved_finding:{finding.get('id', 'unknown')}")
        if disposition == "accepted_risk" and (
            not finding.get("owner") or not finding.get("rationale")
        ):
            blocked.append(f"incomplete_accepted_risk:{finding.get('id', 'unknown')}")

    reasons = failed + blocked
    if failed:
        return {"status": "FAIL", "reasons": reasons}
    if blocked:
        return {"status": "BLOCKED", "reasons": reasons}
    return {"status": "PASS", "reasons": []}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="action", required=True)
    bundle = commands.add_parser("bundle")
    bundle.add_argument("--output", type=Path, required=True)
    bundle.add_argument("--revision", required=True)
    validate = commands.add_parser("validate")
    validate.add_argument("--bundle", type=Path, required=True)
    validate.add_argument("--review-result", type=Path)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    repo = Path(__file__).resolve().parent.parent
    try:
        if args.action == "bundle":
            manifest = build_bundle(repo, args.output, args.revision)
            print(f"BUNDLE_PASS revision={manifest['revision']} output={args.output}")
            return 0
        review = None
        if args.review_result:
            review = json.loads(args.review_result.read_text(encoding="utf-8"))
        result = validate_review(args.bundle, review)
        print(json.dumps(result, sort_keys=True))
        return 0 if result["status"] == "PASS" else 1
    except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"BLOCKED {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
