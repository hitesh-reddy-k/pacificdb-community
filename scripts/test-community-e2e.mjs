#!/usr/bin/env node

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { chmod, mkdir, mkdtemp, readFile, rm, stat, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';

const root = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(root, 'build'));
const engineBinary = path.join(build, 'db_engine');
const cliBinary = path.join(build, 'pacificdb');
const testRoot = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-community-e2e-'));
const dataRoot = path.join(testRoot, 'data');
const backupRoot = path.join(testRoot, 'backup');
const restoreRoot = path.join(testRoot, 'restore');
const cliHome = path.join(testRoot, 'cli');
const engineLog = path.join(testRoot, 'engine.log');
await Promise.all([dataRoot, backupRoot, restoreRoot, cliHome].map((dir) =>
  mkdir(dir, { recursive: true, mode: 0o700 })));

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

const enginePort = await freePort();
const raftPort = await freePort();
const adminPassword = 'community-e2e-password';
const engineEnv = {
  ...process.env,
  PACIFICDB_ENVIRONMENT: 'development',
  DATA_ROOT: dataRoot,
  BACKUP_ROOT: backupRoot,
  RESTORE_DIR: restoreRoot,
  PACIFICDB_CLI_HOME: cliHome,
  ENGINE_BIND_HOST: '127.0.0.1',
  ENGINE_PORT: String(enginePort),
  ENGINE_AUTH_REQUIRED: '1',
  PACIFICDB_ENGINE_ADMIN_USERNAME: 'admin',
  PACIFICDB_ENGINE_ADMIN_PASSWORD: adminPassword,
  RAFT_CLUSTER_ID: 'community-e2e',
  RAFT_NODE_ID: 'node-1',
  RAFT_LISTEN_PORT: String(raftPort),
  RAFT_IS_LEADER: '1',
  MIN_QUORUM_SIZE: '1',
  ENGINE_CPU_CORES: '2',
  CONN_MIN_THREADS: '2',
  CONN_MAX_THREADS: '8',
  CONN_MAX_QUEUE: '256',
  MAX_CONNECTIONS: '64',
  ADAPTIVE_ADMISSION: '0',
  ENGINE_KEEPALIVE_MAX_REQUESTS: '1',
  DBQ_SHARDS: '2',
  DBQ_WORKERS_PER_SHARD: '1',
  DBQ_MAX_QUEUE_PER_SHARD: '256'
};

let engine;
let logStream;
async function waitForEngine(port = enginePort) {
  const probe = new PacificDBClient({ port, timeoutMs: 500 });
  let lastError;
  for (let attempt = 0; attempt < 120; attempt += 1) {
    try {
      const result = await probe.request({ action: 'ping' });
      if (result.status === 'pong') return;
    } catch (error) { lastError = error; }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`engine did not become ready: ${lastError?.message || 'unknown error'}`);
}

async function startEngine(overrides = {}, port = enginePort) {
  logStream = createWriteStream(engineLog, { flags: 'a', mode: 0o600 });
  engine = spawn(engineBinary, [], { cwd: root, env: { ...engineEnv, ...overrides },
    stdio: ['ignore', 'pipe', 'pipe'] });
  engine.stdout.pipe(logStream, { end: false });
  engine.stderr.pipe(logStream, { end: false });
  await waitForEngine(port);
}

async function stopEngine(signal = 'SIGINT') {
  if (!engine || engine.exitCode !== null) return;
  engine.kill(signal);
  const timer = setTimeout(() => engine.kill('SIGKILL'), 15000);
  await once(engine, 'exit');
  clearTimeout(timer);
  logStream.end();
  await once(logStream, 'finish');
}

async function runShell(lines) {
  const child = spawn(cliBinary, ['--port', String(enginePort), 'shell'], {
    cwd: root, env: { ...process.env, PACIFICDB_CLI_HOME: cliHome },
    stdio: ['pipe', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  child.stdin.end(lines.join('\n') + '\n');
  const [code] = await once(child, 'exit');
  assert.equal(code, 0, output);
  return output;
}

function sha256(bytes) {
  return createHash('sha256').update(bytes).digest('hex');
}

function assertPublicOutput(output) {
  assert.doesNotMatch(output, /_debug_metrics|_engineTrace|"_raft"|secret_hash/);
  assert.doesNotMatch(output, new RegExp(adminPassword));
}

try {
  console.log('E2E: start authenticated engine');
  await startEngine();
  const client = new PacificDBClient({ port: enginePort });
  await assert.rejects(client.request({ action: 'listDatabases' }), /unauthorized/);
  await assert.rejects(client.authenticate('admin', 'wrong-password'),
    /authentication_failed/);
  await assert.rejects(client.authenticate('missing-user', 'wrong-password'),
    /authentication_failed/);
  const login = await client.authenticate('admin', adminPassword);
  assert.equal(login.role, 'superadmin');
  const durableAdmin = await client.request({ action: 'api_key_create',
    name: 'certification-admin', role: 'admin' });
  assert.match(durableAdmin.key, /^pdb_[0-9a-f]{12}_[A-Za-z0-9_-]+$/);
  client.token = durableAdmin.key;

  const alpha = (await client.request({ action: 'community_project_create',
    name: 'alpha' })).project;
  const removed = (await client.request({ action: 'community_project_create',
    name: 'delete-me' })).project;
  await client.createDatabase('app');
  client.database = 'app';
  await client.createCollection('users');
  await client.createCollection('vectors');
  await client.insert('users', { id: 'persistent', name: 'Ada', active: true,
    score: 7, ratio: 1.5, empty: null, tags: ['math', 'code'],
    profile: { city: 'London' }, unicode: 'నమస్తే' });
  await client.request({ action: 'community_database_map', database: 'app',
    project_id: alpha.id });

  const racedProjects = await Promise.all(Array.from({ length: 6 }, () =>
    client.request({ action: 'community_project_create', name: 'duplicate-name' })));
  assert.equal(new Set(racedProjects.map((result) => result.project.id)).size, 6);
  const racedKeys = await Promise.all(Array.from({ length: 6 }, (_, index) =>
    client.request({ action: 'api_key_create', name: `race-key-${index}`, role: 'read' })));
  await Promise.all(racedKeys.map((key) => client.request({ action: 'api_key_revoke', id: key.id })));
  await Promise.all(racedKeys.map((key) => assert.rejects(
    new PacificDBClient({ port: enginePort, token: key.key }).request({ action: 'listDatabases' }),
    /unauthorized/)));
  const duplicateWrites = await Promise.allSettled(Array.from({ length: 8 }, (_, index) =>
    client.insert('users', { id: 'duplicate-race', value: index })));
  assert.ok(duplicateWrites.some((result) => result.status === 'fulfilled'));
  assert.equal((await client.request({ action: 'count', collection: 'users',
    filter: { id: 'duplicate-race' } })).count, 1);
  await Promise.all(Array.from({ length: 6 }, (_, index) =>
    client.insert('users', { id: `independent-${index}`, value: 0 })));
  await Promise.all(Array.from({ length: 6 }, (_, index) => Promise.all([
    client.request({ action: 'updateOne', collection: 'users',
      filter: { id: `independent-${index}` }, update: { value: index + 1 } }),
    client.find('users', { id: 'persistent' })
  ])));
  for (let index = 0; index < 6; index += 1) {
    assert.equal((await client.find('users', { id: `independent-${index}` })).data[0].value,
      index + 1);
  }

  const backup = await client.request({ action: 'create_backup', description: 'e2e' });
  assert.ok(backup.backup_id);
  await assert.rejects(client.request({ action: 'verify_backup',
    backup_id: 'backup-does-not-exist' }), /backup_not_found/);
  await assert.rejects(client.request({ action: 'restore_backup',
    backup_id: 'backup-does-not-exist' }), /restore_failed/);
  const disposableBackup = await client.request({ action: 'create_backup',
    description: 'delete-me' });
  const corruptBackup = await client.request({ action: 'create_backup',
    description: 'corruption-check' });
  await writeFile(path.join(backupRoot, corruptBackup.backup_id, 'unexpected-file'),
    'checksum must change');
  await assert.rejects(client.request({ action: 'verify_backup',
    backup_id: corruptBackup.backup_id }), /backup_verification_failed/);
  await client.request({ action: 'delete_backup', backup_id: corruptBackup.backup_id });
  const shownKey = await client.request({ action: 'api_key_create',
    name: 'show-and-revoke', role: 'read' });

  const mediaBytes = Buffer.alloc(700_000);
  for (let index = 0; index < mediaBytes.length; index += 1) mediaBytes[index] = index % 251;
  const mediaFile = path.join(testRoot, 'three-chunk.mp4');
  const uploadedByShell = path.join(testRoot, 'shell-upload.png');
  const videoByShell = path.join(testRoot, 'shell-upload.mp4');
  const genericByShell = path.join(testRoot, 'shell-upload.bin');
  await writeFile(mediaFile, mediaBytes);
  await writeFile(uploadedByShell, Buffer.from('community-image'));
  await writeFile(videoByShell, Buffer.from('community-video'));
  await writeFile(genericByShell, Buffer.from('community-generic-media'));
  const media = await client.uploadMediaFile('videos', mediaFile, { chunkBytes: 262_144 });
  assert.equal(media.chunk_count, 3);
  const incomplete = (await client.request({ action: 'community_media_begin',
    collection: 'videos', filename: 'incomplete.mp4', content_type: 'video/mp4',
    size_bytes: mediaBytes.length, chunk_count: 3, sha256: sha256(mediaBytes) })).media;
  await assert.rejects(client.request({ action: 'community_media_finalize',
    media_id: incomplete.id }), /missing chunks/);
  const resumable = (await client.request({ action: 'community_media_begin',
    collection: 'videos', filename: path.basename(mediaFile), content_type: 'video/mp4',
    size_bytes: mediaBytes.length, chunk_count: 3, sha256: sha256(mediaBytes) })).media;
  const firstChunk = mediaBytes.subarray(0, 262_144);
  await client.request({ action: 'community_media_put_chunk', media_id: resumable.id,
    index: 0, data: firstChunk.toString('base64'), size_bytes: firstChunk.length,
    sha256: sha256(firstChunk) });
  await assert.rejects(client.request({ action: 'community_media_put_chunk',
    media_id: resumable.id, index: 0, data: Buffer.from('different').toString('base64'),
    size_bytes: 9, sha256: sha256(Buffer.from('different')) }),
    /conflicts with committed chunk/);
  const readyBeforeResume = await client.request({ action: 'community_media_list' });
  assert.ok(!readyBeforeResume.media.some((item) => item.id === resumable.id));
  const persistentMedia = await client.uploadMediaFile('videos', mediaFile,
    { chunkBytes: 262_144, resume: resumable.id });
  assert.equal(persistentMedia.status, 'ready');
  const restartIncomplete = (await client.request({ action: 'community_media_begin',
    collection: 'videos', filename: path.basename(mediaFile), content_type: 'video/mp4',
    size_bytes: mediaBytes.length, chunk_count: 3, sha256: sha256(mediaBytes) })).media;
  await client.request({ action: 'community_media_put_chunk', media_id: restartIncomplete.id,
    index: 0, data: firstChunk.toString('base64'), size_bytes: firstChunk.length,
    sha256: sha256(firstChunk) });
  const parallelMediaBytes = Buffer.alloc(280_000, 31);
  const parallelChunks = Array.from({ length: 4 }, (_, index) =>
    parallelMediaBytes.subarray(index * 70_000, (index + 1) * 70_000));
  const parallelMedia = (await client.request({ action: 'community_media_begin',
    collection: 'media', filename: 'parallel.bin', content_type: 'application/octet-stream',
    size_bytes: parallelMediaBytes.length, chunk_count: parallelChunks.length,
    sha256: sha256(parallelMediaBytes) })).media;
  await Promise.all(parallelChunks.map((chunk, index) => client.request({
    action: 'community_media_put_chunk', media_id: parallelMedia.id, index,
    data: chunk.toString('base64'), size_bytes: chunk.length, sha256: sha256(chunk)
  })));
  assert.equal((await client.request({ action: 'community_media_finalize',
    media_id: parallelMedia.id })).media.status, 'ready');

  await client.putVector('vectors', 'east', [1, 0]);
  await client.putVector('vectors', 'north', [0, 1]);
  assert.throws(() => client.queryVector('vectors', []), /non-empty array/);
  await assert.rejects(client.queryVector('vectors', [1, 0], { metric: 'unknown' }),
    /unsupported vector metric/);

  const exportFile = path.join(testRoot, 'backup-export.json');
  const downloadFile = path.join(testRoot, 'download.mp4');
  const shellCommands = [
    'help', 'help projects', 'help backups', 'help media', 'help vectors',
    'login admin', adminPassword, 'whoami',
    'create project shell-created', 'list projects', `use project ${removed.id}`,
    'show project', `delete project ${removed.id}`, 'context show',
    `use project ${alpha.id}`, 'show project',
    'create database temporary', 'list databases', 'use temporary', 'show database',
    'drop database temporary', 'use app', 'show database',
    'create collection shell_docs', 'list collections',
    'insert shell_docs {"id":"shell-1","name":"Grace"}',
    'find shell_docs {"id":"shell-1"}', 'findOne shell_docs {"id":"shell-1"}',
    'update shell_docs {"id":"shell-1"} {"name":"Grace Hopper"}',
    'count shell_docs {}',
    'aggregate shell_docs [{"$match":{"id":"shell-1"}},{"$project":{"name":1}},{"$count":"total"}]',
    'explain shell_docs {"id":"shell-1"}', 'delete shell_docs {"id":"shell-1"}',
    'create backup --name shell-backup', 'list backups',
    `show backup ${backup.backup_id}`, `backup verify ${backup.backup_id}`,
    `backup export ${backup.backup_id} ${exportFile}`,
    `restore backup ${backup.backup_id}`, 'list restores',
    `delete backup ${disposableBackup.backup_id}`,
    'create api-key --name shell-key --role readwrite', 'list api-keys',
    `show api-key ${shownKey.id}`, `revoke api-key ${shownKey.id}`,
    `upload image ${uploadedByShell} --collection images`,
    `upload video ${videoByShell} --collection videos`,
    `upload media ${genericByShell} --collection files`, 'list media',
    'list media --all', `show media ${media.id}`, 'find media three-chunk',
    `download media ${media.id} ${downloadFile}`, `delete media ${media.id}`,
    `media cleanup ${incomplete.id}`,
    'put vector vectors diagonal [0.7,0.7]',
    'query vector vectors [0.9,0.1] --k 3 --metric cosine',
    'context show', 'status', 'history', 'clear',
    'request {"action":"ping"}', 'logout', 'context clear', 'quit'
  ];
  const shellOutput = await runShell(shellCommands);
  console.log('E2E: advertised shell command matrix complete');
  assertPublicOutput(shellOutput);
  assert.match(shellOutput, /PACIFICDB[\s\S]*COMMUNITY BETA/i);
  assert.match(shellOutput, /Grace Hopper/);
  assert.match(shellOutput, /"status": "pong"/);
  assert.equal(sha256(await readFile(downloadFile)), sha256(mediaBytes));

  const invalidCommands = [
    'login admin', adminPassword, `use project ${alpha.id}`,
    'use project definitely-does-not-exist', 'use database-that-does-not-exist',
    'use app', 'aggregate users [{"$group":{}}]', 'quit'
  ];
  const invalidOutput = await runShell(invalidCommands);
  console.log('E2E: shell error matrix complete');
  assertPublicOutput(invalidOutput);
  assert.match(invalidOutput, /project_not_found/);
  assert.match(invalidOutput, /database_not_found/);
  assert.match(invalidOutput, /unsupported Community aggregation stage/);
  const context = JSON.parse(await readFile(path.join(cliHome, 'context.json')));
  assert.equal(context.projectId, alpha.id);

  const exported = JSON.parse(await readFile(exportFile, 'utf8'));
  assert.equal(exported.format, 'pacificdb-full-backup-v1');
  assert.ok(exported.files.length > 0);
  let exportedBytes = 0;
  for (const file of exported.files) {
    const bytes = Buffer.concat(file.chunks.map((chunk) => Buffer.from(chunk, 'base64')));
    assert.equal(bytes.length, file.size_bytes);
    assert.equal(sha256(bytes), file.sha256);
    exportedBytes += bytes.length;
  }
  assert.ok(exportedBytes > 0);
  assert.equal((await stat(exportFile)).mode & 0o777, 0o600);
  assert.equal((await stat(path.join(cliHome, 'context.json'))).mode & 0o777, 0o600);
  assert.equal((await stat(path.join(cliHome, 'history'))).mode & 0o777, 0o600);
  const history = await readFile(path.join(cliHome, 'history'), 'utf8');
  assert.doesNotMatch(history, new RegExp(adminPassword));
  assert.doesNotMatch(history, /pdb_[0-9a-f]{12}_/);

  const restored = await client.request({ action: 'list_restores' });
  const restoreEntry = restored.restores.find((entry) =>
    entry.backup_id === backup.backup_id && entry.status === 'completed');
  assert.ok(restoreEntry);
  assert.equal((await client.request({ action: 'community_media_get',
    media_id: persistentMedia.id })).media.status, 'ready');

  await stopEngine();
  await startEngine();
  console.log('E2E: graceful restart complete');
  client.token = durableAdmin.key;
  client.database = 'app';
  assert.equal((await client.find('users', { id: 'persistent' })).data[0].name, 'Ada');
  assert.equal((await client.request({ action: 'community_project_get', id: alpha.id }))
    .project.name, 'alpha');
  assert.equal((await client.request({ action: 'community_database_project',
    database: 'app' })).mapping.project_id, alpha.id);
  assert.equal((await client.request({ action: 'community_media_get',
    media_id: persistentMedia.id })).media.status, 'ready');
  assert.equal((await client.queryVector('vectors', [0.9, 0.1], { k: 1 }))
    .data[0].id, 'east');
  assert.ok((await client.request({ action: 'get_backup', backup_id: backup.backup_id }))
    .backup.backup_id === backup.backup_id);
  assert.ok((await client.request({ action: 'list_restores' })).restores.some((entry) =>
    entry.backup_id === backup.backup_id && entry.status === 'completed'));
  await assert.rejects(new PacificDBClient({ port: enginePort,
    token: racedKeys[0].key }).request({ action: 'listDatabases' }), /unauthorized/);
  const resumedAfterRestart = await client.uploadMediaFile('videos', mediaFile,
    { chunkBytes: 262_144, resume: restartIncomplete.id });
  assert.equal(resumedAfterRestart.status, 'ready');
  const visibleDatabases = await client.request({ action: 'listDatabases' });
  assert.ok(!visibleDatabases.includes('system') && !visibleDatabases.includes('pacificdb_meta'));
  await assert.rejects(client.request({ action: 'find', dbName: 'pacificdb_meta',
    collection: 'projects', filter: {} }), /reserved_namespace/);

  await client.request({ action: 'updateOne', collection: 'users',
    filter: { id: 'persistent' }, update: { name: 'Ada Lovelace' } });
  const crashProject = (await client.request({ action: 'community_project_create',
    name: 'crash-durable' })).project;
  const concurrent = Array.from({ length: 24 }, (_, index) => client.insert('users', {
    id: `parallel-${index}`, value: index
  }));
  await Promise.all(concurrent);
  await stopEngine('SIGKILL');
  await startEngine();
  console.log('E2E: abrupt restart complete');
  client.token = durableAdmin.key;
  client.database = 'app';
  assert.equal((await client.find('users', { id: 'persistent' })).data[0].name,
    'Ada Lovelace');
  assert.equal((await client.request({ action: 'community_project_get',
    id: crashProject.id })).project.name, 'crash-durable');
  assert.equal((await client.request({ action: 'count', collection: 'users',
    filter: { id: { $in: Array.from({ length: 24 }, (_, index) => `parallel-${index}`) } } }))
    .count, 24);
  assert.equal((await client.request({ action: 'get_backup',
    backup_id: backup.backup_id })).backup.backup_id, backup.backup_id);
  assert.ok((await client.request({ action: 'list_restores' })).restores.some((entry) =>
    entry.backup_id === backup.backup_id && entry.status === 'completed'));
  await assert.rejects(new PacificDBClient({ port: enginePort,
    token: racedKeys[0].key }).request({ action: 'listDatabases' }), /unauthorized/);
  assert.equal((await client.request({ action: 'community_media_get',
    media_id: persistentMedia.id })).media.status, 'ready');
  assert.equal((await client.queryVector('vectors', [0.9, 0.1], { k: 1 }))
    .data[0].id, 'east');

  const freshShellCommands = [
    'login admin', adminPassword, 'list projects', `use project ${alpha.id}`,
    'show project', 'use app', 'find users {"id":"persistent"}', 'exit'
  ];
  const freshShell = await runShell(freshShellCommands);
  assertPublicOutput(freshShell);
  assert.match(freshShell, /Ada Lovelace/);
  assert.match(freshShell, /"name": "alpha"/);

  const storedKeys = await readFile(path.join(dataRoot, 'security', 'api_keys.json'), 'utf8');
  assert.doesNotMatch(storedKeys, new RegExp(durableAdmin.key));
  assert.match(storedKeys, /secret_hash/);
  const log = await readFile(engineLog, 'utf8');
  assert.doesNotMatch(log, new RegExp(adminPassword));
  assert.doesNotMatch(log, new RegExp(durableAdmin.key));

  await stopEngine();
  const restoredIdentity = JSON.parse(await readFile(path.join(
    restoreEntry.target_directory, '.pacificdb-root-identity.json'), 'utf8'));
  const restoredPort = await freePort();
  const restoredRaftPort = await freePort();
  await startEngine({ DATA_ROOT: restoreEntry.target_directory,
    BACKUP_ROOT: path.join(testRoot, 'restored-backups'),
    RESTORE_DIR: path.join(testRoot, 'restored-restores'),
    ENGINE_PORT: String(restoredPort), RAFT_LISTEN_PORT: String(restoredRaftPort),
    RAFT_CLUSTER_ID: restoredIdentity.clusterId, RAFT_NODE_ID: restoredIdentity.nodeId,
    PACIFICDB_ENGINE_ADMIN_USERNAME: '', PACIFICDB_ENGINE_ADMIN_PASSWORD: '' }, restoredPort);
  const restoredClient = new PacificDBClient({ port: restoredPort });
  restoredClient.token = durableAdmin.key;
  restoredClient.database = 'app';
  const restoredDocument = await restoredClient.find('users', { id: 'persistent' });
  assert.equal(restoredDocument.data[0].name, 'Ada');
  assert.equal((await restoredClient.request({ action: 'community_project_get',
    id: alpha.id })).project.name, 'alpha');
  await stopEngine();

  const shellCommandCount = shellCommands.length + invalidCommands.length +
    freshShellCommands.length - 3; // one password response follows each login
  console.log(JSON.stringify({ status: 'PASS', root: testRoot,
    shell_commands_executed: shellCommandCount, media_chunks: 3, concurrent_writes: 24,
    concurrent_project_creates: 6, concurrent_api_key_create_revoke: 6,
    concurrent_media_chunks: 4, graceful_restarts: 1, abrupt_restarts: 1 }, null, 2));
} finally {
  await stopEngine().catch(() => {});
  if (process.env.PACIFICDB_KEEP_E2E_ROOT !== '1') await rm(testRoot, { recursive: true });
}
