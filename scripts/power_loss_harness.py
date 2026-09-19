#!/usr/bin/env python3
"""Prepare and validate operator-driven physical power-loss evidence.

This program never controls host or facility power. It only persists test
metadata, starts an explicitly supplied workload, and validates post-reboot
evidence.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import socket
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Sequence


PHASES = frozenset(
    {
        "wal_sync",
        "sst_creation",
        "manifest_publication",
        "wal_reclamation",
        "compaction",
        "backup",
        "restore",
    }
)
PHYSICAL_CUT_METHODS = frozenset(
    {"managed_pdu", "mains_disconnect", "hardware_power_switch"}
)
FORBIDDEN_EXECUTABLES = frozenset(
    {"shutdown", "reboot", "poweroff", "halt", "init", "ipmitool"}
)
OWNER_FILE = ".pacificdb-power-test-owner"


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def boot_id() -> str:
    path = Path("/proc/sys/kernel/random/boot_id")
    if not path.is_file():
        return "unsupported-platform"
    return path.read_text(encoding="utf-8").strip()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_disposable_root(root: Path | str, repository: Path | str) -> Path:
    supplied = Path(root)
    if not supplied.is_absolute():
        raise ValueError("power-test root must be absolute")
    if supplied.is_symlink():
        raise ValueError("power-test root must not be a symlink")
    resolved = supplied.resolve()
    repo = Path(repository).resolve()
    home = Path.home().resolve()
    if resolved == Path(resolved.anchor):
        raise ValueError("filesystem root is not a disposable power-test root")
    if resolved == home:
        raise ValueError("home directory is not a disposable power-test root")
    if resolved == repo or resolved in repo.parents or repo in resolved.parents:
        raise ValueError("power-test root must be separate from the repository")
    return resolved


def _missing(value) -> bool:
    return value is None or (isinstance(value, str) and not value.strip())


def validate_evidence(evidence: dict) -> dict:
    blocked: list[str] = []
    failed: list[str] = []

    for field in ("run_id", "revision", "dedicated_root", "operator", "phase"):
        if _missing(evidence.get(field)):
            blocked.append(f"missing:{field}")
    if evidence.get("phase") not in PHASES:
        blocked.append("unsupported_phase")
    root = evidence.get("dedicated_root")
    if isinstance(root, str) and not Path(root).is_absolute():
        blocked.append("dedicated_root_not_absolute")

    hardware = evidence.get("hardware")
    if not isinstance(hardware, dict):
        hardware = {}
        blocked.append("missing:hardware")
    for field in (
        "host",
        "filesystem",
        "device",
        "controller",
        "drive_model",
        "firmware",
        "cache_policy",
        "cut_method",
    ):
        if _missing(hardware.get(field)):
            blocked.append(f"missing_hardware:{field}")
    cache_policy = str(hardware.get("cache_policy", "")).strip().lower()
    if cache_policy in {"", "unknown", "unspecified"}:
        blocked.append("unknown_cache_policy")
    if hardware.get("cut_method") not in PHYSICAL_CUT_METHODS:
        blocked.append("non_physical_cut_method")

    required = evidence.get("iterations_required")
    if not isinstance(required, int) or required < 1:
        blocked.append("invalid_iterations_required")
        required = 1
    iterations = evidence.get("iterations")
    if not isinstance(iterations, list):
        iterations = []
        blocked.append("missing:iterations")
    if len(iterations) < required:
        blocked.append(f"insufficient_iterations:{len(iterations)}/{required}")

    top_level_boot = evidence.get("boot_id_before")
    for index, iteration in enumerate(iterations[:required], start=1):
        label = f"iteration_{index}"
        for field in (
            "ledger_sha256",
            "marker_timestamp",
            "power_restored_timestamp",
            "boot_id_after",
            "recovery_result",
            "acknowledged_digest",
            "recovered_digest",
        ):
            if _missing(iteration.get(field)):
                blocked.append(f"{label}:missing_{field}")
        before = iteration.get("boot_id_before", top_level_boot)
        if _missing(before):
            blocked.append(f"{label}:missing_boot_id_before")
        elif iteration.get("boot_id_after") == before:
            blocked.append(f"{label}:boot_id_unchanged")
        recovery = iteration.get("recovery_result")
        if recovery not in {None, "", "PASS"}:
            failed.append(f"{label}:recovery_failed")
        acknowledged = iteration.get("acknowledged_digest")
        recovered = iteration.get("recovered_digest")
        if acknowledged and recovered and acknowledged != recovered:
            failed.append(f"{label}:digest_mismatch")

    reasons = failed + blocked
    if failed:
        return {"status": "FAIL", "reasons": reasons}
    if blocked:
        return {"status": "BLOCKED", "reasons": reasons}
    return {"status": "PASS", "reasons": []}


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_text_synced(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8") as stream:
        stream.write(text)
        stream.flush()
        os.fsync(stream.fileno())


def write_json_synced(path: Path, value: dict) -> None:
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    _write_text_synced(temporary, json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)
    _fsync_directory(path.parent)


def read_evidence(root: Path) -> dict:
    evidence_path = root / "evidence.json"
    if not evidence_path.is_file():
        raise ValueError(f"missing harness evidence: {evidence_path}")
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    owner = (root / OWNER_FILE).read_text(encoding="utf-8").strip()
    if owner != evidence.get("run_id"):
        raise ValueError("power-test ownership marker does not match evidence")
    return evidence


def prepare(args: argparse.Namespace, repository: Path) -> int:
    root = validate_disposable_root(args.root, repository)
    if root.exists() and any(root.iterdir()):
        raise ValueError("power-test root must be empty before prepare")
    root.mkdir(parents=True, exist_ok=True)
    run_id = f"power-{uuid.uuid4()}"
    _write_text_synced(root / OWNER_FILE, run_id + "\n")
    ledger = root / "acknowledged-ids-1.jsonl"
    _write_text_synced(ledger, "")
    evidence = {
        "schema_version": 1,
        "run_id": run_id,
        "revision": args.revision,
        "dedicated_root": str(root),
        "hardware": {
            "host": args.host or socket.gethostname(),
            "filesystem": args.filesystem,
            "device": args.device,
            "controller": args.controller,
            "drive_model": args.drive_model,
            "firmware": args.firmware,
            "cache_policy": args.cache_policy,
            "cut_method": args.cut_method,
        },
        "operator": args.operator,
        "phase": args.phase,
        "iterations_required": args.iterations,
        "boot_id_before": boot_id(),
        "created_at": utc_now(),
        "state": "PREPARED",
        "status": "BLOCKED",
        "iterations": [],
    }
    write_json_synced(root / "evidence.json", evidence)
    print(f"PREPARED run_id={run_id} root={root}")
    return 0


def arm(args: argparse.Namespace, repository: Path) -> int:
    root = validate_disposable_root(args.root, repository)
    evidence = read_evidence(root)
    if evidence.get("state") not in {"PREPARED", "VERIFIED_PARTIAL"}:
        raise ValueError(f"cannot arm from state {evidence.get('state')!r}")
    iteration_number = len(evidence.get("iterations", [])) + 1
    if iteration_number > evidence["iterations_required"]:
        raise ValueError("all configured power-test iterations are already recorded")
    if not args.command:
        raise ValueError("arm requires a workload command after --command")
    executable = Path(args.command[0]).name.lower()
    if executable in FORBIDDEN_EXECUTABLES:
        raise ValueError("power-control commands are forbidden in this harness")

    ledger = root / f"acknowledged-ids-{iteration_number}.jsonl"
    if not ledger.exists():
        _write_text_synced(ledger, "")
    workload_log = root / f"workload-{iteration_number}.log"
    environment = os.environ.copy()
    environment.update(
        PACIFICDB_POWER_RUN_ID=evidence["run_id"],
        PACIFICDB_POWER_PHASE=evidence["phase"],
        PACIFICDB_POWER_ITERATION=str(iteration_number),
        PACIFICDB_POWER_ACK_LEDGER=str(ledger),
    )
    log_stream = workload_log.open("ab", buffering=0)
    try:
        child = subprocess.Popen(
            args.command,
            cwd=root,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=log_stream,
            stderr=subprocess.STDOUT,
            shell=False,
            start_new_session=True,
        )
    finally:
        log_stream.close()

    marker_timestamp = utc_now()
    iteration = {
        "number": iteration_number,
        "boot_id_before": boot_id(),
        "marker_timestamp": marker_timestamp,
        "workload_pid": child.pid,
        "ledger_path": ledger.name,
    }
    evidence["iterations"].append(iteration)
    evidence["state"] = "ARMED"
    evidence["status"] = "BLOCKED"
    marker = {
        "run_id": evidence["run_id"],
        "phase": evidence["phase"],
        "iteration": iteration_number,
        "marker_timestamp": marker_timestamp,
    }
    write_json_synced(root / f"arm-marker-{iteration_number}.json", marker)
    write_json_synced(root / "evidence.json", evidence)
    print(
        f"POWER_CUT_NOW run_id={evidence['run_id']} phase={evidence['phase']} "
        f"iteration={iteration_number}",
        flush=True,
    )
    return 0


def verify(args: argparse.Namespace, repository: Path) -> int:
    root = validate_disposable_root(args.root, repository)
    evidence = read_evidence(root)
    if evidence.get("state") != "ARMED" or not evidence.get("iterations"):
        raise ValueError("verify requires an armed iteration")
    iteration = evidence["iterations"][-1]
    current_boot = boot_id()
    if current_boot == iteration.get("boot_id_before"):
        raise ValueError("boot ID has not changed; physical reboot is not proven")
    recovery_path = Path(args.recovery_result).resolve()
    recovery = json.loads(recovery_path.read_text(encoding="utf-8"))
    ledger = root / iteration["ledger_path"]
    iteration.update(
        ledger_sha256=sha256_file(ledger),
        power_restored_timestamp=recovery.get("power_restored_timestamp", utc_now()),
        boot_id_after=current_boot,
        recovery_result=recovery.get("recovery_result"),
        acknowledged_digest=recovery.get("acknowledged_digest"),
        recovered_digest=recovery.get("recovered_digest"),
    )
    outcome = validate_evidence(evidence)
    evidence.update(outcome)
    evidence["state"] = (
        "VERIFIED"
        if len(evidence["iterations"]) >= evidence["iterations_required"]
        else "VERIFIED_PARTIAL"
    )
    write_json_synced(root / "evidence.json", evidence)
    print(f"{outcome['status']} run_id={evidence['run_id']} reasons={','.join(outcome['reasons'])}")
    return 0 if outcome["status"] == "PASS" else 1


def validate_command(args: argparse.Namespace) -> int:
    evidence = json.loads(Path(args.evidence).read_text(encoding="utf-8"))
    outcome = validate_evidence(evidence)
    print(json.dumps(outcome, sort_keys=True))
    return 0 if outcome["status"] == "PASS" else 1


def parser() -> argparse.ArgumentParser:
    root_parser = argparse.ArgumentParser(description=__doc__)
    commands = root_parser.add_subparsers(dest="action", required=True)

    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--root", type=Path, required=True)
    prepare_parser.add_argument("--phase", choices=sorted(PHASES), required=True)
    prepare_parser.add_argument("--revision", required=True)
    prepare_parser.add_argument("--iterations", type=int, required=True)
    prepare_parser.add_argument("--operator", default="")
    prepare_parser.add_argument("--host", default="")
    prepare_parser.add_argument("--filesystem", default="")
    prepare_parser.add_argument("--device", default="")
    prepare_parser.add_argument("--controller", default="")
    prepare_parser.add_argument("--drive-model", default="")
    prepare_parser.add_argument("--firmware", default="")
    prepare_parser.add_argument("--cache-policy", default="unknown")
    prepare_parser.add_argument("--cut-method", default="")

    arm_parser = commands.add_parser("arm")
    arm_parser.add_argument("--root", type=Path, required=True)
    arm_parser.add_argument("--command", nargs=argparse.REMAINDER, required=True)

    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--root", type=Path, required=True)
    verify_parser.add_argument("--recovery-result", type=Path, required=True)

    validate_parser = commands.add_parser("validate")
    validate_parser.add_argument("--evidence", type=Path, required=True)
    return root_parser


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    repository = Path(__file__).resolve().parent.parent
    try:
        if args.action == "prepare":
            if args.iterations < 1:
                raise ValueError("iterations must be at least one")
            return prepare(args, repository)
        if args.action == "arm":
            return arm(args, repository)
        if args.action == "verify":
            return verify(args, repository)
        return validate_command(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"BLOCKED {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
