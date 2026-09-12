#!/usr/bin/env node

import assert from 'node:assert/strict';
import path from 'node:path';
import { RaftTestCluster, pause } from './lib/raft-test-cluster.mjs';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const cluster = await RaftTestCluster.create({
  build, clusterId: `community-partition-${process.pid}`, useProxies: true,
  allEligible: true, rootPrefix: 'pacificdb-rf3-partition-'
});

async function waitForConvergence(expectedIds, minimumTerm) {
  let last;
  for (let attempt = 0; attempt < 180; attempt += 1) {
    const pings = await cluster.pings();
    const leaders = pings.filter((ping) => ping.isLeader);
    const terms = pings.map((ping) => ping.leader_term);
    const indexes = pings.map((ping) => ping.commit_index);
    const applied = pings.map((ping) => ping.last_applied);
    try {
      assert.equal(leaders.length, 1);
      assert.ok(terms.every((term) => term === terms[0] && term >= minimumTerm));
      assert.ok(indexes.every((index) => index === indexes[0]));
      assert.ok(applied.every((index) => index === indexes[0]));
      for (let index = 0; index < 3; index += 1) {
        const client = cluster.client(index, { timeoutMs: 1500 });
        client.database = 'rf3db';
        for (const id of expectedIds) {
          assert.equal((await client.find('docs', { id })).data.length, 1);
        }
      }
      return { pings, leader: pings.findIndex((ping) => ping.isLeader) };
    } catch (error) {
      last = { error: error.message, pings };
      await pause(200);
    }
  }
  throw new Error(`cluster did not converge: ${JSON.stringify(last)}`);
}

async function runIsolationRound(round, leaderIndex, previousTerm) {
  cluster.isolate(leaderIndex);
  await pause(250);
  const oldLeader = cluster.client(leaderIndex, { timeoutMs: 5000 });
  oldLeader.database = 'rf3db';
  await assert.rejects(oldLeader.insert('docs', {
    id: `minority-uncommitted-${round}`, round
  }), /write_not_committed|quorum|timeout|not_leader|connection/i);

  const majority = [0, 1, 2].filter((index) => index !== leaderIndex);
  const elected = await cluster.waitForLeader(majority, 25_000);
  assert.notEqual(elected.index, leaderIndex);
  assert.ok(elected.ping.leader_term > previousTerm,
    `term did not advance: ${elected.ping.leader_term} <= ${previousTerm}`);
  const newLeader = cluster.client(elected.index, { timeoutMs: 5000 });
  newLeader.database = 'rf3db';
  await cluster.retry(() => newLeader.insert('docs', {
    id: `majority-committed-${round}`, round
  }), `majority write ${round}`, 80, 150);

  cluster.heal();
  const converged = await waitForConvergence(
    ['baseline', ...Array.from({ length: round }, (_, index) => `majority-committed-${index + 1}`)],
    elected.ping.leader_term);
  for (let index = 0; index < 3; index += 1) {
    const client = cluster.client(index, { timeoutMs: 1500 });
    client.database = 'rf3db';
    assert.equal((await client.find('docs', { id: `minority-uncommitted-${round}` })).data.length, 0);
  }
  return { isolated: leaderIndex, elected: elected.index,
    term: converged.pings[0].leader_term, convergedLeader: converged.leader,
    commitIndex: converged.pings[0].commit_index };
}

try {
  await cluster.start();
  // The engine intentionally holds elections behind a ten-second startup barrier.
  await pause(10_500);
  const initial = await cluster.waitForLeader([0, 1, 2], 10_000);
  const leader = cluster.client(initial.index, { timeoutMs: 5000 });
  await cluster.retry(() => leader.createDatabase('rf3db'), 'create database');
  leader.database = 'rf3db';
  await cluster.retry(() => leader.createCollection('docs'), 'create collection');
  await cluster.retry(() => leader.insert('docs', { id: 'baseline', value: 1 }), 'baseline insert');
  cluster.heal();
  const baseline = await waitForConvergence(['baseline'], initial.ping.leader_term);

  const rounds = [];
  rounds.push(await runIsolationRound(1, baseline.leader, baseline.pings[0].leader_term));
  rounds.push(await runIsolationRound(2, rounds[0].convergedLeader, rounds[0].term));

  console.log(JSON.stringify({
    status: 'PASS', partition_transport: 'six directed TCP proxies', rounds,
    minority_writes_acknowledged: 0, majority_writes_acknowledged: rounds.length,
    final_term: rounds.at(-1).term, final_commit_index: rounds.at(-1).commitIndex
  }, null, 2));
} finally {
  await cluster.close();
}
