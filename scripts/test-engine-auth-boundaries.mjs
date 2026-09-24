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
const engineBinary = path.join(buildRoot,
  process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-auth-boundary-'));
const adminPassword = 'auth-boundary-secret-value-9283';

async function freePort() {
  const listener = net.createServer();
  await new Promise((resolve, reject) => {
    listener.once('error', reject);
    listener.listen(0, '127.0.0.1', resolve);
  });
  const port = listener.address().port;
  await new Promise((resolve) => listener.close(resolve));
  return port;
}

async function startEngine(name, bindHost) {
  const home = path.join(root, name);
  const dataRoot = path.join(home, 'data');
  const backupRoot = path.join(home, 'backup');
  const restoreRoot = path.join(home, 'restore');
  await Promise.all([dataRoot, backupRoot, restoreRoot].map((directory) =>
    mkdir(directory, { recursive: true, mode: 0o700 })));
  const [port, raftPort] = await Promise.all([freePort(), freePort()]);
  const logPath = path.join(home, 'engine.log');
  const log = createWriteStream(logPath, { mode: 0o600 });
  const child = spawn(engineBinary, [], {
    cwd: repositoryRoot,
    env: {
      ...process.env,
      PACIFICDB_ENVIRONMENT: 'development',
      PACIFICDB_HOME: home,
      DATA_ROOT: dataRoot,
      BACKUP_ROOT: backupRoot,
      RESTORE_DIR: restoreRoot,
      ENGINE_BIND_HOST: bindHost,
      ENGINE_PORT: String(port),
      ENGINE_AUTH_REQUIRED: '1',
      PACIFICDB_ENGINE_ADMIN_USERNAME: 'admin',
      PACIFICDB_ENGINE_ADMIN_PASSWORD: adminPassword,
      DETERMINISTIC_OP_LOG: path.join(home, 'requests.jsonl'),
      RAFT_CLUSTER_ID: `auth-boundary-${name}`,
      RAFT_NODE_ID: 'node-1',
      RAFT_LISTEN_PORT: String(raftPort),
      RAFT_IS_LEADER: '1',
      MIN_QUORUM_SIZE: '1',
      ENGINE_CPU_CORES: '2',
      CONN_MIN_THREADS: '2',
      CONN_MAX_THREADS: '4',
      CONN_MAX_QUEUE: '32',
      MAX_CONNECTIONS: '16',
      ADAPTIVE_ADMISSION: '0',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stdout.pipe(log, { end: false });
  child.stderr.pipe(log, { end: false });
  return { child, log, logPath, port };
}

async function stopEngine({ child, log }) {
  if (child.exitCode === null && child.signalCode === null) {
    child.kill('SIGINT');
    const forced = setTimeout(() => child.kill('SIGKILL'), 15000);
    await once(child, 'exit');
    clearTimeout(forced);
  }
  if (!log.writableFinished) {
    log.end();
    await once(log, 'finish');
  }
}

async function waitReady(engine) {
  const probe = new PacificDBClient({ port: engine.port, timeoutMs: 300, poolSize: 1 });
  try {
    for (let attempt = 0; attempt < 120; attempt += 1) {
      if (engine.child.exitCode !== null) break;
      try {
        if ((await probe.request({ action: 'ping' })).status === 'pong') return;
      } catch { /* Engine is still starting. */ }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error(`engine did not become ready: ${await readFile(engine.logPath, 'utf8')}`);
  } finally {
    probe.close();
  }
}

async function expectDenied(client, action, fields = {}) {
  await assert.rejects(client.request({ action, ...fields }), /permission_denied/,
    `${action} should require a stronger role`);
}

let active;
try {
  active = await startEngine('valid-host', '127.0.0.1');
  await waitReady(active);
  const anonymous = new PacificDBClient({ port: active.port, poolSize: 1 });
  const admin = new PacificDBClient({ port: active.port, poolSize: 1 });
  let reader;
  let writer;
  try {
    const login = await admin.authenticate('admin', adminPassword);
    assert.equal(login.role, 'superadmin');
    const readKey = await admin.request({ action: 'api_key_create',
      name: 'boundary-read', role: 'read' });
    const writeKey = await admin.request({ action: 'api_key_create',
      name: 'boundary-write', role: 'readwrite' });
    reader = new PacificDBClient({ port: active.port, token: readKey.key, poolSize: 1 });
    writer = new PacificDBClient({ port: active.port, token: writeKey.key, poolSize: 1 });

    const project = await admin.createProject('rbac-boundary');
    await admin.useProject(project.project.id);
    await admin.createDatabase('auth_db', 'vector');
    await admin.useDatabase('auth_db');
    await admin.createCollection('vectors');
    reader.database = 'auth_db';
    writer.database = 'auth_db';

    assert.equal((await reader.request({ action: 'security_whoami' })).role, 'read_only');
    assert.equal((await reader.request({ action: 'community_capabilities' })).status, 'ok');
    for (const action of [
      'config_get', 'config_set', 'config_dump', 'config_reload',
      'insertVector', 'update', 'createTenant', 'storage_flush',
      'observeLeaderTerm', 'observe_leader_term',
    ]) {
      await expectDenied(reader, action, { key: 'PACIFICDB_ENGINE_ADMIN_PASSWORD',
        value: 'changed', term: 999999999 });
    }
    for (const action of ['config_get', 'config_set', 'createTenant',
      'observeLeaderTerm', 'storage_flush']) {
      await expectDenied(writer, action, { key: 'PACIFICDB_ENGINE_ADMIN_PASSWORD',
        value: 'changed', term: 999999999 });
    }
    await assert.rejects(anonymous.request({ action: 'observeLeaderTerm', term: 999999999 }),
      /unauthorized/);

    const inserted = await writer.request({ action: 'insertVector',
      collection: 'vectors', data: { id: 'writer-vector', vector: [1, 0] } });
    assert.equal(inserted.status, 'ok');
    const updated = await writer.request({ action: 'update', collection: 'vectors',
      filter: { id: 'writer-vector' }, update: { label: 'allowed' } });
    assert.equal(updated.status, 'updated');
    const readable = await reader.request({ action: 'queryVector',
      collection: 'vectors', vector: [1, 0], k: 1 });
    assert.equal(readable.status, 'ok');

    const before = await admin.request({ action: 'admin_raft_status' });
    assert.ok(before.currentTerm < 999999999, 'denied term changes must not mutate Raft');

    const secret = await admin.request({ action: 'config_get',
      key: 'PACIFICDB_ENGINE_ADMIN_PASSWORD' });
    assert.equal(secret.value, '[redacted]');
    const allConfig = await admin.request({ action: 'config_get' });
    assert.equal(allConfig.config.PACIFICDB_ENGINE_ADMIN_PASSWORD, '[redacted]');
    assert.doesNotMatch(JSON.stringify(allConfig), new RegExp(adminPassword));

    const secretSet = await admin.request({ action: 'config_set',
      key: 'PACIFICDB_TEST_SECRET', value: 'another-private-value' });
    assert.equal(secretSet.value, '[redacted]');
    const secretRead = await admin.request({ action: 'config_get',
      key: 'PACIFICDB_TEST_SECRET' });
    assert.equal(secretRead.value, '[redacted]');
    const lowerCaseSet = await admin.request({ action: 'config_set',
      key: 'custom_password', value: 'mixed-case-private-value' });
    assert.equal(lowerCaseSet.value, '[redacted]');
    const plainSet = await admin.request({ action: 'config_set',
      key: 'PACIFICDB_TEST_LABEL', value: 'visible' });
    assert.equal(plainSet.value, 'visible');

    const adminObserved = await admin.request({ action: 'observeLeaderTerm',
      term: before.currentTerm });
    assert.equal(adminObserved.status, 'ok');
    assert.equal(adminObserved.current_term, before.currentTerm);
  } finally {
    anonymous.close();
    admin.close();
    reader?.close();
    writer?.close();
  }
  await stopEngine(active);
  const deterministicLog = await readFile(path.join(root, 'valid-host', 'requests.jsonl'), 'utf8');
  for (const secret of [adminPassword, 'another-private-value',
    'mixed-case-private-value']) {
    assert.equal(deterministicLog.includes(secret), false,
      'deterministic request logging must redact configuration secrets');
  }
  active = undefined;

  const invalid = await startEngine('invalid-host', 'not-a-valid-ip-address');
  active = invalid;
  const exited = await Promise.race([
    once(invalid.child, 'exit').then(() => true),
    new Promise((resolve) => setTimeout(() => resolve(false), 15000)),
  ]);
  assert.equal(exited, true, 'invalid bind host must refuse startup');
  await stopEngine(invalid);
  active = undefined;
  assert.equal(invalid.child.exitCode, 78, 'startup refusal must return a failure status');
  const invalidLog = await readFile(invalid.logPath, 'utf8');
  assert.match(invalidLog, /FATAL: Invalid ENGINE_BIND_HOST=not-a-valid-ip-address/);
  assert.doesNotMatch(invalidLog, /Listening on/);

  console.log('engine auth boundary and bind-host checks passed');
} finally {
  if (active) await stopEngine(active);
  await rm(root, { recursive: true, force: true });
}
