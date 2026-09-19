#!/usr/bin/env python3
"""Validate PacificDB operations claims, alert coverage, and runbook links."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Iterable, Sequence


CONTRACT_START = "<!-- operations-contract:start -->"
CONTRACT_END = "<!-- operations-contract:end -->"
CLASSIFICATIONS = frozenset({"guarantee", "observed", "operator_target", "blocked"})
REQUIRED_ALERTS = frozenset(
    {
        "PacificDBWalLatencyHigh",
        "PacificDBWalErrors",
        "PacificDBWriteStalls",
        "PacificDBFlushCompactionBacklog",
        "PacificDBDiskPressure",
        "PacificDBMemoryPressure",
        "PacificDBFileDescriptorPressure",
        "PacificDBConnectionSaturation",
        "PacificDBRaftLag",
        "PacificDBRaftQuorumUnavailable",
        "PacificDBIntegrityCheckStale",
        "PacificDBIntegrityCheckFailed",
        "PacificDBBackupTooOld",
        "PacificDBBackupFailed",
        "PacificDBCertificateExpiringSoon",
    }
)
REQUIRED_RUNBOOKS = frozenset(
    {
        "BACKUP_RESTORE.md",
        "CERTIFICATE_ROTATION.md",
        "LOSS_OF_QUORUM.md",
        "NODE_REPLACEMENT.md",
        "UPGRADE_ROLLBACK.md",
    }
)


def parse_contract(text: str) -> dict:
    start = text.find(CONTRACT_START)
    end = text.find(CONTRACT_END)
    if start < 0 or end < 0 or end <= start:
        raise ValueError("operations contract markers are missing")
    payload = text[start + len(CONTRACT_START):end].strip()
    return json.loads(payload)


def _has_numeric_value(value) -> bool:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return True
    return isinstance(value, str) and re.search(r"\d", value) is not None


def _json_pointer(document, pointer: str):
    if pointer == "":
        return document
    if not pointer.startswith("/"):
        raise ValueError("JSON pointer must start with /")
    current = document
    for raw_part in pointer[1:].split("/"):
        part = raw_part.replace("~1", "/").replace("~0", "~")
        if isinstance(current, list):
            current = current[int(part)]
        else:
            current = current[part]
    return current


def validate_claims(contract: dict, evidence_root: Path) -> list[str]:
    errors: list[str] = []
    claims = contract.get("claims")
    if not isinstance(claims, list):
        return ["claims_missing"]
    seen_ids: set[str] = set()
    seen_classifications: set[str] = set()
    root = evidence_root.resolve()
    for index, claim in enumerate(claims):
        identifier = claim.get("id") or f"claim_{index + 1}"
        if identifier in seen_ids:
            errors.append(f"{identifier}:duplicate_id")
        seen_ids.add(identifier)
        classification = claim.get("classification")
        if classification not in CLASSIFICATIONS:
            errors.append(f"{identifier}:invalid_classification")
            continue
        seen_classifications.add(classification)
        numeric_certification = (
            classification in {"guarantee", "observed"}
            and _has_numeric_value(claim.get("value"))
        )
        if not numeric_certification:
            continue
        evidence_path = claim.get("evidence")
        pointer = claim.get("json_pointer")
        if not evidence_path or pointer is None:
            errors.append(f"{identifier}:numeric_claim_missing_evidence")
            continue
        try:
            path = (root / evidence_path).resolve()
            path.relative_to(root)
        except (ValueError, OSError):
            errors.append(f"{identifier}:evidence_path_outside_root")
            continue
        if not path.is_file():
            errors.append(f"{identifier}:evidence_file_missing")
            continue
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
            observed = _json_pointer(document, pointer)
        except (ValueError, KeyError, IndexError, json.JSONDecodeError):
            errors.append(f"{identifier}:evidence_pointer_invalid")
            continue
        if observed != claim.get("value"):
            errors.append(f"{identifier}:evidence_value_mismatch")

    required = contract.get("require_classifications", [])
    for classification in required:
        if classification not in seen_classifications:
            errors.append(f"classification_missing:{classification}")
    return errors


def _markdown_anchors(text: str) -> set[str]:
    anchors = set()
    for heading in re.findall(r"^#{1,6}\s+(.+?)\s*$", text, re.MULTILINE):
        anchor = heading.strip().lower()
        anchor = re.sub(r"[^a-z0-9 _-]", "", anchor)
        anchor = re.sub(r"[ _]+", "-", anchor)
        anchor = re.sub(r"-+", "-", anchor).strip("-")
        anchors.add(anchor)
    return anchors


def validate_alerts(alert_text: str, docs_root: Path,
                    required_alerts: Iterable[str] = REQUIRED_ALERTS) -> list[str]:
    errors: list[str] = []
    matches = list(re.finditer(r"^\s*-\s+alert:\s*([^\s#]+)", alert_text, re.MULTILINE))
    found: set[str] = set()
    root = docs_root.resolve()
    for index, match in enumerate(matches):
        name = match.group(1)
        found.add(name)
        end = matches[index + 1].start() if index + 1 < len(matches) else len(alert_text)
        block = alert_text[match.start():end]
        link_match = re.search(r"^\s*runbook_url:\s*([^\s#]+)(?:#([^\s]+))?", block, re.MULTILINE)
        if not link_match:
            errors.append(f"{name}:runbook_url_missing")
            continue
        relative, anchor = link_match.groups()
        try:
            runbook = (root / relative).resolve()
            runbook.relative_to(root)
        except (ValueError, OSError):
            errors.append(f"{name}:runbook_path_outside_docs")
            continue
        if not runbook.is_file():
            errors.append(f"{name}:runbook_missing:{relative}")
            continue
        if anchor and anchor not in _markdown_anchors(runbook.read_text(encoding="utf-8")):
            errors.append(f"{name}:runbook_anchor_missing:{anchor}")
    for name in sorted(set(required_alerts) - found):
        errors.append(f"required_alert_missing:{name}")
    return errors


def validate_runbook_inventory(repo: Path) -> list[str]:
    directory = repo / "docs/runbooks"
    errors = []
    for name in sorted(REQUIRED_RUNBOOKS):
        path = directory / name
        if not path.is_file():
            errors.append(f"required_runbook_missing:{name}")
        elif "## Evidence preservation" not in path.read_text(encoding="utf-8"):
            errors.append(f"runbook_evidence_section_missing:{name}")
    return errors


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--docs", type=Path, required=True)
    result.add_argument("--alerts", type=Path, required=True)
    result.add_argument("--evidence-root", type=Path)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    repo = Path(__file__).resolve().parent.parent
    try:
        contract = parse_contract(args.docs.read_text(encoding="utf-8"))
        errors = validate_claims(contract, args.evidence_root or repo)
        errors.extend(validate_alerts(args.alerts.read_text(encoding="utf-8"), repo))
        errors.extend(validate_runbook_inventory(repo))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"OPERATIONS_CONTRACT_FAIL {error}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"OPERATIONS_CONTRACT_FAIL errors={len(errors)}", file=sys.stderr)
        return 1
    print("OPERATIONS_CONTRACT_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
