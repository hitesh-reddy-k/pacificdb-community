#!/usr/bin/env node

import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { constants } from 'node:fs';
import { access, mkdir, readFile, rename, stat, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import { RaftTestCluster, pause } from './lib/raft-test-cluster.mjs';
import { rollbackDecision } from './lib/upgrade-qualification.mjs';

const execFileAsync = promisify(execFile);
const repositoryRoot = path.resolve(import.meta.dirname, '..');

function argumentsFrom(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    const option = argv[index];
    if (!option?.startsWith('--') || argv[index + 1] === undefined) {
      throw new Error(`invalid argument near ${option || '<end>'}`);
    }
    values[option.slice(2)] = argv[index + 1];
  }
  for (const required of ['old-build', 'candidate-build', 'evidence']) {
    if (!values[required]) throw new Error(`--${required} is required`);
  }
  return values;
}

async function resolveEngine(value) {
  const selected = path.resolve(value);
  const details = await stat(selected);
  const executable = details.isDirectory() ? path.join(selected, 'db_engine') : selected;
  await access(executable, constants.X_OK);
  const artifact = await readFile(executable);
  let version = 'unavailable';
  try {
    ({ stdout: version } = await execFileAsync(executable, ['--version'], { timeout: 5000 }));
    version = version.trim();
  } catch (error) {
    version = `unavailable: ${error.message}`;
  }
  return { path: executable,
    sha256: createHash('sha256').update(artifact).digest('hex'), version };
}

async function writeEvidence(filename, evidence) {
  const destination = path.resolve(filename);
  await mkdir(path.dirname(destination), { recursive: true });
  const temporary = `${destination}.tmp-${process.pid}`;
  await writeFile(temporary, `${JSON.stringify(evidence, null, 2)}\n`, { mode: 0o600 });
  await rename(temporary, destination);
}

async function main() {
  const startedAt = new Date().toISOString();
  let options;
  let oldArtifact;
  let candidateArtifact;
  let cluster;
  const evidence = {
    schema_version: 1, qualification: 'mixed-version-rf3-upgrade',
    status: 'FAIL', started_at: startedAt, steps: [],
    linked_qualifications: {
      partition: 'scripts/test-community-rf3-partition.mjs',
      disk_full: 'scripts/test-community-disk-full.sh',
      note: 'Reused certification harnesses; their separate exact-candidate evidence is required.'
    },
    rollback_policy: {
      interrupted_pre_format: rollbackDecision({ interrupted: true, oldArtifactAvailable: true }),
      incompatible_format: rollbackDecision({ incompatible: true, oldArtifactAvailable: true }),
      post_format_transition: rollbackDecision({ formatTransition: true, oldArtifactAvailable: true }),
      missing_old_artifact: rollbackDecision({ oldArtifactAvailable: false })
    }
  };

  try {
    options = argumentsFrom(process.argv.slice(2));
    oldArtifact = await resolveEngine(options['old-build']);
    candidateArtifact = await resolveEngine(options['candidate-build']);
    evidence.artifacts = { old: oldArtifact, candidate: candidateArtifact };
    if (oldArtifact.sha256 === candidateArtifact.sha256) {
      evidence.status = 'BLOCKED';
      evidence.blocker = 'distinct_artifacts_required';
      process.exitCode = 2;
      return;
    }

    cluster = await RaftTestCluster.create({
      binaries: [oldArtifact.path, oldArtifact.path, oldArtifact.path],
      clusterId: `community-upgrade-${process.pid}-${Date.now()}`,
      useProxies: true, allEligible: true, rootPrefix: 'pacificdb-rf3-upgrade-'
    });
    await cluster.start();
    await pause(10_500);
    const initial = await cluster.waitForLeader([0, 1, 2], 15_000);
    const database = 'upgrade_qualification';
    const collection = 'documents';
    const acknowledgedIds = [];

    const leaderClient = async () => {
      const leader = await cluster.waitForLeader([0, 1, 2], 20_000);
      const client = cluster.client(leader.index, { timeoutMs: 8000 });
      client.database = database;
      return { ...leader, client };
    };
    const insertBatch = async (prefix, count) => {
      const { client } = await leaderClient();
      const documents = Array.from({ length: count }, (_, index) => ({
        id: `${prefix}-${index}`, phase: prefix, value: index
      }));
      await cluster.retry(() => client.insertMany(collection, documents),
        `insert ${prefix}`, 80, 150);
      acknowledgedIds.push(...documents.map(({ id }) => id));
      client.close();
    };
    const verifyConvergence = async (label) => {
      let last;
      for (let attempt = 0; attempt < 180; attempt += 1) {
        try {
          const pings = await cluster.pings();
          assert.equal(pings.filter((ping) => ping.isLeader).length, 1);
          assert.ok(pings.every((ping) => !ping.error));
          assert.ok(pings.every((ping) => ping.commit_index === pings[0].commit_index));
          assert.ok(pings.every((ping) => ping.last_applied === ping.commit_index));
          for (let index = 0; index < 3; index += 1) {
            const client = cluster.client(index, { timeoutMs: 3000 });
            client.database = database;
            for (const id of acknowledgedIds) {
              assert.equal((await client.find(collection, { id })).count, 1,
                `${label}: node ${index + 1} missing ${id}`);
            }
            client.close();
          }
          const result = { step: label, status: 'PASS',
            term: pings[0].leader_term, commit_index: pings[0].commit_index,
            acknowledged_ids: acknowledgedIds.length };
          evidence.steps.push(result);
          return result;
        } catch (error) {
          last = error;
          await pause(200);
        }
      }
      throw new Error(`${label} convergence failed: ${last?.message}`);
    };

    const bootstrap = cluster.client(initial.index, { timeoutMs: 8000 });
    await cluster.retry(() => bootstrap.createDatabase(database), 'create upgrade database');
    bootstrap.database = database;
    await cluster.retry(() => bootstrap.createCollection(collection), 'create upgrade collection');
    bootstrap.close();
    await insertBatch('before-upgrade', 12);
    await verifyConvergence('old-cluster-baseline');

    const { client: backupClient, index: backupLeader } = await leaderClient();
    const backup = await backupClient.request({ action: 'create_backup',
      description: 'pre mixed-version upgrade' });
    await backupClient.request({ action: 'verify_backup', backup_id: backup.backup_id });
    const isolatedRestore = path.join(cluster.nodes[backupLeader].environment.RESTORE_DIR,
      'isolated-pre-upgrade-restore');
    const restored = await backupClient.request({ action: 'restore_backup',
      backup_id: backup.backup_id, target_dir: isolatedRestore,
      target_cluster_id: `${cluster.clusterId}-restore`, target_node_id: 'restore-node-1' });
    backupClient.close();
    assert.equal(restored.target_dir, isolatedRestore);
    assert.ok((await stat(isolatedRestore)).isDirectory());
    evidence.backup = { backup_id: backup.backup_id, verified: true,
      isolated_restore: isolatedRestore, isolated_restore_status: 'PASS' };

    const originalLeader = (await cluster.waitForLeader()).index;
    const followers = [0, 1, 2].filter((index) => index !== originalLeader);
    for (let position = 0; position < followers.length; position += 1) {
      const index = followers[position];
      await cluster.restartNode(index, candidateArtifact.path);
      await verifyConvergence(`upgrade-follower-${index + 1}`);
      if (position === 0) {
        await cluster.stopNode(index, 'SIGKILL');
        await cluster.startNode(index);
        await verifyConvergence(`resume-interrupted-follower-${index + 1}`);
        evidence.interrupted_upgrade = { node: index + 1, signal: 'SIGKILL',
          decision: 'RESUMABLE', status: 'PASS' };
      }
      await insertBatch(`after-follower-${index + 1}`, 4);
      await verifyConvergence(`write-after-follower-${index + 1}`);
    }

    await cluster.stopNode(originalLeader, 'SIGKILL');
    const replacement = await cluster.waitForLeader(followers, 45_000, [originalLeader]);
    const serving = cluster.client(replacement.index, { timeoutMs: 8000 });
    serving.database = database;
    const leaderOutId = 'served-while-old-leader-offline';
    await cluster.retry(() => serving.insert(collection, { id: leaderOutId, phase: 'leader-offline' }),
      'write while old leader offline', 80, 150);
    acknowledgedIds.push(leaderOutId);
    serving.close();
    cluster.binaries[originalLeader] = candidateArtifact.path;
    await cluster.startNode(originalLeader);
    await verifyConvergence(`upgrade-final-node-${originalLeader + 1}`);

    await insertBatch('after-full-upgrade', 8);
    await verifyConvergence('all-candidate-cluster');
    await Promise.all([0, 1, 2].map((index) => cluster.stopNode(index)));
    await Promise.all([cluster.startNode(1), cluster.startNode(2)]);
    await cluster.startNode(0);
    await pause(10_500);
    await cluster.waitForLeader([0, 1, 2], 15_000);
    await verifyConvergence('full-candidate-restart');

    evidence.node_upgrade_order = [...followers, originalLeader].map((index) => index + 1);
    evidence.acknowledged_ids = acknowledgedIds;
    evidence.acknowledged_count = acknowledgedIds.length;
    evidence.format_transition = 'none_declared_for_beta14_to_candidate';
    evidence.status = 'PASS';
  } catch (error) {
    evidence.error = { name: error.name, message: error.message, stack: error.stack };
    process.exitCode = 1;
  } finally {
    evidence.finished_at = new Date().toISOString();
    if (cluster) await cluster.close();
    const destination = options?.evidence || process.argv[process.argv.indexOf('--evidence') + 1];
    if (destination) await writeEvidence(destination, evidence);
    console.log(JSON.stringify(evidence, null, 2));
  }
}

await main();
