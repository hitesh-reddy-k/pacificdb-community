#!/usr/bin/env node
// Three fresh RF3 rounds per artifact; disposable data, no publication side effects.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { PacificDBClient } from '../sdk/node/src/index.js';
import { RaftTestCluster, pause } from '../scripts/lib/raft-test-cluster.mjs';

const [oldBinary, newBinary, output] = process.argv.slice(2).map(p => path.resolve(p));
assert.ok(oldBinary && newBinary && output);
await mkdir(output, { recursive: true });
assert.ok(!(await readdir(output)).length, 'output directory must be unused');
const { PacificDBClient: BaselineClient } = await import(pathToFileURL(path.resolve(
  process.env.BENCH_BASELINE_SDK || 'build-baseline-sdk/package/src/index.js')));
const artifacts = await Promise.all([['v1.0.0', oldBinary], ['v1.1.1', newBinary]].map(async ([label, binary]) => ({
  label, binary, sha256: createHash('sha256').update(await readFile(binary)).digest('hex'),
  version: execFileSync(binary, ['--version'], { encoding: 'utf8' }).trim()
})));
for (const artifact of artifacts) assert.ok(artifact.version.includes(artifact.label.slice(1)));
const cases = [{ name: 'rf3-insert', operation: 'insert', calls: 500, concurrency: 8, pool: 8 }];
const ticks = Number(execFileSync('getconf', ['CLK_TCK'], { encoding: 'utf8' }));
async function resources(cluster) {
  const nodes = await Promise.all(cluster.nodes.map(async ({ child }) => {
    const stat = await readFile(`/proc/${child.pid}/stat`, 'utf8');
    const fields = stat.slice(stat.lastIndexOf(')') + 2).split(' ');
    const status = await readFile(`/proc/${child.pid}/status`, 'utf8');
    const io = await readFile(`/proc/${child.pid}/io`, 'utf8');
    const value = key => Number((`${status}\n${io}`).match(new RegExp(`^${key}:\\s*(\\d+)`, 'm'))?.[1] || 0);
    return { cpu_seconds: (Number(fields[11]) + Number(fields[12])) / ticks,
      rss_kib: value('VmRSS'), peak_rss_kib: value('VmHWM'), threads: value('Threads'),
      read_bytes: value('read_bytes'), write_bytes: value('write_bytes') };
  }));
  return Object.fromEntries(Object.keys(nodes[0]).map(key => [key, nodes.reduce((sum, node) => sum + node[key], 0)]));
}
await writeFile(path.join(output, 'method.json'), JSON.stringify({ artifacts, config: {
  cases, repeats: 3, replication_factor: 3, min_quorum: 2, fsync: true,
  topology: 'three engines on distinct loopback addresses, no proxy, fixed leader eligibility',
  dataset: '100 warmup plus 500 timed distinct IDs, 1024 ASCII payload characters',
  resource_notes: 'sum of three engine /proc CPU/RSS/I/O counters; RSS high-water is process-lifetime, including setup',
  limitations: 'Short finite shared-host test; no per-stage append/replication/quorum latency attribution. Catch-up measured separately, outside timing.'
}, date: new Date().toISOString() }, null, 2));
for (let round = 1; round <= 3; round++) for (const artifact of round % 2 ? artifacts : [...artifacts].reverse()) {
  const Client = artifact.label === 'v1.0.0' ? BaselineClient : PacificDBClient;
  const cluster = await RaftTestCluster.create({ binaries: Array(3).fill(artifact.binary), useProxies: false, allEligible: false });
  const clients = new Map();
  cluster.client = index => {
    if (!clients.has(index)) clients.set(index, new Client({ host: cluster.hosts[index], port: cluster.enginePorts[index],
      database: 'bench', poolSize: 8, timeoutMs: 15000 }));
    return clients.get(index);
  };
  const originalEnvironment = cluster.nodeEnvironment.bind(cluster);
  cluster.nodeEnvironment = index => {
    const env = { ...originalEnvironment(index), WAL_FSYNC_ENABLED: '1', RAFT_ASYNC_LOG_FSYNC: '0', TLS_ENABLED: '0' };
    for (const key of Object.keys(env)) if (/^(?:RAFT_STANDALONE_|RAFT_APPLY_PUBLICATION|RAFT_ORDERED_APPLY|PACIFICDB_NATIVE_PATH)/.test(key)) delete env[key];
    return env;
  };
  const expected = new Map(), latency = [], errors = [];
  const doc = id => ({ id, payload: 'x'.repeat(1024), nested: { unicode: '数据库 🌊', array: [1, null, true] } });
  const file = path.join(output, `rf3-insert-${round}-${artifact.label}.json`);
  let result;
  async function convergence() {
    const began = performance.now(); let last;
    for (let attempt = 0; attempt < 300; attempt++) {
      last = await cluster.pings();
      if (last.every(p => !p.error && p.commit_index === last[0].commit_index && p.last_applied === p.commit_index))
        return { milliseconds: performance.now() - began, pings: last };
      await pause(100);
    }
    throw new Error(`convergence failed: ${JSON.stringify(last)}`);
  }
  async function verifyAll() {
    for (let node = 0; node < 3; node++) {
      const client = cluster.client(node);
      assert.equal((await client.request({ action: 'count', collection: 'records', filter: {} })).count, expected.size);
      for (const [id, value] of expected) {
        const row = (await client.find('records', { id }, { limit: 1 })).data[0];
        for (const key of Object.keys(value)) assert.deepEqual(row?.[key], value[key]);
      }
    }
  }
  try {
    await cluster.start(); await cluster.waitForLeader([0]);
    const client = cluster.client(0);
    await cluster.retry(() => client.createDatabase('bench'), 'create bench');
    await cluster.retry(() => client.createCollection('records'), 'create records');
    for (let i = 0; i < 100; i++) { const d = doc(`warm-${i}`); await client.insert('records', d); expected.set(d.id, d); }
    await convergence(); const before = await resources(cluster);
    const started = performance.now(); let next = 0;
    await Promise.all(Array.from({ length: 8 }, async () => {
      while (next < 500) {
        const i = next++, begin = performance.now(), d = doc(`timed-${i}`);
        try { assert.equal((await client.insert('records', d)).status, 'ok'); expected.set(d.id, d); latency.push(performance.now() - begin); }
        catch (error) { errors.push({ operation: i, message: error.message }); }
      }
    }));
    const elapsed = performance.now() - started, after = await resources(cluster);
    latency.sort((a, b) => a - b);
    const quantile = p => latency[Math.min(latency.length - 1, Math.ceil(latency.length * p) - 1)];
    result = { round, artifact, workload: cases[0], calls: 500, successful_calls: latency.length, errors,
      elapsed_ms: elapsed, attempted_calls_per_second: 500000 / elapsed, documents_per_second: 500000 / elapsed,
      latency_ms: { mean: latency.reduce((a, b) => a + b, 0) / latency.length, p50: quantile(.5), p95: quantile(.95), p99: quantile(.99) },
      resources: { before, after, sampled_peak_rss_kib: after.peak_rss_kib,
        delta: Object.fromEntries(['cpu_seconds', 'read_bytes', 'write_bytes'].map(key => [key, after[key] - before[key]])) },
      integrity: 'NOT_RUN' };
    await writeFile(file, JSON.stringify(result, null, 2)); assert.equal(errors.length, 0);
    result.after_timing = await convergence(); await verifyAll();
    // Majority remains available while one follower is stopped.
    clients.get(2).close(); clients.delete(2); await cluster.stopNode(2, 'SIGKILL');
    for (let i = 0; i < 50; i++) { const d = doc(`catchup-${i}`); await client.insert('records', d); expected.set(d.id, d); }
    const catchup = performance.now(); await cluster.startNode(2); await convergence();
    result.follower_restart_and_catchup_ms = performance.now() - catchup; await verifyAll();
    for (const c of clients.values()) c.close(); clients.clear();
    await Promise.all([0, 1, 2].map(index => cluster.stopNode(index, 'SIGKILL')));
    await cluster.start(); await cluster.waitForLeader([0]); await convergence(); await verifyAll();
    result.integrity = 'PASS: every expected field on all three nodes, follower catch-up and all-node SIGKILL/restart';
    await writeFile(file, JSON.stringify(result, null, 2));
    console.log(JSON.stringify({ round, version: artifact.label, calls_per_second: result.attempted_calls_per_second,
      p95_ms: result.latency_ms.p95, catchup_ms: result.follower_restart_and_catchup_ms, integrity: 'PASS' }));
  } finally { for (const client of clients.values()) client.close(); await cluster.close(); }
}
