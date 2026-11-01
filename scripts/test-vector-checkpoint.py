#!/usr/bin/env python3
"""Run native-vector checkpoint regressions without relaxing existing assertions.

Build production and failpoint binaries separately first. Evidence directories
must be empty; every attempted test gets a log and a recorded outcome.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--failpoint-build", type=Path)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--expected-version", default="1.2.0-dev")
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    build = args.build.resolve(strict=True)
    output = args.evidence.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("evidence directory must be empty; retain previous attempts")
    targets = re.findall(r"add_pacificdb_engine_test\((\w+)", (repository / "engine/CMakeLists.txt").read_text())
    targets = [t for t in targets if t not in ("db_engine_vector_exact_benchmark", "db_engine_lsm_checkpoint_crash_driver")]
    targets += ["db_engine_lsm_point_read_flush_test",
                "v11_4_apply_exact_boundary_failpoints_test", "db_engine_startup_delay_failpoint_test",
                "storage_path_security_test", "db_engine_native_shell_parser_test", "db_engine_socket_runtime_test",
                "db_engine_local_engine_state_test", "db_engine_env_config_unicode_path_test",
                "db_engine_storage_root_guard_owner_test", "db_engine_structured_event_test",
                "db_engine_media_upload_state_test", "db_engine_cli_connection_test"]
    cases = []
    for target in targets:
        command = [str(build / target)]
        if target == "db_engine_cli_connection_test":
            command.append(str(repository / "sdk/contracts/connection-urls.json"))
        if target == "db_engine_local_engine_state_test":
            command.append(str(repository / "engine/test/fixtures/local_engine_states.json"))
        cases.append((target, command, 90))
    cases.append(("ctest", ["ctest", "--test-dir", str(build), "--output-on-failure"], 120))
    for name in ("test-community-e2e.mjs", "test-p0-media.mjs", "test-engine-auth-boundaries.mjs",
                 "test-community-rf3.mjs", "test-community-rf3-partition.mjs", "test-community-restart-matrix.mjs",
                 "test-vector-request-isolation.mjs", "test-native-vector-preview.mjs", "test-vector-schema.mjs", "test-vector-incremental.mjs"):
        cases.append((name, ["node", str(repository / "scripts" / name), str(build)], 300))
    if args.failpoint_build:
        cases.append(("test-p0-media-crash.mjs", ["node", str(repository / "scripts/test-p0-media-crash.mjs"),
                                               str(args.failpoint_build.resolve(strict=True))], 120))
        cases.append(("test-vector-schema-crash.mjs", ["node", str(repository / "scripts/test-vector-schema-crash.mjs"),
                                                      str(args.failpoint_build.resolve(strict=True))], 120))
        cases.append(("test-raft-apply-retry.mjs", ["node", str(repository / "scripts/test-raft-apply-retry.mjs"),
                                                 str(args.failpoint_build.resolve(strict=True))], 120))
        cases.append(("test-vector-incremental-crash.mjs", ["node", str(repository / "scripts/test-vector-incremental-crash.mjs"),
                                                         str(args.failpoint_build.resolve(strict=True))], 180))
    outcomes = []
    for name, command, timeout in cases:
        start = time.monotonic()
        code, error = None, None
        with (output / (name + ".log")).open("w") as log:
            try:
                proc = subprocess.Popen(command, cwd=repository, stdout=log, stderr=subprocess.STDOUT,
                                        start_new_session=True,
                                        env={**os.environ, "PACIFICDB_EXPECTED_ENGINE_VERSION": args.expected_version})
                try:
                    code = proc.wait(timeout=timeout)
                    status = "PASS" if code == 0 else "FAIL"
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    status, error = "FAIL", f"Timed out after {timeout} seconds"
            except OSError as exception:
                status, error = "BLOCKED", str(exception)
                log.write(error + "\n")
        outcomes.append({"test": name, "status": status, "exit_code": code,
                         "error": error, "seconds": time.monotonic() - start})
        (output / "tests.json").write_text(json.dumps(outcomes, indent=2))
        print(name, status, flush=True)
    with (build / "db_engine").open("rb") as stream:
        binary_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    failpoint_hash = None
    if args.failpoint_build:
        with (args.failpoint_build.resolve() / "db_engine").open("rb") as stream:
            failpoint_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    inputs = [repository / "engine/CMakeLists.txt", Path(__file__).resolve()]
    for directory in ("engine/include", "engine/src", "engine/test", "engine/tests"):
        inputs.extend(path for path in (repository / directory).rglob("*")
                      if path.is_file() and path.suffix in (".cpp", ".hpp", ".h"))
    fingerprints = {str(path.relative_to(repository)): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in sorted(set(inputs))}
    (output / "source-sha256.json").write_text(json.dumps(fingerprints, indent=2))
    complete = bool(args.failpoint_build) and all(r["status"] == "PASS" for r in outcomes)
    summary_status = ("FAIL" if any(r["status"] == "FAIL" for r in outcomes) else
                      "BLOCKED" if any(r["status"] == "BLOCKED" for r in outcomes) else
                      "PASS" if complete else "INCOMPLETE")
    (output / "summary.json").write_text(json.dumps({
        "status": summary_status, "scope": "checkpoint regressions, not production readiness",
        "expected_version": args.expected_version, "engine_sha256": binary_hash,
        "failpoint_engine_sha256": failpoint_hash,
        "top_level_checks": len(outcomes), "media_crash_executed": bool(args.failpoint_build),
        "outcomes": outcomes}, indent=2))
    if any(r["status"] in ("FAIL", "TIMEOUT") for r in outcomes):
        raise SystemExit(1)
    if not complete:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
