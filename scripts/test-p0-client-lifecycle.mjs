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
const buildRoot = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const engineBinary = path.join(
  buildRoot, process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const cliBinary = path.join(
  buildRoot, process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb');
const testRoot = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-p0-lifecycle-'));
const engineLog = path.join(testRoot, 'engine.log');
const requestedWrites = Number(process.env.PACIFICDB_P0_WRITES || 1000);
const abruptEvery = Number(process.env.PACIFICDB_P0_ABRUPT_EVERY ?? 250);
assert.ok(Number.isSafeInteger(requestedWrites) && requestedWrites >= 80,
  'PACIFICDB_P0_WRITES must be an integer of at least 80');
assert.ok(Number.isSafeInteger(abruptEvery) && abruptEvery >= 0,
  'PACIFICDB_P0_ABRUPT_EVERY must be a non-negative integer');
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

const port = await freePort();
const raftPort = await freePort();
const directories = {
  data: path.join(testRoot, 'data'),
  backup: path.join(testRoot, 'backup'),
  restore: path.join(testRoot, 'restore'),
};
await Promise.all(Object.values(directories).map((directory) =>
  mkdir(directory, { recursive: true, mode: 0o700 })));

const environment = {
  ...process.env,
  PACIFICDB_HOME: testRoot,
  PACIFICDB_ENVIRONMENT: 'development',
  DATA_ROOT: directories.data,
  BACKUP_ROOT: directories.backup,
  RESTORE_DIR: directories.restore,
  ENGINE_BIND_HOST: '127.0.0.1',
  ENGINE_PORT: String(port),
  ENGINE_AUTH_REQUIRED: '0',
  RAFT_CLUSTER_ID: 'p0-client-lifecycle',
  RAFT_NODE_ID: 'node-1',
  RAFT_LISTEN_PORT: String(raftPort),
  RAFT_IS_LEADER: '1',
  MIN_QUORUM_SIZE: '1',
  ENGINE_CPU_CORES: '2',
  CONN_MIN_THREADS: '4',
  CONN_MAX_THREADS: '32',
  CONN_MAX_QUEUE: String(Math.max(16384, requestedWrites + 2048)),
  MAX_CONNECTIONS: '128',
  // This is a durability/lifecycle gate, not an admission-shedding gate.
  // Abrupt clients intentionally leave valid work queued behind closed peers;
  // let acknowledged clients wait for their result instead of converting a
  // healthy backlog into a queue-wait rejection.
  MAX_QUEUE_WAIT_MS: '0',
  ADAPTIVE_ADMISSION: '0',
  ENGINE_KEEPALIVE_MAX_REQUESTS: '1',
  DBQ_SHARDS: '4',
  DBQ_WORKERS_PER_SHARD: '2',
  DBQ_MAX_QUEUE_PER_SHARD: '2048',
};

const log = createWriteStream(engineLog, { flags: 'a', mode: 0o600 });
const engine = spawn(engineBinary, [], {
  cwd: repositoryRoot,
  env: environment,
  stdio: ['ignore', 'pipe', 'pipe'],
});
engine.stdout.pipe(log, { end: false });
engine.stderr.pipe(log, { end: false });
let pipeError;
for (const stream of [engine.stdout, engine.stderr]) {
  stream.on('error', (error) => {
    if (!(process.platform === 'win32' && error.code === 'ECONNRESET')) {
      pipeError ||= error;
    }
  });
}
log.on('error', (error) => { pipeError ||= error; });
let logClosed = false;

async function waitReady() {
  const probe = new PacificDBClient({ port, timeoutMs: 500 });
  let lastError;
  for (let attempt = 0; attempt < 180; attempt += 1) {
    if (engine.exitCode !== null) {
      throw new Error(`engine exited during startup with code ${engine.exitCode}`);
    }
    try {
      if ((await probe.request({ action: 'ping' })).status === 'pong') return;
    } catch (error) { lastError = error; }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`engine did not become ready: ${lastError?.message || 'unknown error'}`);
}

function abruptlyDisconnect(command) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: '127.0.0.1', port });
    let settled = false;
    const finish = (error) => {
      if (settled) return;
      settled = true;
      if (!error || ['ECONNRESET', 'ECONNABORTED', 'EPIPE'].includes(error.code)) {
        setTimeout(resolve, 50);
      } else reject(error);
    };
    socket.on('error', finish);
    socket.once('connect', () => {
      socket.write(JSON.stringify(command) + '\n', (error) => {
        if (error) finish(error);
        else {
          if (typeof socket.resetAndDestroy === 'function') socket.resetAndDestroy();
          else socket.destroy();
          // Keep the reset workload concurrent without creating a zero-delay
          // reset storm that can trigger host TCP/WFP rate protection on
          // Windows before the engine gets a chance to dequeue the requests.
          finish();
        }
      });
    });
  });
}

function processLive(pid) {
  try { process.kill(pid, 0); return true; }
  catch (error) { return error.code === 'EPERM'; }
}

async function stopEngine() {
  if (logClosed) return;
  if (engine.exitCode === null) {
    const stop = spawn(cliBinary, ['--port', String(port), 'stop'], {
      cwd: repositoryRoot, env: environment, stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stopOutput = '';
    let stopPipeError;
    stop.stdout.on('data', (chunk) => { stopOutput += chunk; });
    stop.stderr.on('data', (chunk) => { stopOutput += chunk; });
    for (const stream of [stop.stdout, stop.stderr]) {
      stream.on('error', (error) => {
        if (!(process.platform === 'win32' && error.code === 'ECONNRESET')) {
          stopPipeError ||= error;
        }
      });
    }
    const [stopCode] = await once(stop, 'exit');
    if (stopPipeError) throw stopPipeError;
    if (stopCode !== 0) throw new Error(`pacificdb stop failed: ${stopOutput}`);
    if (engine.exitCode === null) {
      const forced = setTimeout(() => engine.kill('SIGKILL'), 35000);
      await once(engine, 'exit');
      clearTimeout(forced);
    }
  }
  logClosed = true;
  log.end();
  await once(log, 'finish');
}

try {
  await waitReady();
  const setup = new PacificDBClient({ port, timeoutMs: 120000 });
  await setup.createDatabase('lifecycle');
  setup.database = 'lifecycle';
  await setup.createCollection('events');

  const cycleCount = 5;
  const clientCount = 16;
  let issued = 0;
  let acknowledged = 0;
  for (let cycle = 0; cycle < cycleCount; cycle += 1) {
    const writesThisCycle = Math.floor(requestedWrites / cycleCount) +
      (cycle < requestedWrites % cycleCount ? 1 : 0);
    const clients = Array.from({ length: clientCount }, () =>
      new PacificDBClient({ port, database: 'lifecycle', timeoutMs: 120000 }));
    const workers = clients.map(async (client, clientIndex) => {
      for (let offset = clientIndex; offset < writesThisCycle; offset += clientCount) {
        const sequence = issued + offset;
        const id = `cycle-${cycle}-client-${clientIndex}-write-${sequence}`;
        try {
          if (abruptEvery === 0 || sequence % abruptEvery !== 0) {
            const result = await client.insert('events', {
              id, cycle, client: clientIndex, acknowledged: true,
            });
            assert.equal(result.status, 'ok');
            acknowledged += 1;
          } else {
            await abruptlyDisconnect({
              action: 'insert', userId: 'system', dbName: 'lifecycle',
              collection: 'events',
              data: { id, cycle, client: clientIndex, acknowledged: false },
            });
          }
        } catch (error) {
          error.message = `${error.message} (cycle=${cycle}, client=${clientIndex}, ` +
            `offset=${offset}, sequence=${sequence})`;
          throw error;
        }
      }
    });
    await Promise.all(workers);
    issued += writesThisCycle;

    assert.equal(engine.exitCode, null, `engine exited during cycle ${cycle}`);
    assert.equal(processLive(engine.pid), true, `PID ${engine.pid} died during cycle ${cycle}`);
    const reconnect = new PacificDBClient({
      port, database: 'lifecycle', timeoutMs: 120000,
    });
    assert.equal((await reconnect.request({ action: 'ping' })).status, 'pong');
    const found = await reconnect.find('events', { cycle, acknowledged: true }, { limit: -1 });
    const expected = [];
    for (let offset = 0; offset < writesThisCycle; offset += 1) {
      const clientIndex = offset % clientCount;
      const sequence = issued - writesThisCycle + offset;
      if (abruptEvery === 0 || sequence % abruptEvery !== 0) {
        expected.push(`cycle-${cycle}-client-${clientIndex}-write-${sequence}`);
      }
    }
    assert.deepEqual(found.data.map((document) => document.id).sort(), expected.sort(),
      `acknowledged IDs changed during cycle ${cycle}`);
  }

  assert.equal(engine.exitCode, null);
  await stopEngine();
  if (pipeError) throw pipeError;
  const logText = await readFile(engineLog, 'utf8');
  const events = logText.split('\n').flatMap((line) => {
    if (!line.startsWith('{')) return [];
    try { return [JSON.parse(line)]; }
    catch { return []; }
  });
  const eventCodes = new Set(events.map((event) => event.code));
  for (const code of [
    'engine_starting', 'engine_ready', 'client_connected',
    'client_disconnected', 'engine_shutdown_requested',
    'engine_shutdown_complete',
  ]) assert.ok(eventCodes.has(code), `missing lifecycle event ${code}`);
  assert.equal(eventCodes.has('engine_fatal'), false);
  const processMetadata = JSON.parse(await readFile(
    path.join(testRoot, 'engine.metadata.json'), 'utf8'));
  assert.equal(logText.includes(processMetadata.discovery_nonce), false,
    'discovery nonce leaked into lifecycle logs');
  console.log(JSON.stringify({
    status: 'PASS', cycles: 5, clients_per_cycle: 16,
    writes: issued, acknowledged, pid: engine.pid,
  }));
} catch (error) {
  const diagnostic = await readFile(engineLog, 'utf8').catch(() => '');
  error.message += `\nengine pid=${engine.pid} exit=${engine.exitCode}\n` +
    `engine log:\n${diagnostic.split('\n').slice(-200).join('\n')}`;
  console.error(error.message);
  console.error(error.stack || '');
  process.exitCode = 1;
} finally {
  await stopEngine().catch(() => {});
  if (process.env.PACIFICDB_KEEP_P0_ROOT !== '1') {
    await rm(testRoot, { recursive: true, force: true });
  } else {
    console.error(`preserved test root: ${testRoot}`);
  }
}
