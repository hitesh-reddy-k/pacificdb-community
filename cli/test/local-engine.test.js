import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import {
  actionForEngineState,
  classifyEngineState,
  engineStateCode,
  ensureLocalEngine,
} from '../src/local-engine.js';

const fixture = JSON.parse(await readFile(new URL(
  '../../engine/test/fixtures/local_engine_states.json', import.meta.url), 'utf8'));

test('local-engine state decisions match the native shared fixture', () => {
  for (const testCase of fixture) {
    const state = classifyEngineState(testCase.observations);
    assert.equal(state, testCase.expectedState, testCase.name);
    assert.equal(engineStateCode(state, testCase.observations),
      testCase.expectedCode, testCase.name);
    assert.equal(actionForEngineState(state, false), 'none',
      `${testCase.name} must be read-only with --no-start`);
  }
});

test('TLS connections never start a local plaintext engine', async () => {
  assert.equal(await ensureLocalEngine({ host: '127.0.0.1', port: 9000, useTls: true }), false);
});

// A concurrent engine can own its data lock before the launcher publishes metadata.
test('unpublished root owner waits only under the startup lock', async () => {
  const home = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-start-owner-'));
  const previous = Object.fromEntries(['PACIFICDB_HOME', 'PACIFICDB_ENGINE',
    'PACIFICDB_STARTUP_TIMEOUT_MS', 'DATA_ROOT'].map(key => [key, process.env[key]]));
  const listener = net.createServer();
  await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
  const port = listener.address().port;
  await new Promise(resolve => listener.close(resolve));
  try {
    process.env.PACIFICDB_HOME = home;
    process.env.PACIFICDB_ENGINE = process.execPath;
    process.env.PACIFICDB_STARTUP_TIMEOUT_MS = '100';
    process.env.DATA_ROOT = path.join(home, 'data');
    await mkdir(process.env.DATA_ROOT);
    await writeFile(path.join(process.env.DATA_ROOT, '.pacificdb-root.lock'),
      JSON.stringify({schema: 'pacificdb.storage-root-owner.v1', pid: process.pid,
        instance_id: 'engine_starting'}));
    const client = {host: '127.0.0.1', port, useTls: false};
    await assert.rejects(ensureLocalEngine(client), error =>
      error.message.includes('engine_data_root_in_use') &&
      !error.message.includes('startup deadline'));
    await mkdir(path.join(home, '.engine-starting'));
    assert.equal(await ensureLocalEngine(client, {autoStart: false}), false);
    await assert.rejects(ensureLocalEngine(client), error =>
      error.message.includes('another launcher did not finish before the startup deadline'));
  } finally {
    for (const [key, value] of Object.entries(previous)) {
      if (value === undefined) delete process.env[key]; else process.env[key] = value;
    }
    await rm(home, {recursive: true, force: true});
  }
});
