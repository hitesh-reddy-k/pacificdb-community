#!/usr/bin/env node
// Disposable-data diagnostic for the v1.1.1 batch-10 regression.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';

const [oldArg, newArg, outputArg] = process.argv.slice(2);
assert.ok(oldArg && newArg && outputArg,
  'usage: node benchmarks/diagnose-batch-lock.mjs OLD_ENGINE NEW_ENGINE OUTPUT');
const output = path.resolve(outputArg);
await mkdir(output, { recursive: true });
assert.equal((await readdir(output)).length, 0, 'output directory must be unused');

const engines = await Promise.all([
  ['v1.0.0', path.resolve(oldArg)],
  ['v1.1.1', path.resolve(newArg)]
].map(async ([label, binary]) => ({
  label,
  binary,
  version: execFileSync(binary, ['--version'], { encoding: 'utf8' }).trim(),
  sha256: createHash('sha256').update(await readFile(binary)).digest('hex')
})));
assert.match(engines[0].version, /1\.0\.0/);
assert.match(engines[1].version, /1\.1\.1/);

const calls = 100;
const batchSize = 10;
const rounds = 3;
const concurrencies = [1, 8];
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const median = values => {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
};
const mean = values => values.reduce((sum, value) => sum + value, 0) / values.length;

async function freePort() {
  const server = net.createServer();
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}

async function trial(engine, concurrency, round) {
  const name = `batch-10-c${concurrency}-${round}-${engine.label}`;
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-batch-lock-'));
  const port = await freePort();
  const raftPort = await freePort();
  const env = {
    ...process.env,
    PACIFICDB_ENVIRONMENT: 'development',
    DATA_ROOT: path.join(root, 'data'),
    BACKUP_ROOT: path.join(root, 'backup'),
    RESTORE_DIR: path.join(root, 'restore'),
    ENGINE_BIND_HOST: '127.0.0.1',
    ENGINE_PORT: String(port),
    ENGINE_AUTH_REQUIRED: '0',
    RAFT_CLUSTER_ID: 'batch-lock-diagnostic',
    RAFT_NODE_ID: 'diagnostic-1',
    RAFT_LISTEN_PORT: String(raftPort),
    RAFT_IS_LEADER: '1',
    RAFT_PEERS: '',
    MIN_QUORUM_SIZE: '1',
    ENGINE_CPU_CORES: '2',
    CONN_MIN_THREADS: '4',
    CONN_MAX_THREADS: '64',
    MAX_CONNECTIONS: '128',
    ADAPTIVE_ADMISSION: '0',
    DBQ_SHARDS: '4',
    DBQ_WORKERS_PER_SHARD: '2',
    ENGINE_KEEPALIVE_MAX_REQUESTS: '10000',
    TLS_ENABLED: '0',
    WAL_FSYNC_ENABLED: '1',
    RAFT_ASYNC_LOG_FSYNC: '0'
  };
  for (const key of Object.keys(env)) {
    if (/^(?:RAFT_STANDALONE_|RAFT_APPLY_PUBLICATION|RAFT_ORDERED_APPLY|PACIFICDB_NATIVE_PATH)/.test(key)) {
      delete env[key];
    }
  }

  let child;
  let log;
  let client;
  const connect = () => new PacificDBClient({
    host: '127.0.0.1', port, database: 'bench', poolSize: concurrency, timeoutMs: 60000
  });
  async function start() {
    log = createWriteStream(path.join(output, `${name}-engine.log`), { flags: 'a' });
    child = spawn(engine.binary, [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    child.stdout.pipe(log, { end: false });
    child.stderr.pipe(log, { end: false });
    client = connect();
    for (let attempt = 0; attempt < 200; attempt++) {
      assert.equal(child.exitCode, null, 'engine exited during startup');
      try {
        if ((await client.request({ action: 'ping' })).status === 'pong') return;
      } catch {}
      await pause(100);
    }
    throw new Error('startup timeout');
  }
  async function stop(signal) {
    client?.close();
    if (child && child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit');
      child.kill(signal);
      const timer = setTimeout(() => child.kill('SIGKILL'), 20000);
      const [, actualSignal] = await exited;
      clearTimeout(timer);
      if (signal === 'SIGKILL') assert.equal(actualSignal, 'SIGKILL');
    }
    if (log) {
      log.end();
      await once(log, 'finish');
    }
    child = undefined;
  }

  try {
    await start();
    await client.createDatabase('bench');
    await client.createCollection('records');
    for (let warmup = 0; warmup < 10; warmup++) {
      const docs = Array.from({ length: batchSize }, (_, index) => ({
        id: `warm-${warmup}-${index}`, payload: 'x'.repeat(1024)
      }));
      const result = await client.insertMany('records', docs);
      assert.equal(result.inserted, batchSize);
    }

    const latenciesMs = [];
    const walAppendUs = [];
    const storageNonWalUs = [];
    const lockWaitUs = [];
    const physicalSyncs = [];
    let next = 0;
    const started = performance.now();
    await Promise.all(Array.from({ length: concurrency }, async () => {
      while (next < calls) {
        const call = next++;
        const docs = Array.from({ length: batchSize }, (_, index) => ({
          id: `timed-${call}-${index}`, payload: 'x'.repeat(1024)
        }));
        const before = performance.now();
        const response = await client.request({
          action: 'insertMany', collection: 'records', data: docs, includeTelemetry: true
        });
        latenciesMs.push(performance.now() - before);
        assert.equal(response.inserted, batchSize);
        assert.equal(response.failed, 0);
        const bulk = response._bulkMetrics;
        const lsm = bulk?.lsm;
        if (lsm?.timings_us && bulk?.timings_us) {
          const wal = Number(lsm.timings_us.wal_append || 0);
          walAppendUs.push(wal);
          storageNonWalUs.push(Math.max(0, Number(bulk.timings_us.lsm_total || 0) - wal));
          physicalSyncs.push(Number(lsm.wal_physical_syncs || 0));
        }
        if (response._engineTrace?.timings_us) {
          lockWaitUs.push(Number(response._engineTrace.timings_us.lock_wait || 0));
        }
      }
    }));
    const elapsedMs = performance.now() - started;
    const expectedCount = (10 + calls) * batchSize;
    assert.equal((await client.request({ action: 'count', collection: 'records', filter: {} })).count,
      expectedCount);
    await stop('SIGKILL');
    await start();
    assert.equal((await client.request({ action: 'count', collection: 'records', filter: {} })).count,
      expectedCount);

    latenciesMs.sort((a, b) => a - b);
    const result = {
      name,
      engine,
      round,
      concurrency,
      calls,
      batch_size: batchSize,
      calls_per_second: calls * 1000 / elapsedMs,
      latency_ms: {
        mean: mean(latenciesMs),
        p50: latenciesMs[Math.ceil(latenciesMs.length * 0.50) - 1],
        p95: latenciesMs[Math.ceil(latenciesMs.length * 0.95) - 1],
        p99: latenciesMs[Math.ceil(latenciesMs.length * 0.99) - 1]
      },
      server_metrics: walAppendUs.length ? {
        samples: walAppendUs.length,
        mean_wal_append_us: mean(walAppendUs),
        mean_storage_non_wal_us: mean(storageNonWalUs),
        mean_lock_wait_us: lockWaitUs.length ? mean(lockWaitUs) : 'NOT_EXPOSED_BY_ENGINE',
        reported_physical_syncs: physicalSyncs.reduce((sum, value) => sum + value, 0)
      } : 'NOT_EXPOSED_BY_ENGINE',
      integrity: `PASS: ${expectedCount} documents before and after SIGKILL restart`
    };
    await writeFile(path.join(output, `${name}.json`), JSON.stringify(result, null, 2) + '\n');
    console.log(JSON.stringify({ name, calls_per_second: result.calls_per_second,
      mean_latency_ms: result.latency_ms.mean, server_metrics: result.server_metrics }));
    return result;
  } finally {
    await stop('SIGINT').catch(() => {});
    await rm(root, { recursive: true, force: true });
  }
}

const results = [];
for (const concurrency of concurrencies) {
  for (let round = 1; round <= rounds; round++) {
    const order = round % 2 ? engines : [...engines].reverse();
    for (const engine of order) results.push(await trial(engine, concurrency, round));
  }
}
const summary = [];
for (const concurrency of concurrencies) {
  for (const engine of engines) {
    const rows = results.filter(row => row.concurrency === concurrency && row.engine.label === engine.label);
    summary.push({
      engine: engine.label,
      concurrency,
      median_calls_per_second: median(rows.map(row => row.calls_per_second)),
      median_mean_latency_ms: median(rows.map(row => row.latency_ms.mean)),
      median_mean_wal_append_us: typeof rows[0].server_metrics === 'object'
        ? median(rows.map(row => row.server_metrics.mean_wal_append_us)) : 'NOT_EXPOSED_BY_ENGINE',
      median_mean_storage_non_wal_us: typeof rows[0].server_metrics === 'object'
        ? median(rows.map(row => row.server_metrics.mean_storage_non_wal_us)) : 'NOT_EXPOSED_BY_ENGINE',
      median_mean_lock_wait_us: typeof rows[0].server_metrics === 'object' &&
          typeof rows[0].server_metrics.mean_lock_wait_us === 'number'
        ? median(rows.map(row => row.server_metrics.mean_lock_wait_us)) : 'NOT_EXPOSED_BY_ENGINE',
      median_reported_physical_syncs: typeof rows[0].server_metrics === 'object'
        ? median(rows.map(row => row.server_metrics.reported_physical_syncs)) : 'NOT_EXPOSED_BY_ENGINE'
    });
  }
}
await writeFile(path.join(output, 'summary.json'), JSON.stringify({
  date: new Date().toISOString(),
  source_revision: execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(),
  client: 'v1.1.1 candidate Node client for both engines to isolate the engine path',
  environment: { node: process.version, kernel: os.release(), logical_cpus: os.cpus().length },
  method: 'Three alternating fresh-data runs; 100 insertMany calls of 10 x 1KiB documents; fsync enabled; RF1; concurrency 1 and 8; exact count before/after SIGKILL.',
  summary
}, null, 2) + '\n');
