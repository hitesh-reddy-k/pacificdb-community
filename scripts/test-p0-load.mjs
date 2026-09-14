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

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const engineBinary = path.join(build, process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const cliBinary = path.join(build, process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-p0-load-'));
const includeSoak = process.env.PACIFICDB_P0_SOAK === '1';
const writes = Number(includeSoak ? (process.env.PACIFICDB_P0_SOAK_WRITES || 100000) :
  (process.env.PACIFICDB_P0_WRITES || 10000));
const retainedRecords = Number(includeSoak
  ? (process.env.PACIFICDB_P0_SOAK_RECORDS || 256) : writes);
const clientCount = Number(includeSoak ? 16 :
  (process.env.PACIFICDB_P0_CLIENTS || 8));
assert.ok(Number.isSafeInteger(writes) && writes > 0);
assert.ok(Number.isSafeInteger(retainedRecords) && retainedRecords > 0 &&
  retainedRecords <= writes);
assert.ok(Number.isSafeInteger(clientCount) && clientCount > 0 && clientCount <= 128);

async function withLocalPortRetry(operation) {
  const deadline = Date.now() + 120000;
  let delay = 25;
  while (true) {
    try {
      return await operation();
    } catch (error) {
      if (error?.code !== 'EADDRNOTAVAIL' || Date.now() >= deadline) throw error;
      await new Promise((resolve) => setTimeout(resolve, delay));
      delay = Math.min(delay * 2, 1000);
    }
  }
}

async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => server.once('error', reject)
    .listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}
const port = await freePort();
const raftPort = await freePort();
for (const directory of ['data', 'backup', 'restore'])
  await mkdir(path.join(root, directory), { recursive: true, mode: 0o700 });
const env = { ...process.env, PACIFICDB_HOME: root, PACIFICDB_ENVIRONMENT: 'development',
  DATA_ROOT: path.join(root, 'data'), BACKUP_ROOT: path.join(root, 'backup'),
  RESTORE_DIR: path.join(root, 'restore'), ENGINE_BIND_HOST: '127.0.0.1',
  ENGINE_PORT: String(port), ENGINE_AUTH_REQUIRED: '0', RAFT_CLUSTER_ID: 'p0-load',
  RAFT_NODE_ID: 'node-1', RAFT_LISTEN_PORT: String(raftPort), RAFT_IS_LEADER: '1',
  MIN_QUORUM_SIZE: '1', ENGINE_CPU_CORES: '2', CONN_MIN_THREADS: '4',
  CONN_MAX_THREADS: '24', CONN_MAX_QUEUE: '4096', MAX_CONNECTIONS: '256',
  ADAPTIVE_ADMISSION: '0', DBQ_SHARDS: '4', DBQ_WORKERS_PER_SHARD: '2',
  DBQ_MAX_QUEUE_PER_SHARD: '4096' };
let engine;
let log;
let pipeError;
async function start() {
  pipeError = undefined;
  log = createWriteStream(path.join(root, 'engine.log'), { flags: 'a', mode: 0o600 });
  engine = spawn(engineBinary, [], { cwd: repositoryRoot, env,
    stdio: ['ignore', 'pipe', 'pipe'] });
  engine.stdout.pipe(log, { end: false });
  engine.stderr.pipe(log, { end: false });
  if (process.platform === 'win32') {
    engine.stdout.on('error', (error) => {
      if (error.code !== 'ECONNRESET') pipeError ||= error;
    });
    engine.stderr.on('error', (error) => {
      if (error.code !== 'ECONNRESET') pipeError ||= error;
    });
  }
  const probe = new PacificDBClient({ port, timeoutMs: 500 });
  for (let attempt = 0; attempt < 240; attempt += 1) {
    if (pipeError) throw pipeError;
    if (engine.exitCode !== null) throw new Error(`engine exited with ${engine.exitCode}`);
    try { if ((await probe.request({ action: 'ping' })).status === 'pong') return; }
    catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('engine startup timed out');
}
async function stop() {
  if (!engine || engine.exitCode !== null) return;
  const stopper = spawn(cliBinary, ['--port', String(port), 'stop'], {
    cwd: repositoryRoot, env, stdio: 'ignore',
  });
  const [code] = await once(stopper, 'exit');
  if (code !== 0) throw new Error(`pacificdb stop exited ${code}`);
  if (engine.exitCode === null) {
    const timer = setTimeout(() => engine.kill('SIGKILL'), 35000);
    await once(engine, 'exit');
    clearTimeout(timer);
  }
  log.end();
  await once(log, 'finish');
  if (pipeError) throw pipeError;
}
async function exactIds() {
  const client = new PacificDBClient({ port, database: 'load', timeoutMs: 60000 });
  const response = await withLocalPortRetry(() =>
    client.find('records', {}, { limit: -1 }));
  return response.data.map((document) => document.id).sort();
}

const expected = Array.from({ length: retainedRecords }, (_, index) =>
  `load-${String(index).padStart(8, '0')}`);
const startedAt = Date.now();
try {
  await start();
  const originalPid = engine.pid;
  const setup = new PacificDBClient({ port, timeoutMs: 30000 });
  await setup.createDatabase('load');
  setup.database = 'load';
  await setup.createCollection('records');
  const clients = Array.from({ length: clientCount }, () =>
    new PacificDBClient({ port, database: 'load', timeoutMs: 60000 }));
  let acknowledged = 0;
  await Promise.all(clients.map(async (client, worker) => {
    for (let index = worker; index < writes; index += clientCount) {
      const id = expected[index % retainedRecords];
      const result = index < retainedRecords
        ? await withLocalPortRetry(() =>
          client.insert('records', { id, index, worker, value: 1 }))
        : await withLocalPortRetry(() =>
          client.updateOne('records', { id }, { index, worker, value: 2 }));
      assert.equal(result.status, index < retainedRecords ? 'ok' : 'updated');
      acknowledged += 1;
      if (includeSoak && index % 100 === 0)
        assert.equal((await withLocalPortRetry(() =>
          client.find('records', { id }, { limit: 1 }))).count, 1);
    }
  }));
  assert.equal(engine.pid, originalPid);
  assert.equal(engine.exitCode, null);
  assert.equal(acknowledged, writes);
  assert.deepEqual(await exactIds(), expected);
  await stop();
  await start();
  assert.deepEqual(await exactIds(), expected);
  const metadata = JSON.parse(await readFile(path.join(root, 'engine.metadata.json'), 'utf8'));
  console.log(JSON.stringify({ status: 'PASS', writes, retained_records: retainedRecords,
    clients: clientCount, acknowledged, elapsed_ms: Date.now() - startedAt,
    initial_pid: originalPid, recovery_pid: engine.pid,
    data_root_fingerprint: metadata.data_root_fingerprint }));
} finally {
  await stop().catch(() => {});
  if (process.env.PACIFICDB_KEEP_P0_ROOT !== '1')
    await rm(root, { recursive: true, force: true });
  else console.error(`preserved test root: ${root}`);
}
