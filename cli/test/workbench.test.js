import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import test from 'node:test';
import { spawn } from 'node:child_process';
import http from 'node:http';
import { once } from 'node:events';
import { createWorkbenchServer } from '../src/workbench.js';

test('malformed request targets return an error without stopping Workbench', async (t) => {
  const child = spawn(process.execPath, ['--input-type=module', '-e', `
    import { createWorkbenchServer } from ${JSON.stringify(new URL('../src/workbench.js', import.meta.url).href)};
    const workspace = await createWorkbenchServer({ clientFactory: () => ({}) });
    console.log(workspace.url);
  `], { stdio: ['ignore', 'pipe', 'pipe'] });
  t.after(async () => {
    if (child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit');
      child.kill();
      await exited;
    }
  });
  const [output] = await once(child.stdout, 'data');
  const url = new URL(String(output).trim());
  const status = await new Promise((resolve, reject) => {
    const request = http.request({ hostname: url.hostname, port: url.port,
      path: 'http://[', timeout: 5000 }, (response) => {
      response.resume();
      resolve(response.statusCode);
    });
    request.on('error', reject);
    request.on('timeout', () => request.destroy(new Error('request timed out')));
    request.end();
  });
  assert.equal(status, 400);
  assert.equal((await fetch(url)).status, 200);
});

function fakeFactory() {
  const state = {
    projects: [], databases: new Map(), collections: new Map(),
    documents: new Map(), media: null, upload: null,
  };
  return { state, factory: () => ({
    database: '',
    close() {},
    async request(command) {
      switch (command.action) {
        case 'ping': return { status: 'pong', isLeader: true };
        case 'health_check': return { status: 'healthy', memory_usage_ratio: 0.25 };
        case 'get_cluster_status': return { role: 'leader', raft_current_term: 2,
          raft_commit_index: 42 };
        case 'community_project_list': return { projects: state.projects };
        case 'community_project_create': {
          const project = { id: `project_${state.projects.length + 1}`, name: command.name };
          state.projects.push(project);
          return { project };
        }
        case 'community_project_get': return {
          project: state.projects.find((item) => item.id === command.id),
        };
        case 'community_database_list': return {
          databases: state.databases.get(command.project_id) || [],
        };
        case 'createDatabase': return { status: 'ok' };
        case 'community_database_map': {
          const names = state.databases.get(command.project_id) || [];
          names.push(command.database);
          state.databases.set(command.project_id, names);
          return { status: 'ok' };
        }
        case 'listCollections': return {
          collections: state.collections.get(this.database) || [],
        };
        case 'createCollection': {
          const names = state.collections.get(this.database) || [];
          names.push(command.collection);
          state.collections.set(this.database, names);
          return { status: 'ok' };
        }
        case 'count': return { status: 'ok', count: (state.documents.get(command.collection) || []).length };
        case 'listIndexes': return { status: 'ok', indexes: [{ name: '_id_' }] };
        case 'community_media_list': return {
          media: state.media ? [state.media] : [],
        };
        case 'community_media_get': return { media: state.media };
        default: throw new Error(`unexpected action: ${command.action}`);
      }
    },
    async find(name) { return { data: state.documents.get(name) || [] }; },
    async insert(name, data) {
      const documents = state.documents.get(name) || [];
      documents.push(data);
      state.documents.set(name, documents);
      return { status: 'ok' };
    },
    async updateOne(name, filter, update) {
      const document = (state.documents.get(name) || []).find(
        (value) => value.id === filter.id);
      if (document) Object.assign(document, update);
      return { status: 'ok' };
    },
    async deleteOne(name, filter) {
      state.documents.set(name, (state.documents.get(name) || []).filter(
        (value) => value.id !== filter.id));
      return { status: 'ok' };
    },
    async putVector(name, id, vector) { return { name, id, vector }; },
    async queryVector(name, vector) { return { data: [{ name, vector }] }; },
    async uploadMediaFile(name, filename) {
      state.upload = await readFile(filename);
      state.media = { id: 'media_1', database: this.database,
        collection: name, filename: 'demo.bin', status: 'ready' };
      return state.media;
    },
    async downloadMediaFile(id, destination) {
      assert.equal(id, 'media_1');
      await writeFile(destination, state.upload);
    },
  }) };
}

test('workbench scopes data to a project and protects its API', async (t) => {
  const { state, factory } = fakeFactory();
  const workbench = await createWorkbenchServer({ clientFactory: factory,
    connectionInfo: { host: '127.0.0.1', port: 9000, tls: false } });
  t.after(() => workbench.close());
  const html = await (await fetch(workbench.url)).text();
  assert.match(html, /PacificDB Workbench/);
  const logo = await fetch(new URL('/logo.png', workbench.url));
  assert.equal(logo.status, 200);
  assert.equal(logo.headers.get('content-type'), 'image/png');
  assert.deepEqual(Buffer.from(await logo.arrayBuffer()).subarray(0, 8),
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]));
  const token = html.match(/name="pacificdb-token" content="([a-f0-9]+)"/)[1];
  const post = async (body, headers = {}) => {
    const response = await fetch(new URL('/api/execute', workbench.url), {
      method: 'POST', headers: { 'Content-Type': 'application/json',
        'X-PacificDB-Workbench-Token': token, ...headers },
      body: JSON.stringify(body),
    });
    return { status: response.status, body: await response.json() };
  };
  assert.equal((await post({ op: 'projects.list' },
    { 'X-PacificDB-Workbench-Token': 'wrong' })).status, 403);
  assert.equal((await post({ op: 'projects.list' },
    { Origin: 'https://attacker.example' })).status, 403);
  assert.deepEqual((await post({ op: 'connection.info' })).body.result,
    { host: '127.0.0.1', port: 9000, tls: false });
  const engineStatus = await post({ op: 'engine.status' });
  assert.equal(engineStatus.status, 200);
  assert.equal(engineStatus.body.result.health.status, 'healthy');
  assert.equal(engineStatus.body.result.cluster.raft_commit_index, 42);
  assert.ok(Number.isFinite(engineStatus.body.result.latencyMs));
  assert.equal((await post({ op: 'documents.find', projectId: 'missing',
    database: 'app', collection: 'users' })).status, 400);

  const created = await post({ op: 'projects.create', name: 'demo' });
  assert.equal(created.body.result.project.id, 'project_1');
  const scope = { projectId: 'project_1', database: 'app', collection: 'users' };
  assert.equal((await post({ op: 'databases.create', projectId: scope.projectId,
    name: scope.database })).status, 200);
  assert.equal((await post({ op: 'collections.create', ...scope,
    name: scope.collection })).status, 200);
  assert.equal((await post({ op: 'documents.insert', ...scope,
    data: { id: 'one', name: 'Ada' } })).status, 200);
  assert.deepEqual((await post({ op: 'documents.find', ...scope })).body.result.data,
    [{ id: 'one', name: 'Ada' }]);
  assert.deepEqual((await post({ op: 'collections.summary', ...scope })).body.result,
    { count: 1, indexes: 1 });
  assert.equal((await post({ op: 'collections.summary', ...scope,
    collection: 'missing' })).status, 400);
  assert.equal((await post({ op: 'documents.update', ...scope, filter: {},
    update: { name: 'Grace' } })).status, 400);
  await post({ op: 'documents.update', ...scope, filter: { id: 'one' },
    update: { name: 'Grace' } });
  assert.equal(state.documents.get('users')[0].name, 'Grace');
  await post({ op: 'documents.delete', ...scope, filter: { id: 'one' } });
  assert.deepEqual(state.documents.get('users'), []);
  assert.equal((await post({ op: 'vectors.query', ...scope,
    vector: [0.2, 0.8] })).status, 200);
  assert.equal((await post({ op: 'documents.find', ...scope,
    projectId: 'other' })).status, 400);
  assert.equal((await post({ op: 'dropDatabase', ...scope })).status, 400);
});

test('workbench streams media through the scoped collection', async (t) => {
  const { state, factory } = fakeFactory();
  state.projects.push({ id: 'project_1', name: 'demo' });
  state.databases.set('project_1', ['app']);
  state.collections.set('app', ['media']);
  const workbench = await createWorkbenchServer({ clientFactory: factory });
  t.after(() => workbench.close());
  const html = await (await fetch(workbench.url)).text();
  const token = html.match(/name="pacificdb-token" content="([a-f0-9]+)"/)[1];
  const data = Buffer.from('a real media upload');
  const upload = await fetch(new URL('/api/media/upload', workbench.url), {
    method: 'POST', headers: {
      'X-PacificDB-Workbench-Token': token,
      'X-Project-Id': 'project_1', 'X-Database': 'app',
      'X-Collection': 'media', 'X-Filename': 'demo.bin',
      'Content-Type': 'application/octet-stream',
    }, body: data,
  });
  assert.equal(upload.status, 200);
  assert.deepEqual(state.upload, data);
  const download = await fetch(new URL('/api/media/download', workbench.url), {
    method: 'POST', headers: {
      'X-PacificDB-Workbench-Token': token,
      'Content-Type': 'application/json',
    }, body: JSON.stringify({ projectId: 'project_1', database: 'app',
      mediaId: 'media_1' }),
  });
  assert.equal(download.status, 200);
  assert.deepEqual(Buffer.from(await download.arrayBuffer()), data);
  const denied = await fetch(new URL('/api/media/download', workbench.url), {
    method: 'POST', headers: {
      'X-PacificDB-Workbench-Token': token,
      'Content-Type': 'application/json',
    }, body: JSON.stringify({ projectId: 'project_1', database: 'other',
      mediaId: 'media_1' }),
  });
  assert.equal(denied.status, 400);
});
