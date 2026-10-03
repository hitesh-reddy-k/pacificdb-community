import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile, rm, stat } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PassThrough } from 'node:stream';
import test from 'node:test';
import { main } from '../src/cli.js';
import { parseShellCommand, runShell } from '../src/shell.js';

async function shell(t, saved, commands, reply, selected = '') {
  const home = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-direct-cli-'));
  t.after(() => rm(home, { recursive: true, force: true }));
  if (saved) await writeFile(path.join(home, 'context.json'), JSON.stringify(saved));
  const frames = [], client = { database: selected, async request(command) {
    frames.push({ userId: 'system', dbName: this.database, ...command });
    return reply(command);
  } };
  const input = new PassThrough(), output = new PassThrough();
  let text = ''; output.on('data', bytes => { text += bytes; });
  const running = runShell(client, { input, output }, { cliHome: home });
  input.end(commands + '\nexit\n'); await running;
  return { frames, client, home, text, saved: JSON.parse(await readFile(path.join(home, 'context.json'))) };
}

test('direct shell selects only successful creations and failed drop keeps context', async t => {
  const r = await shell(t, null, 'create database app\ncreate collection users\ncreate database denied\ndrop database app\ninsert users {"id":"1"}', c => {
    if ((c.action === 'createDatabase' && c.dbName === 'denied') || c.action === 'dropDatabase') throw new Error('permission_denied');
    return { status: 'ok' };
  });
  assert.deepEqual(r.frames.map(f => f.action), ['createDatabase', 'createCollection', 'createDatabase', 'dropDatabase', 'insert']);
  assert.equal(r.client.database, 'app'); assert.deepEqual(r.saved, { database: 'app' });
  assert.ok(r.frames.every(f => !('project_id' in f)));
});

for (const saved of [{ database: 'app' }, { database: 'app', projectId: 'legacy', token: 'old-secret' }]) {
  test(`restored ${Object.hasOwn(saved, 'projectId') ? 'legacy' : 'direct'} context validates once and removes extra fields`, async t => {
    const r = await shell(t, saved, 'create collection users\nfind users {}\ncontext show', c => c.action === 'listDatabases' ? ['app'] : { status: 'ok' });
    assert.deepEqual(r.frames.map(f => f.action), ['listDatabases', 'createCollection', 'find']);
    assert.deepEqual(r.saved, { database: 'app' });
    assert.doesNotMatch(r.text, /old-secret|projectId|legacy/);
    if (process.platform !== 'win32') assert.equal((await stat(path.join(r.home, 'context.json'))).mode & 0o777, 0o600);
  });
}

test('stale restored database blocks dependent operation without deleting its data', async t => {
  const r = await shell(t, { database: 'missing' }, 'insert users {}', () => []);
  assert.deepEqual(r.frames.map(f => f.action), ['listDatabases']);
  assert.match(r.text, /database_not_found/); assert.deepEqual(r.saved, { database: 'missing' });
});

test('explicit URL selection overrides saved context and project commands are unavailable', async t => {
  const r = await shell(t, { database: 'old', projectId: 'p' }, 'insert users {}', () => ({ status: 'ok' }), 'url-db');
  assert.equal(r.frames[0].dbName, 'url-db');
  for (const command of ['create project p', 'list projects', 'use project p', 'show project', 'delete project p']) {
    assert.throws(() => parseShellCommand(command, { database: 'app' }), /unknown command/);
  }
  assert.equal(parseShellCommand('request {"action":"community_project_list"}').command.action, 'community_project_list');
});

test('URL passwords do not reach shell output or persisted history', async t => {
  const r = await shell(t, { database: 'app' }, 'pacificdb://demo:dummy-secret@localhost/app\nrequest {"action":"ping","url":"pacificdb://demo:dummy-secret@localhost/app"}', () => ({ status: 'ok' }));
  assert.doesNotMatch(r.text, /dummy-secret/);
  const history = await readFile(path.join(r.home, 'history'), 'utf8').catch(() => '');
  assert.doesNotMatch(history, /dummy-secret/);
});

test('malformed raw JSON cannot echo credential contents', async t => {
  const r = await shell(t, { database: 'app' }, 'request {"token":"dummy-secret", nope}', () => ({ status: 'ok' }));
  assert.doesNotMatch(r.text, /dummy-secret/);
  await assert.rejects(main(['request', '{"token":"dummy-secret", nope}', '--no-start'],
    { input: new PassThrough(), output: new PassThrough() }), e => !e.message.includes('dummy-secret'));
});

test('URL command authenticates first, honors environment and closes one-shot sockets', async t => {
  const frames = [], sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket); socket.on('close', () => sockets.delete(socket));
    let wire = '';
    socket.on('data', bytes => {
      wire += bytes;
      while (wire.includes('\n')) {
        const end = wire.indexOf('\n'), frame = JSON.parse(wire.slice(0, end)); wire = wire.slice(end + 1);
        frames.push(frame);
        socket.write(JSON.stringify(frame.action === 'security_authenticate' ? { token: 'dummy-token' } : { status: 'ok' }) + '\n');
      }
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const s of sockets) s.destroy(); await new Promise(resolve => server.close(resolve)); });
  const url = `pacificdb://demo:dummy-secret@127.0.0.1:${server.address().port}/app`;
  const output = new PassThrough(); let text = ''; output.on('data', bytes => { text += bytes; });
  await main(['ping', '--url', url, '--no-start'], { input: new PassThrough(), output });
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.deepEqual(frames.map(f => f.action), ['security_authenticate', 'ping']);
  assert.equal(frames[1].dbName, 'app'); assert.equal(frames[1].token, 'dummy-token');
  assert.equal(sockets.size, 0); assert.doesNotMatch(text, /dummy-secret|dummy-token/);
  const oldUrl = process.env.PACIFICDB_URL;
  try {
    process.env.PACIFICDB_URL = url;
    await main(['ping', '--no-start'], { input: new PassThrough(), output });
    assert.equal(frames.at(-1).dbName, 'app');
    process.env.PACIFICDB_URL = 'invalid';
    await main(['ping', '--url', url, '--no-start'], { input: new PassThrough(), output });
    const count = frames.length;
    await assert.rejects(main(['ping', '--url', url, '--database', 'conflict'], { input: new PassThrough(), output }), /invalid_connection_url/);
    assert.equal(frames.length, count);
  } finally {
    if (oldUrl === undefined) delete process.env.PACIFICDB_URL; else process.env.PACIFICDB_URL = oldUrl;
  }
});

test('flags with missing values fail before networking', async () => {
  for (const flag of ['--url', '--host', '--port', '--database', '--ui-port']) await assert.rejects(main(['ping', flag], { input: new PassThrough(), output: new PassThrough() }), /requires a value/);
});
