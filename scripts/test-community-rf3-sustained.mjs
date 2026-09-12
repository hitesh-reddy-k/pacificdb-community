#!/usr/bin/env node

import assert from 'node:assert/strict';
import path from 'node:path';
import { RaftTestCluster, pause } from './lib/raft-test-cluster.mjs';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const durationSeconds = Number(process.env.PACIFICDB_RF3_DURATION_SECONDS || 20);
const repeats = Number(process.env.PACIFICDB_RF3_REPEATS || 1);
const clientCounts = (process.env.PACIFICDB_RF3_CLIENTS || '64,128')
  .split(',').map(Number);
const operationIntervalMs = Number(process.env.PACIFICDB_RF3_OPERATION_INTERVAL_MS || 2000);
const convergenceTimeoutSeconds = Number(process.env.PACIFICDB_RF3_CONVERGENCE_TIMEOUT_SECONDS || 300);
assert.ok(Number.isInteger(durationSeconds) && durationSeconds >= 5);
assert.ok(Number.isInteger(repeats) && repeats >= 1);
assert.ok(clientCounts.every((count) => Number.isInteger(count) && count > 0));
assert.ok(Number.isInteger(operationIntervalMs) && operationIntervalMs >= 100);

async function readDocumentSet(client) {
  const documents = [];
  for (let offset = 0;; offset += 1000) {
    const page = await client.find('docs', {}, { limit: 1000, offset });
    documents.push(...page.data);
    if (page.data.length < 1000) break;
  }
  return documents;
}

async function waitForFinalConvergence(cluster, expectedCount) {
  const startedAt = Date.now();
  let last;
  const attempts = convergenceTimeoutSeconds * 5;
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    let pings = [];
    try {
      pings = await cluster.pings();
      assert.equal(pings.filter((ping) => ping.isLeader).length, 1);
      assert.ok(pings.every((ping) => !ping.error));
      assert.ok(pings.every((ping) => ping.leader_term === pings[0].leader_term));
      assert.ok(pings.every((ping) => ping.commit_index === pings[0].commit_index));
      assert.ok(pings.every((ping) => ping.last_applied === ping.commit_index));
      const counts = [];
      for (let index = 0; index < 3; index += 1) {
        const client = cluster.client(index, { timeoutMs: 10_000 });
        client.database = 'load';
        counts.push((await client.request({ action: 'count', collection: 'docs', filter: {} })).count);
      }
      assert.ok(counts.every((count) => count === expectedCount));
      return { pings, counts, convergence_ms: Date.now() - startedAt };
    } catch (error) {
      last = { message: error.message, pings };
      if (attempt > 0 && attempt % 25 === 0) {
        console.error(JSON.stringify({ status: 'CONVERGENCE_WAIT',
          elapsed_ms: Date.now() - startedAt, pings }));
      }
      await pause(200);
    }
  }
  throw new Error(`RF3 failed to converge: ${JSON.stringify(last)}`);
}

async function runRound(clientCount, repeat) {
  const cluster = await RaftTestCluster.create({
    build, clusterId: `sustained-${clientCount}-${repeat}-${process.pid}`,
    useProxies: false, allEligible: false,
    rootPrefix: `pacificdb-rf3-sustained-${clientCount}-${repeat}-`
  });
  const expected = new Map();
  const errors = [];
  const samples = [];
  let operations = 0;
  let passed = false;
  try {
    await cluster.start();
    const leaderState = await cluster.waitForLeader([0], 10_000);
    const setup = cluster.client(0, { timeoutMs: 10_000 });
    await cluster.retry(() => setup.createDatabase('load'), 'create load database');
    setup.database = 'load';
    await cluster.retry(() => setup.createCollection('docs'), 'create docs collection');
    const initialTerm = (await setup.request({ action: 'ping' })).leader_term;
    const startedAt = Date.now();
    const finishAt = startedAt + durationSeconds * 1000;
    let stop = false;

    const monitor = (async () => {
      while (!stop && Date.now() < finishAt) {
        const pings = await cluster.pings();
        samples.push({ at_ms: Date.now() - startedAt,
          terms: pings.map((ping) => ping.leader_term),
          leaders: pings.map((ping) => Boolean(ping.isLeader)),
          lag: pings.map((ping) => Number(ping.commit_index) - Number(ping.last_applied)) });
        if (pings.some((ping) => ping.error) ||
            pings.filter((ping) => ping.isLeader).length !== 1 ||
            pings.some((ping) => ping.leader_term !== initialTerm)) {
          errors.push({ category: 'cluster_state', pings });
          stop = true;
          return;
        }
        await pause(1000);
      }
    })();

    const workers = Array.from({ length: clientCount }, (_, worker) => (async () => {
      const client = cluster.client(leaderState.index, { timeoutMs: 15_000 });
      client.database = 'load';
      let sequence = 0;
      await pause(Math.floor(worker * operationIntervalMs / clientCount));
      while (!stop && Date.now() < finishAt) {
        const cycleStarted = Date.now();
        const id = `r${repeat}-c${worker}-s${sequence}`;
        try {
          await client.insert('docs', { id, owner: worker, sequence, value: 1 });
          expected.set(id, 1);
          operations += 1;
          const inserted = await client.find('docs', { id }, { limit: 1 });
          assert.equal(inserted.data.length, 1);
          assert.equal(inserted.data[0].value, 1);
          operations += 1;
          if (sequence % 4 === 0) {
            await client.updateOne('docs', { id }, { value: 2 });
            expected.set(id, 2);
            const updated = await client.find('docs', { id }, { limit: 1 });
            assert.equal(updated.data[0].value, 2);
            operations += 2;
          }
          if (sequence % 10 === 0) {
            const transient = `${id}-deleted`;
            await client.insert('docs', { id: transient, value: 1 });
            await client.deleteOne('docs', { id: transient });
            assert.equal((await client.find('docs', { id: transient }, { limit: 1 })).data.length, 0);
            operations += 3;
          }
        } catch (error) {
          errors.push({ category: 'workload', worker, sequence, message: error.message });
          stop = true;
          return;
        }
        sequence += 1;
        const delay = operationIntervalMs - (Date.now() - cycleStarted);
        if (delay > 0) await pause(delay);
      }
    })());

    await Promise.all(workers);
    stop = true;
    await monitor;
    assert.deepEqual(errors, [], `workload errors: ${JSON.stringify(errors.slice(0, 5))}`);
    const convergence = await waitForFinalConvergence(cluster, expected.size);
    for (let index = 0; index < 3; index += 1) {
      const client = cluster.client(index, { timeoutMs: 15_000 });
      client.database = 'load';
      const documents = await readDocumentSet(client);
      const actual = new Map(documents.map((document) => [document.id, document.value]));
      assert.equal(actual.size, expected.size);
      assert.deepEqual(actual, expected);
    }
    passed = true;
    return {
      clients: clientCount, repeat, duration_seconds: durationSeconds,
      operations, acknowledged_documents: expected.size, errors: 0,
      initial_term: initialTerm, final_term: convergence.pings[0].leader_term,
      max_apply_lag: Math.max(0, ...samples.flatMap((sample) => sample.lag)),
      convergence_ms: convergence.convergence_ms,
      final_commit_index: convergence.pings[0].commit_index,
      final_last_applied: convergence.pings.map((ping) => ping.last_applied)
    };
  } catch (error) {
    cluster.keep = true;
    console.error(JSON.stringify({ status: 'ROUND_FAIL', clients: clientCount,
      repeat, root: cluster.root, operations, acknowledged_documents: expected.size,
      error: error.message }));
    throw error;
  } finally {
    await cluster.close();
    if (!passed) console.error(`retained failed cluster at ${cluster.root}`);
  }
}

const rounds = [];
for (const clients of clientCounts) {
  for (let repeat = 1; repeat <= repeats; repeat += 1) {
    const result = await runRound(clients, repeat);
    rounds.push(result);
    console.log(JSON.stringify({ status: 'ROUND_PASS', ...result }));
  }
}
console.log(JSON.stringify({
  status: 'PASS', configured_duration_seconds: durationSeconds,
  configured_repeats: repeats, client_counts: clientCounts,
  rounds: rounds.length, total_operations: rounds.reduce((sum, round) => sum + round.operations, 0),
  total_acknowledged_documents: rounds.reduce((sum, round) => sum + round.acknowledged_documents, 0)
}, null, 2));
