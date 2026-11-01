#!/usr/bin/env node
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';

const build = path.resolve(process.argv[2] || 'build-v1.2-safety');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-request-isolation-'));
async function port() {
  const server = net.createServer();
  await new Promise((resolve, reject) => server.once('error', reject).listen(0, '127.0.0.1', resolve));
  const selected = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return selected;
}
const enginePort = await port();
let raftPort = await port();
while (raftPort === enginePort) raftPort = await port();
for (const name of ['data', 'backup', 'restore']) await mkdir(path.join(root, name));
const accounts = path.join(root, 'accounts.json');
await writeFile(accounts, JSON.stringify(['alice', 'bob'].map((username) =>
  ({ username, password: 'isolated-test-password', role: 'ADMIN' }))), { mode: 0o600 });
const log = createWriteStream(path.join(root, 'engine.log'), { mode: 0o600 });
const engine = spawn(path.join(build, 'db_engine'), [], {
  env: { ...process.env, PACIFICDB_ENVIRONMENT: 'development', PACIFICDB_HOME: root,
    DATA_ROOT: path.join(root, 'data'), BACKUP_ROOT: path.join(root, 'backup'),
    RESTORE_DIR: path.join(root, 'restore'), PACIFICDB_ENGINE_ACCOUNTS_FILE: accounts,
    ENGINE_BIND_HOST: '127.0.0.1', ENGINE_PORT: String(enginePort), ENGINE_AUTH_REQUIRED: '1',
    RAFT_LISTEN_PORT: String(raftPort), RAFT_IS_LEADER: '1', RAFT_NODE_ID: 'node-1',
    RAFT_CLUSTER_ID: 'request-isolation', MIN_QUORUM_SIZE: '1', ENGINE_CPU_CORES: '2',
    CONN_MIN_THREADS: '12', CONN_MAX_THREADS: '12', DBQ_SHARDS: '2', DBQ_WORKERS_PER_SHARD: '1',
    FAST_FIND_DIRECT: '0', ADAPTIVE_ADMISSION: '0', MAX_SCAN_ROWS: '50', MAX_RESULT_DOCS: '10',
    VECTOR_HNSW_ENABLED: 'true' }, stdio: ['ignore', 'pipe', 'pipe'] });
engine.stdout.pipe(log, { end: false });
engine.stderr.pipe(log, { end: false });
const clients = [];
let passed = false;
try {
  const probe = new PacificDBClient({ port: enginePort, timeoutMs: 300 });
  clients.push(probe);
  let ready = false;
  for (let attempt = 0; attempt < 150; ++attempt) {
    if (engine.exitCode !== null) throw new Error(`engine exited ${engine.exitCode}`);
    try { ready = (await probe.request({ action: 'ping' })).status === 'pong'; } catch {}
    if (ready) break;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  assert.ok(ready);
  probe.close();
  for (const [username, userId] of [['alice', 'tenant-a'], ['bob', 'tenant-b']]) {
    const client = new PacificDBClient({ port: enginePort, userId, poolSize: 4 });
    clients.push(client);
    await client.authenticate(username, 'isolated-test-password');
    await client.createDatabase('app');
    client.database = 'app';
    for (const collection of ['short', 'long']) {
      await client.createCollection(collection);
      const count = collection === 'short' ? 5 : 150;
      await client.insertMany(collection, Array.from({ length: count }, (_, i) =>
        ({ id: String(i), owner: username, vector: [i, 1], version: 1 })));
    }
  }
  const owners = clients.slice(1);
  // One persistent socket must survive an error after a limited internal scan.
  // A scope per connection (instead of per RPC) leaks this error's limit flag.
  const reused = new PacificDBClient({ port: enginePort, userId: 'tenant-a', database: 'app',
    token: owners[0].token, poolSize: 1 });
  clients.push(reused);
  await assert.rejects(reused.request({ action: 'aggregate', collection: 'long',
    pipeline: [{ $group: {} }] }), /unsupported Community aggregation stage/);
  const afterError = await reused.queryVector('short', [1, 0], { k: 3, searchMode: 'exact' });
  assert.equal(afterError.partial, undefined);
  const boundedAggregate = await reused.request({ action: 'aggregate', collection: 'long',
    pipeline: [{ $count: 'total' }] });
  assert.equal(boundedAggregate.partial, true);
  assert.equal((await reused.queryVector('short', [1, 0], { k: 3 })).partial, undefined);
  reused.close();
  for (let round = 0; round < 30; ++round) {
    await Promise.all(owners.flatMap((client, index) => [
      (async () => {
        const result = await client.find('long', { version: { $gt: 99999 } });
        assert.equal(result.partial, true, JSON.stringify(result));
        assert.equal(result.partial_reason, 'scan_limit');
      })(),
      (async () => {
        const result = await client.queryVector('short', [1, 0], { k: 3, metric: 'dot' });
        assert.equal(result.partial, undefined);
        assert.equal(result.search.executed_mode, 'exact');
        assert.equal(result.data[0].id, '4');
        assert.ok(result.data.every((row) => row.owner === (index === 0 ? 'alice' : 'bob')));
      })(),
      assert.rejects(client.queryVector('long', [1, 0], { k: 3 }), /vector_scan_limit_exceeded/),
      assert.rejects(client.queryVector('short', [1, 0], { k: 3, searchMode: 'ann' }), /vector_index_unavailable/),
      (async () => {
        const updated = await client.request({ action: 'updateOne', collection: 'short',
          filter: { id: '0' }, update: { $set: { epoch: round } } });
        assert.equal(updated.status, 'updated');
      })()
    ]));
  }
  for (const client of owners) {
    const fallback = await client.queryVector('short', [1, 0],
      { k: 3, searchMode: 'ann', allowExactFallback: true });
    assert.equal(fallback.search.fallback, true);
    const invisible = await client.request({ action: 'queryVector', collection: 'short',
      vector: [1, 0], k: 3, search_mode: 'exact', minVersion: 100 });
    assert.equal(invisible.count, 0);
    const documents = await client.request({ action: 'find', collection: 'short',
      filter: { version: { $gte: 0 } }, minVersion: 100 });
    assert.equal(documents.count, 0);
    assert.equal((await client.queryVector('short', [1, 0], { k: 3 })).count, 3);
    assert.equal((await client.find('short', { version: { $gte: 0 } })).count, 5);
    const strong = await client.request({ action: 'queryVector', collection: 'short',
      vector: [1, 0], k: 3, search_mode: 'exact', consistency: 'strong' });
    assert.equal(strong.count, 3);
  }
  console.log(JSON.stringify({ status: 'PASS', submitted_parallel_requests: 10, transport_slots: 8, rounds: 30,
    tenants: 2, forced_queued_find: true, legacy_ann_enabled_setting: true }));
  passed = true;
} finally {
  for (const client of clients) client.close();
  if (engine.exitCode === null) {
    engine.kill('SIGINT');
    const timer = setTimeout(() => engine.kill('SIGKILL'), 15000);
    await once(engine, 'exit'); clearTimeout(timer);
  }
  log.end(); await once(log, 'finish');
  if (passed && engine.exitCode !== 0) {
    console.error(`Nonzero engine exit; evidence retained: ${root}`);
    assert.equal(engine.exitCode, 0);
  }
  if (passed && process.env.PACIFICDB_KEEP_REQUEST_TEST !== '1') await rm(root, { recursive: true, force: true });
  else console.error(`Request isolation test evidence retained: ${root}`);
}
