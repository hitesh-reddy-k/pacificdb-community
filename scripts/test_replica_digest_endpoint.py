#!/usr/bin/env python3
import json
import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "build-release-integrity" / "db_engine"


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def request(port, payload):
    wire = json.dumps(payload, separators=(",", ":")).encode() + b"\n"
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.sendall(wire)
        response = bytearray()
        while b"\n" not in response:
            chunk = connection.recv(65536)
            if not chunk:
                raise RuntimeError("engine closed before returning JSON")
            response.extend(chunk)
    return json.loads(response.split(b"\n", 1)[0])


def main():
    if not ENGINE.is_file():
        raise RuntimeError(f"missing engine binary: {ENGINE}")
    with tempfile.TemporaryDirectory(prefix="pacificdb-replica-digest-") as directory:
        root = Path(directory)
        port = free_port()
        raft_port = free_port()
        password = "replica-digest-test-password"
        environment = os.environ.copy()
        environment.update(
            PACIFICDB_ENVIRONMENT="development",
            DATA_ROOT=str(root / "data"),
            BACKUP_ROOT=str(root / "backup"),
            RESTORE_DIR=str(root / "restore"),
            TMP_DIR=str(root / "tmp"),
            LOG_DIR=str(root / "logs"),
            ENGINE_BIND_HOST="127.0.0.1",
            ENGINE_PORT=str(port),
            ENGINE_AUTH_REQUIRED="1",
            PACIFICDB_ENGINE_ADMIN_USERNAME="admin",
            PACIFICDB_ENGINE_ADMIN_PASSWORD=password,
            RAFT_CLUSTER_ID="replica-digest-test",
            RAFT_NODE_ID="node-1",
            RAFT_LISTEN_PORT=str(raft_port),
            RAFT_IS_LEADER="1",
            MIN_QUORUM_SIZE="1",
            ENGINE_CPU_CORES="2",
            CONN_MIN_THREADS="2",
            CONN_MAX_THREADS="8",
            DBQ_SHARDS="2",
            DBQ_WORKERS_PER_SHARD="1",
            ADAPTIVE_ADMISSION="0",
            REPLICA_DIGEST_MAX_DOCS="100",
        )
        log_path = root / "engine.log"
        with log_path.open("wb") as log:
            engine = subprocess.Popen(
                [str(ENGINE)],
                cwd=ROOT,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
        try:
            for _ in range(150):
                if engine.poll() is not None:
                    raise RuntimeError(
                        f"engine exited early: {log_path.read_text(errors='replace')}"
                    )
                try:
                    if request(port, {"action": "ping"}).get("status") == "pong":
                        break
                except OSError:
                    pass
                time.sleep(0.1)
            else:
                raise RuntimeError("engine did not become ready")

            unauthorized = request(
                port,
                {
                    "action": "admin_replica_digest",
                    "userId": "system",
                    "dbName": "app",
                    "collection": "docs",
                    "fence": 0,
                    "maxDocs": 10,
                },
            )
            assert unauthorized.get("error") == "unauthorized", unauthorized

            authenticated = request(
                port,
                {
                    "action": "security_authenticate",
                    "username": "admin",
                    "password": password,
                },
            )
            token = authenticated["token"]
            created_database = request(
                port,
                {
                    "action": "createDatabase",
                    "token": token,
                    "userId": "system",
                    "dbName": "app",
                    "dbType": "binary",
                },
            )
            assert not created_database.get("error"), created_database
            created_collection = request(
                port,
                {
                    "action": "createCollection",
                    "token": token,
                    "userId": "system",
                    "dbName": "app",
                    "collection": "docs",
                },
            )
            assert not created_collection.get("error"), created_collection
            for identifier, value in (("a", 1), ("b", 2)):
                inserted = request(
                    port,
                    {
                        "action": "insert",
                        "token": token,
                        "userId": "system",
                        "dbName": "app",
                        "collection": "docs",
                        "data": {"id": identifier, "value": value},
                    },
                )
                assert not inserted.get("error"), inserted

            status = request(port, {"action": "ping"})
            fence = status["last_applied"]
            digest = request(
                port,
                {
                    "action": "admin_replica_digest",
                    "token": token,
                    "userId": "system",
                    "dbName": "app",
                    "collection": "docs",
                    "fence": fence,
                    "maxDocs": 10,
                },
            )
            assert digest.get("ok") is True, digest
            assert digest["clusterId"] == "replica-digest-test", digest
            assert digest["digestSchema"] == "pacificdb-logical-v1", digest
            assert digest["documentCount"] == 2, digest
            assert len(digest["digest"]) == 64, digest
            assert "data" not in digest and "documents" not in digest, digest

            future = request(
                port,
                {
                    "action": "admin_replica_digest",
                    "token": token,
                    "userId": "system",
                    "dbName": "app",
                    "collection": "docs",
                    "fence": fence + 1,
                    "maxDocs": 10,
                },
            )
            assert future.get("error") == "fence_above_last_applied", future

            negative_fence = request(
                port,
                {
                    "action": "admin_replica_digest",
                    "token": token,
                    "userId": "system",
                    "dbName": "app",
                    "collection": "docs",
                    "fence": -1,
                    "maxDocs": 10,
                },
            )
            assert negative_fence.get("error") == "invalid_fence", negative_fence

            bounded = request(
                port,
                {
                    "action": "admin_replica_digest",
                    "token": token,
                    "userId": "system",
                    "dbName": "app",
                    "collection": "docs",
                    "fence": fence,
                    "maxDocs": 1,
                },
            )
            assert bounded.get("error") == "TRUNCATED", bounded
            assert "digest" not in bounded, bounded
            print("REPLICA_DIGEST_ENDPOINT_PASS")
        finally:
            if engine.poll() is None:
                engine.terminate()
                try:
                    engine.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    engine.kill()
                    engine.wait(timeout=5)


if __name__ == "__main__":
    main()
