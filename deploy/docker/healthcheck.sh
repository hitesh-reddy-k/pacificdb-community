#!/bin/sh
set -eu

probe_kind=${1:-liveness}
host=${POD_IP:-127.0.0.1}
port=${ENGINE_PORT:-9000}
timeout_seconds=${PACIFICDB_HEALTH_TIMEOUT_SECONDS:-4}
request='{"action":"ping"}'

if [ "${PACIFICDB_ENVIRONMENT:-development}" = production ]; then
  tls_dir=${PACIFICDB_HEALTH_TLS_DIR:-/etc/pacificdb/client-tls}
  server_name=${PACIFICDB_TLS_SERVER_NAME:-pacificdb}
  for required_file in ca.crt health.crt health.key; do
    if [ ! -r "$tls_dir/$required_file" ]; then
      echo "$probe_kind probe cannot read $tls_dir/$required_file" >&2
      exit 1
    fi
  done
  response=$(
    printf '%s\n' "$request" |
      timeout "$timeout_seconds" openssl s_client -quiet \
        -connect "$host:$port" -servername "$server_name" \
        -verify_hostname "$server_name" -verify_return_error \
        -CAfile "$tls_dir/ca.crt" \
        -cert "$tls_dir/health.crt" -key "$tls_dir/health.key" 2>/dev/null
  )
else
  response=$(printf '%s\n' "$request" | nc -w "$timeout_seconds" "$host" "$port")
fi

printf '%s\n' "$response" | grep -Eq '"status"[[:space:]]*:[[:space:]]*"pong"'
