#!/usr/bin/env bash
# Consume the exact staged distributions. All consumer roots are disposable.
set -euo pipefail
REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
STAGING=$(realpath "${1:-$REPOSITORY/build-v1.2-release/staging/sdk}")
EVIDENCE=$(realpath -m "${2:-$REPOSITORY/build-v1.2-release/evidence/sdk}")
mkdir -p "$EVIDENCE"
TEST_ROOT=$(mktemp -d)
trap 'result=$?; if test "$result" = 0; then rm -rf "$TEST_ROOT"; else echo "preserved failed consumer: $TEST_ROOT" >&2; fi' EXIT
cd "$REPOSITORY"
python3 scripts/verify-sdk-packages.py --npm-dist "$STAGING/npm" --python-dist "$STAGING/python" --java-target "$STAGING/java" --allow-unsigned-local --output "$EVIDENCE/before-installed.json"
(cd "$TEST_ROOT" && npm install --ignore-scripts --no-audit --no-fund "$STAGING/npm/"*.tgz)
export PACIFICDB_TEST_NODE_MODULE="file://$TEST_ROOT/node_modules/@pacificdb/client/src/index.js"
scripts/test-node-installed-client.sh "$STAGING/npm" 2>&1 | tee "$EVIDENCE/node-installed.log"
scripts/test-python-installed-client.sh "$STAGING/python/"*.whl 2>&1 | tee "$EVIDENCE/python-installed.log"
PACIFICDB_TEST_ENGINE_BUILD= scripts/test-python-installed-client.sh "$STAGING/python/"*.tar.gz 2>&1 | tee "$EVIDENCE/python-sdist-installed.log"
scripts/test-java-installed-client.sh "$STAGING/java" 2>&1 | tee "$EVIDENCE/java-installed.log"
python3 scripts/verify-sdk-packages.py --npm-dist "$STAGING/npm" --python-dist "$STAGING/python" --java-target "$STAGING/java" --allow-unsigned-local --hash-manifest "$EVIDENCE/before-installed.json" --output "$EVIDENCE/after-installed.json"
python3 - "$EVIDENCE" <<'PY'
import json,os,sys
from pathlib import Path
root=Path(sys.argv[1]);inspection=json.loads((root/'after-installed.json').read_text())
(root/'installed-summary.json').write_text(json.dumps({'status':'PASS','version':inspection['version'],'git_revision':inspection['git_revision'],'source_clean':inspection['source_clean'],'live_engine':'PASS' if os.environ.get('PACIFICDB_TEST_ENGINE_BUILD') else 'Not tested','artifacts':inspection['artifacts'],'logs':['node-installed.log','python-installed.log','java-installed.log','python-sdist-installed.log'],'publication':'not performed'},indent=2)+'\n')
PY
