import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { MediaUploadError, PacificDBClient } from '../src/index.js';

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

test('requires project then database when creating data structures', async (t) => {
  const requests = [];
  let mapped = false;
  const server = net.createServer((socket) => socket.once('data', (data) => {
    const request = JSON.parse(data);
    requests.push(request);
    let response = { status: 'ok' };
    if (request.action === 'community_project_create') {
      response = { status: 'ok', project: { id: 'project_1', name: request.name } };
    } else if (request.action === 'community_project_get') {
      response = { status: 'ok', project: { id: 'project_1' } };
    } else if (request.action === 'community_database_map') {
      mapped = true;
    } else if (request.action === 'community_database_list') {
      response = { status: 'ok', databases: mapped ? ['app'] : [] };
    }
    socket.end(JSON.stringify(response) + '\n');
  }));
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  const db = new PacificDBClient({ host: '127.0.0.1', port: server.address().port });
  t.after(() => db.close());

  await assert.rejects(db.createDatabase('app'), /select a project/);
  await assert.rejects(db.createCollection('users'), /select a project/);
  assert.equal(requests.length, 0);
  await db.createProject('demo');
  await assert.rejects(db.createCollection('users'), /select a database/);
  await assert.rejects(db.createDatabase('a'.repeat(129)), /1-128 bytes/);
  assert.deepEqual(requests.map(({ action }) => action), ['community_project_create']);
  await db.createDatabase('app');
  await db.createCollection('users');
  assert.deepEqual(requests.map(({ action }) => action), [
    'community_project_create', 'community_project_get', 'createDatabase',
    'community_database_map', 'community_database_list', 'createCollection'
  ]);
  assert.equal(requests.at(-1).dbName, 'app');
  assert.equal(requests[3].project_id, 'project_1');
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

test('reuses a bounded persistent connection pool under concurrent load', async () => {
  let connections = 0;
  const sockets = new Set();
  const server = net.createServer((socket) => {
    connections += 1;
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let wire = '';
    socket.on('data', (chunk) => {
      wire += chunk;
      while (wire.includes('\n')) {
        const newline = wire.indexOf('\n');
        const request = JSON.parse(wire.slice(0, newline));
        wire = wire.slice(newline + 1);
        setTimeout(() => socket.write(JSON.stringify({ sequence: request.sequence }) + '\n'), 5);
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = new PacificDBClient({ host: '127.0.0.1',
    port: server.address().port, poolSize: 4 });
  try {
    const first = await Promise.all(Array.from({ length: 24 }, (_, sequence) =>
      client.request({ action: 'ping', sequence })));
    assert.deepEqual(first.map(({ sequence }) => sequence),
      Array.from({ length: 24 }, (_, sequence) => sequence));
    assert.equal(connections, 4);
    await Promise.all(Array.from({ length: 8 }, (_, sequence) =>
      client.request({ action: 'ping', sequence })));
    assert.equal(connections, 4);
  } finally {
    client.close();
    for (const socket of sockets) socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test('returns a completed connection to the pool before resolving sequential requests', async () => {
  let connections = 0;
  const sockets = new Set();
  const server = net.createServer((socket) => {
    connections += 1;
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let wire = '';
    socket.on('data', (chunk) => {
      wire += chunk;
      while (wire.includes('\n')) {
        const newline = wire.indexOf('\n');
        const request = JSON.parse(wire.slice(0, newline));
        wire = wire.slice(newline + 1);
        socket.write(JSON.stringify({ sequence: request.sequence }) + '\n');
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = new PacificDBClient({ host: '127.0.0.1',
    port: server.address().port, poolSize: 16 });
  try {
    for (let sequence = 0; sequence < 32; sequence += 1) {
      assert.equal((await client.request({ action: 'ping', sequence })).sequence, sequence);
    }
    assert.equal(connections, 1);
  } finally {
    client.close();
    for (const socket of sockets) socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test('retires a pooled connection when the server marks its response as final', async () => {
  let connections = 0;
  const sockets = new Set();
  const server = net.createServer((socket) => {
    connections += 1;
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    socket.once('data', (data) => {
      const request = JSON.parse(data);
      socket.write(JSON.stringify({
        status: 'ok', sequence: request.sequence,
        _pacificdb_connection_close: true,
      }) + '\n');
      setTimeout(() => socket.end(), 25);
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = new PacificDBClient({ host: '127.0.0.1',
    port: server.address().port, poolSize: 1, timeoutMs: 500 });
  try {
    assert.deepEqual(await client.request({ action: 'ping', sequence: 1 }),
      { status: 'ok', sequence: 1 });
    assert.deepEqual(await client.request({ action: 'ping', sequence: 2 }),
      { status: 'ok', sequence: 2 });
    assert.equal(connections, 2);
  } finally {
    client.close();
    for (const socket of sockets) socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test('prewarms the configured pool and sends insertMany data in one request', async () => {
  let connections = 0;
  let received;
  let resolvePrewarmed;
  const prewarmed = new Promise((resolve) => { resolvePrewarmed = resolve; });
  const sockets = new Set();
  const server = net.createServer((socket) => {
    connections += 1;
    if (connections === 3) resolvePrewarmed();
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let wire = '';
    socket.on('data', (chunk) => {
      wire += chunk;
      const newline = wire.indexOf('\n');
      if (newline < 0) return;
      received = JSON.parse(wire.slice(0, newline));
      socket.write(JSON.stringify({ status: 'ok', inserted: received.data.length }) + '\n');
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = new PacificDBClient({ host: '127.0.0.1',
    port: server.address().port, database: 'app', poolSize: 3 });
  try {
    await client.connect();
    await prewarmed;
    assert.equal(connections, 3);
    assert.deepEqual(await client.insertMany('users', [{ id: '1' }, { id: '2' }]),
      { status: 'ok', inserted: 2 });
    assert.equal(received.action, 'insertMany');
    assert.deepEqual(received.data, [{ id: '1' }, { id: '2' }]);
    assert.equal(received.documents, undefined);
    assert.throws(() => client.insertMany('users', {}), /must be an array/);
  } finally {
    client.close();
    for (const socket of sockets) socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test('reports a non-PacificDB server without leaking a JSON parser error', async (t) => {
  const server = net.createServer((socket) => socket.once('data', () =>
    socket.end('HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n')));
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  const client = new PacificDBClient({ host: '127.0.0.1', port: server.address().port });
  await assert.rejects(client.request({ action: 'ping' }),
    /returned a non-JSON response; verify the host and port/);
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
        response = { status: 'ok', max_request_bytes: 1_048_576,
          media_chunk_source_max_bytes: 262_144 };
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
  const uploaded = await client.uploadMediaFile('videos', input);
  assert.equal(uploaded.status, 'ready');
  assert.equal(requests.filter((r) =>
    r.action === 'community_media_put_chunk').length, 3);

  const downloaded = await client.downloadMediaFile(uploaded.id, output);
  assert.deepEqual(await readFile(output), source);
  assert.equal(downloaded.sha256,
    createHash('sha256').update(source).digest('hex'));
});

test('reports a stable upload id and resumes only missing media chunks', async (t) => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-node-resume-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const input = path.join(directory, 'resume.bin');
  await writeFile(input, Buffer.alloc(150_000, 11));
  const received = new Set();
  const afterResume = [];
  let interrupted = false;
  let resumed = false;
  const progress = () => ({ id: 'media_resume', status: 'uploading',
    received_indices: [...received].sort((a, b) => a - b),
    received_chunks: received.size, received_bytes: [...received]
      .reduce((total, index) => total + (index < 2 ? 65_536 : 18_928), 0) });
  const server = net.createServer((socket) => {
    let wire = '';
    socket.on('data', (data) => {
    wire += data;
    const newline = wire.indexOf('\n');
    if (newline < 0) return;
    const request = JSON.parse(wire.slice(0, newline));
    if (request.action === 'community_capabilities') {
      return socket.end(JSON.stringify({ max_request_bytes: 262_144,
        media_chunk_source_max_bytes: 65_536 }) + '\n');
    }
    if (request.action === 'community_media_begin') {
      resumed = Boolean(request.resume_id);
      return socket.end(JSON.stringify({ status: 'ok', media: progress() }) + '\n');
    }
    if (request.action === 'community_media_put_chunk') {
      received.add(request.index);
      if (resumed) afterResume.push(request.index);
      if (request.index === 1 && !interrupted) {
        interrupted = true;
        return socket.destroy();
      }
      return socket.end(JSON.stringify({ status: 'ok', ...progress() }) + '\n');
    }
    if (request.action === 'community_media_finalize') {
      return socket.end(JSON.stringify({ status: 'ok', media: {
        ...progress(), status: 'ready' } }) + '\n');
    }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => server.close());
  const client = new PacificDBClient({ host: '127.0.0.1',
    port: server.address().port, database: 'app' });

  await assert.rejects(client.uploadMediaFile('videos', input, { chunkBytes: 65_536 }),
    (error) => error instanceof MediaUploadError &&
      error.code === 'media_upload_interrupted' &&
      error.uploadId === 'media_resume' && error.nextChunk === 1 &&
      error.receivedChunks === 1 && error.resumable === true);
  const ready = await client.uploadMediaFile('videos', input,
    { chunkBytes: 65_536, resume: 'media_resume' });
  assert.equal(ready.status, 'ready');
  assert.deepEqual(afterResume, [2]);
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
