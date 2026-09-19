#!/usr/bin/env node

import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { RaftTestCluster } from './lib/raft-test-cluster.mjs';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const monitor = path.join(import.meta.dirname, 'replica-integrity-monitor.mjs');
const cluster = await RaftTestCluster.create({
  build, clusterId: `replica-integrity-rf3-${process.pid}`,
  useProxies: true, allEligible: false, rootPrefix: 'pacificdb-integrity-rf3-'
});

async function runMonitor(expectedStatus, expectedExit) {
  const output = path.join(cluster.root, `integrity-${Date.now()}.json`);
  const child = spawn(process.execPath,
    [monitor, '--config', path.join(cluster.root, 'integrity-config.json'),
      '--output', output], { cwd: repositoryRoot, stdio: 'ignore' });
  const exitCode = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', resolve);
  });
  const evidence = JSON.parse(await readFile(output, 'utf8'));
  assert.equal(exitCode, expectedExit);
  assert.equal(evidence.status, expectedStatus);
  return evidence;
}

const clients = [];
try {
  await cluster.start();
  for (let index = 0; index < 3; index += 1) {
    clients.push(cluster.client(index, { timeoutMs: 4000 }));
    clients[index].database = 'integrity_db';
  }
  await cluster.retry(async () => {
    assert.equal((await clients[0].request({ action: 'ping' })).isLeader, true);
  }, 'leader readiness');
  await clients[0].createDatabase('integrity_db');
  await clients[0].createCollection('docs');
  await clients[0].insertMany('docs', [
    { id: 'one', value: 1 }, { id: 'two', value: 2 },
  ]);
  await cluster.retry(async () => {
    for (let index = 0; index < 3; index += 1) {
      assert.equal((await clients[index].find('docs', {})).data.length, 2);
    }
  }, 'initial replica convergence');

  await writeFile(path.join(cluster.root, 'integrity-config.json'), JSON.stringify({
    userId: 'system', database: 'integrity_db', collection: 'docs', maxDocs: 100,
    timeoutMs: 1000, lagTimeoutMs: 1200, pollIntervalMs: 100,
    nodes: cluster.hosts.map((host, index) => ({
      id: `node-${index + 1}`, host, port: cluster.enginePorts[index],
    })),
  }));

  const initial = await runMonitor('CONSISTENT', 0);
  assert.equal(new Set(initial.nodes.map(({ digest }) => digest)).size, 1);

  cluster.isolate(2);
  await clients[0].insert('docs', { id: 'partitioned', value: 3 });
  const lagging = await runMonitor('LAGGING', 3);
  assert.notEqual(lagging.nodes[0].lastApplied, lagging.nodes[2].lastApplied);

  cluster.heal();
  await cluster.retry(async () => {
    assert.equal((await clients[2].find('docs', { id: 'partitioned' })).data.length, 1);
  }, 'healed follower convergence');
  let healed;
  await cluster.retry(async () => {
    healed = await runMonitor('CONSISTENT', 0);
  }, 'integrity convergence after heal', 30, 200);
  assert.equal(new Set(healed.nodes.map(({ digest }) => digest)).size, 1);
  console.log('REPLICA_INTEGRITY_RF3_PASS');
} finally {
  for (const client of clients) client.close();
  await cluster.close();
}
