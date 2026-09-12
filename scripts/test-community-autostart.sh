#!/usr/bin/env bash
set -euo pipefail

build_dir=$(realpath "${1:-build}")
native_home=$(mktemp -d /tmp/pacificdb-native-autostart-XXXXXX)
node_home=$(mktemp -d /tmp/pacificdb-node-autostart-XXXXXX)
disabled_home=$(mktemp -d /tmp/pacificdb-disabled-autostart-XXXXXX)
blocking_home=$(mktemp -d /tmp/pacificdb-blocking-autostart-XXXXXX)
engine_pids=()

free_port() {
  python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

cleanup() {
  for pid in "${engine_pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  rm -rf -- "$native_home" "$node_home" "$disabled_home" "$blocking_home"
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
  engine_pids+=("$(cat "$home/engine.pid")")
}

native_port=$(free_port)
native_raft_port=$(free_port)
run_concurrent_start "'$build_dir/pacificdb'" "$native_home" "$native_port" "$native_raft_port"
printf 'help\nquit\n' | PACIFICDB_HOME="$native_home" \
  "$build_dir/pacificdb" --port "$native_port" >"$native_home/shell.out"
grep -q 'COMMUNITY BETA' "$native_home/shell.out"

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
grep -q 'COMMUNITY BETA' "$node_home/shell.out"

disabled_port=$(free_port)
if PACIFICDB_HOME="$disabled_home" "$build_dir/pacificdb" \
    --port "$disabled_port" --no-start ping >/dev/null 2>&1; then
  echo 'native --no-start unexpectedly started an engine' >&2
  exit 1
fi
test ! -e "$disabled_home/engine.pid"

printf 'Community automatic-start checks passed\n'
