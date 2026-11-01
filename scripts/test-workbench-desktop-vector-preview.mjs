#!/usr/bin/env node
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { startDesktopEngine } from '../desktop/engine.mjs';
import { PacificDBClient } from '../sdk/node/src/index.js';

const executable = path.resolve(process.argv[2] || 'build', process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const root = await mkdtemp(path.join(os.tmpdir(), 'workbench-vector-opt-in-'));
const names = ['PACIFICDB_VECTOR_PREVIEW_ENABLED', 'PACIFICDB_WORKBENCH_VECTOR_PREVIEW', 'VECTOR_HNSW_ENABLED'];
const saved = Object.fromEntries(names.map(name => [name, process.env[name]]));
let engine, client;
try {
  process.env.PACIFICDB_VECTOR_PREVIEW_ENABLED = '1';
  process.env.VECTOR_HNSW_ENABLED = '0';
  for (const enabled of [false, true]) {
    process.env.PACIFICDB_WORKBENCH_VECTOR_PREVIEW = enabled ? '1' : '0';
    engine = await startDesktopEngine({ executable, directory: path.join(root, enabled ? 'on' : 'off') });
    client = new PacificDBClient({ port: engine.port, poolSize: 1 });
    await client.createDatabase('preview'); client.database = 'preview';
    await client.createCollection('vectors');
    await client.putVector('vectors', 'one', [1, 0]);
    const options = { name: 'ann', algorithm: 'hnsw', dimensions: 2, metric: 'cosine', max_elements: 100,
      max_scan_rows: 100, max_source_bytes: 1048576, max_index_bytes: 4194304 };
    if (!enabled) await assert.rejects(client.createVectorIndex('vectors', options), /preview_disabled/);
    else {
      await client.createVectorIndex('vectors', options);
      let state;
      const deadline = Date.now() + 15000;
      do {
        state = (await client.vectorIndexStatus('vectors', 'ann')).index.state;
        if (state === 'READY' || state === 'FAILED') break;
        await new Promise(resolve => setTimeout(resolve, 50));
      } while (Date.now() < deadline);
      assert.equal(state, 'READY');
      const results = await client.queryVector('vectors', [1, 0], { k: 1, searchMode: 'ann', indexName: 'ann', requireIndex: true, allowExactFallback: false });
      assert.equal(results.data[0].id, 'one');
      assert.equal(results.search.executed_mode, 'ann');
    }
    client.close(); client = null;
    await engine.stop(); engine = null;
  }
  console.log('WORKBENCH_DESKTOP_VECTOR_OPT_IN_PASS: inherited flags isolated; explicit preview creates and queries ANN');
} finally {
  client?.close(); await engine?.stop();
  for (const name of names) { if (saved[name] === undefined) delete process.env[name]; else process.env[name] = saved[name]; }
  await rm(root, { recursive: true, force: true });
}
