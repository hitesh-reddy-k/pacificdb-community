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
  const state = { databases: new Set(['mapped', 'unmapped']), collections: new Map(),
    documents: new Map(), media: null, upload: null, frames: [], created: 0, closed: 0,
    downloads: 0, readOnly: false };
  const record = (client, action, values = {}) => {
    state.frames.push({ dbName: client.database, action, ...values });
    if (state.readOnly && ['insert', 'updateOne', 'deleteOne', 'dropDatabase'].includes(action)) throw new Error('permission_denied');
  };
  const check = (client, collection) => {
    if (!state.databases.has(client.database)) throw new Error('database_not_found');
    if (collection && !state.collections.get(client.database)?.includes(collection)) throw new Error('collection_not_found');
    return `${client.database}/${collection}`;
  };
  const factory = () => {
    state.created++;
    return {
      database: '', close() { state.closed++; },
      async request(command) {
        record(this, command.action, command);
        switch (command.action) {
          case 'ping': return { status: 'pong' };
          case 'health_check': return { status: 'healthy' };
          case 'get_cluster_status': return { role: 'leader', raft_commit_index: 42 };
          case 'listDatabases': return [...state.databases];
          case 'createDatabase': state.databases.add(command.dbName); return { status: 'ok' };
          case 'dropDatabase': state.databases.delete(command.dbName); return { status: 'ok' };
          case 'listCollections': check(this); return state.collections.get(this.database) || [];
          case 'createCollection': check(this); state.collections.set(this.database, [...(state.collections.get(this.database) || []), command.collection]); return { status: 'ok' };
          case 'count': return { count: (state.documents.get(check(this, command.collection)) || []).length };
          case 'listIndexes': check(this, command.collection); return { indexes: [{ name: '_id_' }] };
          case 'community_media_get': return { media: state.media };
          case 'community_media_delete': state.media = null; return { status: 'ok' };
          default: throw new Error(`unexpected action: ${command.action}`);
        }
      },
      async find(name) { record(this, 'find'); return { data: state.documents.get(check(this, name)) || [] }; },
      async insert(name, data) {
        // Yield so parallel HTTP requests exercise client isolation.
        await new Promise(setImmediate); record(this, 'insert');
        const key = check(this, name); state.documents.set(key, [...(state.documents.get(key) || []), data]);
        return { status: 'ok' };
      },
      async updateOne(name, filter, update) { record(this, 'updateOne'); const document = (state.documents.get(check(this, name)) || []).find(v => v.id === filter.id); if (document) Object.assign(document, update); return { status: 'ok' }; },
      async deleteOne(name, filter) { record(this, 'deleteOne'); const key = check(this, name); state.documents.set(key, (state.documents.get(key) || []).filter(v => v.id !== filter.id)); return { status: 'ok' }; },
      async putVector(name, id, vector) { record(this, 'insertVector'); check(this, name); return { id, vector }; },
      async queryVector(name, vector) { record(this, 'queryVector'); check(this, name); return { data: [{ vector }] }; },
      async uploadMediaFile(name, filename) { record(this, 'upload'); check(this, name); state.upload = await readFile(filename); state.media = { id: 'media_1', database: this.database, collection: name, status: 'ready' }; return state.media; },
      async downloadMediaFile(id, destination) { record(this, 'download'); state.downloads++; await writeFile(destination, state.upload); },
    };
  };
  return { state, factory };
}

async function workspace(t, connectionInfo = null) {
  const { state, factory } = fakeFactory();
  const workbench = await createWorkbenchServer({ clientFactory: factory, connectionInfo });
  t.after(() => workbench.close());
  const html = await (await fetch(workbench.url)).text();
  const token = html.match(/name="pacificdb-token" content="([a-f0-9]+)"/)[1];
  const post = async (body, headers = {}) => {
    const response = await fetch(new URL('/api/execute', workbench.url), { method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-PacificDB-Workbench-Token': token, ...headers }, body: JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  return { state, workbench, token, post, html };
}

test('direct database adapter preserves browser protections and exposes safe connection info', async t => {
  const { state, workbench, post } = await workspace(t, { host: '127.0.0.1', port: 9000, tls: false,
    userId: 'alice', authenticationRequired: true, token: 'private-token', password: 'private-password', url: 'private-url' });
  const logo = await fetch(new URL('/logo.png', workbench.url));
  assert.equal(logo.status, 200);
  assert.deepEqual(Buffer.from(await logo.arrayBuffer()).subarray(0, 8), Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]));
  for (const headers of [{ 'X-PacificDB-Workbench-Token': 'wrong' }, { Origin: 'https://attacker.example' }]) assert.equal((await post({ op: 'databases.list' }, headers)).status, 403);
  // fetch normalizes Host; use a raw HTTP request to exercise DNS-rebinding protection.
  const wrongHost = await new Promise((resolve, reject) => {
    const req = http.request(new URL('/api/execute', workbench.url), { method: 'POST', headers: { Host: 'attacker.example' } }, res => { res.resume(); resolve(res.statusCode); });
    req.on('error', reject); req.end('{}');
  });
  assert.equal(wrongHost, 403);
  assert.deepEqual((await post({ op: 'connection.info' })).body.result, { host: '127.0.0.1', port: 9000, tls: false, userId: 'alice', authenticationRequired: true });
  for (const op of ['projects.list', 'projects.create', 'projects.delete']) assert.equal((await post({ op })).status, 400);
  state.frames.length = 0;
  assert.deepEqual((await post({ op: 'databases.list' })).body.result.databases, ['mapped', 'unmapped']);
  assert.equal((await post({ op: 'databases.create', name: 'app' })).status, 200);
  assert.equal((await post({ op: 'databases.delete', database: 'app' })).status, 200);
  assert.deepEqual(state.frames.map(f => f.action), ['listDatabases', 'createDatabase', 'dropDatabase']);
  assert.ok(state.frames.every(f => !('project_id' in f)));
  assert.equal(state.created, state.closed);
});

test('CRUD queries dispatch one engine action and use independent request clients', async t => {
  const { state, post } = await workspace(t);
  const scope = { database: 'unmapped', collection: 'users' };
  state.collections.set('unmapped', ['users']); state.collections.set('mapped', ['users']);
  for (const database of ['mapped', 'unmapped']) assert.equal((await post({ op: 'documents.insert', database, collection: 'users', data: { id: 'one', name: database } })).status, 200);
  state.frames.length = 0;
  assert.deepEqual((await post({ op: 'documents.find', ...scope })).body.result.data, [{ id: 'one', name: 'unmapped' }]);
  assert.deepEqual(state.frames.map(f => f.action), ['find']);
  await post({ op: 'documents.update', ...scope, filter: { id: 'one' }, update: { name: 'Ada' } });
  assert.equal(state.documents.get('unmapped/users')[0].name, 'Ada');
  assert.equal(state.documents.get('mapped/users')[0].name, 'mapped');
  const parallel = await Promise.all(['mapped', 'unmapped'].map(database => post({ op: 'documents.insert', database, collection: 'users', data: { id: 'two', name: database } })));
  assert.ok(parallel.every(r => r.status === 200));
  for (const database of ['mapped', 'unmapped']) assert.equal(state.documents.get(`${database}/users`)[1].name, database);
  await post({ op: 'documents.delete', ...scope, filter: { id: 'one' } });
  state.frames.length = 0;
  assert.deepEqual((await post({ op: 'collections.summary', ...scope })).body.result, { count: 1, indexes: 1 });
  assert.deepEqual(state.frames.map(f => f.action), ['count', 'listIndexes']);
  assert.equal(state.created, state.closed);
});

test('engine failures and input/page/read-only limits remain enforced', async t => {
  const { state, workbench, token, post } = await workspace(t);
  const scope = { database: 'unmapped', collection: 'users' };
  state.collections.set(scope.database, ['users']);
  for (const body of [{ op: 'documents.find', ...scope, limit: 501 }, { op: 'documents.find', ...scope, offset: -1 },
    { op: 'documents.find', ...scope, collection: '' }, { op: 'documents.find', ...scope, filter: [] },
    { op: 'documents.update', ...scope, filter: {}, update: {} }]) assert.equal((await post(body)).status, 400);
  assert.equal(state.frames.length, 0);
  assert.match((await post({ op: 'documents.find', ...scope, collection: 'missing' })).body.error, /collection_not_found/);
  state.readOnly = true;
  assert.match((await post({ op: 'documents.insert', ...scope, data: { id: 'denied' } })).body.error, /permission_denied/);
  assert.equal(state.documents.size, 0);
  const tooLarge = await fetch(new URL('/api/execute', workbench.url), { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-PacificDB-Workbench-Token': token }, body: JSON.stringify({ op: 'documents.insert', ...scope, data: { text: 'x'.repeat(1024*1024) } }) });
  assert.equal(tooLarge.status, 400);
  assert.equal(state.created, state.closed);
});

test('media transfer and deletion require matching database and collection before access', async t => {
  const { state, workbench, token, post } = await workspace(t);
  state.collections.set('unmapped', ['media', 'other']);
  const bytes = Buffer.from('a real media upload');
  const upload = await fetch(new URL('/api/media/upload', workbench.url), { method: 'POST', headers: {
    'X-PacificDB-Workbench-Token': token, 'X-Database': 'unmapped', 'X-Collection': 'media', 'X-Filename': 'demo.bin', 'Content-Type': 'application/octet-stream' }, body: bytes });
  assert.equal(upload.status, 200); assert.deepEqual(state.upload, bytes);
  const download = body => fetch(new URL('/api/media/download', workbench.url), { method: 'POST', headers: { 'X-PacificDB-Workbench-Token': token, 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  const scope = { database: 'unmapped', collection: 'media', mediaId: 'media_1' };
  for (const wrong of [{ ...scope, database: 'mapped' }, { ...scope, collection: 'other' }]) {
    const before = state.downloads;
    assert.equal((await download(wrong)).status, 400);
    assert.equal(state.downloads, before);
    assert.equal((await post({ op: 'media.delete', ...wrong })).status, 400);
    assert.ok(state.media);
  }
  const got = await download(scope); assert.equal(got.status, 200); assert.deepEqual(Buffer.from(await got.arrayBuffer()), bytes);
  assert.equal((await post({ op: 'media.delete', ...scope })).status, 200);
  assert.equal(state.media, null);
  // A streamed response can reach the browser before pipeline closes its file handle.
  for (let attempt = 0; state.created !== state.closed && attempt < 100; attempt++) await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(state.created, state.closed);
});


test('direct database byte limit and percent-encoded upload scope match engine names', async t => {
  const {state, workbench, token, post} = await workspace(t);
  const database = 'a'.repeat(255), collection = '文件%41';
  assert.equal((await post({op: 'databases.create', name: database})).status, 200);
  assert.equal((await post({op: 'databases.create', name: database+'a'})).status, 400);
  state.databases.add(database); state.collections.set(database, [collection]);
  assert.equal((await post({op: 'collections.list', database})).status, 200);
  assert.equal((await post({op: 'collections.list', database: database+'a'})).status, 400);
  const unicode = '数据库'; state.databases.add(unicode); state.collections.set(unicode, [collection]);
  const send = name => fetch(new URL('/api/media/upload', workbench.url), {method: 'POST', headers: {
    'X-PacificDB-Workbench-Token': token, 'X-Database': name,
    'X-Collection': encodeURIComponent(collection), 'X-Filename': encodeURIComponent('文件.txt')}, body: Buffer.from('bytes')});
  assert.equal((await send(encodeURIComponent(unicode))).status, 200);
  assert.equal(state.media.database, unicode); assert.equal(state.media.collection, collection);
  assert.equal((await send('%not-utf8')).status, 400);
});
