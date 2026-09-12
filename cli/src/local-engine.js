import net from 'node:net';
import { open, mkdir, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

function isLocalHost(host) {
  return ['127.0.0.1', 'localhost', '::1'].includes(host);
}

function dataHome() {
  if (process.env.PACIFICDB_HOME) return path.resolve(process.env.PACIFICDB_HOME);
  if (process.platform === 'win32') {
    return path.join(process.env.LOCALAPPDATA || os.homedir(), 'PacificDB');
  }
  if (process.platform === 'darwin') {
    return path.join(os.homedir(), 'Library', 'Application Support', 'PacificDB');
  }
  return path.join(process.env.XDG_DATA_HOME || path.join(os.homedir(), '.local', 'share'),
    'pacificdb');
}

function canConnect(host, port, timeoutMs = 300) {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host, port });
    const done = (connected) => { socket.destroy(); resolve(connected); };
    socket.setTimeout(timeoutMs, () => done(false));
    socket.once('connect', () => done(true));
    socket.once('error', () => done(false));
  });
}

function protocolReady(host, port, timeoutMs = 1000) {
  return new Promise((resolve) => {
    let response = '';
    const socket = net.createConnection({ host, port });
    const done = (ready) => { socket.destroy(); resolve(ready); };
    socket.setTimeout(timeoutMs, () => done(false));
    socket.once('error', () => done(false));
    socket.once('connect', () => socket.write('{"action":"ping","userId":"system"}\n'));
    socket.on('data', (chunk) => {
      response += chunk;
      const newline = response.indexOf('\n');
      if (newline < 0) return;
      try { done(JSON.parse(response.slice(0, newline)).status === 'pong'); }
      catch { done(false); }
    });
    socket.once('end', () => done(false));
  });
}

function localEnvironment(home, port) {
  const raftPort = Number(port) === 9000 ? 9100 : Math.min(Number(port) + 1, 65535);
  return { ...process.env,
    PACIFICDB_HOME: home,
    PACIFICDB_ENVIRONMENT: process.env.PACIFICDB_ENVIRONMENT || 'development',
    DATA_ROOT: process.env.DATA_ROOT || path.join(home, 'data'),
    BACKUP_ROOT: process.env.BACKUP_ROOT || path.join(home, 'backup'),
    RESTORE_DIR: process.env.RESTORE_DIR || path.join(home, 'restore'),
    ENGINE_BIND_HOST: '127.0.0.1',
    ENGINE_AUTH_REQUIRED: process.env.ENGINE_AUTH_REQUIRED || '0',
    ENGINE_PORT: String(port),
    RAFT_LISTEN_PORT: process.env.RAFT_LISTEN_PORT || String(raftPort),
    RAFT_CLUSTER_ID: process.env.RAFT_CLUSTER_ID || 'pacificdb-local',
    RAFT_NODE_ID: process.env.RAFT_NODE_ID || 'node-1',
    RAFT_IS_LEADER: process.env.RAFT_IS_LEADER || '1',
    MIN_QUORUM_SIZE: process.env.MIN_QUORUM_SIZE || '1',
    ENGINE_KEEPALIVE_MAX_REQUESTS: process.env.ENGINE_KEEPALIVE_MAX_REQUESTS || '1'
  };
}

export async function ensureLocalEngine(client, { autoStart = true, output } = {}) {
  if (!autoStart || !isLocalHost(client.host)) return false;
  if (await protocolReady(client.host, client.port)) return false;
  if (await canConnect(client.host, client.port)) {
    throw new Error(`Port ${client.port} is in use by a service that is not PacificDB; ` +
      'choose another --port');
  }

  const home = dataHome();
  const environment = localEnvironment(home, client.port);
  await Promise.all([
    mkdir(environment.DATA_ROOT, { recursive: true, mode: 0o700 }),
    mkdir(environment.BACKUP_ROOT, { recursive: true, mode: 0o700 }),
    mkdir(environment.RESTORE_DIR, { recursive: true, mode: 0o700 })
  ]);
  const startLock = path.join(home, '.engine-starting');
  try {
    await mkdir(startLock);
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
    for (let attempt = 0; attempt < 300; attempt += 1) {
      if (await protocolReady(client.host, client.port)) return false;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error('Another PacificDB process did not finish starting the local engine');
  }
  try {
    const log = await open(path.join(home, 'engine.log'), 'a', 0o600);
    const engine = process.env.PACIFICDB_ENGINE ||
      (process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
    const child = spawn(engine, [], { detached: true, windowsHide: true,
      env: environment, stdio: ['ignore', log.fd, log.fd] });
    try {
      await new Promise((resolve, reject) => {
        child.once('spawn', resolve);
        child.once('error', reject);
      });
    } catch (error) {
      throw new Error(`Local engine is not installed (${error.message}). ` +
        'Install the PacificDB Community package or use --host.');
    } finally {
      await log.close();
    }
    child.unref();
    await writeFile(path.join(home, 'engine.pid'), `${child.pid}\n`, { mode: 0o600 });

    for (let attempt = 0; attempt < 300; attempt += 1) {
      if (await protocolReady(client.host, client.port)) {
        output?.write(`✓ Local engine started at ${client.host}:${client.port}\n`);
        return true;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error(`Local engine did not start. See ${path.join(home, 'engine.log')}`);
  } finally {
    await rm(startLock, { recursive: true, force: true });
  }
}
