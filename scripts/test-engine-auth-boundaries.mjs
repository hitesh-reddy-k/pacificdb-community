#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
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
const accountPassword = 'account-boundary-secret-value-4917';

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
  const accountsFile = path.join(home, 'accounts.json');
  await writeFile(accountsFile, JSON.stringify([
    { username: 'alice', password: accountPassword, role: 'ADMIN' },
    { username: 'bob', password: accountPassword, role: 'ADMIN' },
    { username: 'reader', password: accountPassword, role: 'READ_ONLY' },
    { username: 'writer', password: accountPassword, role: 'WRITE' },
    { username: 'outsider', password: accountPassword, role: 'WRITE' },
  ]), { mode: 0o600 });
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
      PACIFICDB_ENGINE_ACCOUNTS_FILE: accountsFile,
      DETERMINISTIC_OP_LOG: path.join(home, 'requests.jsonl'),
      RAFT_CLUSTER_ID: `auth-boundary-${name}`,
      RAFT_NODE_ID: 'node-1',
      RAFT_LISTEN_PORT: String(raftPort),
      RAFT_IS_LEADER: '1',
      MIN_QUORUM_SIZE: '1',
      ENGINE_CPU_CORES: '2',
      CONN_MIN_THREADS: '12',
      CONN_MAX_THREADS: '12',
      CONN_MAX_QUEUE: '64',
      MAX_CONNECTIONS: '32',
      ADAPTIVE_ADMISSION: '0',
      PACIFICDB_MEDIA_UPLOAD_LEASE_MS: '50',
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
  const extraClients = [];
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
    anonymous.close();
    reader.close();
    writer.close();

    const clientFor = async (username, userId, database = '') => {
      const client = new PacificDBClient({ port: active.port, userId, database, poolSize: 1 });
      await client.authenticate(username, accountPassword);
      extraClients.push(client);
      return client;
    };
    const alice = await clientFor('alice', 'tenant-a');
    const bob = await clientFor('bob', 'tenant-b');
    const aclReader = await clientFor('reader', 'tenant-a', 'shared');
    const aclWriter = await clientFor('writer', 'tenant-a', 'shared');
    const outsider = await clientFor('outsider', 'tenant-a', 'shared');

    await alice.createDatabase('shared');
    await alice.createCollection('docs');
    await alice.request({ action: 'insert', collection: 'docs',
      data: { id: 'alice-row', owner: 'alice' } });
    await bob.createDatabase('shared');
    await bob.createCollection('docs');
    await bob.request({ action: 'insert', collection: 'docs',
      data: { id: 'bob-row', owner: 'bob' } });

    await assert.rejects(alice.request({ action: 'find', userId: 'tenant-b',
      dbName: 'shared', collection: 'docs', filter: {} }), /permission_denied/);
    await assert.rejects(alice.request({ action: 'find', userId: 'tenant-b',
      dbName: '', database: 'shared', collection: 'docs', filter: {} }),
    /permission_denied/, 'the database alias must not bypass scope authorization');
    await assert.rejects(alice.request({ action: 'find', dbName: 'shared',
      database: 'different', collection: 'docs', filter: {} }),
    /permission_denied/, 'conflicting database aliases must fail closed');
    await assert.rejects(outsider.request({ action: 'find', collection: 'docs', filter: {} }),
      /permission_denied/);

    await alice.request({ action: 'security_database_acl_grant', dbName: 'shared',
      principal: 'reader', level: 'read-write' });
    await alice.request({ action: 'security_database_acl_grant', dbName: 'shared',
      principal: 'writer', level: 'read-only' });
    assert.equal((await aclReader.request({ action: 'find', collection: 'docs', filter: {} })).data.length, 1);
    await assert.rejects(aclReader.request({ action: 'insert', collection: 'docs',
      data: { id: 'reader-write' } }), /permission_denied/,
    'a database grant must not exceed the account role');
    assert.equal((await aclWriter.request({ action: 'find', collection: 'docs', filter: {} })).data.length, 1);
    await assert.rejects(aclWriter.request({ action: 'insert', collection: 'docs',
      data: { id: 'writer-before-upgrade' } }), /permission_denied/,
    'the global role must not exceed a read-only database grant');
    await alice.request({ action: 'security_database_acl_grant', dbName: 'shared',
      principal: 'writer', level: 'read-write' });
    assert.equal((await aclWriter.request({ action: 'insert', collection: 'docs',
      data: { id: 'writer-after-upgrade' } })).status, 'ok');
    await alice.request({ action: 'security_database_acl_grant', dbName: 'shared',
      principal: 'bob', level: 'owner' });
    await bob.request({ action: 'security_database_acl_grant', userId: 'tenant-a',
      dbName: 'shared', principal: 'outsider', level: 'read-only' });
    assert.equal((await outsider.request({ action: 'find', collection: 'docs',
      filter: {} })).data.length, 2);
    await alice.request({ action: 'security_database_acl_revoke', dbName: 'shared',
      principal: 'outsider' });
    await alice.request({ action: 'security_database_acl_revoke', dbName: 'shared',
      principal: 'bob' });
    await assert.rejects(alice.request({ action: 'security_database_acl_revoke',
      dbName: 'shared', principal: 'alice' }), /invalid_database_acl/,
    'the final owner must not be removable');

    const chunkSha = createHash('sha256').update('AAA').digest('hex');
    await bob.request({ action: 'createDatabase', userId: 'tenant-a', dbName: 'foreign' });
    await bob.request({ action: 'createCollection', userId: 'tenant-a',
      dbName: 'foreign', collection: 'docs' });
    const bobReady = (await bob.request({ action: 'community_media_begin',
      userId: 'tenant-a', dbName: 'foreign', collection: 'docs', filename: 'bob.bin',
      size_bytes: 3, chunk_count: 1, sha256: chunkSha })).media;
    await bob.request({ action: 'community_media_put_chunk', userId: 'tenant-a',
      dbName: 'foreign', media_id: bobReady.id, index: 0, data: 'QUFB',
      size_bytes: 3, sha256: chunkSha });
    await bob.request({ action: 'community_media_finalize', userId: 'tenant-a',
      dbName: 'foreign', media_id: bobReady.id });
    const bobIncomplete = (await bob.request({ action: 'community_media_begin',
      userId: 'tenant-a', dbName: 'foreign', collection: 'docs', filename: 'pending.bin',
      size_bytes: 3, chunk_count: 1, sha256: chunkSha })).media;
    const aliceMedia = (await alice.request({ action: 'community_media_begin',
      dbName: 'shared', collection: 'docs', filename: 'alice.bin', size_bytes: 3,
      chunk_count: 1, sha256: chunkSha })).media;

    for (const [action, fields] of [
      ['community_media_get', { media_id: bobReady.id }],
      ['community_media_get_chunk', { media_id: bobReady.id, index: 0 }],
      ['community_media_put_chunk', { media_id: bobIncomplete.id, index: 0,
        data: 'QUFB', size_bytes: 3, sha256: chunkSha }],
      ['community_media_finalize', { media_id: bobIncomplete.id }],
      ['community_media_delete', { media_id: bobReady.id }],
      ['community_media_cleanup', { media_id: bobIncomplete.id }],
    ]) {
      await assert.rejects(alice.request({ action, userId: 'tenant-a',
        dbName: 'shared', ...fields }), /media_(?:chunk_)?not_found/,
      `${action} must hide a foreign media id`);
    }
    await assert.rejects(alice.request({ action: 'community_media_begin',
      userId: 'tenant-a', dbName: 'shared', collection: 'docs', filename: 'bob.bin',
      size_bytes: 3, chunk_count: 1, sha256: chunkSha, resume_id: bobReady.id }),
    /media_not_found/, 'ready resume must authorize the stored database');

    const aliasList = await alice.request({ action: 'community_media_list',
      userId: 'tenant-a', dbName: '', db: 'shared', all: true });
    assert.deepEqual(aliasList.media.map((item) => item.id), [aliceMedia.id]);
    const globalList = await alice.request({ action: 'community_media_list',
      userId: 'tenant-a', dbName: '', all: true });
    assert.deepEqual(globalList.media.map((item) => item.id), [aliceMedia.id]);
    assert.deepEqual((await alice.request({ action: 'community_media_list',
      userId: 'untouched-namespace', dbName: '', all: true })).media, []);
    assert.deepEqual((await admin.request({ action: 'security_database_unassigned_list',
      userId: 'untouched-namespace' })).databases, [],
    'an unauthorized media listing must not initialize foreign storage');

    await new Promise((resolve) => setTimeout(resolve, 80));
    await alice.request({ action: 'community_media_cleanup', userId: 'tenant-a',
      dbName: '' });
    const foreignAfterCleanup = await bob.request({ action: 'community_media_get',
      userId: 'tenant-a', dbName: 'foreign', media_id: bobIncomplete.id });
    assert.equal(foreignAfterCleanup.media.status, 'uploading',
      'namespace cleanup must not mutate foreign media');

    const aliceReadKey = await alice.request({ action: 'api_key_create',
      name: 'alice-read', role: 'read' });
    const aliceWriteKey = await alice.request({ action: 'api_key_create',
      name: 'alice-write', role: 'readwrite' });
    const aliceKeyReader = new PacificDBClient({ port: active.port, userId: 'tenant-a',
      database: 'shared', token: aliceReadKey.key, poolSize: 1 });
    const aliceKeyWriter = new PacificDBClient({ port: active.port, userId: 'tenant-a',
      database: 'shared', token: aliceWriteKey.key, poolSize: 1 });
    extraClients.push(aliceKeyReader, aliceKeyWriter);
    assert.equal((await aliceKeyReader.request({ action: 'find', collection: 'docs',
      filter: {} })).data.length, 2);
    await assert.rejects(aliceKeyReader.request({ action: 'insert', collection: 'docs',
      data: { id: 'read-key-write' } }), /permission_denied/);
    assert.equal((await aliceKeyWriter.request({ action: 'insert', collection: 'docs',
      data: { id: 'write-key-row' } })).status, 'ok');

    await assert.rejects(alice.request({ action: 'bulk', dbName: 'shared', collection: 'docs',
      ops: [
        { action: 'insertOne', data: { id: 'must-not-commit' } },
        { action: 'deleteOne', userId: 'tenant-b', dbName: 'shared',
          collection: 'docs', filter: { id: 'bob-row' } },
      ] }), /permission_denied/);
    assert.equal((await alice.request({ action: 'find', dbName: 'shared', collection: 'docs',
      filter: { id: 'must-not-commit' } })).data.length, 0,
    'bulk authorization must finish before its first mutation');

    const foreignList = await alice.request({ action: 'listDatabases', userId: 'tenant-b',
      dbName: '' });
    assert.deepEqual(foreignList, [], 'database listing must hide foreign ownership');

    const unassigned = await admin.request({ action: 'security_database_unassigned_list',
      userId: 'system' });
    assert.ok(unassigned.databases.some((entry) => entry.userId === 'system' &&
      entry.dbName === 'system'), 'legacy database must be listed for migration');
    await admin.request({ action: 'security_database_owner_assign', userId: 'system',
      dbName: 'system', principal: 'alice' });
    const assigned = await alice.request({ action: 'security_database_acl_get',
      userId: 'system', dbName: 'system' });
    assert.deepEqual(assigned.security.owners, ['alice']);

    await alice.request({ action: 'createDatabase', userId: 'tenant-a', dbName: 'transfer' });
    await alice.request({ action: 'security_database_owner_transfer', userId: 'tenant-a',
      dbName: 'transfer', principal: 'bob' });
    await assert.rejects(alice.request({ action: 'security_database_acl_get',
      userId: 'tenant-a', dbName: 'transfer' }), /permission_denied/);
    const bobTransferred = await bob.request({ action: 'security_database_acl_get',
      userId: 'tenant-a', dbName: 'transfer' });
    assert.deepEqual(bobTransferred.security.owners, ['bob']);

    assert.equal((await admin.request({ action: 'insert', userId: 'tenant-b',
      dbName: 'shared', collection: 'docs', data: { id: 'superadmin-recovery' } })).status, 'ok');
    const audit = await readFile(path.join(root, 'valid-host', 'data', 'security', 'audit.log'), 'utf8');
    assert.match(audit, /SUPERADMIN_OVERRIDE/);
    assert.match(audit, /tenant-b\/shared/);
  } finally {
    anonymous.close();
    admin.close();
    reader?.close();
    writer?.close();
    for (const client of extraClients) client.close();
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
