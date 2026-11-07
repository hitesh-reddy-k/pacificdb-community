#!/usr/bin/env bash
set -euo pipefail
REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
WHEEL=$(realpath "${1:?usage: test-python-installed-client.sh wheel-or-sdist}")
TEST_ROOT=$(mktemp -d)
trap 'result=$?; if test "$result" = 0; then rm -rf "$TEST_ROOT"; else echo "preserved failed consumer: $TEST_ROOT" >&2; fi' EXIT
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
PACIFICDB_EXPECTED_PYTHON_VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pythonVersion"])' "$REPOSITORY/release.json") \
"$TEST_ROOT/venv/bin/python" - <<'PY'
import importlib.metadata
import os
import pathlib
import pacificdb
import sys
assert pathlib.Path(pacificdb.__file__).is_relative_to(sys.prefix)
assert pacificdb.PacificDB is pacificdb.PacificDBClient
assert not importlib.metadata.requires('pacificdb')
assert importlib.metadata.version('pacificdb') == os.environ['PACIFICDB_EXPECTED_PYTHON_VERSION'] == pacificdb.__version__
print('PASS: installed wheel import and zero runtime dependencies')
PY
"$TEST_ROOT/venv/bin/python" -m pytest -q sdk/python/tests
if test -n "${PACIFICDB_TEST_ENGINE_BUILD:-}"; then
  : "${PACIFICDB_TEST_NODE_MODULE:?installed Node module required for engine qualification}"
  PACIFICDB_TEST_SKIP_JAVA_BUILD=1 PACIFICDB_TEST_LANGUAGES=python PACIFICDB_TEST_PYTHON="$TEST_ROOT/venv/bin/python" node "$REPOSITORY/scripts/test-cross-sdk-e2e.mjs" "$PACIFICDB_TEST_ENGINE_BUILD"
fi
