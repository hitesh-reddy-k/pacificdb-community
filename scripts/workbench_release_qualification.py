#!/usr/bin/env python3
"""Qualify Workbench installers against one clean source revision."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Sequence

from scripts.power_loss_harness import validate_evidence as validate_power_evidence
from scripts.release_qualification import (
    collect_artifact,
    collect_repository_state,
    write_json_atomic,
)
from scripts.security_review import validate_review


PLATFORMS = frozenset({"linux-x64", "windows-x64", "macos-arm64", "macos-x64"})


def _gate(name: str, status: str, reasons: list[str] | None = None) -> dict:
    return {"name": name, "status": status, "reasons": reasons or []}


def _missing_or_nonpositive(value) -> bool:
    return not isinstance(value, int) or isinstance(value, bool) or value <= 0


def validate_load_evidence(evidence: dict | None, revision: str) -> dict:
    if not isinstance(evidence, dict):
        return {"status": "BLOCKED", "reasons": ["load_evidence_missing"]}
    failed: list[str] = []
    blocked: list[str] = []
    if evidence.get("revision") != revision:
        failed.append("load_revision_mismatch")
    if evidence.get("status") != "PASS":
        failed.append("load_status_not_pass")
    if not isinstance(evidence.get("duration_seconds"), int) or evidence["duration_seconds"] < 28_800:
        blocked.append("load_duration_below_28800_seconds")
    if not isinstance(evidence.get("records"), int) or evidence["records"] < 500_000:
        blocked.append("load_records_below_500000")
    if _missing_or_nonpositive(evidence.get("operations")):
        blocked.append("load_operations_missing")
    if evidence.get("errors") != 0:
        failed.append("load_errors_nonzero")
    if _missing_or_nonpositive(evidence.get("peak_resident_bytes")):
        blocked.append("load_peak_resident_bytes_missing")
    if failed:
        return {"status": "FAIL", "reasons": failed + blocked}
    if blocked:
        return {"status": "BLOCKED", "reasons": blocked}
    return {"status": "PASS", "reasons": []}


def validate_audit_report(report: dict | None) -> dict:
    if not isinstance(report, dict):
        return {"status": "BLOCKED", "reasons": ["runtime_audit_missing"]}
    vulnerabilities = report.get("metadata", {}).get("vulnerabilities", {})
    total = vulnerabilities.get("total")
    if not isinstance(total, int):
        return {"status": "BLOCKED", "reasons": ["runtime_audit_total_missing"]}
    if total:
        return {"status": "FAIL", "reasons": [f"runtime_advisories:{total}"]}
    return {"status": "PASS", "reasons": []}


def build_platform_result(repo: Path | str, name: str, version: str,
                          artifact: Path | str) -> dict:
    if name not in PLATFORMS:
        raise ValueError(f"unsupported Workbench platform: {name}")
    state = collect_repository_state(repo)
    return {
        "schema_version": 1,
        "name": name,
        "version": version,
        "revision": state["revision"],
        "status": "BLOCKED" if state["dirty"] else "PASS",
        "reasons": ["dirty_worktree"] if state["dirty"] else [],
        "artifact": collect_artifact(repo, artifact),
    }


def _validate_platforms(results: Sequence[dict], revision: str, version: str,
                        artifacts: Sequence[dict]) -> list[dict]:
    by_name: dict[str, dict] = {}
    duplicate: set[str] = set()
    for result in results:
        name = result.get("name")
        if name in by_name:
            duplicate.add(str(name))
        by_name[str(name)] = result
    artifact_by_path = {item["path"]: item for item in artifacts}
    gates = []
    for name in sorted(PLATFORMS):
        result = by_name.get(name)
        if result is None:
            gates.append(_gate(f"platform:{name}", "BLOCKED", ["platform_result_missing"]))
            continue
        reasons: list[str] = []
        status = result.get("status") if result.get("status") in {"PASS", "FAIL", "BLOCKED"} else "FAIL"
        if name in duplicate:
            status = "FAIL"
            reasons.append("duplicate_platform_result")
        if result.get("revision") != revision:
            status = "FAIL"
            reasons.append("platform_revision_mismatch")
        if result.get("version") != version:
            status = "FAIL"
            reasons.append("platform_version_mismatch")
        supplied = result.get("artifact") if isinstance(result.get("artifact"), dict) else {}
        actual = artifact_by_path.get(supplied.get("path"))
        if actual is None:
            status = "FAIL"
            reasons.append("platform_artifact_missing")
        elif supplied.get("sha256") != actual.get("sha256"):
            status = "FAIL"
            reasons.append("platform_artifact_digest_mismatch")
        gates.append(_gate(f"platform:{name}", status, reasons + result.get("reasons", [])))
    return gates


def aggregate_evidence(*, repo: Path | str, version: str,
                       artifacts: Sequence[Path | str],
                       platform_results: Sequence[dict],
                       physical_evidence: dict | None,
                       security_bundle: Path | str | None,
                       security_review: dict | None,
                       load_evidence: dict | None,
                       audit_report: dict | None) -> dict:
    root = Path(repo).resolve()
    state = collect_repository_state(root)
    artifact_records = [collect_artifact(root, artifact) for artifact in artifacts]
    gates = [_gate("source", "BLOCKED" if state["dirty"] else "PASS",
                   ["dirty_worktree"] if state["dirty"] else [])]
    gates.extend(_validate_platforms(platform_results, state["revision"], version,
                                     artifact_records))

    power = validate_power_evidence(physical_evidence) if isinstance(physical_evidence, dict) else {
        "status": "BLOCKED", "reasons": ["physical_power_evidence_missing"]}
    if isinstance(physical_evidence, dict) and physical_evidence.get("revision") != state["revision"]:
        power = {"status": "FAIL", "reasons": ["physical_power_revision_mismatch"]}
    gates.append(_gate("physical_power", power["status"], power.get("reasons")))

    if security_bundle is None:
        security = {"status": "BLOCKED", "reasons": ["security_bundle_missing"]}
    else:
        security = validate_review(security_bundle, security_review)
        manifest_path = Path(security_bundle) / "bundle-manifest.json"
        if manifest_path.is_file():
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            if manifest.get("revision") != state["revision"]:
                security = {"status": "FAIL", "reasons": ["security_bundle_revision_mismatch"]}
    gates.append(_gate("security_review", security["status"], security.get("reasons")))

    load = validate_load_evidence(load_evidence, state["revision"])
    gates.append(_gate("long_duration_load", load["status"], load.get("reasons")))
    audit = validate_audit_report(audit_report)
    gates.append(_gate("runtime_dependencies", audit["status"], audit.get("reasons")))

    statuses = {gate["status"] for gate in gates}
    decision = "FAIL" if "FAIL" in statuses else "BLOCKED" if "BLOCKED" in statuses else "PASS"
    return {"schema_version": 1, "version": version, **state,
            "artifacts": artifact_records, "gates": gates, "decision": decision}


def _json(path: Path | None) -> dict | None:
    return json.loads(path.read_text(encoding="utf-8")) if path else None


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="action", required=True)
    platform = commands.add_parser("platform")
    platform.add_argument("--name", choices=sorted(PLATFORMS), required=True)
    platform.add_argument("--version", required=True)
    platform.add_argument("--artifact", type=Path, required=True)
    platform.add_argument("--output", type=Path, required=True)
    aggregate = commands.add_parser("aggregate")
    aggregate.add_argument("--version", required=True)
    aggregate.add_argument("--artifact", type=Path, action="append", default=[])
    aggregate.add_argument("--platform-result", type=Path, action="append", default=[])
    aggregate.add_argument("--physical-power-evidence", type=Path)
    aggregate.add_argument("--security-bundle", type=Path)
    aggregate.add_argument("--security-review", type=Path)
    aggregate.add_argument("--load-evidence", type=Path)
    aggregate.add_argument("--audit-report", type=Path)
    aggregate.add_argument("--output", type=Path, required=True)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    repo = Path(__file__).resolve().parent.parent
    try:
        if args.action == "platform":
            evidence = build_platform_result(repo, args.name, args.version, args.artifact)
        else:
            evidence = aggregate_evidence(repo=repo, version=args.version,
                artifacts=args.artifact,
                platform_results=[_json(path) for path in args.platform_result],
                physical_evidence=_json(args.physical_power_evidence),
                security_bundle=args.security_bundle,
                security_review=_json(args.security_review),
                load_evidence=_json(args.load_evidence),
                audit_report=_json(args.audit_report))
        write_json_atomic(args.output, evidence)
        outcome = evidence["decision"] if "decision" in evidence else evidence["status"]
        print(f"{outcome} evidence={args.output}")
        return 0 if outcome == "PASS" else 1
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"BLOCKED {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
