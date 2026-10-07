#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, rm } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';
import { RaftTestCluster } from './lib/raft-test-cluster.mjs';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const buildRoot = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const engineBinary = path.join(buildRoot,
  process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-raft-frame-'));

async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function rssKb(pid) {
  if (process.platform !== 'linux') return null;
  const status = await readFile(`/proc/${pid}/status`, 'utf8');
  return Number(status.match(/^VmRSS:\s+(\d+)\s+kB$/m)?.[1] || 0);
}

const [port, raftPort] = await Promise.all([freePort(), freePort()]);
for (const directory of ['data', 'backup', 'restore']) {
  await mkdir(path.join(root, directory), { recursive: true, mode: 0o700 });
}
const logPath = path.join(root, 'engine.log');
const log = createWriteStream(logPath, { mode: 0o600 });
const engine = spawn(engineBinary, [], {
  cwd: repositoryRoot,
  env: {
    ...process.env,
    PACIFICDB_ENVIRONMENT: 'development', PACIFICDB_HOME: root,
    DATA_ROOT: path.join(root, 'data'), BACKUP_ROOT: path.join(root, 'backup'),
    RESTORE_DIR: path.join(root, 'restore'), ENGINE_BIND_HOST: '127.0.0.1',
    ENGINE_PORT: String(port), ENGINE_AUTH_REQUIRED: '0',
    RAFT_BIND_HOST: '127.0.0.1', RAFT_LISTEN_PORT: String(raftPort),
    RAFT_CLUSTER_ID: 'raft-frame-limit', RAFT_NODE_ID: 'node-1',
    RAFT_IS_LEADER: '1', MIN_QUORUM_SIZE: '1', ENGINE_CPU_CORES: '2',
    CONN_MIN_THREADS: '2', CONN_MAX_THREADS: '4', MAX_CONNECTIONS: '16',
    ADAPTIVE_ADMISSION: '0', RAFT_INBOUND_IDLE_TIMEOUT_MS: '1000',
  },
  stdio: ['ignore', 'pipe', 'pipe'],
});
engine.stdout.pipe(log, { end: false });
engine.stderr.pipe(log, { end: false });

async function stop() {
  if (engine.exitCode === null && engine.signalCode === null) {
    engine.kill('SIGINT');
    const forced = setTimeout(() => engine.kill('SIGKILL'), 15000);
    await once(engine, 'exit');
    clearTimeout(forced);
  }
  log.end();
  if (!log.writableFinished) await once(log, 'finish');
}

const client = new PacificDBClient({ port, timeoutMs: 300, poolSize: 1 });
try {
  for (let attempt = 0; attempt < 150; attempt += 1) {
    try {
      if ((await client.request({ action: 'ping' })).status === 'pong') break;
    } catch { /* still starting */ }
    if (attempt === 149) throw new Error(await readFile(logPath, 'utf8'));
    await new Promise((resolve) => setTimeout(resolve, 100));
  }

  const before = await rssKb(engine.pid);
  const fragmented = net.createConnection({ host: '127.0.0.1', port: raftPort });
  await once(fragmented, 'connect');
  const heartbeat = Buffer.from(JSON.stringify({
    type: 'heartbeat', clusterId: 'wrong-cluster', term: 0, leader: 'test'
  }));
  const heartbeatHeader = Buffer.alloc(4);
  heartbeatHeader.writeUInt32LE(heartbeat.length);
  const fragmentedResponse = new Promise((resolve, reject) => {
    const deadline = setTimeout(() => reject(new Error(
      'fragmented legal Raft frame timed out')), 2000);
    fragmented.once('data', (data) => { clearTimeout(deadline); resolve(data.toString()); });
    fragmented.once('close', () => { clearTimeout(deadline); reject(new Error(
      'fragmented legal Raft frame was closed')); });
  });
  fragmented.write(heartbeatHeader.subarray(0, 2));
  await new Promise((resolve) => setTimeout(resolve, 25));
  fragmented.write(heartbeatHeader.subarray(2));
  fragmented.write(heartbeat.subarray(0, 3));
  await new Promise((resolve) => setTimeout(resolve, 25));
  fragmented.write(heartbeat.subarray(3));
  assert.match(await fragmentedResponse, /cluster_mismatch/);
  fragmented.destroy();

  const socket = net.createConnection({ host: '127.0.0.1', port: raftPort });
  socket.on('error', () => {});
  await once(socket, 'connect');
  const header = Buffer.alloc(4);
  header.writeUInt32LE(64 * 1024 * 1024 + 1);
  const started = Date.now();
  socket.write(header);
  let closed = false;
  const close = once(socket, 'close').then(() => { closed = true; });
  let peak = before;
  while (!closed && Date.now() - started < 1500) {
    await new Promise((resolve) => setTimeout(resolve, 20));
    const current = await rssKb(engine.pid);
    if (current !== null) peak = Math.max(peak, current);
  }
  await Promise.race([
    close,
    new Promise((_, reject) => setTimeout(() => reject(
      new Error('oversized Raft frame was not rejected promptly')), 2000)),
  ]);
  assert.ok(Date.now() - started < 750, 'oversized header must be rejected before body wait');
  if (before !== null) {
    assert.ok(peak - before < 16 * 1024,
      `oversized header increased RSS by ${peak - before} KiB`);
  }
  assert.equal((await client.request({ action: 'ping' })).status, 'pong');
} finally {
  client.close();
  await stop();
  await rm(root, { recursive: true, force: true });
}

const cluster = await RaftTestCluster.create({
  build: buildRoot, clusterId: `raft-frame-catchup-${process.pid}`,
  useProxies: false, allEligible: false, rootPrefix: 'pacificdb-raft-frame-catchup-'
});
try {
  await cluster.start();
  const leader = cluster.client(0, { timeoutMs: 15000 });
  await leader.createDatabase('frame_limit');
  leader.database = 'frame_limit';
  await leader.createCollection('docs');
  await cluster.stopNode(2);
  const value = 'x'.repeat(8 * 1024 * 1024);
  for (let index = 0; index < 9; index += 1) {
    await leader.insert('docs', { id: `large-${index}`, value });
  }
  await cluster.startNode(2);
  const follower = cluster.client(2, { timeoutMs: 15000 });
  follower.database = 'frame_limit';
  await cluster.retry(async () => {
    const result = await follower.find('docs', { id: 'large-8' }, { limit: 1 });
    assert.equal(result.data[0]?.value.length, value.length);
  }, 'byte-bounded follower catchup', 200, 100);
  leader.close();
  follower.close();
} finally {
  await cluster.close();
}

console.log('RAFT_FRAME_LIMIT_PASS');
