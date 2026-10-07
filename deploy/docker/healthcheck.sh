#!/bin/sh
# One bounded JSON reply; reusable engine sockets need not close to pass a probe.
exec python3 - "$@" <<'PY'
import json
import os
import socket
import ssl
import sys
import time
from pathlib import Path

kind = sys.argv[1] if len(sys.argv) > 1 else 'liveness'
try:
    if kind not in ('startup', 'readiness', 'liveness'):
        raise ValueError('unknown probe')
    host = os.getenv('POD_IP') or os.getenv('ENGINE_BIND_HOST', '127.0.0.1')
    if host in ('0.0.0.0', '*'):
        host = '127.0.0.1'
    timeout = float(os.getenv('PACIFICDB_HEALTH_TIMEOUT_SECONDS', '4'))
    if not 0 < timeout <= 10:
        raise ValueError('invalid timeout')
    deadline = time.monotonic() + timeout
    production = os.getenv('PACIFICDB_ENVIRONMENT', 'development') == 'production'
    request = {'action': 'ping'}
    if production and kind == 'readiness':
        with open(os.getenv('PACIFICDB_HEALTH_TOKEN_FILE', '/etc/pacificdb/audit-monitor/token')) as token_file:
            token = token_file.read(1025).strip()
        if not token or len(token) > 1024:
            raise ValueError('invalid credential')
        request = {'action': 'security_metrics', 'token': token}
    with socket.create_connection((host, int(os.getenv('ENGINE_PORT', '9000'))), timeout=timeout) as raw:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ValueError('deadline exceeded')
        raw.settimeout(remaining)
        connection = raw
        if production:
            tls_dir = Path(os.getenv('PACIFICDB_HEALTH_TLS_DIR', '/etc/pacificdb/client-tls'))
            context = ssl.create_default_context(cafile=str(tls_dir / 'ca.crt'))
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(str(tls_dir / 'health.crt'), str(tls_dir / 'health.key'))
            connection = context.wrap_socket(raw, server_hostname=os.getenv('PACIFICDB_TLS_SERVER_NAME', 'pacificdb'))
        with connection:
            connection.settimeout(max(0.001, deadline - time.monotonic()))
            connection.sendall(json.dumps(request).encode() + b'\n')
            reply = bytearray()
            while b'\n' not in reply:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or len(reply) >= 65536:
                    raise ValueError('bounded response exceeded')
                connection.settimeout(remaining)
                chunk = connection.recv(min(4096, 65536 - len(reply)))
                if not chunk:
                    raise ValueError('incomplete response')
                reply.extend(chunk)
            value = json.loads(reply.split(b'\n', 1)[0])
            healthy = (all(value.get(field) is True for field in ('success', 'auditHealthy', 'auditLoggingEnabled'))
                       if production and kind == 'readiness' else value.get('status') == 'pong')
            if not healthy:
                raise ValueError('unhealthy')
except Exception:
    print(f'{kind} probe failed', file=sys.stderr)
    sys.exit(1)
PY
