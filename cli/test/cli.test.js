import assert from 'node:assert/strict';
import test from 'node:test';
import { PassThrough } from 'node:stream';
import { main } from '../src/cli.js';

test('rejects unknown commands with useful usage', async () => {
  await assert.rejects(() => main(['unknown'], {
    input: new PassThrough(), output: new PassThrough()
  }), /usage:/);
});
