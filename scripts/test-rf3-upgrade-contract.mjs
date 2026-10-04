#!/usr/bin/env node

import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { chmod, cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { promisify } from 'node:util';
import { RaftTestCluster } from './lib/raft-test-cluster.mjs';
import { rollbackDecision } from './lib/upgrade-qualification.mjs';

const execFileAsync = promisify(execFile);

test('upgrade artifact intake accepts files and build directories and rejects nonexecutables',
  { skip: process.platform === 'win32' }, async (t) => {
    const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-upgrade-intake-'));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const executable = path.join(directory, 'db_engine');
    await writeFile(executable, '#!/usr/bin/env node\nconsole.log("fixture engine 1.1.1");\n', { mode: 0o700 });
    const script = path.resolve('scripts/test-community-rf3-upgrade.mjs');
    const evidencePath = path.join(directory, 'evidence.json');
    for (const selected of [executable, directory]) {
      await assert.rejects(execFileAsync(process.execPath, [script, '--old-build', selected,
        '--candidate-build', selected, '--evidence', evidencePath]), (error) => error.code === 2);
      const evidence = JSON.parse(await readFile(evidencePath, 'utf8'));
      assert.equal(evidence.status, 'BLOCKED');
      assert.equal(evidence.blocker, 'distinct_artifacts_required');
      assert.equal(evidence.artifacts.old.path, executable);
      assert.equal(evidence.artifacts.old.version, 'fixture engine 1.1.1');
    }
    if (process.getuid() !== 0) {
      const traversalOnly = path.join(directory, 'traversal-only');
      await mkdir(traversalOnly);
      await cp(executable, path.join(traversalOnly, 'db_engine'));
      await chmod(traversalOnly, 0o111);
      try {
        await assert.rejects(execFileAsync(process.execPath, [script, '--old-build', traversalOnly,
          '--candidate-build', traversalOnly, '--evidence', evidencePath]), (error) => error.code === 2);
        assert.equal(JSON.parse(await readFile(evidencePath, 'utf8')).artifacts.old.version, 'fixture engine 1.1.1');
      } finally {
        await chmod(traversalOnly, 0o700);
      }
    }
    const nonexecutable = path.join(directory, 'nonexecutable');
    await writeFile(nonexecutable, 'not executable', { mode: 0o600 });
    await assert.rejects(execFileAsync(process.execPath, [script, '--old-build', nonexecutable,
      '--candidate-build', executable, '--evidence', evidencePath]), (error) => error.code === 1);
    const evidence = JSON.parse(await readFile(evidencePath, 'utf8'));
    assert.equal(evidence.status, 'FAIL');
    assert.match(evidence.error.message, /EACCES/);
  });

async function fakeEngine(root, name) {
  const executable = path.join(root, name);
  await writeFile(executable, `#!/usr/bin/env node
import net from 'node:net';
import { appendFileSync } from 'node:fs';
appendFileSync(process.env.FAKE_ENGINE_LOG,
  JSON.stringify({ executable: process.argv[1], dataRoot: process.env.DATA_ROOT,
    enginePort: process.env.ENGINE_PORT, raftPort: process.env.RAFT_LISTEN_PORT }) + '\\n');
const server = net.createServer((socket) => socket.once('data', () =>
  socket.end(JSON.stringify({ status: 'pong' }) + '\\n')));
server.listen(Number(process.env.ENGINE_PORT), process.env.ENGINE_BIND_HOST);
process.on('SIGINT', () => server.close(() => process.exit(0)));
`);
  await chmod(executable, 0o755);
  return executable;
}

test('restartNode swaps one binary while preserving node identity and storage', async (t) => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-upgrade-contract-'));
  const log = path.join(directory, 'engines.jsonl');
  const oldBinary = await fakeEngine(directory, 'old-engine');
  const candidateBinary = await fakeEngine(directory, 'candidate-engine');
  const previousLog = process.env.FAKE_ENGINE_LOG;
  process.env.FAKE_ENGINE_LOG = log;
  const cluster = await RaftTestCluster.create({
    binaries: [oldBinary, oldBinary, oldBinary], useProxies: false,
    rootPrefix: 'pacificdb-upgrade-contract-cluster-'
  });
  t.after(async () => {
    await cluster.close();
    if (previousLog === undefined) delete process.env.FAKE_ENGINE_LOG;
    else process.env.FAKE_ENGINE_LOG = previousLog;
    await rm(directory, { recursive: true, force: true });
  });
  await cluster.startNode(2);
  const before = cluster.nodes[2].environment;
  await cluster.restartNode(2, candidateBinary);
  const after = cluster.nodes[2].environment;
  const launches = (await readFile(log, 'utf8')).trim().split('\n').map(JSON.parse);
  assert.deepEqual(launches.map(({ executable }) => executable),
    [oldBinary, candidateBinary]);
  assert.equal(after.DATA_ROOT, before.DATA_ROOT);
  assert.equal(after.ENGINE_PORT, before.ENGINE_PORT);
  assert.equal(after.RAFT_LISTEN_PORT, before.RAFT_LISTEN_PORT);
});

test('upgrade rollback decisions fail closed at format boundaries', () => {
  assert.equal(rollbackDecision({ interrupted: true, formatTransition: false,
    oldArtifactAvailable: true }), 'RESUMABLE');
  assert.equal(rollbackDecision({ incompatible: true,
    oldArtifactAvailable: true }), 'REJECTED_SAFE');
  assert.equal(rollbackDecision({ formatTransition: true,
    oldArtifactAvailable: true }), 'RESTORE_REQUIRED');
  assert.equal(rollbackDecision({ formatTransition: false,
    oldArtifactAvailable: false }), 'BLOCKED');
});

test('RF3 fixture can service its persistent-client capacity without socket starvation', async (t) => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-rf3-capacity-contract-'));
  const binary = await fakeEngine(directory, 'capacity-engine');
  const cluster = await RaftTestCluster.create({
    binaries: [binary, binary, binary], useProxies: false,
    rootPrefix: 'pacificdb-rf3-capacity-contract-cluster-'
  });
  t.after(async () => {
    await cluster.close();
    await rm(directory, { recursive: true, force: true });
  });
  const environment = cluster.nodeEnvironment(0);
  assert.ok(Number(environment.CONN_MAX_THREADS) >= 130);
  assert.ok(Number(environment.CONN_MIN_THREADS) >= 8);
});
