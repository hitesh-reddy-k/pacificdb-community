#!/usr/bin/env node

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { open } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, rm, stat, statfs, symlink } from 'node:fs/promises';
import net from 'node:net';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2]);
const mountRoot = path.resolve(process.argv[3]);
const binary = path.join(build, 'db_engine');
const cliBinary = path.join(build, 'pacificdb');
const allCategories = [
  'project-create', 'project-delete', 'database-create', 'database-drop',
  'collection-create', 'collection-drop', 'document-insert', 'document-update',
  'document-delete', 'index-create', 'index-drop', 'api-key-create',
  'api-key-revoke', 'backup-create', 'backup-restore', 'media-begin',
  'media-chunk', 'media-finalize', 'vector-put', 'cli-history', 'cli-context'
];
const categories = process.env.PACIFICDB_ENOSPC_CATEGORIES
  ? process.env.PACIFICDB_ENOSPC_CATEGORIES.split(',').map((value) => value.trim()).filter(Boolean)
  : allCategories;
for (const category of categories) {
  assert.ok(allCategories.includes(category), `unknown category ${category}`);
}

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

async function fillFilesystem(filename) {
  const file = await open(filename, 'w');
  const chunk = Buffer.alloc(1024 * 1024, 0xa5);
  let bytes = 0;
  let observedEnospc = false;
  try {
    for (;;) {
      await file.write(chunk);
      bytes += chunk.length;
    }
  } catch (error) {
    assert.equal(error.code, 'ENOSPC');
    observedEnospc = true;
  } finally {
    await file.close();
  }
  assert.ok(observedEnospc && bytes > 0);
  return bytes;
}

async function runCase(category) {
  const root = path.join(mountRoot, category);
  const dataRoot = path.join(root, 'data');
  const backupRoot = path.join(root, 'backup');
  const restoreRoot = path.join(root, 'restore');
  await Promise.all([dataRoot, backupRoot, restoreRoot].map((directory) =>
    mkdir(directory, { recursive: true })));
  const port = await freePort();
  const raftPort = await freePort();
  const password = `disk-full-${category}`;
  const environment = {
    ...process.env,
    PACIFICDB_ENVIRONMENT: 'development', DATA_ROOT: dataRoot,
    BACKUP_ROOT: backupRoot, RESTORE_DIR: restoreRoot,
    TMP_DIR: path.join(root, 'tmp'), LOG_DIR: path.join(root, 'logs'),
    ENGINE_BIND_HOST: '127.0.0.1', ENGINE_PORT: String(port),
    ENGINE_AUTH_REQUIRED: '1', PACIFICDB_ENGINE_ADMIN_USERNAME: 'admin',
    PACIFICDB_ENGINE_ADMIN_PASSWORD: password,
    RAFT_CLUSTER_ID: `enospc-${category}`, RAFT_NODE_ID: 'node-1',
    RAFT_LISTEN_PORT: String(raftPort), RAFT_IS_LEADER: '1', MIN_QUORUM_SIZE: '1',
    INSERT_SYNC_THRESHOLD_BYTES: String(4 * 1024 * 1024),
    ENGINE_CPU_CORES: '2', CONN_MIN_THREADS: '2', CONN_MAX_THREADS: '4',
    DBQ_SHARDS: '2', DBQ_WORKERS_PER_SHARD: '1', ADAPTIVE_ADMISSION: '0'
  };
  let engine;
  let engineOutput = '';
  async function waitReady() {
    const probe = new PacificDBClient({ port, timeoutMs: 300 });
    let last;
    for (let attempt = 0; attempt < 150; attempt += 1) {
      if (engine?.exitCode !== null) {
        throw new Error(`engine exited with ${engine.exitCode}: ${engineOutput.slice(-8192)}`);
      }
      try {
        if ((await probe.request({ action: 'ping' })).status === 'pong') return;
      } catch (error) { last = error; }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error(`engine failed to start for ${category}: ${last?.message}`);
  }
  async function start() {
    engineOutput = '';
    engine = spawn(binary, [], { cwd: repositoryRoot, env: environment,
      stdio: ['ignore', 'pipe', 'pipe'] });
    const capture = (chunk) => {
      engineOutput = (engineOutput + chunk).slice(-65536);
    };
    engine.stdout.on('data', capture);
    engine.stderr.on('data', capture);
    await waitReady();
  }
  async function stop(signal = 'SIGINT') {
    if (!engine || engine.exitCode !== null) return;
    engine.kill(signal);
    const timer = setTimeout(() => engine.kill('SIGKILL'), 8000);
    await once(engine, 'exit');
    clearTimeout(timer);
  }

  const filler = path.join(mountRoot, `filler-${category}`);
  try {
    await start();
    let client = new PacificDBClient({ port, timeoutMs: 5000 });
    await client.authenticate('admin', password);
    const baselineProject = (await client.request({ action: 'community_project_create',
      name: `baseline-${category}` })).project;
    await client.createDatabase('app');
    client.database = 'app';
    await client.createCollection('docs');
    await client.createCollection('vectors');
    await client.insert('docs', { id: 'baseline', value: 1 });
    let targetKey;
    let targetBackup;
    let incomplete;
    if (category === 'collection-drop') await client.createCollection('drop-target');
    if (category === 'index-drop') {
      await client.request({ action: 'createIndex', collection: 'docs',
        name: 'value_1', fields: { value: 1 } });
    }
    if (category === 'api-key-revoke') {
      targetKey = await client.request({ action: 'api_key_create',
        name: 'revoke-under-enospc', role: 'read' });
    }
    if (category === 'backup-restore') {
      targetBackup = await client.request({ action: 'create_backup',
        description: 'restore-under-enospc' });
    }
    if (category === 'media-chunk' || category === 'media-finalize') {
      const bytes = Buffer.alloc(65536, 7);
      incomplete = (await client.request({ action: 'community_media_begin',
        collection: 'media', filename: 'disk.bin', content_type: 'application/octet-stream',
        size_bytes: 65536, chunk_count: 1,
        sha256: createHash('sha256').update(bytes).digest('hex') })).media;
      if (category === 'media-finalize') {
        await client.request({ action: 'community_media_put_chunk',
          media_id: incomplete.id, index: 0, data: bytes.toString('base64'),
          size_bytes: bytes.length,
          sha256: createHash('sha256').update(bytes).digest('hex') });
      }
    }
    const cliHome = path.join(root, 'cli-state');
    if (category === 'cli-history' || category === 'cli-context') {
      await mkdir(cliHome, { recursive: true });
      if (category === 'cli-context') await symlink('/dev/null', path.join(cliHome, 'history'));
    }

    const filledBytes = await fillFilesystem(filler);
    const operations = {
      'project-create': () => client.request({ action: 'community_project_create', name: 'must-fail' }),
      'project-delete': () => client.request({ action: 'community_project_delete', id: baselineProject.id }),
      'database-create': () => client.createDatabase('must-fail'),
      'database-drop': () => client.request({ action: 'dropDatabase', dbName: 'app' }),
      'collection-create': () => client.createCollection('must-fail'),
      'collection-drop': () => client.request({ action: 'dropCollection', collection: 'drop-target' }),
      'document-insert': () => client.insert('docs', { id: 'must-fail', value: 2 }),
      'document-update': () => client.updateOne('docs', { id: 'baseline' }, { value: 2 }),
      'document-delete': () => client.deleteOne('docs', { id: 'baseline' }),
      'index-create': () => client.request({ action: 'createIndex', collection: 'docs',
        name: 'value_1', fields: { value: 1 } }),
      'index-drop': () => client.request({ action: 'dropIndex', collection: 'docs', name: 'value_1' }),
      'api-key-create': () => client.request({ action: 'api_key_create', name: 'must-fail', role: 'read' }),
      'api-key-revoke': () => client.request({ action: 'api_key_revoke', id: targetKey.id }),
      'backup-create': () => client.request({ action: 'create_backup', description: 'must-fail' }),
      'backup-restore': () => client.request({ action: 'restore_backup',
        backup_id: targetBackup.backup_id, target_dir: path.join(root, 'restore-under-enospc') }),
      'media-begin': () => client.request({ action: 'community_media_begin',
        collection: 'media', filename: 'must-fail.bin', content_type: 'application/octet-stream',
        size_bytes: 1, chunk_count: 1, sha256: '0'.repeat(64) }),
      'media-chunk': () => client.request({ action: 'community_media_put_chunk',
        media_id: incomplete.id, index: 0, data: Buffer.alloc(65536, 7).toString('base64'),
        size_bytes: 65536,
        sha256: createHash('sha256').update(Buffer.alloc(65536, 7)).digest('hex') }),
      'media-finalize': () => client.request({ action: 'community_media_finalize',
        media_id: incomplete.id }),
      'vector-put': () => client.putVector('vectors', 'must-fail', [1, 0]),
      'cli-history': () => runCliStateFailure(cliHome, port, ['status', 'exit'],
        /could not save CLI history/),
      'cli-context': () => runCliStateFailure(cliHome, port, ['context clear', 'exit'],
        /could not save CLI context/)
    };
    let rejected = false;
    let response;
    if (category.startsWith('cli-')) {
      response = await operations[category]();
      rejected = Boolean(response?.error) || !['ok', 'updated', 'deleted'].includes(response?.status);
    } else {
      try {
        response = await operations[category]();
      } catch {
        rejected = true;
      }
      if (response !== undefined) {
        rejected = Boolean(response?.error) || !['ok', 'updated', 'deleted'].includes(response?.status);
      }
    }
    assert.ok(rejected,
      `${category} falsely acknowledged under ENOSPC: ${JSON.stringify(response)}`);

    await stop('SIGKILL');
    await rm(filler, { force: true });
    await start();
    client = new PacificDBClient({ port, timeoutMs: 5000 });
    await client.authenticate('admin', password);
    client.database = 'app';
    assert.equal((await client.request({ action: 'community_project_get',
      id: baselineProject.id })).project.name, `baseline-${category}`);
    assert.equal((await client.find('docs', { id: 'baseline' })).data[0].value, 1);
    await stop();
    await rm(root, { recursive: true, force: true });
    return filledBytes;
  } finally {
    await stop('SIGKILL').catch(() => {});
    await rm(filler, { force: true }).catch(() => {});
    await rm(root, { recursive: true, force: true }).catch(() => {});
  }
}

async function runCliStateFailure(cliHome, port, commands, expected) {
  const child = spawn(cliBinary, ['--port', String(port), 'shell'], {
    cwd: repositoryRoot,
    env: { ...process.env, PACIFICDB_CLI_HOME: cliHome },
    stdio: ['pipe', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  child.stdin.end(commands.join('\n') + '\n');
  await once(child, 'exit');
  return expected.test(output)
    ? { error: 'cli_state_write_failed' }
    : { status: 'ok', output };
}

const results = {};
for (const category of categories) {
  const space = await statfs(mountRoot);
  assert.ok(space.bavail * space.bsize > 100 * 1024 * 1024,
    `tmpfs space was not released before ${category}`);
  results[category] = await runCase(category);
}
const filesystem = await stat(mountRoot);
assert.ok(filesystem.isDirectory());
console.log(JSON.stringify({ status: 'PASS', genuine_enospc: true,
  isolated_mount_namespace: true, write_categories: categories.length,
  filled_bytes_by_case: results }, null, 2));
