#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${1:-$HERE/.env}"
[[ -f "$ENV_FILE" ]] || { echo "missing config: $ENV_FILE" >&2; exit 1; }
set -a
. "$ENV_FILE"
set +a
exec "$HERE/build/db_engine"
