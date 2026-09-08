import assert from 'node:assert/strict';
import { mkdtemp, readFile, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { PassThrough } from 'node:stream';
import { main } from '../src/cli.js';

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
  assert.match(text, /Shell commands:/);
  assert.match(text, /createDatabase/);
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
