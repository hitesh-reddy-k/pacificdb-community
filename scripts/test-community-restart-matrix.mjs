#!/usr/bin/env node

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const engineBinary = path.join(build, 'db_engine');
const cliBinary = path.join(build, 'pacificdb');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-restart-matrix-'));
const dataRoot = path.join(root, 'data');
const cliHome = path.join(root, 'cli');
const engineLog = path.join(root, 'engine.log');
await Promise.all([dataRoot, cliHome].map((directory) => mkdir(directory, { recursive: true })));

async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const selected = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return selected;
}

const port = await freePort();
const raftPort = await freePort();
const password = 'restart-matrix-password';
const environment = {
  ...process.env,
  PACIFICDB_ENVIRONMENT: 'development',
  DATA_ROOT: dataRoot,
  BACKUP_ROOT: path.join(root, 'backups'),
  RESTORE_DIR: path.join(root, 'restores'),
  TMP_DIR: path.join(root, 'tmp'),
  LOG_DIR: path.join(root, 'logs'),
  PACIFICDB_CLI_HOME: cliHome,
  ENGINE_BIND_HOST: '127.0.0.1',
  ENGINE_PORT: String(port),
  ENGINE_AUTH_REQUIRED: '1',
  PACIFICDB_ENGINE_ADMIN_USERNAME: 'admin',
  PACIFICDB_ENGINE_ADMIN_PASSWORD: password,
  RAFT_CLUSTER_ID: 'community-restart-matrix',
  RAFT_NODE_ID: 'node-1',
  RAFT_LISTEN_PORT: String(raftPort),
  RAFT_IS_LEADER: '1',
  MIN_QUORUM_SIZE: '1',
  INSERT_SYNC_THRESHOLD_BYTES: String(4 * 1024 * 1024),
  ENGINE_CPU_CORES: '2',
  CONN_MIN_THREADS: '2',
  CONN_MAX_THREADS: '8',
  DBQ_SHARDS: '2',
  DBQ_WORKERS_PER_SHARD: '1',
  ADAPTIVE_ADMISSION: '0'
};

let engine;
let log;
async function waitReady() {
  const probe = new PacificDBClient({ port, timeoutMs: 300 });
  let last;
  for (let attempt = 0; attempt < 150; attempt += 1) {
    try {
      if ((await probe.request({ action: 'ping' })).status === 'pong') return;
    } catch (error) { last = error; }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`engine did not become ready: ${last?.message}`);
}

async function start() {
  log = createWriteStream(engineLog, { flags: 'a', mode: 0o600 });
  engine = spawn(engineBinary, [], { cwd: repositoryRoot, env: environment,
    stdio: ['ignore', 'pipe', 'pipe'] });
  engine.stdout.pipe(log, { end: false });
  engine.stderr.pipe(log, { end: false });
  await waitReady();
}

async function stop(signal = 'SIGINT') {
  if (!engine || engine.exitCode !== null) return;
  engine.kill(signal);
  const timer = setTimeout(() => engine.kill('SIGKILL'), 12000);
  await once(engine, 'exit');
  clearTimeout(timer);
  log.end();
  await once(log, 'finish');
}

function clientWith(token, database = '') {
  const client = new PacificDBClient({ port, database, timeoutMs: 5000 });
  client.token = token;
  return client;
}

async function interactiveOriginalProjectFlow() {
  const child = spawn(cliBinary, ['--port', String(port), 'shell'], {
    cwd: repositoryRoot, env: environment, stdio: ['pipe', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  const waitFor = async (pattern) => {
    for (let attempt = 0; attempt < 150; attempt += 1) {
      const match = output.match(pattern);
      if (match) return match;
      if (child.exitCode !== null) throw new Error(output);
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    throw new Error(`shell output timeout for ${pattern}: ${output}`);
  };
  child.stdin.write(`login admin\n${password}\nlist projects\ncreate project emergency-persistence-test\n`);
  const match = await waitFor(/"id":\s*"(project_[0-9a-f]+)"/);
  const id = match[1];
  child.stdin.end(`list projects\nuse project ${id}\nshow project\nexit\n`);
  const [code] = await once(child, 'exit');
  assert.equal(code, 0, output);
  assert.match(output, /emergency-persistence-test/);
  assert.ok(!output.includes('project_not_found'));
  return id;
}

async function freshShellProjectRead(id, unknown = false) {
  const commands = [`login admin`, password, 'list projects', `use project ${id}`,
    'show project'];
  if (unknown) commands.push('use project definitely-does-not-exist', 'show project');
  commands.push('exit');
  const child = spawn(cliBinary, ['--port', String(port), 'shell'], {
    cwd: repositoryRoot, env: environment, stdio: ['pipe', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  child.stdin.end(commands.join('\n') + '\n');
  const [code] = await once(child, 'exit');
  assert.equal(code, 0, output);
  assert.match(output, /emergency-persistence-test/);
  if (unknown) {
    assert.match(output, /project_not_found/);
    assert.match(output, new RegExp(id));
  }
}

const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');

try {
  await start();
  const bootstrap = new PacificDBClient({ port });
  await bootstrap.authenticate('admin', password);
  const durableAdmin = await bootstrap.request({ action: 'api_key_create',
    name: 'restart-admin', role: 'admin' });
  const revokeLater = await bootstrap.request({ action: 'api_key_create',
    name: 'restart-reader', role: 'read' });
  const client = clientWith(durableAdmin.key);

  // Create/read baseline for every persistent entity.
  const project = (await client.request({ action: 'community_project_create',
    name: 'lifecycle-project' })).project;
  await client.createDatabase('app');
  client.database = 'app';
  await client.createCollection('docs');
  await client.createCollection('vectors');
  await client.insert('docs', { id: 'lifecycle-document', indexed: 'before', value: 1 });
  await client.request({ action: 'createIndex', collection: 'docs', name: 'indexed_1',
    fields: { indexed: 1 } });
  await client.putVector('vectors', 'vector-life', [1, 0]);
  const mediaBytes = Buffer.alloc(350_000, 41);
  const mediaPath = path.join(root, 'media.bin');
  await writeFile(mediaPath, mediaBytes);
  const media = await client.uploadMediaFile('media', mediaPath, { chunkBytes: 131_072 });
  const backup = await client.request({ action: 'create_backup', description: 'lifecycle-one' });
  const restore = await client.request({ action: 'restore_backup', backup_id: backup.backup_id });
  await assert.rejects(client.request({ action: 'restore_backup',
    backup_id: 'missing-backup' }), /restore_failed/);
  const emergencyProjectId = await interactiveOriginalProjectFlow();

  assert.equal((await client.request({ action: 'community_project_get', id: project.id })).project.name,
    'lifecycle-project');
  assert.equal((await client.find('docs', { id: 'lifecycle-document' })).count, 1);
  assert.equal((await client.queryVector('vectors', [1, 0], { k: 1 })).data[0].id,
    'vector-life');
  assert.equal((await client.request({ action: 'community_media_get', media_id: media.id })).media.status,
    'ready');

  // First graceful restart/read, then modify every applicable entity.
  await stop();
  await start();
  await freshShellProjectRead(emergencyProjectId);
  let resumed = clientWith(durableAdmin.key, 'app');
  assert.equal((await resumed.find('docs', { id: 'lifecycle-document' })).data[0].value, 1);
  assert.ok((await resumed.request({ action: 'listIndexes', collection: 'docs' })).indexes.length >= 1);
  assert.equal((await resumed.request({ action: 'get_backup', backup_id: backup.backup_id })).backup.backup_id,
    backup.backup_id);
  assert.ok((await resumed.request({ action: 'list_restores' })).restores.some((entry) =>
    entry.target_directory === restore.target_dir));
  await resumed.request({ action: 'community_database_map', database: 'app', project_id: project.id });
  await resumed.createDatabase('temporary-lifecycle');
  await resumed.createCollection('extra');
  await resumed.request({ action: 'updateOne', collection: 'docs',
    filter: { id: 'lifecycle-document' }, update: { indexed: 'after', value: 2 } });
  await resumed.putVector('vectors', 'vector-life', [0, 1]);
  const incomplete = (await resumed.request({ action: 'community_media_begin',
    collection: 'media', filename: 'media.bin', content_type: 'application/octet-stream',
    size_bytes: mediaBytes.length, chunk_count: 3, sha256: sha256(mediaBytes) })).media;
  const first = mediaBytes.subarray(0, 131_072);
  await resumed.request({ action: 'community_media_put_chunk', media_id: incomplete.id,
    index: 0, data: first.toString('base64'), size_bytes: first.length, sha256: sha256(first) });
  await resumed.request({ action: 'api_key_revoke', id: revokeLater.id });
  const secondBackup = await resumed.request({ action: 'create_backup', description: 'lifecycle-two' });

  // Second graceful restart/read and exact original project flow.
  await stop();
  await start();
  await freshShellProjectRead(emergencyProjectId, true);
  resumed = clientWith(durableAdmin.key, 'app');
  assert.equal((await resumed.find('docs', { id: 'lifecycle-document' })).data[0].value, 2);
  assert.equal((await resumed.queryVector('vectors', [0, 1], { k: 1 })).data[0].id,
    'vector-life');
  await assert.rejects(clientWith(revokeLater.key).request({ action: 'listDatabases' }),
    /unauthorized/);
  assert.equal((await resumed.request({ action: 'community_database_project', database: 'app' }))
    .mapping.project_id, project.id);
  assert.equal((await resumed.request({ action: 'community_media_get', media_id: incomplete.id }))
    .media.status, 'uploading');
  assert.ok((await resumed.request({ action: 'list_restores' })).restores.some((entry) =>
    entry.backup_id === 'missing-backup' && entry.status === 'failed'));

  const completedResume = await resumed.uploadMediaFile('media', mediaPath,
    { chunkBytes: 131_072, resume: incomplete.id });
  assert.equal(completedResume.status, 'ready');
  await resumed.request({ action: 'deleteOne', collection: 'docs',
    filter: { id: 'lifecycle-document' } });
  await resumed.insert('docs', { id: 'after-delete', indexed: 'final', value: 3 });
  await resumed.putVector('vectors', 'vector-final', [1, 1]);
  await resumed.request({ action: 'delete_backup', backup_id: secondBackup.backup_id });
  await resumed.request({ action: 'dropDatabase', dbName: 'temporary-lifecycle' });

  // Abrupt termination/recovery/read final state.
  await stop('SIGKILL');
  await start();
  resumed = clientWith(durableAdmin.key, 'app');
  assert.equal((await resumed.find('docs', { id: 'lifecycle-document' })).count, 0);
  assert.equal((await resumed.find('docs', { id: 'after-delete' })).data[0].value, 3);
  assert.equal((await resumed.request({ action: 'community_project_get', id: project.id })).project.name,
    'lifecycle-project');
  assert.ok(!(await resumed.request({ action: 'listDatabases' })).includes('temporary-lifecycle'));
  assert.ok((await resumed.request({ action: 'listIndexes', collection: 'docs' })).indexes.length >= 1);
  await assert.rejects(clientWith(revokeLater.key).request({ action: 'listDatabases' }),
    /unauthorized/);
  assert.equal((await resumed.request({ action: 'get_backup', backup_id: backup.backup_id })).backup.backup_id,
    backup.backup_id);
  await assert.rejects(resumed.request({ action: 'get_backup', backup_id: secondBackup.backup_id }),
    /backup_not_found/);
  assert.equal((await resumed.request({ action: 'community_media_get', media_id: incomplete.id }))
    .media.status, 'ready');
  assert.equal((await resumed.queryVector('vectors', [1, 1], { k: 1 })).data[0].id,
    'vector-final');
  const finalRestores = await resumed.request({ action: 'list_restores' });
  assert.ok(finalRestores.restores.some((entry) => entry.status === 'completed'));
  assert.ok(finalRestores.restores.some((entry) => entry.status === 'failed'));

  console.log(JSON.stringify({ status: 'PASS', persistent_entities: 13,
    graceful_restarts: 2, abrupt_restarts: 1, fresh_shells: 3,
    exact_project_id: emergencyProjectId }, null, 2));
} finally {
  await stop().catch(() => {});
  if (process.env.PACIFICDB_KEEP_RESTART_ROOT !== '1')
    await rm(root, { recursive: true, force: true });
}
