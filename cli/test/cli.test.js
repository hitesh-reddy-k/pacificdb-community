import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { PassThrough } from 'node:stream';
import { main } from '../src/cli.js';
import { parseShellCommand, runShell } from '../src/shell.js';

test('friendly parser maps Community commands and rejects Cloud commands', () => {
  const context = { database: 'app' };
  assert.equal(parseShellCommand('find users {"active":true}', context)
    .command.action, 'find');
  assert.equal(parseShellCommand('findOne users {}', context)
    .command.limit, 1);
  assert.equal(parseShellCommand('query vector embeddings [1,0] --k 3', context)
    .command.k, 3);
  assert.equal(parseShellCommand('backup verify backup-1', context)
    .command.action, 'verify_backup');
  assert.equal(parseShellCommand('find media movie', context).kind, 'mediaFind');
  assert.equal(parseShellCommand('delete media media_1', context)
    .command.action, 'community_media_delete');
  assert.equal(parseShellCommand('delete backup backup-1', context)
    .command.action, 'delete_backup');
  assert.throws(() => parseShellCommand('create organization demo', context),
                /unknown command/);
});

test('shell persists local context and dispatches project/database commands', async () => {
  const cliHome = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-shell-context-'));
  const requests = [];
  const client = {
    database: '', token: '',
    async request(command) {
      requests.push(command);
      if (command.action === 'community_project_create') {
        return { status: 'ok', project: { id: 'project_1', name: command.name } };
      }
      if (command.action === 'community_project_get') {
        if (command.id !== 'project_1') throw new Error('project_not_found');
        return { status: 'ok', project: { id: 'project_1', name: 'demo' } };
      }
      if (command.action === 'listDatabases') return ['app'];
      return { status: 'ok' };
    }
  };
  const input = new PassThrough();
  const output = new PassThrough();
  const running = runShell(client, { input, output }, { cliHome });
  input.write('create project demo\n');
  input.write('use project project_1\n');
  input.write('create database app\n');
  input.write('use app\n');
  input.end('exit\n');
  await running;

  assert.deepEqual(requests.map((request) => request.action), [
    'community_project_create', 'community_project_get', 'createDatabase',
    'community_database_map', 'listDatabases'
  ]);
  const context = JSON.parse(await readFile(path.join(cliHome, 'context.json')));
  assert.equal(context.projectId, 'project_1');
  assert.equal(context.database, 'app');
  assert.equal((await readFile(path.join(cliHome, 'history'), 'utf8')).includes('exit'),
               false);
});

test('shell rejects nonexistent project and database without changing context', async () => {
  const cliHome = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-shell-invalid-context-'));
  await writeFile(path.join(cliHome, 'context.json'), JSON.stringify({
    projectId: 'project_existing', database: 'existing'
  }), { mode: 0o600 });
  const client = {
    database: '', token: '',
    async request(command) {
      if (command.action === 'community_project_get') throw new Error('project_not_found');
      if (command.action === 'listDatabases') return ['existing'];
      return { status: 'ok' };
    }
  };
  const input = new PassThrough();
  const output = new PassThrough();
  let text = '';
  output.on('data', (chunk) => { text += chunk; });
  const running = runShell(client, { input, output }, { cliHome });
  input.write('use project definitely-does-not-exist\n');
  input.write('use missing-database\n');
  input.end('exit\n');
  await running;

  const context = JSON.parse(await readFile(path.join(cliHome, 'context.json')));
  assert.deepEqual(context, { projectId: 'project_existing', database: 'existing' });
  assert.equal(client.database, 'existing');
  assert.match(text, /project_not_found/);
  assert.match(text, /database_not_found/);
  assert.doesNotMatch(text, /"projectId": "definitely-does-not-exist"/);
  assert.doesNotMatch(text, /"database": "missing-database"/);
});

test('rejects unknown commands with useful usage', async () => {
  await assert.rejects(() => main(['unknown'], {
    input: new PassThrough(), output: new PassThrough()
  }), /usage:/);
});

test('shell help lists commands without contacting the server', async () => {
  const input = new PassThrough();
  const output = new PassThrough();
  let text = '';
  output.on('data', (chunk) => { text += chunk; });
  const running = main(['shell'], { input, output });
  input.write('help\n');
  await new Promise(setImmediate);
  input.end('quit\n');
  await running;
  assert.match(text, /Authentication\n[\s\S]*whoami/);
  assert.match(text, /Projects\n[\s\S]*create project/);
  assert.match(text, /Backups\n[\s\S]*show backup/);
  assert.match(text, /Media\n[\s\S]*download media/);
  assert.match(text, /Vectors\n[\s\S]*query vector/);
  assert.doesNotMatch(text, /autoscal|billing|organization/i);
});

test('hides internal telemetry from normal output', async (t) => {
  const server = net.createServer((socket) => socket.once('data', () => socket.end(JSON.stringify({
    status: 'ok', data: [{ id: '1', name: 'Ada', _mvcc_version: 2,
      _raft_commit_index: 3, _visibility_state: 'COMMITTED_VISIBLE', committed: true,
      created_at_ms: 1, created_txn: 2, deleted_at_ms: 0, deleted_txn: 0,
      tenant_id: 'system', version: 2 }], _debug_metrics: { queue: 1 },
    _engineTrace: { trace: 1 }, _raft: { term: 2 }, requestId: 'request-1',
    trace_id: 'trace-1', traceparent: 'trace-parent', term: 2, isLeader: true,
    leader_term: 2, commit_index: 3, last_applied: 3,
    consistency_mode: 'EVENTUAL', consistency_semantics: 'eventual',
    returned_doc_version: 2, sst_visibility_source: 'lsm_queued_find'
  }) + '\n')));
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  const output = new PassThrough();
  let text = '';
  output.on('data', (chunk) => { text += chunk; });
  await main(['request', '{"action":"find"}', '--host', '127.0.0.1',
    '--port', String(server.address().port)], { input: new PassThrough(), output });
  assert.deepEqual(JSON.parse(text), { status: 'ok', data: [{ id: '1', name: 'Ada' }] });
});

test('uploads and downloads media files', async (t) => {
  const requests = [];
  const bytes = Buffer.from([0, 1, 2, 255]);
  const server = net.createServer((socket) => {
    socket.once('data', (data) => {
      const request = JSON.parse(data);
      requests.push(request);
      const response = request.action === 'find'
        ? { status: 'ok', data: [{ id: 'logo', kind: 'media',
            dataBase64: bytes.toString('base64') }] }
        : { status: 'ok', id: 'logo' };
      socket.end(JSON.stringify(response) + '\n');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());

  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-cli-'));
  const input = path.join(directory, 'logo.gif');
  const output = path.join(directory, 'download.gif');
  await writeFile(input, bytes);
  const connection = ['--host', '127.0.0.1', '--port', String(server.address().port),
                      '--database', 'app'];
  await main(['put-media', 'assets', 'logo', input, '--content-type', 'image/gif', ...connection],
             { input: new PassThrough(), output: new PassThrough() });
  await main(['get-media', 'assets', 'logo', output, ...connection],
             { input: new PassThrough(), output: new PassThrough() });
  await main(['put-vector', 'vectors', 'logo-vector', '[1,0]',
              '--metadata', '{"modality":"image"}', ...connection],
             { input: new PassThrough(), output: new PassThrough() });
  await main(['query-vector', 'vectors', '[1,0]', '--k', '1', ...connection],
             { input: new PassThrough(), output: new PassThrough() });

  assert.equal(requests[0].data.contentType, 'image/gif');
  assert.deepEqual(await readFile(output), bytes);
  assert.equal(requests[2].action, 'insertVector');
  assert.deepEqual(requests[2].data.vector, [1, 0]);
  assert.equal(requests[3].action, 'queryVector');
});
