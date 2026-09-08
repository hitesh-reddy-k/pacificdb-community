import assert from 'node:assert/strict';
import net from 'node:net';
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
