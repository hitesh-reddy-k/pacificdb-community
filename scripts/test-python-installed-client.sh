#!/usr/bin/env bash
set -euo pipefail
REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
WHEEL=$(realpath "${1:?usage: test-python-installed-client.sh wheel}")
TEST_ROOT=$(mktemp -d)
trap 'rm -rf "$TEST_ROOT"' EXIT
python3 -m venv "$TEST_ROOT/venv"
"$TEST_ROOT/venv/bin/python" -m pip install --no-deps "$WHEEL"
"$TEST_ROOT/venv/bin/python" -m pip install 'pytest==8.3.5'
mkdir -p "$TEST_ROOT/sdk/python" "$TEST_ROOT/sdk/contracts" "$TEST_ROOT/scripts" "$TEST_ROOT/engine/src" "$TEST_ROOT/sdk/java/src/main/java/io/pacificdb"
cp -R "$REPOSITORY/sdk/python/tests" "$TEST_ROOT/sdk/python/"
cp "$REPOSITORY/sdk/contracts/connection-urls.json" "$REPOSITORY/sdk/contracts/community-capabilities.json" "$TEST_ROOT/sdk/contracts/"
cp "$REPOSITORY/scripts/test-sdk-capability-matrix.py" "$TEST_ROOT/scripts/"
cp "$REPOSITORY/engine/src/server.cpp" "$TEST_ROOT/engine/src/"
cp "$REPOSITORY/sdk/java/src/main/java/io/pacificdb/PacificDBClient.java" "$REPOSITORY/sdk/java/src/main/java/io/pacificdb/Operations.java" "$TEST_ROOT/sdk/java/src/main/java/io/pacificdb/"
cd "$TEST_ROOT"
unset PYTHONPATH
"$TEST_ROOT/venv/bin/python" - <<'PY'
import importlib.metadata
import pathlib
import pacificdb
import sys
assert pathlib.Path(pacificdb.__file__).is_relative_to(sys.prefix)
assert pacificdb.PacificDB is pacificdb.PacificDBClient
assert not importlib.metadata.requires('pacificdb')
print('PASS: installed wheel import and zero runtime dependencies')
PY
"$TEST_ROOT/venv/bin/python" -m pytest -q sdk/python/tests
if test -n "${PACIFICDB_TEST_ENGINE_BUILD:-}"; then
  PACIFICDB_TEST_PYTHON="$TEST_ROOT/venv/bin/python" node "$REPOSITORY/scripts/test-cross-sdk-e2e.mjs" "$PACIFICDB_TEST_ENGINE_BUILD"
fi
