#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
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
const frames = [];
const originalRequest = PacificDBClient.prototype.request;

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

  // Seed legacy-mapped data through the retained API; the UI must also see unmapped data.
  const legacy = new PacificDBClient({ port, poolSize: 1 });
  let project;
  try {
    project = (await legacy.createProject('legacy-demo')).project;
    await legacy.useProject(project.id);
    await legacy.createDatabase('legacy_app');
    await legacy.createCollection('records');
    await legacy.insert('records', { id: 'kept', value: 'legacy' });
  } finally { legacy.close(); }
  PacificDBClient.prototype.request = function(command) {
    frames.push({ ...command, dbName: command.dbName ?? this.database });
    return originalRequest.call(this, command);
  };
  const output = new PassThrough();
  workbench = await main(['workbench', '--url', `pacificdb://127.0.0.1:${port}/legacy_app`, '--no-start'], {
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

  assert.equal((await post('connection.info')).database, 'legacy_app');
  const scope = { database: 'app', collection: 'records' };
  await post('databases.create', { name: 'app' });
  assert.deepEqual((await post('databases.list')).databases.sort(), ['app', 'legacy_app']);
  assert.equal((await post('documents.find', { database: 'legacy_app', collection: 'records', filter: { id: 'kept' } })).data[0].value, 'legacy');
  frames.length = 0;
  await post('collections.create', { ...scope, name: 'records' });
  const collectionList = await post('collections.list', scope);
  assert.ok(collectionList.collections.includes('records'),
    JSON.stringify(collectionList));
  await post('documents.insert', { ...scope, data: { id: 'one', value: 1 } });
  frames.length = 0;
  assert.equal((await post('documents.find', { ...scope,
    filter: { id: 'one' } })).data[0].value, 1);
  assert.deepEqual(frames.map(frame => frame.action), ['find']);
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
    }, body: JSON.stringify({ database: 'app', collection: 'media', mediaId }),
  });
  assert.equal(download.status, 200);
  assert.deepEqual(Buffer.from(await download.arrayBuffer()), bytes);
  assert.ok(frames.every(frame => !frame.action.startsWith('community_project_') && frame.action !== 'community_database_list'));
  const legacyCheck = new PacificDBClient({ port, poolSize: 1 });
  try { assert.ok((await legacyCheck.request({ action: 'community_project_list' })).projects.some(item => item.id === project.id)); }
  finally { legacyCheck.close(); }
  // Execute the copyable website snippets against the same disposable engine.
  const docs = await readFile(new URL('../site/docs.html', import.meta.url), 'utf8');
  const snippet = id => docs.match(new RegExp(`<code id="${id}">([\\s\\S]*?)</code>`))[1]
    .replaceAll('&#x27;', "'").replaceAll('&quot;', '"').replaceAll('&lt;', '<').replaceAll('&gt;', '>').replaceAll('&amp;', '&');
  const shell = spawnSync(path.join(build, process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb'),
    ['--port', String(port), '--no-start'], { input: snippet('quickstart-code') + '\nquit\n', encoding: 'utf8', timeout: 15_000,
      env: { ...process.env, PACIFICDB_CLI_HOME: path.join(root, 'snippet-cli') } });
  assert.equal(shell.status, 0, shell.stderr);
  assert.doesNotMatch(shell.stdout, /unknown (?:or invalid )?command|unknown_command|select a project|Error:/i);
  assert.match(shell.stdout, /Hello PacificDB/);
  frames.length = 0;
  await writeFile(path.join(root, 'demo.mp4'), Buffer.from('website media fixture'));
  const nodeCode = snippet('node-code').replace(/import[^;]+;/, '')
    .replace('127.0.0.1:9000', `127.0.0.1:${port}`).replace('port: 9000', `port: ${port}`)
    .replace('process.env.PACIFICDB_URL', 'undefined')
    .replace("'./demo.mp4'", JSON.stringify(path.join(root, 'demo.mp4')))
    .replace("'./downloaded-node.mp4'", JSON.stringify(path.join(root, 'downloaded-node.mp4')));
  await new (Object.getPrototypeOf(async function(){}).constructor)('PacificDBClient', 'PacificDB', nodeCode)(PacificDBClient, PacificDBClient);
  assert.ok(frames.every(frame => !frame.action.startsWith('community_project_') && frame.action !== 'community_database_map'), 'beginner snippet must use direct engine APIs');
  console.log('WORKBENCH_DATABASE_FIRST_E2E_AND_DOC_EXAMPLES_PASS');
} finally {
  PacificDBClient.prototype.request = originalRequest;
  if (workbench) await workbench.close();
  if (engine && engine.exitCode === null) {
    engine.kill('SIGINT');
    const timeout = setTimeout(() => engine.kill('SIGKILL'), 10000);
    await once(engine, 'exit');
    clearTimeout(timeout);
  }
  await rm(root, { recursive: true, force: true });
}
