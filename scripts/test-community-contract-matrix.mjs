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
const binary = path.join(build, 'db_engine');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-contract-'));
const dataRoot = path.join(root, 'data');
const logFile = path.join(root, 'engine.log');
await mkdir(dataRoot, { recursive: true });

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
const password = 'contract-matrix-password';
const log = createWriteStream(logFile, { mode: 0o600 });
const engine = spawn(binary, [], {
  cwd: repositoryRoot,
  env: {
    ...process.env,
    PACIFICDB_ENVIRONMENT: 'development',
    DATA_ROOT: dataRoot,
    BACKUP_ROOT: path.join(root, 'backup'),
    RESTORE_DIR: path.join(root, 'restore'),
    TMP_DIR: path.join(root, 'tmp'),
    LOG_DIR: path.join(root, 'logs'),
    ENGINE_BIND_HOST: '127.0.0.1',
    ENGINE_PORT: String(port),
    ENGINE_AUTH_REQUIRED: '1',
    PACIFICDB_ENGINE_ADMIN_USERNAME: 'admin',
    PACIFICDB_ENGINE_ADMIN_PASSWORD: password,
    RAFT_CLUSTER_ID: 'community-contract',
    RAFT_NODE_ID: 'node-1',
    RAFT_LISTEN_PORT: String(raftPort),
    RAFT_IS_LEADER: '1',
    MIN_QUORUM_SIZE: '1',
    MAX_REQUEST_SIZE_MB: '2',
    INSERT_SYNC_THRESHOLD_BYTES: String(2 * 1024 * 1024),
    ENGINE_CPU_CORES: '2',
    CONN_MIN_THREADS: '2',
    CONN_MAX_THREADS: '8',
    DBQ_SHARDS: '2',
    DBQ_WORKERS_PER_SHARD: '1',
    ADAPTIVE_ADMISSION: '0'
  },
  stdio: ['ignore', 'pipe', 'pipe']
});
engine.stdout.pipe(log, { end: false });
engine.stderr.pipe(log, { end: false });

async function stop() {
  if (engine.exitCode === null) {
    engine.kill('SIGINT');
    const timer = setTimeout(() => engine.kill('SIGKILL'), 10000);
    await once(engine, 'exit');
    clearTimeout(timer);
  }
  log.end();
  await once(log, 'finish');
}

async function waitReady() {
  const client = new PacificDBClient({ port, timeoutMs: 250 });
  let last;
  for (let attempt = 0; attempt < 120; attempt += 1) {
    try {
      if ((await client.request({ action: 'ping' })).status === 'pong') return;
    } catch (error) { last = error; }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`engine did not become ready: ${last?.message}`);
}

async function rejects(client, command, pattern) {
  await assert.rejects(client.request(command), pattern);
}

async function transportFailureMatrix(mode) {
  const categories = [
    ['authentication', { action: 'security_whoami' }],
    ['projects', { action: 'community_project_list' }],
    ['databases', { action: 'listDatabases' }],
    ['queries', { action: 'find', dbName: 'app', collection: 'docs', filter: {} }],
    ['backups', { action: 'list_backups' }],
    ['api_keys', { action: 'api_key_list' }],
    ['media', { action: 'community_media_list' }],
    ['vectors', { action: 'queryVector', dbName: 'app', collection: 'vectors', vector: [1], k: 1 }],
    ['system', { action: 'ping' }]
  ];
  for (const [category, command] of categories) {
    const fakePort = await freePort();
    const server = net.createServer((socket) => {
      socket.once('data', () => {
        if (mode === 'malformed') socket.end('not-json\n');
      });
    });
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(fakePort, '127.0.0.1', resolve);
    });
    const client = new PacificDBClient({ port: fakePort, timeoutMs: 75 });
    const pattern = mode === 'malformed' ? /JSON|Unexpected token|not valid/i : /timed out/;
    await assert.rejects(client.request(command), pattern, `${mode}: ${category}`);
    await new Promise((resolve) => server.close(resolve));
  }
}

try {
  await waitReady();
  const client = new PacificDBClient({ port, timeoutMs: 5000 });
  await client.authenticate('admin', password);

  // Database names and boundaries.
  for (const name of ['', '.', '..', 'bad/name', 'bad\\name', 'a'.repeat(256)]) {
    await rejects(client, { action: 'createDatabase', dbName: name },
      /empty|required|unsafe|forbidden|maximum|invalid/i);
  }
  for (const reserved of ['system', 'pacificdb_meta']) {
    await rejects(client, { action: 'createDatabase', dbName: reserved }, /reserved_namespace/);
  }
  const validNames = ['app', '第二数据库', 'a'.repeat(255), 'second'];
  const databaseIds = new Map();
  for (const name of validNames) {
    const created = await client.createDatabase(name);
    assert.equal(created.status, 'ok');
    databaseIds.set(name, created.dbId);
  }
  assert.equal((await client.createDatabase('app')).dbId, databaseIds.get('app'));
  const listed = await client.request({ action: 'listDatabases' });
  for (const name of validNames) assert.ok(listed.includes(name), `missing database ${name}`);
  await rejects(client, { action: 'dropDatabase', dbName: 'missing-db' }, /database_not_found/);

  // Document types, ID behavior, filters, limits and request boundary.
  client.database = 'app';
  await client.createCollection('docs');
  const representative = {
    id: 'types', string: 'Pacific', integer: 42, floating: 3.25,
    boolean: true, nullable: null, array: [1, 'two', false],
    nested: { city: 'Hyderabad', level: { value: 2 } }, emptyArray: [],
    emptyObject: {}, unicode: 'నమస్తే 🌊'
  };
  assert.equal((await client.insert('docs', representative)).status, 'ok');
  assert.equal((await client.find('docs', { id: 'types' })).data[0].unicode, representative.unicode);
  assert.equal((await client.find('docs', { 'nested.city': 'Hyderabad' })).count, 1);
  const generated = await client.insert('docs', { id: '', value: 'generated' });
  assert.ok(generated.id);
  assert.equal((await client.insert('docs', { ...representative, string: 'Pacific-updated' })).status,
    'ok');
  const duplicateIdRows = await client.find('docs', { id: 'types' });
  assert.equal(duplicateIdRows.count, 1);
  assert.equal(duplicateIdRows.data[0].string, 'Pacific-updated');
  for (let index = 0; index < 5; index += 1)
    await client.insert('docs', { id: `limit-${index}`, ordinal: index });
  assert.equal((await client.find('docs', {}, { limit: 2, offset: 1 })).data.length, 2);
  assert.equal((await client.request({ action: 'updateOne', collection: 'docs',
    filter: { id: 'types' }, update: { floating: 8.5 } })).status, 'updated');
  assert.equal((await client.find('docs', { id: 'types' })).data[0].floating, 8.5);
  assert.equal((await client.request({ action: 'updateOne', collection: 'docs',
    filter: { id: 'missing' }, update: { value: 1 } })).status, 'not_found');
  assert.equal((await client.request({ action: 'deleteOne', collection: 'docs',
    filter: { id: 'missing' } })).status, 'not_found');
  const beforeEmptyDelete = (await client.request({ action: 'count', collection: 'docs', filter: {} })).count;
  assert.equal((await client.request({ action: 'deleteOne', collection: 'docs', filter: {} })).status, 'deleted');
  assert.equal((await client.request({ action: 'count', collection: 'docs', filter: {} })).count,
    beforeEmptyDelete - 1);
  await rejects(client, { action: 'insert', collection: 'docs',
    data: { id: 'too-large', value: 'x'.repeat(3 * 1024 * 1024) } },
  /closed|too large|request|response/i);

  // Every Community aggregation stage, combinations, malformed forms and equivalence.
  const empty = await client.request({ action: 'aggregate', collection: 'docs', pipeline: [] });
  const normal = await client.find('docs', {});
  assert.equal(empty.count, normal.count);
  const stages = [
    [{ $match: { id: 'types' } }],
    [{ $project: { id: 1 } }],
    [{ $sort: { ordinal: -1 } }],
    [{ $skip: 1 }],
    [{ $limit: 1 }],
    [{ $count: 'total' }],
    [{ $match: { ordinal: { $gte: 1 } } }, { $sort: { ordinal: -1 } },
      { $skip: 1 }, { $limit: 2 }, { $project: { id: 1, ordinal: 1 } }]
  ];
  for (const pipeline of stages) {
    const response = await client.request({ action: 'aggregate', collection: 'docs', pipeline });
    assert.equal(response.status, 'ok');
  }
  for (const pipeline of [
    {}, [{ $group: {} }], [{ $limit: 0 }], [{ $skip: -1 }],
    [{ $sort: { ordinal: 2 } }], [{ $project: {} }], [{ $count: '' }],
    [{ $count: 'n' }, { $limit: 1 }], [{ $match: {}, $limit: 1 }]
  ]) await rejects(client, { action: 'aggregate', collection: 'docs', pipeline },
    /pipeline|unsupported|limit|skip|sort|project|count|stage/i);

  // Explain must validate resources and match the real scalar-index/full-scan choice.
  assert.equal((await client.request({ action: 'explain', collection: 'docs',
    filter: { id: 'types' } })).query_plan.plan, 'INDEX_LOOKUP');
  assert.equal((await client.request({ action: 'explain', collection: 'docs',
    filter: { ordinal: { $gt: 1 } } })).query_plan.plan, 'FULL_SCAN');
  await rejects(client, { action: 'explain', collection: 'missing', filter: {} },
    /collection_not_found/);
  await rejects(client, { action: 'aggregate', collection: 'missing', pipeline: [] },
    /collection_not_found/);
  await rejects(client, { action: 'count', collection: 'missing', filter: {} },
    /collection_not_found/);
  await rejects(client, { action: 'explain', collection: 'docs', filter: [] }, /filter/);

  // API-key roles and least privilege.
  const read = await client.request({ action: 'api_key_create', name: 'reader', role: 'read' });
  const readwrite = await client.request({ action: 'api_key_create', name: 'writer', role: 'readwrite' });
  const admin = await client.request({ action: 'api_key_create', name: 'root', role: 'admin' });
  const readClient = new PacificDBClient({ port, database: 'app' }); readClient.token = read.key;
  const writeClient = new PacificDBClient({ port, database: 'app' }); writeClient.token = readwrite.key;
  const adminClient = new PacificDBClient({ port, database: 'app' }); adminClient.token = admin.key;
  assert.equal((await readClient.find('docs', {})).status, 'ok');
  await assert.rejects(readClient.insert('docs', { id: 'read-mutation' }),
    /forbidden|permission_denied/);
  assert.equal((await writeClient.insert('docs', { id: 'writer-mutation' })).status, 'ok');
  await rejects(writeClient, { action: 'api_key_create', name: 'denied', role: 'read' },
    /forbidden|permission_denied/);
  assert.match((await adminClient.request({ action: 'api_key_create', name: 'allowed', role: 'read' })).key,
    /^pdb_/);
  await rejects(client, { action: 'api_key_create', name: 'bad', role: 'owner' }, /role/);
  const storedKeys = await readFile(path.join(dataRoot, 'security', 'api_keys.json'), 'utf8');
  for (const secret of [read.key, readwrite.key, admin.key]) assert.ok(!storedKeys.includes(secret));

  // Vector validation, ordering, dimension behavior, metrics, k bounds and duplicate IDs.
  await client.createCollection('vectors');
  await client.putVector('vectors', 'east', [1, 0]);
  await client.putVector('vectors', 'north', [0, 1]);
  assert.equal((await client.queryVector('vectors', [0.9, 0.1], { k: 1 })).data[0].id, 'east');
  for (const metric of ['cosine', 'l2', 'euclidean', 'dot', 'dot_product'])
    assert.ok((await client.queryVector('vectors', [1, 0], { k: 1, metric })).data.length === 1);
  assert.equal((await client.queryVector('vectors', [1, 0], { k: 20 })).data.length, 2);
  assert.throws(() => client.putVector('vectors', 'empty', []), /non-empty/);
  assert.throws(() => client.putVector('vectors', 'bad', [1, 'x']), /finite numbers/);
  assert.equal((await client.putVector('vectors', 'dimension-3', [1, 2, 3])).status, 'ok');
  assert.equal((await client.queryVector('vectors', [1, 0], { k: 20 })).data.length, 2);
  assert.equal((await client.putVector('vectors', 'east', [0.5, 0.5])).status, 'ok');
  assert.equal((await client.find('vectors', { id: 'east' })).count, 1);
  assert.equal((await client.queryVector('vectors', [1, 2, 3], { k: 1 })).data[0].id,
    'dimension-3');
  await assert.rejects(client.queryVector('vectors', [1, 0], { k: 0 }), /positive/);
  await assert.rejects(client.queryVector('vectors', [1, 0], { metric: 'unknown' }), /unsupported/);
  await assert.rejects(client.queryVector('missing-vectors', [1, 0], { k: 1 }), /collection_not_found/);

  await transportFailureMatrix('timeout');
  await transportFailureMatrix('malformed');

  console.log(JSON.stringify({ status: 'PASS', database_name_cases: 13,
    document_contract_cases: 15, aggregation_cases: 16, explain_cases: 4,
    role_cases: 8, vector_cases: 18, timeout_categories: 9,
    malformed_response_categories: 9 }, null, 2));
} finally {
  await stop().catch(() => {});
  if (process.env.PACIFICDB_KEEP_CONTRACT_ROOT !== '1')
    await rm(root, { recursive: true, force: true });
}
