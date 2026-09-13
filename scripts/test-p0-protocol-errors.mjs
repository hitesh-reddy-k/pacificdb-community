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
const testRoot = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-p0-protocol-'));
const engineLog = path.join(testRoot, 'engine.log');
const maxBytes = 1024;
const trace = (name) => {
  if (process.env.PACIFICDB_P0_TRACE === '1') console.error(`[protocol] ${name}`);
};

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
  PACIFICDB_ENVIRONMENT: 'development',
  PACIFICDB_HOME: testRoot,
  DATA_ROOT: directories.data,
  BACKUP_ROOT: directories.backup,
  RESTORE_DIR: directories.restore,
  ENGINE_BIND_HOST: '127.0.0.1',
  ENGINE_PORT: String(port),
  ENGINE_AUTH_REQUIRED: '0',
  ENGINE_MAX_IN_MEMORY_PAYLOAD: String(maxBytes),
  ADAPTIVE_SOCKET_TIMEOUT: '0',
  SOCKET_RECV_TIMEOUT_SEC: '1',
  ENGINE_KEEPALIVE_IDLE_MS: '1000',
  ENGINE_KEEPALIVE_MAX_REQUESTS: '1',
  RAFT_CLUSTER_ID: 'p0-protocol',
  RAFT_NODE_ID: 'node-1',
  RAFT_LISTEN_PORT: String(raftPort),
  RAFT_IS_LEADER: '1',
  MIN_QUORUM_SIZE: '1',
  ENGINE_CPU_CORES: '2',
  CONN_MIN_THREADS: '2',
  CONN_MAX_THREADS: '4',
  CONN_MAX_QUEUE: '16',
  MAX_CONNECTIONS: '4',
  CONNECTION_LIMIT_WAIT_MS: '0',
  ADAPTIVE_ADMISSION: '0',
  PACIFICDB_INSTANCE_ID: 'engine_protocol_test',
  PACIFICDB_DISCOVERY_NONCE: 'nonce_protocol_test_0123456789',
  PACIFICDB_DATA_ROOT_FINGERPRINT: 'sha256:protocol-test-fingerprint',
};

const log = createWriteStream(engineLog, { flags: 'a', mode: 0o600 });
const engine = spawn(engineBinary, [], {
  cwd: repositoryRoot,
  env: environment,
  stdio: ['ignore', 'pipe', 'pipe'],
});
engine.stdout.pipe(log, { end: false });
engine.stderr.pipe(log, { end: false });

async function waitReady() {
  const probe = new PacificDBClient({ port, timeoutMs: 300 });
  let lastError;
  for (let attempt = 0; attempt < 150; attempt += 1) {
    if (engine.exitCode !== null) {
      throw new Error(`engine exited during startup with code ${engine.exitCode}`);
    }
    try {
      if ((await probe.request({ action: 'ping' })).status === 'pong') return;
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`engine did not become ready: ${lastError?.message || 'unknown error'}`);
}

function lineExchange(bytes, { timeoutMs = 5000 } = {}) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: '127.0.0.1', port });
    let response = Buffer.alloc(0);
    let settled = false;
    const finish = (callback, value) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      callback(value);
    };
    socket.setTimeout(timeoutMs, () => finish(reject,
      new Error('client timed out before a complete newline response')));
    socket.on('error', (error) => {
      trace(`line-socket-error:${error.code || 'unknown'}`);
      finish(reject, error);
    });
    socket.once('connect', () => socket.write(bytes));
    socket.on('data', (chunk) => {
      response = Buffer.concat([response, chunk]);
      const newline = response.indexOf(0x0a);
      if (newline < 0) return;
      try {
        finish(resolve, JSON.parse(response.subarray(0, newline).toString('utf8')));
      } catch (error) {
        finish(reject, error);
      }
    });
    socket.once('end', () => {
      if (!settled) finish(reject, new Error('EOF before a complete newline response'));
    });
  });
}

function decodeMessagePack(buffer) {
  let offset = 0;
  const read = () => {
    assert.ok(offset < buffer.length, 'truncated MessagePack value');
    const prefix = buffer[offset++];
    if (prefix <= 0x7f) return prefix;
    if ((prefix & 0xf0) === 0x80) {
      const entries = prefix & 0x0f;
      const value = {};
      for (let index = 0; index < entries; index += 1) value[read()] = read();
      return value;
    }
    if ((prefix & 0xe0) === 0xa0) {
      const length = prefix & 0x1f;
      const value = buffer.subarray(offset, offset + length).toString('utf8');
      offset += length;
      return value;
    }
    if (prefix === 0xcc) return buffer[offset++];
    if (prefix === 0xcd) {
      const value = buffer.readUInt16BE(offset); offset += 2; return value;
    }
    if (prefix === 0xce) {
      const value = buffer.readUInt32BE(offset); offset += 4; return value;
    }
    if (prefix === 0xd9) {
      const length = buffer[offset++];
      const value = buffer.subarray(offset, offset + length).toString('utf8');
      offset += length;
      return value;
    }
    throw new Error(`unsupported MessagePack prefix 0x${prefix.toString(16)}`);
  };
  const decoded = read();
  assert.equal(offset, buffer.length, 'unexpected bytes after MessagePack value');
  return decoded;
}

function pdb2Exchange(frame, { timeoutMs = 5000 } = {}) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: '127.0.0.1', port });
    let response = Buffer.alloc(0);
    let settled = false;
    const finish = (callback, value) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      callback(value);
    };
    socket.setTimeout(timeoutMs, () => finish(reject,
      new Error('client timed out before a complete PDB2 response')));
    socket.on('error', (error) => {
      trace(`pdb2-socket-error:${error.code || 'unknown'}`);
      finish(reject, error);
    });
    socket.once('connect', () => socket.write(frame));
    socket.on('data', (chunk) => {
      response = Buffer.concat([response, chunk]);
      if (response.length < 8) return;
      if (response.subarray(0, 4).toString('ascii') !== 'PDB2') {
        finish(reject, new Error('response did not use PDB2 framing'));
        return;
      }
      const framedLength = response.readUInt32BE(4);
      const payloadLength = framedLength & 0x7fffffff;
      if (response.length < 8 + payloadLength) return;
      try {
        assert.notEqual(framedLength & 0x80000000, 0, 'PDB2 error bit was not set');
        finish(resolve, decodeMessagePack(response.subarray(8, 8 + payloadLength)));
      } catch (error) {
        finish(reject, error);
      }
    });
    socket.once('end', () => {
      if (!settled) finish(reject, new Error('EOF before a complete PDB2 response'));
    });
  });
}

function oversizedPdb2Frame() {
  const frame = Buffer.alloc(8);
  frame.write('PDB2', 0, 'ascii');
  frame.writeUInt32BE(maxBytes + 1, 4);
  return frame;
}

async function admissionResponse() {
  const blockers = Array.from({ length: Number(environment.MAX_CONNECTIONS) }, () =>
    net.createConnection({ host: '127.0.0.1', port }));
  // Windows reports the server's intentional admission close as ECONNRESET.
  // These sockets exist only to occupy admission slots, so consume that
  // expected transport event while the response socket verifies the contract.
  for (const blocker of blockers) blocker.on('error', (error) => {
    trace(`admission-blocker-error:${error.code || 'unknown'}`);
  });
  await Promise.all(blockers.map((blocker) => once(blocker, 'connect')));
  for (const blocker of blockers) blocker.write('{"action":"ping"');
  await new Promise((resolve) => setTimeout(resolve, 200));
  try {
    return await lineExchange('{"action":"ping","userId":"system"}\n');
  } finally {
    for (const blocker of blockers) blocker.destroy();
  }
}

async function stopEngine() {
  if (engine.exitCode === null) {
    engine.kill('SIGINT');
    const forced = setTimeout(() => engine.kill('SIGKILL'), 15000);
    await once(engine, 'exit');
    clearTimeout(forced);
  }
  log.end();
  await once(log, 'finish');
}

try {
  trace('wait-ready');
  await waitReady();
  await new Promise((resolve) => setTimeout(resolve, 100));

  const healthClient = new PacificDBClient({ port, timeoutMs: 1000 });
  const healthRequest = async (command) => {
    let lastError;
    for (let attempt = 0; attempt < 20; attempt += 1) {
      try {
        return await healthClient.request(command);
      } catch (error) {
        lastError = error;
        if (error?.response?.error !== 'server_busy') throw error;
        await new Promise((resolve) => setTimeout(resolve, 25));
      }
    }
    throw lastError;
  };
  trace('health-identity');
  const ordinaryPing = await healthRequest({ action: 'ping' });
  assert.equal(ordinaryPing.edition, 'community');
  assert.equal('instance_id' in ordinaryPing, false);
  assert.equal('engine_pid' in ordinaryPing, false);
  assert.equal('data_root_fingerprint' in ordinaryPing, false);
  assert.equal('discovery_nonce' in ordinaryPing, false);
  const wrongNoncePing = await healthRequest({
    action: 'ping', local_discovery_nonce: 'wrong',
  });
  assert.equal('instance_id' in wrongNoncePing, false);
  const localDiscoveryPing = await healthRequest({
    action: 'ping',
    local_discovery_nonce: environment.PACIFICDB_DISCOVERY_NONCE,
  });
  const processMetadata = JSON.parse(await readFile(
    path.join(testRoot, 'engine.metadata.json'), 'utf8'));
  assert.equal(localDiscoveryPing.instance_id, environment.PACIFICDB_INSTANCE_ID);
  assert.equal(localDiscoveryPing.engine_pid, engine.pid);
  assert.equal(localDiscoveryPing.data_root_fingerprint,
    processMetadata.data_root_fingerprint);
  assert.match(localDiscoveryPing.data_root_fingerprint, /^sha256:[0-9a-f]{64}$/);
  assert.notEqual(localDiscoveryPing.data_root_fingerprint,
    environment.PACIFICDB_DATA_ROOT_FINGERPRINT,
    'engine must derive the fingerprint instead of trusting caller input');
  assert.equal('discovery_nonce' in localDiscoveryPing, false);

  trace('oversized-pdb2');
  assert.deepEqual(await pdb2Exchange(oversizedPdb2Frame()), {
    error: 'payload_too_large',
    max_bytes: maxBytes,
  });
  trace('oversized-line');
  assert.deepEqual(await lineExchange('x'.repeat(maxBytes + 1)), {
    error: 'payload_too_large',
    max_bytes: maxBytes,
  });
  trace('incomplete-line');
  assert.deepEqual(await lineExchange('{"action":"ping"'), {
    error: 'request_incomplete',
    retryable: true,
  });
  trace('invalid-json');
  assert.deepEqual(await lineExchange('{not-json}\n'), {
    error: 'invalid_json',
  });
  trace('unknown-action');
  assert.deepEqual(await lineExchange('{"action":"not_a_real_action"}\n'), {
    action: 'not_a_real_action',
    error: 'unknown_action',
    message: 'Unknown action',
  });
  trace('admission');
  assert.deepEqual(await admissionResponse(), {
    error: 'server_busy',
    reason: 'connection_limit',
    retry_after_ms: 200,
  });

  trace('postconditions');
  assert.equal(engine.exitCode, null, 'protocol errors stopped the engine');
  await new Promise((resolve) => setTimeout(resolve, 50));
  assert.equal((await readFile(engineLog, 'utf8')).includes(
    environment.PACIFICDB_DISCOVERY_NONCE), false,
  'discovery nonce leaked into engine.log');
  console.log(JSON.stringify({ status: 'PASS', cases: 6, port }));
} catch (error) {
  const diagnostic = await readFile(engineLog, 'utf8').catch(() => '');
  error.message += `\nengine log:\n${diagnostic.slice(-12000)}`;
  throw error;
} finally {
  await stopEngine().catch(() => {});
  if (process.env.PACIFICDB_KEEP_P0_ROOT !== '1') {
    await rm(testRoot, { recursive: true, force: true });
  } else {
    console.error(`preserved test root: ${testRoot}`);
  }
}
