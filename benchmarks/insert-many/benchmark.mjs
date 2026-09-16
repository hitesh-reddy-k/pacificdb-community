#!/usr/bin/env node

import assert from 'node:assert/strict';
import { performance } from 'node:perf_hooks';
import { MongoClient } from 'mongodb';
import { PacificDBClient } from '../../sdk/node/src/index.js';

function positiveInteger(name, fallback) {
  const value = Number.parseInt(process.env[name] || String(fallback), 10);
  if (!Number.isSafeInteger(value) || value < 1) throw new Error(`${name} must be positive`);
  return value;
}

const records = positiveInteger('BENCH_RECORDS', 50_000);
const warmupRecords = positiveInteger('BENCH_WARMUP_RECORDS', 5_000);
const batchSize = positiveInteger('BENCH_BATCH_SIZE', 500);
const concurrency = positiveInteger('BENCH_CONCURRENCY', 4);
const poolSize = positiveInteger('BENCH_POOL_SIZE', 16);
const samples = positiveInteger('BENCH_SAMPLES', 5);
const pacificHost = process.env.PACIFICDB_HOST || '127.0.0.1';
const pacificPort = positiveInteger('PACIFICDB_PORT', 9000);
const mongoUri = process.env.MONGODB_URI || 'mongodb://127.0.0.1:27017';
const runId = `${process.pid}-${Date.now()}`;

function document(prefix, index) {
  return { id: `${prefix}-${index}`, sequence: index, active: index % 2 === 0,
    category: `category-${index % 32}`, payload: `payload-${index}`.padEnd(96, 'x') };
}

function percentile(values, fraction) {
  if (!values.length) return 0;
  const ordered = [...values].sort((a, b) => a - b);
  return ordered[Math.min(ordered.length - 1, Math.ceil(ordered.length * fraction) - 1)];
}

async function ingest(total, size, workers, phase, write) {
  let next = 0;
  const latenciesMs = [];
  const started = performance.now();
  await Promise.all(Array.from({ length: workers }, async (_, worker) => {
    while (true) {
      const offset = next;
      next += size;
      if (offset >= total) return;
      const count = Math.min(size, total - offset);
      const docs = Array.from({ length: count }, (_, index) =>
        document(`run-${runId}-${phase}-worker-${worker}`, offset + index));
      const batchStarted = performance.now();
      await write(docs);
      latenciesMs.push(performance.now() - batchStarted);
    }
  }));
  const elapsedMs = performance.now() - started;
  return { records: total, elapsed_ms: elapsedMs,
    records_per_second: total / (elapsedMs / 1000),
    batch_latency_ms_p50: percentile(latenciesMs, 0.50),
    batch_latency_ms_p95: percentile(latenciesMs, 0.95),
    batches: latenciesMs.length };
}

async function benchmarkPacificDB(sample) {
  const database = `insert_many_bench_${runId}_${sample}`;
  const collection = 'records';
  const client = new PacificDBClient({ host: pacificHost, port: pacificPort,
    database, poolSize, timeoutMs: 120_000 });
  try {
    await client.connect();
    await client.createDatabase(database);
    await client.createCollection(collection);
    await ingest(warmupRecords, batchSize, concurrency, 'warmup',
      async (docs) => assert.equal((await client.insertMany(collection, docs)).failed, 0));
    const result = await ingest(records, batchSize, concurrency, 'measured', async (docs) => {
      const response = await client.insertMany(collection, docs);
      assert.equal(response.failed, 0);
      assert.equal(response.inserted, docs.length);
    });
    const counted = await client.request({ action: 'count', collection, filter: {} });
    assert.equal(counted.count, records + warmupRecords);
    return result;
  } finally {
    client.close();
  }
}

async function benchmarkMongoDB(sample) {
  const client = new MongoClient(mongoUri, { maxPoolSize: poolSize,
    writeConcern: { w: 1, j: true } });
  const database = client.db(`insert_many_bench_${runId}_${sample}`);
  const collection = database.collection('records');
  try {
    await client.connect();
    await ingest(warmupRecords, batchSize, concurrency, 'warmup',
      (docs) => collection.insertMany(docs));
    const result = await ingest(records, batchSize, concurrency, 'measured',
      (docs) => collection.insertMany(docs));
    assert.equal(await collection.countDocuments({}), records + warmupRecords);
    return result;
  } finally {
    await database.dropDatabase().catch(() => {});
    await client.close();
  }
}

function summarize(engine, runs) {
  return { engine,
    records_per_second_median: percentile(runs.map((run) => run.records_per_second), 0.5),
    batch_latency_ms_p50_median: percentile(runs.map((run) => run.batch_latency_ms_p50), 0.5),
    batch_latency_ms_p95_median: percentile(runs.map((run) => run.batch_latency_ms_p95), 0.5),
    samples: runs };
}

const pacificRuns = [];
const mongoRuns = [];
for (let sample = 0; sample < samples; sample += 1) {
  // Alternate order so filesystem cache and thermal effects do not always
  // favor the same engine.
  if (sample % 2 === 0) {
    pacificRuns.push(await benchmarkPacificDB(sample));
    mongoRuns.push(await benchmarkMongoDB(sample));
  } else {
    mongoRuns.push(await benchmarkMongoDB(sample));
    pacificRuns.push(await benchmarkPacificDB(sample));
  }
}

const pacific = summarize('PacificDB', pacificRuns);
const mongo = summarize('MongoDB', mongoRuns);
const comparison = {
  benchmark: 'durably acknowledged insertMany',
  configuration: { records, warmup_records: warmupRecords, batch_size: batchSize,
    concurrency, pool_size: poolSize, samples,
    pacificdb_wal_fsync: true, mongodb_write_concern: { w: 1, j: true } },
  results: [pacific, mongo],
  pacificdb_to_mongodb_throughput_ratio:
    pacific.records_per_second_median / mongo.records_per_second_median
};
console.log(JSON.stringify(comparison, null, 2));
