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
