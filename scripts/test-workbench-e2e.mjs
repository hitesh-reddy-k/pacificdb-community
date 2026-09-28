#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, rm } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PassThrough } from 'node:stream';
const { main } = await import(process.env.PACIFICDB_TEST_CLI_MODULE || '../cli/src/cli.js');
import { PacificDBClient } from '../sdk/node/src/index.js';

const build = path.resolve(process.argv[2] || 'build');
const binary = path.join(build, process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-workbench-e2e-'));
let engine;
let workbench;

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

async function waitReady(port) {
  const client = new PacificDBClient({ port, timeoutMs: 300, poolSize: 1 });
  try {
    for (let attempt = 0; attempt < 150; attempt += 1) {
      if (engine.exitCode !== null) break;
      try {
        if ((await client.request({ action: 'ping' })).status === 'pong') return;
      } catch { /* still starting */ }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error('engine did not become ready');
  } finally { client.close(); }
}

try {
  const [port, raftPort] = await Promise.all([freePort(), freePort()]);
  const directories = ['data', 'backup', 'restore'];
  await Promise.all(directories.map((name) =>
    mkdir(path.join(root, name), { recursive: true, mode: 0o700 })));
  engine = spawn(binary, [], { stdio: 'ignore', env: {
    ...process.env,
    PACIFICDB_ENVIRONMENT: 'development',
    PACIFICDB_HOME: root,
    DATA_ROOT: path.join(root, 'data'),
    BACKUP_ROOT: path.join(root, 'backup'),
    RESTORE_DIR: path.join(root, 'restore'),
    ENGINE_AUTH_REQUIRED: '0',
    ENGINE_BIND_HOST: '127.0.0.1',
    ENGINE_PORT: String(port),
    RAFT_LISTEN_PORT: String(raftPort),
    RAFT_CLUSTER_ID: 'workbench-e2e',
    RAFT_NODE_ID: 'node-1',
    RAFT_IS_LEADER: '1',
    MIN_QUORUM_SIZE: '1',
    ENGINE_CPU_CORES: '2',
  } });
  await waitReady(port);

  const output = new PassThrough();
  workbench = await main(['workbench', '--port', String(port), '--no-start'], {
    input: new PassThrough(), output,
  });
  const htmlResponse = await fetch(workbench.url);
  assert.equal(htmlResponse.status, 200);
  const html = await htmlResponse.text();
  const token = html.match(/name="pacificdb-token" content="([a-f0-9]+)"/)[1];
  assert.ok(token.length >= 32);
  assert.equal((await fetch(new URL('/app.js', workbench.url))).status, 200);
  assert.equal((await fetch(new URL('/app.css', workbench.url))).status, 200);

  const post = async (op, values = {}) => {
    const response = await fetch(new URL('/api/execute', workbench.url), {
      method: 'POST', headers: {
        'Content-Type': 'application/json',
        'X-PacificDB-Workbench-Token': token,
      }, body: JSON.stringify({ op, ...values }),
    });
    const payload = await response.json();
    assert.equal(response.status, 200, JSON.stringify(payload));
    return payload.result;
  };

  const project = (await post('projects.create', { name: 'workbench-demo' })).project;
  assert.equal((await post('projects.list')).projects[0].id, project.id);
  const scope = { projectId: project.id, database: 'app', collection: 'records' };
  await post('databases.create', { projectId: project.id, name: 'app' });
  assert.deepEqual((await post('databases.list', scope)).databases, ['app']);
  await post('collections.create', { ...scope, name: 'records' });
  const collectionList = await post('collections.list', scope);
  assert.ok(collectionList.collections.includes('records'),
    JSON.stringify(collectionList));
  await post('documents.insert', { ...scope, data: { id: 'one', value: 1 } });
  assert.equal((await post('documents.find', { ...scope,
    filter: { id: 'one' } })).data[0].value, 1);
  await post('documents.update', { ...scope, filter: { id: 'one' },
    update: { value: 2 } });
  assert.equal((await post('documents.find', { ...scope,
    filter: { id: 'one' } })).data[0].value, 2);
  await post('vectors.put', { ...scope, id: 'vector-one', vector: [1, 0] });
  assert.equal((await post('vectors.query', { ...scope,
    vector: [1, 0], k: 1 })).data[0].id, 'vector-one');
  await post('documents.delete', { ...scope, filter: { id: 'one' } });
  assert.equal((await post('documents.find', { ...scope,
    filter: { id: 'one' } })).count, 0);

  await post('collections.create', { ...scope, name: 'media' });
  const bytes = Buffer.from('PacificDB workbench media transfer');
  const upload = await fetch(new URL('/api/media/upload', workbench.url), {
    method: 'POST', headers: {
      'Content-Type': 'application/octet-stream',
      'X-PacificDB-Workbench-Token': token,
      'X-Project-Id': project.id,
      'X-Database': 'app',
      'X-Collection': 'media',
      'X-Filename': 'sample.txt',
    }, body: bytes,
  });
  const uploaded = await upload.json();
  assert.equal(upload.status, 200, JSON.stringify(uploaded));
  const mediaId = uploaded.result.id;
  assert.ok((await post('media.list', { ...scope, collection: 'media' }))
    .media.some((item) => item.id === mediaId));
  const download = await fetch(new URL('/api/media/download', workbench.url), {
    method: 'POST', headers: {
      'Content-Type': 'application/json',
      'X-PacificDB-Workbench-Token': token,
    }, body: JSON.stringify({ projectId: project.id, database: 'app', mediaId }),
  });
  assert.equal(download.status, 200);
  assert.deepEqual(Buffer.from(await download.arrayBuffer()), bytes);
  console.log('WORKBENCH_E2E_PASS');
} finally {
  if (workbench) await workbench.close();
  if (engine && engine.exitCode === null) {
    engine.kill('SIGINT');
    const timeout = setTimeout(() => engine.kill('SIGKILL'), 10000);
    await once(engine, 'exit');
    clearTimeout(timeout);
  }
  await rm(root, { recursive: true, force: true });
}
