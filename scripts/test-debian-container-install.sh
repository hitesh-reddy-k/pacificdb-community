#!/usr/bin/env bash
set -euo pipefail

package=${1:?usage: test-debian-container-install.sh PACKAGE.deb}
package=$(realpath "$package")
expected_version=${2:-0.1.0~beta.11}
image=${PACIFICDB_DEBIAN_TEST_IMAGE:-ubuntu:24.04}

docker run --rm --network bridge \
  --mount "type=bind,src=$package,dst=/tmp/pacificdb.deb,readonly" \
  -e EXPECTED_VERSION="$expected_version" "$image" bash -ceu '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq /tmp/pacificdb.deb
    test "$(dpkg-query -W -f="\${Version}" pacificdb-community)" = "$EXPECTED_VERSION"
    test "$(command -v pacificdb)" = /usr/bin/pacificdb
    test "$(command -v db_engine)" = /usr/bin/db_engine
    ! command -v pacificdb-local
    test -s /usr/share/pacificdb/pacificdb-logo.png

    root=$(mktemp -d /tmp/pacificdb-installed-XXXXXX)
    export PACIFICDB_HOME="$root"
    export ENGINE_PORT=19000 RAFT_LISTEN_PORT=19001
    printf "quit\n" | pacificdb --port 19000 >"$root/shell.out"
    grep -q "COMMUNITY BETA" "$root/shell.out"
    server_pid=$(cat "$root/engine.pid")
    trap "kill $server_pid 2>/dev/null || true" EXIT
    pacificdb --port 19000 ping | grep -q pong
    pacificdb --port 19000 request \
      "{\"action\":\"createDatabase\",\"dbName\":\"apt_test\"}" | grep -q "\"status\": \"ok\""
    pacificdb --port 19000 request \
      "{\"action\":\"createCollection\",\"dbName\":\"apt_test\",\"collection\":\"docs\"}" >/dev/null
    pacificdb --port 19000 request \
      "{\"action\":\"insert\",\"dbName\":\"apt_test\",\"collection\":\"docs\",\"data\":{\"id\":\"1\",\"value\":\"installed\"}}" >/dev/null
    pacificdb --port 19000 request \
      "{\"action\":\"find\",\"dbName\":\"apt_test\",\"collection\":\"docs\",\"filter\":{\"id\":\"1\"}}" | grep -q installed
    kill "$server_pid"
    trap - EXIT

    apt-get remove -y -qq pacificdb-community
    test ! -e /usr/bin/pacificdb
    test ! -e /usr/bin/db_engine
    test -d "$root/data"
    printf "{\"status\":\"PASS\",\"installer\":\"apt\",\"version\":\"%s\",\"data_preserved_on_remove\":true}\n" "$EXPECTED_VERSION"
  '
