import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { PacificDBClient } from '../src/index.js';

test('sends one JSON command and parses one response', async (t) => {
  const server = net.createServer((socket) => {
    socket.once('data', (data) => {
      const request = JSON.parse(data);
      socket.end(JSON.stringify({ ok: true, action: request.action,
        database: request.dbName }) + '\n');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  const client = new PacificDBClient({
    host: '127.0.0.1', port: server.address().port, database: 'app'
  });
  assert.deepEqual(await client.request({ action: 'ping' }),
                   { ok: true, action: 'ping', database: 'app' });
});

test('preserves the public engine detail in request errors', async (t) => {
  const server = net.createServer((socket) => socket.once('data', () =>
    socket.end(JSON.stringify({ error: 'execution_exception',
      message: 'media upload has missing chunks', _engineTrace: { internal: true } }) + '\n')));
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  const client = new PacificDBClient({ host: '127.0.0.1', port: server.address().port });
  await assert.rejects(client.request({ action: 'community_media_finalize' }),
    /execution_exception: media upload has missing chunks/);
});

test('stores arbitrary media bytes and vector data through public methods', async (t) => {
  const requests = [];
  const server = net.createServer((socket) => {
    socket.once('data', (data) => {
      const request = JSON.parse(data);
      requests.push(request);
      if (request.action === 'find') {
        socket.end(JSON.stringify({ status: 'ok', data: [{
          id: 'logo', kind: 'media', contentType: 'image/gif',
          dataBase64: Buffer.from([0, 1, 2, 255]).toString('base64')
        }] }) + '\n');
      } else {
        socket.end(JSON.stringify({ status: 'ok', id: request.data?.id }) + '\n');
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  const client = new PacificDBClient({
    host: '127.0.0.1', port: server.address().port, database: 'app'
  });

  await client.putMedia('assets', 'logo', Buffer.from([0, 1, 2, 255]),
                        { contentType: 'image/gif' });
  const media = await client.getMedia('assets', 'logo');
  await client.putVector('embeddings', 'logo-vector', [0.25, 0.75],
                         { modality: 'image' });
  await client.queryVector('embeddings', [0.25, 0.75],
                           { k: 3, filter: { modality: 'image' } });

  assert.equal(requests[0].action, 'insert');
  assert.equal(requests[0].data.kind, 'media');
  assert.equal(requests[0].data.contentType, 'image/gif');
  assert.deepEqual(media.data, Buffer.from([0, 1, 2, 255]));
  assert.equal(requests[2].action, 'insertVector');
  assert.deepEqual(requests[2].data.vector, [0.25, 0.75]);
  assert.equal(requests[3].action, 'queryVector');
  assert.deepEqual(requests[3].filter, { modality: 'image' });
});

test('uploads and downloads media sequentially in bounded chunks', async (t) => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-node-media-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const input = path.join(directory, 'input.mp4');
  const output = path.join(directory, 'output.mp4');
  const source = Buffer.alloc(700_000, 7);
  await writeFile(input, source);

  const requests = [];
  const chunks = new Map();
  let manifest;
  const server = net.createServer((socket) => {
    let wire = '';
    socket.on('data', (data) => {
      wire += data;
      const newline = wire.indexOf('\n');
      if (newline < 0) return;
      const request = JSON.parse(wire.slice(0, newline));
      requests.push(request);
      let response;
      if (request.action === 'community_capabilities') {
        response = { status: 'ok', max_request_bytes: 1_048_576 };
      } else if (request.action === 'community_media_begin') {
        manifest = { id: 'media_test', status: 'uploading',
          filename: request.filename, size_bytes: request.size_bytes,
          chunk_count: request.chunk_count, sha256: request.sha256 };
        response = { status: 'ok', media: manifest };
      } else if (request.action === 'community_media_put_chunk') {
        chunks.set(request.index, { data: request.data, sha256: request.sha256 });
        response = { status: 'ok', stored: true };
      } else if (request.action === 'community_media_finalize') {
        manifest.status = 'ready';
        response = { status: 'ok', media: manifest };
      } else if (request.action === 'community_media_get') {
        response = { status: 'ok', media: manifest };
      } else if (request.action === 'community_media_get_chunk') {
        response = { status: 'ok', chunk: chunks.get(request.index) };
      }
      socket.end(JSON.stringify(response) + '\n');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());

  const client = new PacificDBClient({
    host: '127.0.0.1', port: server.address().port, database: 'app'
  });
  const uploaded = await client.uploadMediaFile('videos', input,
                                                 { chunkBytes: 262_144 });
  assert.equal(uploaded.status, 'ready');
  assert.equal(requests.filter((r) =>
    r.action === 'community_media_put_chunk').length, 3);

  const downloaded = await client.downloadMediaFile(uploaded.id, output);
  assert.deepEqual(await readFile(output), source);
  assert.equal(downloaded.sha256,
    createHash('sha256').update(source).digest('hex'));
});

test('exports complete backup files in verified bounded chunks', async (t) => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-node-backup-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const output = path.join(directory, 'backup.json');
  const source = Buffer.alloc(700_000, 9);
  const sha256 = createHash('sha256').update(source).digest('hex');
  const requests = [];
  const server = net.createServer((socket) => {
    let wire = '';
    socket.on('data', (data) => {
      wire += data;
      const newline = wire.indexOf('\n');
      if (newline < 0) return;
      const request = JSON.parse(wire.slice(0, newline));
      requests.push(request);
      let response;
      if (request.action === 'export_backup_manifest') {
        response = { status: 'ok', format: 'pacificdb-full-backup-v1',
          backup: { backup_id: 'backup-1' },
          files: [{ path: 'data/record.bin', size_bytes: source.length, sha256 }] };
      } else if (request.action === 'export_backup_file_chunk') {
        const bytes = source.subarray(request.offset,
          Math.min(source.length, request.offset + request.max_bytes));
        response = { status: 'ok', path: request.path, offset: request.offset,
          size_bytes: bytes.length,
          sha256: createHash('sha256').update(bytes).digest('hex'),
          data: bytes.toString('base64') };
      }
      socket.end(JSON.stringify(response) + '\n');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());

  const client = new PacificDBClient({ host: '127.0.0.1',
    port: server.address().port });
  const result = await client.exportBackup('backup-1', output,
                                            { chunkBytes: 262_144 });
  const exported = JSON.parse(await readFile(output, 'utf8'));
  const restored = Buffer.concat(exported.files[0].chunks.map(
    (chunk) => Buffer.from(chunk, 'base64')));
  assert.deepEqual(restored, source);
  assert.equal(result.sizeBytes, source.length);
  assert.equal(requests.filter((request) =>
    request.action === 'export_backup_file_chunk').length, 3);
});
