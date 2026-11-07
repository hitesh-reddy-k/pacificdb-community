#!/usr/bin/env python3
"""Read-only-source, bounded before/after replay of the local GloVe study.

Requires numpy, psutil and the repository's Python SDK. Each engine runs on its
own disposable copy of a stopped benchmark dataset; original evidence is never
opened by a server. This is a small diagnostic replay, not release qualification.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import statistics
import subprocess
import tempfile
import threading
import time

import numpy as np
import psutil
from pacificdb import PacificDB


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def replay(label, build, args, base, queries, truth):
    binary = (build / "db_engine").resolve(strict=True)
    result = {"label": label, "binary_sha256": sha256(binary), "status": "RUNNING",
              "rows": len(base), "dimensions": base.shape[1], "records": [],
              "warmups": args.warmups, "peak_sampled_pss_bytes": 0,
              "peak_sampled_rss_bytes": 0, "memory_samples": [], "monitor_errors": [],
              "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "host_before": {"available_memory": psutil.virtual_memory().available,
                              "swap_used": psutil.swap_memory().used, "loadavg": os.getloadavg()}}
    records = result["records"]
    stop_monitor = threading.Event()
    client = None
    with tempfile.TemporaryDirectory(prefix="pacificdb-v12-replay-") as temp:
        root = Path(temp)
        data = root / "data"
        source = args.publication / "pacific-data-r1/data"
        # Cooperate with StorageRootGuard: reject a live source and prevent it
        # from starting while the copy is captured. The source stays read-only.
        with (source / ".pacificdb-root.lock").open("rb") as source_lock:
            fcntl.flock(source_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
            shutil.copytree(source, data)
        identity_path = data / ".pacificdb-root-identity.json"
        identity = json.loads(identity_path.read_text())
        # Relocate ONLY the disposable root identity, retaining cluster/node identity.
        identity["canonicalRoot"] = str(data.resolve())
        identity_path.write_text(json.dumps(identity))
        port, raft_port = free_port(), free_port()
        while raft_port == port:
            raft_port = free_port()
        env = {**os.environ, "PACIFICDB_ENVIRONMENT": "development", "PACIFICDB_HOME": str(root),
               "DATA_ROOT": str(data), "BACKUP_ROOT": str(root / "backup"), "RESTORE_DIR": str(root / "restore"),
               "WAL_DIR": str(data / "wal"), "SST_DIR": str(data / "sst"), "TMP_DIR": str(data / "tmp"),
               "SNAPSHOT_DIR": str(data / "snapshots"), "LOG_DIR": str(data / "logs"),
               "ENGINE_HOST": "127.0.0.1", "ENGINE_BIND_HOST": "127.0.0.1", "ENGINE_PORT": str(port),
               "RAFT_LISTEN_HOST": "127.0.0.1", "RAFT_BIND_HOST": "127.0.0.1", "RAFT_LISTEN_PORT": str(raft_port),
               "RAFT_CLUSTER_ID": identity["clusterId"], "RAFT_NODE_ID": identity["nodeId"],
               "RAFT_IS_LEADER": "true", "RAFT_LEADER_ELIGIBLE": "true", "MIN_QUORUM_SIZE": "1",
               "TLS_ENABLED": "false", "ENGINE_AUTH_REQUIRED": "false", "VECTOR_HNSW_ENABLED": "false",
               "WAL_ENABLED": "true", "WAL_FSYNC_ENABLED": "true", "WAL_FLUSH_ON_COMMIT": "true",
               "WAL_CHECKSUM_ENABLED": "true", "WAL_GROUP_COMMIT_ENABLED": "true",
               "MAX_SCAN_ROWS": str(len(base) + 1), "MAX_RESULT_DOCS": str(len(base) + 1),
               "MAX_QUERY_TIMEOUT_SEC": "60", "ENGINE_CPU_CORES": "4", "PROMETHEUS_ENABLED": "false",
               "AUDIT_LOGGING_ENABLED": "false"}
        use_flat = getattr(args, "pacific_index", "none") == "flat"
        if use_flat:
            env["PACIFICDB_VECTOR_PREVIEW_ENABLED"] = "1"
        command = [str(binary)]
        cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
        if len(cpus) >= 6:
            command = ["taskset", "-c", ",".join(map(str, cpus[:4])), *command]
        result["server_cpus"] = cpus[:4]
        result["client_cpus"] = cpus[4:6]
        with (args.output / f"{label}-server.log").open("w") as log:
            proc = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

            def monitor():
                while not stop_monitor.wait(.5):
                    try:
                        process = psutil.Process(proc.pid)
                        infos = []
                        for child in [process, *process.children(recursive=True)]:
                            try:
                                infos.append(child.memory_full_info())
                            except psutil.NoSuchProcess:
                                continue
                        pss, rss = sum(info.pss for info in infos), sum(info.rss for info in infos)
                        result["memory_samples"].append({"monotonic": time.perf_counter(), "pss": pss, "rss": rss})
                        result["peak_sampled_pss_bytes"] = max(result["peak_sampled_pss_bytes"], pss)
                        result["peak_sampled_rss_bytes"] = max(result["peak_sampled_rss_bytes"], rss)
                        if pss > 6 * 1024**3:
                            result["memory_guard_exceeded"] = True
                            os.killpg(proc.pid, signal.SIGTERM)
                            return
                    except (psutil.Error, ProcessLookupError) as error:
                        if proc.poll() is not None:
                            return
                        result["monitor_errors"].append(str(error))

            thread = threading.Thread(target=monitor, daemon=True)
            thread.start()
            old_affinity = cpus
            try:
                if len(cpus) >= 6:
                    os.sched_setaffinity(0, cpus[4:6])
                deadline = time.monotonic() + 90
                while True:
                    if proc.poll() is not None:
                        raise RuntimeError(f"engine exited at startup: {proc.returncode}")
                    probe = None
                    try:
                        probe = PacificDB.connect(f"pacificdb://127.0.0.1:{port}/publish_bench", timeout=1, pool_size=1)
                        if probe.request({"action": "ping"}).get("status") == "pong":
                            break
                    except Exception:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(.1)
                    finally:
                        if probe:
                            probe.close()
                client = PacificDB.connect(f"pacificdb://127.0.0.1:{port}/publish_bench", timeout=60, pool_size=1)
                count = client.count("vectors")
                assert not count.get("partial") and count["count"] == len(base), count
                result["validated_count"] = count["count"]
                if use_flat:
                    build_start = time.perf_counter()
                    creation = client.request({"action":"createVectorIndex","collection":"vectors","options":{
                        "name":"benchmark-flat","algorithm":"flat","dimensions":int(base.shape[1]),"metric":"cosine",
                        "max_elements":len(base),"max_scan_rows":len(base)+1,
                        "max_source_bytes":512*1024**2,"max_index_bytes":512*1024**2}})
                    if creation.get("error"): raise RuntimeError(creation)
                    index_deadline = time.monotonic()+300
                    while True:
                        state = client.request({"action":"vectorIndexStatus","collection":"vectors","name":"benchmark-flat"})
                        if state.get("error"): raise RuntimeError(state)
                        if state["index"]["state"] == "READY": break
                        if state["index"]["state"] == "FAILED" or time.monotonic()>index_deadline: raise RuntimeError(state)
                        time.sleep(.05)
                    result["index_build_seconds"] = time.perf_counter()-build_start
                    result["index_status"] = state["index"]
                    time.sleep(2) # source staging destruction is outside publication
                for iteration in range(args.warmups + args.queries):
                    qi = iteration if iteration < args.warmups else 100 + iteration - args.warmups
                    phase = "warmup" if iteration < args.warmups else "measured"
                    row = {"query": qi, "phase": phase, "status": "STARTED"}
                    records.append(row)
                    start = time.perf_counter()
                    row["start_monotonic"] = start
                    response = client.vectors.query(collection="vectors", vector=queries[qi].tolist(),
                                                    metric="cosine", k=10, search_mode="exact",
                                                    **({"index_name":"benchmark-flat","require_index":True} if use_flat else {}))
                    elapsed = (time.perf_counter() - start) * 1000
                    row["end_monotonic"] = time.perf_counter()
                    assert not response.get("partial"), response
                    if use_flat:
                        execution = response["search"]
                        assert execution["index_type"] == "flat" and execution["executed_mode"] == "exact" and not execution["fallback"], execution
                        row["execution"] = execution
                    docs = response["data"]
                    assert all(doc.get("kind") == "vector" for doc in docs)
                    ids = [int(doc["id"]) for doc in docs]
                    assert len(ids) == 10 and len(set(ids)) == 10
                    np.testing.assert_array_equal(np.array([d["vector"] for d in docs], dtype=np.float32), base[ids])
                    b, q = base[ids].astype(np.float64), queries[qi].astype(np.float64)
                    scores = (b @ q) / (np.linalg.norm(b, axis=1) * np.linalg.norm(q))
                    assert np.all(np.diff(scores) <= 1e-12), "scores are not ranked"
                    np.testing.assert_allclose([d["score"] for d in docs], scores, rtol=1e-12, atol=1e-12)
                    recall = float(np.mean(scores >= truth["scores"][qi, -1] - 1e-6))
                    assert recall == 1.0, (qi, ids, recall)
                    row.update(status="PASS", ms=elapsed, ids=ids, tie_aware_recall=recall,
                               strict_recall=len(set(ids) & set(truth["ids"][qi].tolist())) / 10)
                    (args.output / f"{label}.json").write_text(json.dumps(result, indent=2))
                    print(label, phase, qi, round(elapsed, 2), "ms", flush=True)
                times = [r["ms"] for r in records if r["phase"] == "measured"]
                samples = result["memory_samples"]
                assert len(samples) >= 5 and not result["monitor_errors"], "incomplete memory monitoring"
                assert samples[0]["monotonic"] <= records[0]["start_monotonic"] + 1, "late memory monitoring"
                assert samples[-1]["monotonic"] >= records[-1]["end_monotonic"] - 1, "memory monitoring stopped early"
                assert max(b["monotonic"] - a["monotonic"] for a, b in zip(samples, samples[1:])) < 2, "memory telemetry gap"
                result.update(status="PASS", measured_queries=len(times), p50_ms=statistics.median(times),
                              p95_ms=None, p99_ms=None)
            except Exception as error:
                result.update(status="FAIL", error=f"{type(error).__name__}: {error}")
            finally:
                if client:
                    client.close()
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
                stop_monitor.set()
                thread.join(timeout=2)
                if old_affinity:
                    os.sched_setaffinity(0, old_affinity)
    result["host_after"] = {"available_memory": psutil.virtual_memory().available,
                            "swap_used": psutil.swap_memory().used, "loadavg": os.getloadavg()}
    (args.output / f"{label}.json").write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-build", type=Path, required=True)
    parser.add_argument("--candidate-build", type=Path, required=True)
    parser.add_argument("--publication", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--queries", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.queries <= 1000 or not 0 <= args.warmups <= 100:
        parser.error("queries must be 1..1000; warmups 0..100")
    args.publication = args.publication.resolve(strict=True)
    args.output = args.output.resolve()
    if args.output == args.publication or args.publication in args.output.parents:
        parser.error("output must be outside original publication evidence")
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        parser.error("output directory must be empty to preserve earlier attempts")
    manifest = json.loads((args.publication / "data/manifest.json").read_text())
    for name in ("base.npy", "queries.npy", "truth.npz"):
        assert sha256(args.publication / "data" / name) == manifest["hashes"][name], name
    base = np.load(args.publication / "data/base.npy", mmap_mode="r")
    queries = np.load(args.publication / "data/queries.npy", mmap_mode="r")
    truth = np.load(args.publication / "data/truth.npz")
    results = [replay(label, build, args, base, queries, truth) for label, build in
               (("baseline", args.baseline_build), ("candidate", args.candidate_build))]
    (args.output / "summary.json").write_text(json.dumps(results, indent=2))
    if any(r["status"] != "PASS" for r in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
