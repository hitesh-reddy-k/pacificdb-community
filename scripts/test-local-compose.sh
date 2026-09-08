#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

project="pacificdb-community-smoke-$$"
cleanup() { docker compose -p "$project" down -v >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker compose -p "$project" up -d --build database
pdb() { docker compose -p "$project" run --rm -T shell "$@"; }
pdb ping --host database --port 9000 >/dev/null
pdb request '{"action":"createDatabase","dbName":"quickstart"}' --host database >/dev/null
pdb request '{"action":"createCollection","collection":"users"}' --host database --database quickstart >/dev/null
pdb request '{"action":"insert","collection":"users","data":{"id":"1","name":"Ada"}}' --host database --database quickstart >/dev/null
result=$(pdb request '{"action":"find","collection":"users","filter":{"id":"1"}}' --host database --database quickstart)
grep -q '"name": "Ada"' <<<"$result"
printf 'PacificDB local Compose CRUD passed\n'
