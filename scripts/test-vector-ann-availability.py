#!/usr/bin/env python3
"""Strict ANN availability/recovery qualification on disposable GloVe data.

Uses the published benchmark workload, but requires all 360 mixed-write reads
to succeed. This is a single-system qualification, not a paired latency claim.
Requires the benchmark Python dependencies and checksum-frozen input directory.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import traceback

repository = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repository / "sdk/python"))
spec = importlib.util.spec_from_file_location("benchmark", repository / "scripts/benchmark-community-v12.py")
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "publication", "output", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--rows", type=int, choices=(10000, 100000, 1000000), default=1000000)
    args = parser.parse_args()
    for name in ("build", "publication"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        parser.error("output must be empty; preserve earlier attempts")
    args.scratch = args.scratch.resolve()
    args.scratch.mkdir(parents=True, exist_ok=True)
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 6:
        parser.error("six available CPUs required for the published CPU allocation")
    args.server_cpus, args.startup_timeout = cpus[:4], 240
    os.sched_setaffinity(0, cpus[4:6])
    manifest = json.loads((args.publication / "data/manifest.json").read_text())
    for name in ("base.npy", "queries.npy", "truth.npz"):
        assert b.digest(args.publication / "data" / name) == manifest["hashes"][name], name
    base = b.np.load(args.publication / "data/base.npy", mmap_mode="r")[:args.rows]
    queries = b.np.load(args.publication / "data/queries.npy")
    assert len(base) == args.rows and base.shape[1] == 25 and len(queries) >= 1100
    frozen_truth = b.np.load(args.publication / "data/truth.npz")
    truth = ([{"ids": ids.tolist(), "cutoff": float(scores[-1])} for ids, scores in
              zip(frozen_truth["ids"][100:200], frozen_truth["scores"][100:200])]
             if args.rows == 1000000 else b.reference(base, queries[100:200]))
    result = {"status": "RUNNING", "scope": "strict ANN availability and crash/reconstruction; not production certification or paired performance benchmark",
              "rows": args.rows, "dimensions": 25, "metric": "cosine", "ef_search": 600,
              "driver_sha256": b.digest(__file__), "benchmark_helper_sha256": b.digest(repository / "scripts/benchmark-community-v12.py"),
              "source_manifest": manifest, "engine_sha256": b.digest(args.build / "db_engine"),
              "engine_identity": json.loads(subprocess.check_output([str(args.build / "db_engine"), "--build-info"], text=True)),
              "server_cpus": args.server_cpus, "client_cpus": cpus[4:6],
              "started_at": time.time(), "phase": "starting"}
    server = b.Server("pacific", args, args.rows, 25, args.output)
    stopped = threading.Event()

    def heartbeat():
        while not stopped.wait(10):
            b.save(args.output / "heartbeat.json", {"pid": os.getpid(), "engine_pid": server.proc.pid if server.proc else None,
                    "phase": result["phase"], "time": time.time()})

    monitor = threading.Thread(target=heartbeat, daemon=True)
    monitor.start()
    try:
        server.start(); server.setup()
        result["phase"] = "ingestion"; b.save(args.output / "summary.json", result)
        result["load"] = server.load(base, 1000)
        assert server.count() == args.rows
        result["wal_status"] = server.connection.request({"action": "admin_wal_status"})
        result["phase"] = "index_build"; b.save(args.output / "summary.json", result)
        result["index"] = server.build_index("ann")
        checks = []
        for qi in range(100, 200):
            docs, ms, execution = server.query(queries[qi], "ann", 600)
            checks.append({"ms": ms, **b.validate(docs, queries[qi], base, truth[qi-100])})
        result["static"] = {"queries": 100, "recall_at_10": b.statistics.mean(r["tie_aware_recall"] for r in checks), "records": checks}
        assert result["static"]["recall_at_10"] >= .95
        result["phase"] = "mutations_mixed_reads_crash_recovery"; b.save(args.output / "summary.json", result)
        # Same 10 updates, five deletes, four clients/40 writes/360 strict reads,
        # SIGKILL, acknowledged-record verification and ANN reconstruction.
        # The helper retains every freshness error; this qualification rejects
        # any error rather than treating completion of the workload as a pass.
        result["lifecycle"] = b.extended(server, base, queries, 600)
        mixed = result["lifecycle"]["mixed_90_read_10_write"]
        assert mixed["read_successes"] == mixed["reads_attempted"] == 360 and not mixed["read_errors"], mixed
        result["status"] = "PASS"
    except BaseException:
        result["status"] = "FAIL"; result["error"] = traceback.format_exc()
    finally:
        try:
            server.stop()
        except BaseException:
            result["status"] = "FAIL"; result["shutdown_error"] = traceback.format_exc()
        stopped.set(); monitor.join(timeout=2)
        result["phase"] = "finished"; result["finished_at"] = time.time()
        result["resources"] = server.memory
        b.save(args.output / "summary.json", result)
    print(json.dumps({"status": result["status"], "rows": args.rows, "output": str(args.output)}), flush=True)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
