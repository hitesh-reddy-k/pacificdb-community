#!/usr/bin/env node
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';
import { startDesktopEngine } from '../desktop/engine.mjs';

const build = path.resolve(process.argv[2] || 'build');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-index-scale-'));
const records = Number(process.argv[3] || 50000);
assert.ok(Number.isSafeInteger(records) && records > 0 && records % 1000 === 0,
  'record count must be a positive multiple of 1000');
let engine, client;
try {
  engine = await startDesktopEngine({ executable: path.join(build,
    process.platform === 'win32' ? 'db_engine.exe' : 'db_engine'), directory: root });
  client = new PacificDBClient({ port: engine.port, poolSize: 1, timeoutMs: 30000 });
  await client.createProject('Index scale regression');
  await client.createDatabase('index_scale');
  await client.useDatabase('index_scale');
  await client.createCollection('records');
  for (let offset = 0; offset < records; offset += 1000) {
    const result = await client.insertMany('records', Array.from({ length: 1000 }, (_, index) =>
      ({ id: `record-${offset + index}`, country: 'India' })));
    assert.equal(result.inserted, 1000);
    assert.equal(result.failed, 0);
  }
  for (const action of ['createIndex', 'rebuildIndex', 'validateIndex']) {
    const started = performance.now();
    const result = await client.request({ action, collection: 'records',
      name: 'country_idx', ...(action === 'createIndex' ? { fields: { country: 1 } } : {}) });
    const elapsedMs = Math.round(performance.now() - started);
    assert.ok(elapsedMs < 15000, `${action} took ${elapsedMs} ms for ${records} identical values`);
    assert.ok(['ok', 'clean', 'CLEAN'].includes(result.status), JSON.stringify(result));
    console.log('INDEX_SCALE_CHECK', action, elapsedMs, 'ms');
  }
  assert.equal((await client.request({ action: 'count', collection: 'records',
    filter: { country: 'India' } })).count, records);
  const page = await client.find('records', { country: 'India' }, { limit: 10 });
  assert.equal(page.data.length, 10);
  assert.ok(page.data.every((document) => document.country === 'India'));
  console.log('INDEX_LOW_CARDINALITY_PASS', records);
} finally {
  client?.close();
  await engine?.stop();
  await rm(root, { recursive: true, force: true });
}
