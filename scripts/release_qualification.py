#!/usr/bin/env python3
"""Create release evidence bound to an exact PacificDB source revision."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable, Sequence


VALID_STATUSES = frozenset({"PASS", "FAIL", "BLOCKED", "EXCLUDED"})


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def _git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        text=True,
        shell=False,
    )
    return result.stdout.strip()


def collect_repository_state(repo: Path | str) -> dict:
    root = Path(repo).resolve()
    revision = _git(root, "rev-parse", "HEAD")
    dirty = bool(_git(root, "status", "--porcelain", "--untracked-files=all"))
    return {
        "revision": revision,
        "dirty": dirty,
        "release_eligible": not dirty,
    }


def collect_artifact(repo: Path | str, artifact: Path | str) -> dict:
    root = Path(repo).resolve()
    candidate = Path(artifact).resolve()
    try:
        relative = candidate.relative_to(root)
    except ValueError as error:
        raise ValueError("artifact must be inside the repository") from error
    if not candidate.is_file():
        raise ValueError(f"artifact is not a regular file: {relative.as_posix()}")

    digest = hashlib.sha256()
    with candidate.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {
        "path": relative.as_posix(),
        "sha256": digest.hexdigest(),
        "size_bytes": candidate.stat().st_size,
    }


def decide(gates: Iterable[dict], *, stable: bool) -> str:
    materialized = list(gates)
    for gate in materialized:
        status = gate.get("status")
        if status not in VALID_STATUSES:
            raise ValueError(f"invalid gate status: {status!r}")

    if any(gate["status"] == "FAIL" for gate in materialized):
        return "FAIL"
    if any(
        gate.get("required", False) and gate["status"] != "PASS"
        for gate in materialized
    ):
        return "BLOCKED"
    if stable and materialized and all(gate["status"] == "PASS" for gate in materialized):
        return "STABLE"
    return "CONTROLLED_CANDIDATE"


def _gate(name: str, *, required: bool, command: Sequence[str] | None = None,
          status: str = "BLOCKED", reason: str = "not_run") -> dict:
    gate = {
        "name": name,
        "required": required,
        "status": status,
        "reason": reason,
        "duration_ms": 0,
    }
    if command is not None:
        gate["command"] = list(command)
    return gate


def default_gates() -> list[dict]:
    return [
        _gate("source", required=True,
              command=["scripts/test-community.sh", "build-release-integrity"]),
        _gate("sdk", required=True, command=["npm", "test", "--prefix", "sdk/node"]),
        _gate("packages", required=True, reason="platform_matrix_required"),
        _gate("container", required=True, reason="docker_required"),
        _gate("helm", required=True, command=["scripts/test-helm-deployment.sh"]),
        _gate("mixed_version_upgrade", required=True,
              command=["node", "scripts/test-community-rf3-upgrade.mjs"]),
        _gate("physical_power", required=True, reason="external_physical_evidence_required"),
        _gate("security_review", required=True,
              reason="independent_external_review_required"),
        _gate("operations", required=True,
              command=["python3", "scripts/validate_operations.py"]),
        _gate("replica_integrity", required=True,
              command=["node", "scripts/test-replica-integrity-monitor.mjs",
                       "build-release-integrity"]),
        _gate(
            "long_duration_load",
            required=False,
            status="EXCLUDED",
            reason="release_owner_excluded_2026-09-16",
        ),
    ]


def run_command(repo: Path, gate: dict) -> dict:
    command = gate.get("command")
    if not command:
        return dict(gate)
    executable = command[0]
    if "/" not in executable and shutil.which(executable) is None:
        result = dict(gate)
        result.update(status="BLOCKED", reason=f"tool_unavailable:{executable}")
        return result
    if "/" in executable and not (repo / executable).is_file():
        result = dict(gate)
        result.update(status="BLOCKED", reason=f"command_unavailable:{executable}")
        return result

    started = time.monotonic()
    completed = subprocess.run(
        command,
        cwd=repo,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        shell=False,
        env=os.environ.copy(),
    )
    duration_ms = round((time.monotonic() - started) * 1000)
    result = dict(gate)
    result.update(
        status="PASS" if completed.returncode == 0 else "FAIL",
        reason="command_passed" if completed.returncode == 0 else "command_failed",
        exit_code=completed.returncode,
        duration_ms=duration_ms,
    )
    return result


def build_evidence(repo: Path, version: str, *, run: bool,
                   artifacts: Sequence[Path]) -> dict:
    started_at = utc_now()
    state = collect_repository_state(repo)
    gates = [run_command(repo, gate) for gate in default_gates()] if run else default_gates()
    artifact_records = [collect_artifact(repo, path) for path in artifacts]
    finished_at = utc_now()
    return {
        "schema_version": 1,
        "version": version,
        **state,
        "started_at": started_at,
        "finished_at": finished_at,
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "gates": gates,
        "artifacts": artifact_records,
        "decision": decide(gates, stable=state["release_eligible"]),
    }


def write_json_atomic(path: Path, value: dict) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def exit_code_for(decision: str, *, allow_dirty_development: bool, dirty: bool) -> int:
    if dirty and not allow_dirty_development:
        return 2
    if dirty and allow_dirty_development:
        return 0
    return 0 if decision in {"STABLE", "CONTROLLED_CANDIDATE"} else 1


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, action="append", default=[])
    parser.add_argument("--run", action="store_true", help="run locally available gates")
    parser.add_argument(
        "--allow-dirty-development",
        action="store_true",
        help="write explicitly non-release evidence for a dirty worktree",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    repo = Path(__file__).resolve().parent.parent
    evidence = build_evidence(repo, args.version, run=args.run, artifacts=args.artifact)
    if evidence["dirty"] and not args.allow_dirty_development:
        evidence["decision"] = "BLOCKED"
        evidence["development_evidence"] = False
        write_json_atomic(args.output, evidence)
        print(f"BLOCKED dirty_worktree evidence={args.output}", file=sys.stderr)
        return 2
    evidence["development_evidence"] = evidence["dirty"]
    write_json_atomic(args.output, evidence)
    print(f"{evidence['decision']} evidence={args.output}")
    return exit_code_for(
        evidence["decision"],
        allow_dirty_development=args.allow_dirty_development,
        dirty=evidence["dirty"],
    )


if __name__ == "__main__":
    raise SystemExit(main())
