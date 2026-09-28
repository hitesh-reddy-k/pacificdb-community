import { execFile, spawn } from 'node:child_process';
import { mkdir, open } from 'node:fs/promises';
import net from 'node:net';
import path from 'node:path';
import { PacificDBClient } from '@pacificdb/client';

async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const { port } = server.address();
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return port;
}

// The desktop owns its engine and uses a separate data directory from the CLI.
// Closing this handle flushes the engine; it never signals an unrelated process.
export async function startDesktopEngine({ executable, directory, signal, timeoutMs = 30_000 }) {
  await Promise.all(['data', 'backup', 'restore'].map((name) =>
    mkdir(path.join(directory, name), { recursive: true, mode: 0o700 })));
  const port = await freePort();
  let raftPort = await freePort();
  while (raftPort === port) raftPort = await freePort();
  const logPath = path.join(directory, 'engine.log');
  const log = await open(logPath, 'a+', 0o600);
  const inherited = { ...process.env };
  // Desktop configuration must not import a terminal's engine configuration.
  const configuration = /^(?:PACIFICDB_|PACIFIC_|ENGINE_|DATA_|WAL_|LSM_|SST_|SNAPSHOT_|BACKUP_|RESTORE_|TMP_|LOG_|RAFT_|TLS_|RBAC_|JWT_|API_KEYS_|AUDIT_|ENCRYPTION_|MEMTABLE_|BLOCK_CACHE_|COLUMN_|INDEX_|MAX_|LEVEL_|COMPACTION_|BLOOM_|QUERY_|SLOW_|CONN_|DBQ_|MULTI_|TENANT_|DEFAULT_TENANT_|PROMETHEUS_|TRACING_|HEALTH_|READY_|LIVE_|STRUCTURED_|REQUEST_|ADMISSION_|ANTI_|DEGRADED_|OOM_|DISK_|TXN_|ENABLE_|MIN_QUORUM_|PRESIGNED_|FAILURE_DETECTOR_|HEARTBEAT_MESH_|REGION|ZONE|NODE_NAME|CLUSTER_NAME|READ_ONLY_MODE|IN_MEMORY_OLTP|MEMORY_PRESSURE_THRESHOLD_PCT|MEMORY_BACKPRESSURE_ENABLED|METRICS_PORT|METRICS_INTERVAL_MS|NODE_ENV)/i;
  for (const name of Object.keys(inherited)) if (configuration.test(name)) delete inherited[name];
  const child = spawn(executable, [], {
    cwd: directory, windowsHide: true, stdio: ['ignore', log.fd, log.fd],
    env: { ...inherited,
      PACIFICDB_HOME: directory, PACIFICDB_ENVIRONMENT: 'development',
      DATA_ROOT: path.join(directory, 'data'), BACKUP_ROOT: path.join(directory, 'backup'),
      RESTORE_DIR: path.join(directory, 'restore'), ENGINE_BIND_HOST: '127.0.0.1',
      ENGINE_PORT: String(port), ENGINE_AUTH_REQUIRED: '0',
      TLS_ENABLED: '0', RAFT_PEERS: '',
      RAFT_LISTEN_PORT: String(raftPort), RAFT_CLUSTER_ID: 'pacificdb-desktop',
      RAFT_NODE_ID: 'desktop-1', RAFT_IS_LEADER: '1', MIN_QUORUM_SIZE: '1',
      ENGINE_CPU_CORES: '2', ENGINE_KEEPALIVE_MAX_REQUESTS: '10000',
    },
  });
  let spawnError;
  const exited = new Promise((resolve) => {
    child.once('error', (error) => { spawnError = error; resolve(); });
    child.once('exit', resolve);
  });
  let stopPromise;
  function stop() {
    if (stopPromise) return stopPromise;
    stopPromise = (async () => {
      if (!spawnError && child.exitCode === null && child.signalCode === null) {
        const deadline = setTimeout(() => child.kill('SIGKILL'), 35_000);
        try {
          if (process.platform === 'win32') {
            // Node's SIGINT forcibly terminates Windows processes. The native CLI
            // checks this engine's identity and signals its graceful shutdown event.
            await new Promise((resolve) => execFile(path.join(path.dirname(executable), 'pacificdb.exe'),
              ['stop', '--host', '127.0.0.1', '--port', String(port), '--no-start'], {
                cwd: directory, windowsHide: true, timeout: 30_000,
                env: { ...inherited, PACIFICDB_HOME: directory, PACIFICDB_ENGINE: executable },
              }, (error) => {
                if (error && child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
                resolve();
              }));
          } else child.kill('SIGINT');
          await exited;
        } finally { clearTimeout(deadline); }
      }
    })();
    return stopPromise;
  }
  const cancel = () => { void stop(); };
  signal?.addEventListener('abort', cancel, { once: true });
  const client = new PacificDBClient({ host: '127.0.0.1', port, poolSize: 1, timeoutMs: 400 });
  try {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (signal?.aborted) throw new Error('Startup canceled');
      if (spawnError) throw spawnError;
      if (child.exitCode !== null || child.signalCode !== null) throw new Error('The database engine stopped during startup');
      try {
        if ((await client.request({ action: 'ping' })).status === 'pong') {
          signal?.removeEventListener('abort', cancel);
          return { port, pid: child.pid, logPath, stop, exited };
        }
      } catch { /* A process can be alive before its listener is ready. */ }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error('The database did not become ready within 30 seconds');
  } catch (error) {
    await stop();
    const tail = await (async () => {
      const size = (await log.stat()).size;
      const bytes = Buffer.alloc(Math.min(size, 8192));
      await log.read(bytes, 0, bytes.length, size - bytes.length);
      return bytes.toString('utf8').slice(-2000);
    })().catch(() => '');
    throw new Error(`${error.message}\n\nEngine log: ${logPath}${tail ? `\n${tail}` : ''}`);
  } finally {
    await log.close();
    client.close();
    signal?.removeEventListener('abort', cancel);
  }
}
