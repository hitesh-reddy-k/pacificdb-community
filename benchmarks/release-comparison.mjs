#!/usr/bin/env node
// Disposable databases only. Matched release SDKs and identical workload/configuration.
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { createHash } from 'node:crypto';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { PacificDBClient } from '../sdk/node/src/index.js';

const [oldPath, newPath, outputPath] = process.argv.slice(2).map(p => path.resolve(p));
assert.ok(oldPath && newPath && outputPath, 'usage: node benchmarks/release-comparison.mjs OLD_ENGINE NEW_ENGINE OUTPUT');
await mkdir(outputPath, { recursive: true });
assert.ok(!(await readdir(outputPath)).length, 'output directory must be unused; preserve previous raw evidence');
const baselineSdkPath = path.resolve(process.env.BENCH_BASELINE_SDK || 'build-baseline-sdk/package/src/index.js');
const { PacificDBClient: BaselineClient } = await import(pathToFileURL(baselineSdkPath));
const repeats = Number(process.env.BENCH_REPEATS || 3);
assert.ok(Number.isInteger(repeats) && repeats >= 3);
const ticks = Number(execFileSync('getconf', ['CLK_TCK'], { encoding: 'utf8' }).trim());
const cases = [
  { name: 'read', operation: 'read', calls: 10000 },
  { name: 'insert', operation: 'insert', calls: 1000 },
  { name: 'update', operation: 'update', calls: 1000 },
  { name: 'delete', operation: 'delete', calls: 1000 },
  { name: 'mixed-70r-30u', operation: 'mixed', calls: 3000 },
  ...[10, 100, 1000].map(batch => ({ name: `batch-${batch}`, operation: 'batch', batch, calls: Math.max(10, 1000 / batch) })),
  ...[1, 4, 8, 16, 32].map(pool => ({ name: `read-pool-${pool}`, operation: 'read', pool, concurrency: pool, calls: 10000 })),
  ...[128, 16384].map(payload => ({ name: `insert-payload-${payload}`, operation: 'insert', payload, calls: 500 })),
  { name: 'vector-insert-d32', operation: 'vectorInsert', calls: 500 },
  { name: 'vector-search-d32', operation: 'vectorSearch', calls: 1000 },
  { name: 'index-rebuild-low-cardinality', operation: 'index', calls: 3, concurrency: 1, pool: 1 }
];
const selected = process.env.BENCH_CASES?.split(',');
const workloadCases = selected ? cases.filter(c => selected.includes(c.name)) : cases;
assert.ok(workloadCases.length);
const config = { engine_cpu_cores: 2, shards: 4, workers_per_shard: 2,
  fsync: true, replication_factor: 1, auth: false, tls: false,
  gateway: 'direct JSON/TCP loopback', dataset_records: 1000, default_payload_characters: 1024,
  warmup_calls: 100, repeats, cases: workloadCases,
  resource_notes: 'Linux /proc process counters; main-thread context switches; RSS sampled every 500 ms. No hardware allocation/IOPS/network attribution.' };
const artifacts = [];
for (const [label, binary] of [['v1.0.0', oldPath], ['v1.1.1', newPath]]) {
  artifacts.push({ label, binary, sha256: createHash('sha256').update(await readFile(binary)).digest('hex'),
    version: execFileSync(binary, ['--version'], { encoding: 'utf8' }).trim() });
}
assert.ok(artifacts[0].version.includes('1.0.0'));
assert.ok(artifacts[1].version.includes('1.1.1'));
assert.notEqual(artifacts[0].sha256, artifacts[1].sha256);
await writeFile(path.join(outputPath, 'method.json'), JSON.stringify({ config, artifacts,
  date: new Date().toISOString(), node: process.version, kernel: os.release(), os: os.version(),
  cpu: os.cpus().map(c => c.model), logical_cpus: os.cpus().length, total_memory_bytes: os.totalmem(),
  filesystem: execFileSync('findmnt', ['-T', outputPath, '-no', 'SOURCE,FSTYPE,OPTIONS'], { encoding: 'utf8' }).trim(),
  ordering: 'alternating old/new and new/old per independent round; no concurrent build/test/benchmark',
  source_revision: execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(),
  sdk_sources: await Promise.all([['v1.0.0', baselineSdkPath], ['v1.1.1', path.resolve('sdk/node/src/index.js')]].map(async ([label, file]) => ({ label, file, sha256: createHash('sha256').update(await readFile(file)).digest('hex') }))),
  client: 'matched published 1.0.0 Node SDK vs local candidate 1.1.1 Node SDK; same API calls/configuration; full stack comparison, not an isolated engine-change attribution',
  limitations: 'Finite in-memory dataset and short phases on a shared laptop; not a service-level or cross-product claim.' }, null, 2) + '\n');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function freePort() {
  const s = net.createServer(); await new Promise(resolve => s.listen(0, '127.0.0.1', resolve));
  const port = s.address().port; await new Promise(resolve => s.close(resolve)); return port;
}
async function resource(pid) {
  const [stat, status, io, fds] = await Promise.all([readFile(`/proc/${pid}/stat`, 'utf8'),
    readFile(`/proc/${pid}/status`, 'utf8'), readFile(`/proc/${pid}/io`, 'utf8'), readdir(`/proc/${pid}/fd`)]);
  const fields = stat.slice(stat.lastIndexOf(')') + 2).split(' ');
  const values = Object.fromEntries([...(`${status}\n${io}`).matchAll(/^([\w]+):\s*(\d+)/gm)].map(m => [m[1], Number(m[2])]));
  return { cpu_seconds: (Number(fields[11]) + Number(fields[12])) / ticks, rss_kib: values.VmRSS,
    peak_rss_kib: values.VmHWM, threads: values.Threads, open_fds: fds.length,
    read_bytes: values.read_bytes, write_bytes: values.write_bytes, rchar: values.rchar, wchar: values.wchar,
    voluntary_context_switches: values.voluntary_ctxt_switches,
    involuntary_context_switches: values.nonvoluntary_ctxt_switches };
}
const vector = i => Array.from({ length: 32 }, (_, n) => n === i % 32 ? 1 : 0);
async function trial(artifact, spec, round) {
  const name = `${spec.name}-${round}-${artifact.label}`;
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-release-bench-'));
  const port = await freePort(), raftPort = await freePort();
  const env = { ...process.env, PACIFICDB_ENVIRONMENT: 'development', DATA_ROOT: path.join(root, 'data'),
    BACKUP_ROOT: path.join(root, 'backup'), RESTORE_DIR: path.join(root, 'restore'),
    ENGINE_BIND_HOST: '127.0.0.1', ENGINE_PORT: String(port), ENGINE_AUTH_REQUIRED: '0',
    RAFT_CLUSTER_ID: 'release-benchmark', RAFT_NODE_ID: 'bench-1', RAFT_LISTEN_PORT: String(raftPort),
    RAFT_IS_LEADER: '1', RAFT_PEERS: '', MIN_QUORUM_SIZE: '1', ENGINE_CPU_CORES: '2',
    CONN_MIN_THREADS: '4', CONN_MAX_THREADS: '64', MAX_CONNECTIONS: '128', ADAPTIVE_ADMISSION: '0',
    DBQ_SHARDS: '4', DBQ_WORKERS_PER_SHARD: '2', ENGINE_KEEPALIVE_MAX_REQUESTS: '10000',
    TLS_ENABLED: '0', WAL_FSYNC_ENABLED: '1', RAFT_ASYNC_LOG_FSYNC: '0' };
  for (const key of Object.keys(env)) if (/^(?:RAFT_STANDALONE_|RAFT_APPLY_PUBLICATION|RAFT_ORDERED_APPLY|PACIFICDB_NATIVE_PATH)/.test(key)) delete env[key];
  let child, log, sampling;
  const Client = artifact.label === 'v1.0.0' ? BaselineClient : PacificDBClient;
  let client = new Client({ host: '127.0.0.1', port, database: 'bench', poolSize: spec.pool || 8, timeoutMs: 60000 });
  const expected = new Map(), errors = [], latency = [];
  const record = (id, value = 0) => ({ id, value, payload: 'x'.repeat(spec.payload || 1024), nested: { unicode: '数据库 🌊', array: [1, null, true] } });
  async function start() {
    if (client._pool.closed) client = new Client({ host: '127.0.0.1', port, database: 'bench', poolSize: spec.pool || 8, timeoutMs: 60000 });
    log = createWriteStream(path.join(outputPath, `${name}-engine.log`), { flags: 'a' });
    child = spawn(artifact.binary, [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    child.stdout.pipe(log, { end: false }); child.stderr.pipe(log, { end: false });
    for (let i = 0; i < 200; i++) {
      assert.equal(child.exitCode, null, 'engine exited during startup');
      try { if ((await client.request({ action: 'ping' })).status === 'pong') return; } catch {}
      await pause(100);
    }
    throw new Error('startup timeout');
  }
  async function stop(signal) {
    client.close(); if (!child) return;
    if (child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit'); child.kill(signal); const timer = setTimeout(() => child.kill('SIGKILL'), 20000);
      const [, actual] = await exited; clearTimeout(timer); if (signal === 'SIGKILL') assert.equal(actual, 'SIGKILL');
    }
    log.end(); await once(log, 'finish'); child = null;
  }
  async function verify() {
    for (const [id, doc] of expected) {
      const response = await client.find('records', { id }, { limit: 1 });
      if (doc === null) assert.equal(response.data.length, 0);
      else { assert.equal(response.data.length, 1); for (const key of Object.keys(doc)) assert.deepEqual(response.data[0][key], doc[key]); }
    }
    if (['vectorInsert', 'vectorSearch'].includes(spec.operation)) {
      for (let i = 0; i < 100; i++) assert.deepEqual((await client.find('vectors', { id: `seed-vector-${i}` }, { limit: 1 })).data[0].vector, vector(i));
      if (spec.operation === 'vectorInsert') for (let i = 0; i < spec.calls; i++) assert.deepEqual((await client.find('vectors', { id: `timed-vector-${i}` }, { limit: 1 })).data[0].vector, vector(i));
    }
  }
  try {
    await start(); await client.createDatabase('bench'); await client.createCollection('records');
    const vectors = ['vectorInsert', 'vectorSearch'].includes(spec.operation);
    if (vectors) await client.createCollection('vectors');
    const seedCount = spec.operation === 'index' ? 5000 : spec.operation === 'mixed' ? spec.calls : 1000;
    for (let offset = 0; offset < seedCount; offset += 100) {
      const docs = Array.from({ length: Math.min(100, seedCount - offset) }, (_, j) => record(`seed-${offset + j}`));
      const result = await client.insertMany('records', docs); assert.equal(result.failed, 0); assert.equal(result.inserted, docs.length);
      for (const doc of docs) expected.set(doc.id, doc);
    }
    if (vectors) for (let i = 0; i < 100; i++) await client.putVector('vectors', `seed-vector-${i}`, vector(i));
    for (let i = 0; i < config.warmup_calls; i++) await client.find('records', { id: `seed-${i}` }, { limit: 1 });
    if (spec.operation === 'index') await client.request({ action: 'createIndex', collection: 'records', name: 'value_idx', fields: { value: 1 } });
    else if (!['read', 'vectorSearch', 'delete'].includes(spec.operation)) {
      for (let i = 0; i < 20; i++) { const doc = record(`warm-${i}`); await client.insert('records', doc); expected.set(doc.id, doc); }
    }
    const before = await resource(child.pid); let peakRss = before.rss_kib;
    sampling = setInterval(() => { resource(child.pid).then(s => { peakRss = Math.max(peakRss, s.rss_kib); }).catch(() => {}); }, 500);
    const started = performance.now(); let next = 0, documents = 0;
    await Promise.all(Array.from({ length: spec.concurrency || 8 }, async () => {
      while (next < spec.calls) {
        const i = next++; const begin = performance.now();
        try {
          switch (spec.operation) {
            case 'read': case 'mixed': {
              const id = `seed-${i % seedCount}`;
              if (spec.operation === 'mixed' && i % 10 < 3) {
                const result = await client.updateOne('records', { id }, { value: i + 1 }); assert.equal(result.status, 'updated');
                expected.set(id, record(id, i + 1));
              } else assert.equal((await client.find('records', { id }, { limit: 1 })).data.length, 1);
              documents++; break;
            }
            case 'insert': { const doc = record(`timed-${i}`); assert.equal((await client.insert('records', doc)).status, 'ok'); expected.set(doc.id, doc); documents++; break; }
            case 'update': { const id = `seed-${i}`; assert.equal((await client.updateOne('records', { id }, { value: 1 })).status, 'updated'); expected.set(id, record(id, 1)); documents++; break; }
            case 'delete': { const id = `seed-${i}`; assert.equal((await client.deleteOne('records', { id })).status, 'deleted'); expected.set(id, null); documents++; break; }
            case 'batch': { const docs = Array.from({ length: spec.batch }, (_, j) => record(`batch-${i}-${j}`)); const r = await client.insertMany('records', docs); assert.equal(r.failed, 0); assert.equal(r.inserted, docs.length); for (const doc of docs) expected.set(doc.id, doc); documents += docs.length; break; }
            case 'vectorInsert': assert.equal((await client.putVector('vectors', `timed-vector-${i}`, vector(i))).status, 'ok'); documents++; break;
            case 'vectorSearch': { const r = await client.queryVector('vectors', vector(i), { k: 5 }); assert.equal(r.data.length, 5); documents++; break; }
            case 'index': { const r = await client.request({ action: 'rebuildIndex', collection: 'records', name: 'value_idx' }); assert.equal(r.status, 'ok'); documents += seedCount; break; }
          }
          latency.push(performance.now() - begin);
        } catch (e) { errors.push({ operation: i, message: e.message }); }
      }
    }));
    const elapsedMs = performance.now() - started; clearInterval(sampling); const after = await resource(child.pid);
    latency.sort((a, b) => a - b); const percentile = p => latency[Math.min(latency.length - 1, Math.ceil(latency.length * p) - 1)];
    const result = { name, round, artifact, workload: spec, seed_records: seedCount, example_document_json_bytes: Buffer.byteLength(JSON.stringify(record('seed-0'))), calls: spec.calls, successful_calls: latency.length,
      errors, elapsed_ms: elapsedMs, attempted_calls_per_second: spec.calls * 1000 / elapsedMs,
      successful_calls_per_second: latency.length * 1000 / elapsedMs, documents_per_second: documents * 1000 / elapsedMs,
      latency_ms: { mean: latency.reduce((a,b) => a+b, 0) / latency.length, p50: percentile(.5), p95: percentile(.95), p99: percentile(.99) },
      resources: { before, after, sampled_peak_rss_kib: peakRss,
        delta: Object.fromEntries(Object.keys(before).filter(k => !/rss|threads|fds/.test(k)).map(k => [k, after[k] - before[k]])) }, integrity: 'NOT_RUN' };
    await writeFile(path.join(outputPath, `${name}.json`), JSON.stringify(result, null, 2) + '\n');
    assert.equal(errors.length, 0, 'timed failures retained; integrity check still required');
    await verify(); await stop('SIGKILL'); await start(); await verify();
    if (vectors) {
      const r = await client.queryVector('vectors', vector(0), { k: 5 }); assert.equal(r.data.length, 5);
      const expectedCount = spec.operation === 'vectorInsert' ? 600 : 100;
      assert.equal((await client.request({ action: 'count', collection: 'vectors', filter: {} })).count, expectedCount);
    }
    result.integrity = 'PASS: full expected application fields and deleted-ID absence before/after SIGKILL; exact vectors plus count/search after recovery';
    await writeFile(path.join(outputPath, `${name}.json`), JSON.stringify(result, null, 2) + '\n');
    console.log(JSON.stringify({ name, calls_per_second: result.attempted_calls_per_second, p95_ms: result.latency_ms.p95, errors: errors.length, integrity: 'PASS' }));
  } finally { clearInterval(sampling); await stop('SIGINT').catch(() => {}); await rm(root, { recursive: true, force: true }); }
}
for (const spec of workloadCases) for (let round = 1; round <= repeats; round++) {
  for (const artifact of round % 2 ? artifacts : [...artifacts].reverse()) await trial(artifact, spec, round);
}
