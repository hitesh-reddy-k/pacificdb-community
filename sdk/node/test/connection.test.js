import assert from 'node:assert/strict';
import { readFile, writeFile, mkdtemp, rm } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import os from 'node:os';
import path from 'node:path';
import { inspect } from 'node:util';
import net from 'node:net';
import test from 'node:test';
import * as sdk from '../src/index.js';

async function peer(t, handler) {
  const frames = [], sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let wire = '';
    socket.on('data', async bytes => {
      wire += bytes;
      while (wire.includes('\n')) {
        const end = wire.indexOf('\n');
        const frame = JSON.parse(wire.slice(0, end));
        wire = wire.slice(end + 1);
        frames.push(frame);
        const reply = await handler(frame);
        if (!socket.destroyed) socket.end(JSON.stringify(reply) + '\n');
      }
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => server.close(resolve));
  });
  return { port: server.address().port, frames };
}

test('direct creation selects database and creates collections without catalog calls', async t => {
  const p = await peer(t, frame => frame.action === 'listDatabases' ? ['app'] : { status: 'ok' });
  const db = new sdk.PacificDBClient({ port: p.port });
  t.after(() => db.close());
  await db.createDatabase('app');
  assert.equal(db.database, 'app');
  await db.createCollection('users');
  await db.insert('users', { id: '1', name: 'Ada' });
  await db.useDatabase('app');
  assert.deepEqual(p.frames.map(f => f.action), ['createDatabase', 'createCollection', 'insert', 'listDatabases']);
  assert.equal(p.frames[1].dbName, 'app');
  assert.ok(p.frames.every(f => !('project_id' in f)));
});

test('failed direct creation or selection preserves database', async t => {
  const p = await peer(t, frame => frame.action === 'listDatabases' ? ['app'] : { error: 'permission_denied' });
  const db = new sdk.PacificDBClient({ port: p.port, database: 'app' });
  t.after(() => db.close());
  await assert.rejects(db.createDatabase('denied'), /permission_denied/);
  assert.equal(db.database, 'app');
  await assert.rejects(db.useDatabase('missing'), /database_not_found/);
  assert.equal(db.database, 'app');
});

test('URL grammar uses shared normalized fixtures and rejects malformed values', async () => {
  assert.equal(typeof sdk.parseConnectionUrl, 'function', 'URL parser must be public');
  const fixture = JSON.parse(await readFile(new URL('../../contracts/connection-urls.json', import.meta.url)));
  for (const entry of fixture.valid) {
    const parsed = sdk.parseConnectionUrl(entry.url, entry.options);
    for (const [key, value] of Object.entries(entry.expected)) assert.deepEqual(parsed[key], value, `${entry.url}: ${key}`);
  }
  for (const entry of fixture.invalid) {
    assert.throws(() => sdk.parseConnectionUrl(entry.url, entry.options), e => e.code === entry.code && !e.message.includes(entry.url));
  }
});

test('lazy URL requests authenticate once before concurrent writes with captured scope', async t => {
  assert.equal(typeof sdk.PacificDBClient.fromUrl, 'function');
  let entered, release;
  const started = new Promise(resolve => { entered = resolve; });
  const blocked = new Promise(resolve => { release = resolve; });
  const p = await peer(t, async frame => {
    if (frame.action === 'security_authenticate') {
      entered(); await blocked;
      assert.equal(frame.username, 'demo'); assert.equal(frame.password, 'dummy-password');
      return { token: 'dummy-token' };
    }
    assert.equal(frame.token, 'dummy-token');
    return { status: 'ok' };
  });
  const db = sdk.PacificDBClient.fromUrl(`pacificdb://demo:dummy-password@127.0.0.1:${p.port}/app`);
  t.after(() => db.close());
  const calls = [db.insert('users', { id: '1' }), db.insert('users', { id: '2' })];
  await started;
  db.database = 'other';
  release();
  await Promise.all(calls);
  assert.deepEqual(p.frames.map(f => f.action), ['security_authenticate', 'insert', 'insert']);
  assert.ok(p.frames.slice(1).every(f => f.dbName === 'app'));
  assert.doesNotMatch(JSON.stringify(db) + inspect(db), /dummy-password|dummy-token/);
});

test('eager factory authenticates before returning and failure sends no command', async t => {
  assert.equal(typeof sdk.PacificDB, 'function');
  const p = await peer(t, () => ({ error: 'authentication_failed', message: 'dummy-password' }));
  const url = `pacificdb://demo:dummy-password@127.0.0.1:${p.port}/app`;
  await assert.rejects(sdk.PacificDB.connect(url), e => /authentication_failed/.test(e.message) && !e.message.includes('dummy-password'));
  assert.equal(p.frames.length, 1);
  const db = sdk.PacificDB.fromUrl(url);
  t.after(() => db.close());
  await assert.rejects(db.insert('users', {}), /authentication_failed/);
  await assert.rejects(db.insert('users', {}));
  assert.equal(p.frames.length, 2, 'failed authentication client must not send more requests');
});

test('invalid URL fails without any network connections', async t => {
  assert.equal(typeof sdk.PacificDBClient.fromUrl, 'function');
  const p = await peer(t, () => ({ status: 'ok' }));
  assert.throws(() => sdk.PacificDBClient.fromUrl(`pacificdb://demo:dummy-password@127.0.0.1:${p.port}/app?token=secret`), /invalid_connection_url/);
  assert.equal(p.frames.length, 0);
});

test('malformed authentication success cannot release queued writes', async t => {
  const p = await peer(t, () => ({ status: 'ok' }));
  const db = sdk.PacificDB.fromUrl(`pacificdb://demo:dummy@127.0.0.1:${p.port}/app`);
  t.after(() => db.close());
  await assert.rejects(db.insert('users', {}), /invalid authentication response/);
  assert.deepEqual(p.frames.map(f => f.action), ['security_authenticate']);
});

test('explicit legacy creation keeps its original project and user across awaits', async t => {
  let entered, release;
  const started = new Promise(resolve => { entered = resolve; });
  const blocked = new Promise(resolve => { release = resolve; });
  const p = await peer(t, async frame => {
    if (frame.action === 'community_project_get') { entered(); await blocked; return { project: { id: 'p' } }; }
    return { status: 'ok' };
  });
  const db = new sdk.PacificDB({ port: p.port, database: 'original', userId: 'original-user', projectId: 'p' });
  t.after(() => db.close());
  const creation = db.createDatabase('new');
  await started;
  db.projectId = 'other'; db.userId = 'other-user';
  release(); await creation;
  assert.ok(p.frames.every(f => f.userId === 'original-user'));
  assert.ok(p.frames.slice(1).every(f => f.project_id === 'p'));
});

for (const operation of ['upload', 'download', 'backup']) {
  test(`${operation} captures database and token across all transfer requests`, async t => {
    const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-scope-'));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const bytes = Buffer.alloc(65536, 'a');
    const sha256 = createHash('sha256').update(bytes).digest('hex');
    const input = path.join(directory, 'input');
    await writeFile(input, bytes);
    let entered, release;
    const started = new Promise(resolve => { entered = resolve; });
    const blocked = new Promise(resolve => { release = resolve; });
    let first = true;
    const p = await peer(t, async frame => {
      if (first) { first = false; entered(); await blocked; }
      switch (frame.action) {
        case 'community_capabilities': return { max_request_bytes: 1024 * 1024 };
        case 'community_media_begin': return { media: { id: 'm', status: 'uploading' } };
        case 'community_media_put_chunk': return { media: { received_indices: [0], received_bytes: bytes.length } };
        case 'community_media_finalize': return { media: { id: 'm', status: 'ready' } };
        case 'community_media_get': return { media: { status: 'ready', chunk_count: 1, size_bytes: bytes.length, sha256 } };
        case 'community_media_get_chunk': return { chunk: { data: bytes.toString('base64'), sha256 } };
        case 'export_backup_manifest': return { format: 'test', backup: {}, files: [{ path: 'db', size_bytes: bytes.length, sha256 }] };
        case 'export_backup_file_chunk': return { data: bytes.toString('base64'), sha256, size_bytes: bytes.length, offset: 0 };
        default: throw new Error(`unexpected action ${frame.action}`);
      }
    });
    const db = new sdk.PacificDB({ port: p.port, database: 'original', token: 'original-token' });
    t.after(() => db.close());
    const call = operation === 'upload' ? db.uploadMediaFile('assets', input) :
      operation === 'download' ? db.downloadMediaFile('m', path.join(directory, 'output')) :
        db.exportBackup('b', path.join(directory, 'backup'));
    await started;
    db.database = 'changed'; db.token = 'changed-token';
    release();
    await call;
    assert.ok(p.frames.length >= 2);
    assert.ok(p.frames.every(f => f.dbName === 'original' && f.token === 'original-token'), JSON.stringify(p.frames.map(f => ({ action: f.action, dbName: f.dbName }))));
  });
}

test('raw credential diagnostics sanitize codes, keys and exception causes', async t => {
  const db = new sdk.PacificDBClient({ database: 'app' });
  t.after(() => db.close());
  db._pool.request = async () => {
    const error = Object.assign(new Error('private-new-password'), {
      code: 'raw-secret-token', response: { 'private-new-password': 'raw-secret-token' },
      cause: new Error('private-new-password raw-secret-token')
    });
    throw error;
  };
  await assert.rejects(db.request({ action: 'updateTenantUserPassword',
    newPassword: 'private-new-password', token: 'raw-secret-token' }), error => {
    const diagnostic = inspect(error);
    assert.ok(!diagnostic.includes('private-new-password'));
    assert.ok(!diagnostic.includes('raw-secret-token'));
    assert.equal(error.code, '[redacted]');
    return true;
  });
});

test('backup export owns a unique private temporary and preserves unrelated part files', async t => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-owned-export-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const destination = path.join(directory, 'backup.json');
  await writeFile(destination, 'keep destination');
  await writeFile(`${destination}.part`, 'another transfer owns this');
  const bytes = Buffer.from('bounded backup');
  const hash = createHash('sha256').update(bytes).digest('hex');
  const db = new sdk.PacificDBClient({ database: 'app' });
  t.after(() => db.close());
  let corrupt = true;
  db._pool.request = async wire => {
    const req = JSON.parse(wire);
    if (req.action === 'export_backup_manifest') return { format: 'pacificdb-full-backup-v1', backup: { backup_id: 'backup-1' },
      files: [{ path: 'data/a.bin', size_bytes: bytes.length, sha256: hash }] };
    return { path: req.path, offset: req.offset, next_offset: req.offset + bytes.length,
      size_bytes: bytes.length, sha256: corrupt ? '0'.repeat(64) : hash, data: bytes.toString('base64') };
  };
  await assert.rejects(db.exportBackup('backup-1', destination), /invalid backup chunk/);
  assert.equal(await readFile(destination, 'utf8'), 'keep destination');
  assert.equal(await readFile(`${destination}.part`, 'utf8'), 'another transfer owns this');
  corrupt = false;
  await db.exportBackup('backup-1', destination);
  assert.equal(JSON.parse(await readFile(destination, 'utf8')).files[0].chunks[0], bytes.toString('base64'));
  assert.equal(await readFile(`${destination}.part`, 'utf8'), 'another transfer owns this');
});
