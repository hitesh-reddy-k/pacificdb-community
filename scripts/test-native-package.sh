#!/usr/bin/env bash
set -euo pipefail

package=${1:?usage: test-native-package.sh PACKAGE.deb}
root=$(mktemp -d /tmp/pacificdb-package-root-XXXXXX)
home=$(mktemp -d /tmp/pacificdb-package-home-XXXXXX)
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
raft_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
server_pid=

cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

dpkg-deb -x "$package" "$root"
test -x "$root/usr/bin/db_engine"
test -x "$root/usr/bin/pacificdb"
test -x "$root/usr/bin/pacificdb-local"
printf 'help\nquit\n' | "$root/usr/bin/pacificdb" shell | grep -q 'Shell commands:'

PACIFICDB_HOME="$home" ENGINE_PORT="$port" RAFT_LISTEN_PORT="$raft_port" \
  "$root/usr/bin/pacificdb-local" >"$home/server.log" 2>&1 &
server_pid=$!

for _ in {1..60}; do
  if "$root/usr/bin/pacificdb" --port "$port" ping >/dev/null 2>&1; then break; fi
  sleep 0.25
done
"$root/usr/bin/pacificdb" --port "$port" ping | grep -q 'pong'
"$root/usr/bin/pacificdb" --port "$port" request \
  '{"action":"createDatabase","dbName":"package_test"}' >/dev/null
"$root/usr/bin/pacificdb" --port "$port" request \
  '{"action":"createCollection","dbName":"package_test","collection":"users"}' >/dev/null
"$root/usr/bin/pacificdb" --port "$port" request \
  '{"action":"insert","dbName":"package_test","collection":"users","data":{"id":"1","name":"Ada"}}' >/dev/null
"$root/usr/bin/pacificdb" --port "$port" request \
  '{"action":"find","dbName":"package_test","collection":"users","filter":{"id":"1"}}' | grep -q 'Ada'
clean_find=$("$root/usr/bin/pacificdb" --port "$port" request \
  '{"action":"find","dbName":"package_test","collection":"users","filter":{"id":"1"}}')
! grep -Eq '_debug_metrics|_engineTrace|_raft|requestId|trace_id|consistency_semantics' \
  <<<"$clean_find"

printf '\000\001GIF89a\377' >"$home/input.gif"
"$root/usr/bin/pacificdb" --port "$port" --database package_test \
  put-media users hero "$home/input.gif" --content-type image/gif >/dev/null
"$root/usr/bin/pacificdb" --port "$port" --database package_test \
  get-media users hero "$home/output.gif" >/dev/null
cmp "$home/input.gif" "$home/output.gif"
"$root/usr/bin/pacificdb" --port "$port" --database package_test \
  put-vector users hero-vector '[1,0]' --metadata '{"modality":"image"}' >/dev/null
"$root/usr/bin/pacificdb" --port "$port" --database package_test \
  query-vector users '[1,0]' --k 1 | grep -q 'hero-vector'

echo "native package smoke passed"
