#!/usr/bin/env bash
set -euo pipefail

build_dir=$(realpath "${1:-build}")
native_home=$(mktemp -d /tmp/pacificdb-native-autostart-XXXXXX)
node_home=$(mktemp -d /tmp/pacificdb-node-autostart-XXXXXX)
disabled_home=$(mktemp -d /tmp/pacificdb-disabled-autostart-XXXXXX)
blocking_home=$(mktemp -d /tmp/pacificdb-blocking-autostart-XXXXXX)
occupied_home=$(mktemp -d /tmp/pacificdb-occupied-port-XXXXXX)
early_native_home=$(mktemp -d /tmp/pacificdb-early-native-XXXXXX)
early_node_home=$(mktemp -d /tmp/pacificdb-early-node-XXXXXX)
engine_pids=()

free_port() {
  python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

assert_process_metadata() {
  local home=$1 expected_port=$2
  python3 - "$home" "$expected_port" <<'PY'
import json
import os
import pathlib
import stat
import sys

home = pathlib.Path(sys.argv[1])
metadata_path = home / 'engine.metadata.json'
metadata = json.loads(metadata_path.read_text(encoding='utf-8'))
assert metadata['schema'] == 'pacificdb.local-engine.v1'
assert metadata['version'] == 1
assert metadata['pid'] == int((home / 'engine.pid').read_text())
assert metadata['port'] == int(sys.argv[2])
assert pathlib.Path(metadata['executable']).is_absolute()
assert metadata['instance_id'].startswith('engine_')
assert len(metadata['discovery_nonce']) >= 32
assert metadata['data_root_fingerprint'].startswith('sha256:')
if os.name != 'nt':
    assert stat.S_IMODE(metadata_path.stat().st_mode) == 0o600
    assert stat.S_IMODE((home / 'engine.pid').stat().st_mode) == 0o600
PY
}

cleanup() {
  for pid_file in "$native_home/engine.pid" "$node_home/engine.pid" \
      "$disabled_home/engine.pid" "$blocking_home/engine.pid" \
      "$occupied_home/engine.pid" "$early_native_home/engine.pid" \
      "$early_node_home/engine.pid"; do
    if [[ -s "$pid_file" ]]; then
      pid=$(tr -cd '0-9' <"$pid_file")
      [[ -n "$pid" ]] && engine_pids+=("$pid")
    fi
  done
  for pid in "${engine_pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  for _ in {1..150}; do
    live=0
    for pid in "${engine_pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then live=1; fi
    done
    [[ "$live" == 0 ]] && break
    sleep 0.1
  done
  for pid in "${engine_pids[@]}"; do
    kill -KILL "$pid" 2>/dev/null || true
  done
  rm -rf -- "$native_home" "$node_home" "$disabled_home" "$blocking_home" \
    "$occupied_home" "$early_native_home" "$early_node_home"
}
trap cleanup EXIT

run_concurrent_start() {
  local command=$1 home=$2 port=$3 raft_port=$4
  PACIFICDB_HOME="$home" ENGINE_PORT="$port" RAFT_LISTEN_PORT="$raft_port" \
    PACIFICDB_ENGINE="$build_dir/db_engine" bash -c "$command --port '$port' ping" \
    >"$home/first.out" &
  local first=$!
  PACIFICDB_HOME="$home" ENGINE_PORT="$port" RAFT_LISTEN_PORT="$raft_port" \
    PACIFICDB_ENGINE="$build_dir/db_engine" bash -c "$command --port '$port' ping" \
    >"$home/second.out" &
  local second=$!
  wait "$first" "$second"
  grep -q pong "$home/first.out"
  grep -q pong "$home/second.out"
  test -s "$home/engine.pid"
  assert_process_metadata "$home" "$port"
  engine_pids+=("$(cat "$home/engine.pid")")
}

native_port=$(free_port)
native_raft_port=$(free_port)
run_concurrent_start "'$build_dir/pacificdb'" "$native_home" "$native_port" "$native_raft_port"
printf 'help\nquit\n' | PACIFICDB_HOME="$native_home" \
  "$build_dir/pacificdb" --port "$native_port" >"$native_home/shell.out"
grep -q 'PacificDB' "$native_home/shell.out"
grep -q 'v1.0.0' "$native_home/shell.out"
grep -Eq 'ENGINE_CPU_CORES override: [0-9]+ -> 2' "$native_home/engine.log"

blocking_port=$(free_port)
blocking_raft_port=$(free_port)
PACIFICDB_HOME="$blocking_home" ENGINE_PORT="$blocking_port" \
  RAFT_LISTEN_PORT="$blocking_raft_port" SERVER_IO_MODEL=blocking \
  timeout 20 "$build_dir/pacificdb" --port "$blocking_port" ping \
  >"$blocking_home/ping.out"
grep -q pong "$blocking_home/ping.out"
engine_pids+=("$(cat "$blocking_home/engine.pid")")

node_port=$(free_port)
node_raft_port=$(free_port)
run_concurrent_start "node cli/bin/pacificdb.js" "$node_home" "$node_port" "$node_raft_port"
printf 'help\nquit\n' | PACIFICDB_HOME="$node_home" PACIFICDB_ENGINE="$build_dir/db_engine" \
  node cli/bin/pacificdb.js --port "$node_port" >"$node_home/shell.out"
grep -q 'PacificDB' "$node_home/shell.out"
grep -q 'v1.0.0' "$node_home/shell.out"
grep -Eq 'ENGINE_CPU_CORES override: [0-9]+ -> 2' "$node_home/engine.log"

disabled_port=$(free_port)
if PACIFICDB_HOME="$disabled_home" "$build_dir/pacificdb" \
    --port "$disabled_port" --no-start ping >/dev/null 2>&1; then
  echo 'native --no-start unexpectedly started an engine' >&2
  exit 1
fi
test ! -e "$disabled_home/engine.pid"
test -z "$(find "$disabled_home" -mindepth 1 -print -quit)"

if PACIFICDB_HOME="$disabled_home" PACIFICDB_ENGINE="$build_dir/db_engine" \
    node cli/bin/pacificdb.js --port "$disabled_port" --no-start ping \
    >/dev/null 2>&1; then
  echo 'npm --no-start unexpectedly started an engine' >&2
  exit 1
fi
test -z "$(find "$disabled_home" -mindepth 1 -print -quit)"

early_native_port=$(free_port)
if PACIFICDB_HOME="$early_native_home" RAFT_NODE_ID= \
    PACIFICDB_STARTUP_TIMEOUT_MS=5000 "$build_dir/pacificdb" \
    --port "$early_native_port" ping >"$early_native_home/out" 2>&1; then
  echo 'native CLI ignored an early engine exit' >&2
  exit 1
fi
grep -q 'engine_exited' "$early_native_home/out"
grep -q 'exit code 78' "$early_native_home/out"
grep -q 'RAFT_NODE_ID or NODE_ID is required' "$early_native_home/out"

early_node_port=$(free_port)
if PACIFICDB_HOME="$early_node_home" PACIFICDB_ENGINE="$build_dir/db_engine" \
    RAFT_NODE_ID= PACIFICDB_STARTUP_TIMEOUT_MS=5000 \
    node cli/bin/pacificdb.js --port "$early_node_port" ping \
    >"$early_node_home/out" 2>&1; then
  echo 'npm CLI ignored an early engine exit' >&2
  exit 1
fi
grep -q 'engine_exited' "$early_node_home/out"
grep -q 'exit code 78' "$early_node_home/out"
grep -q 'RAFT_NODE_ID or NODE_ID is required' "$early_node_home/out"

occupied_port=$(free_port)
python3 - "$occupied_port" "$occupied_home/ready" <<'PY' &
import pathlib
import socket
import sys

server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', int(sys.argv[1])))
server.listen()
pathlib.Path(sys.argv[2]).touch()
while True:
    client, _ = server.accept()
    try:
        client.recv(4096)
        client.sendall(b'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n')
    except OSError:
        pass
    finally:
        client.close()
PY
occupied_pid=$!
engine_pids+=("$occupied_pid")
for _ in {1..100}; do
  test -e "$occupied_home/ready" && break
  sleep 0.01
done
test -e "$occupied_home/ready"

if PACIFICDB_HOME="$occupied_home/native" "$build_dir/pacificdb" \
    --port "$occupied_port" ping >"$occupied_home/native.out" 2>&1; then
  echo 'native CLI accepted a non-PacificDB listener' >&2
  exit 1
fi
grep -q 'not PacificDB' "$occupied_home/native.out"
! grep -q 'json.exception' "$occupied_home/native.out"

if PACIFICDB_HOME="$occupied_home/node" PACIFICDB_ENGINE="$build_dir/db_engine" \
    node cli/bin/pacificdb.js --port "$occupied_port" ping \
    >"$occupied_home/node.out" 2>&1; then
  echo 'npm CLI accepted a non-PacificDB listener' >&2
  exit 1
fi
grep -q 'not PacificDB' "$occupied_home/node.out"
! grep -q 'SyntaxError' "$occupied_home/node.out"

printf 'Community automatic-start checks passed\n'
