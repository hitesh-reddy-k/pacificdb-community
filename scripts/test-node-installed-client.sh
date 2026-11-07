#!/usr/bin/env bash
set -euo pipefail
REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
PACKAGES=$(realpath "${1:?usage: test-node-installed-client.sh staged-npm-directory}")
TEST_ROOT=$(mktemp -d)
trap 'result=$?; if test "$result" = 0; then rm -rf "$TEST_ROOT"; else echo "preserved failed consumer: $TEST_ROOT" >&2; fi' EXIT
cd "$TEST_ROOT"
printf '{"private":true,"type":"module"}\n' > package.json
npm install --ignore-scripts --no-audit --no-fund "$PACKAGES"/*.tgz
mkdir test
cp "$REPOSITORY/sdk/node/test/"*.test.js test/
# Exercise the installed public entry and connection parser, without repository imports.
python3 - <<'PY'
from pathlib import Path
for path in Path('test').glob('*.js'):
    path.write_text(path.read_text().replace("'../src/index.js'", "'@pacificdb/client'").replace("'../src/connection-url.js'", "'../node_modules/@pacificdb/client/src/connection-url.js'").replace("../../contracts/connection-urls.json", "../sdk/contracts/connection-urls.json"))
PY
mkdir -p sdk/contracts
cp "$REPOSITORY/sdk/contracts/connection-urls.json" sdk/contracts/
PACIFICDB_CONNECTION_FIXTURES="$TEST_ROOT/sdk/contracts/connection-urls.json" node --test test/*.test.js
./node_modules/.bin/pacificdb --version
export PACIFICDB_TEST_CLI_ENTRY="$TEST_ROOT/node_modules/@pacificdb/cli/bin/pacificdb.js"
export PACIFICDB_TEST_NODE_MODULE="file://$TEST_ROOT/node_modules/@pacificdb/client/src/index.js"
node --input-type=module - <<'JS'
const sdk=await import(process.env.PACIFICDB_TEST_NODE_MODULE);
if(sdk.releaseInfo.releaseVersion!=='1.2.0-rc.2')throw Error('installed identity mismatch');
console.log('PASS: installed npm candidate identity');
JS
if test -n "${PACIFICDB_TEST_ENGINE_BUILD:-}"; then
  PACIFICDB_TEST_SKIP_JAVA_BUILD=1 PACIFICDB_TEST_LANGUAGES=node \
    node "$REPOSITORY/scripts/test-cross-sdk-e2e.mjs" "$PACIFICDB_TEST_ENGINE_BUILD"
fi
