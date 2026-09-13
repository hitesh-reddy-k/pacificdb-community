#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const cli = path.join(build, process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb');
const engine = path.join(build, process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-p0-discovery-'));
const ownedPids = new Set();
async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => server.once('error', reject)
    .listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}
function run(args, env) {
  return new Promise((resolve) => {
    const child = spawn(cli, args, { cwd: repositoryRoot, env: { ...process.env, ...env } });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk; });
    child.stderr.on('data', (chunk) => { output += chunk; });
    child.once('close', (code) => resolve({ code, output }));
  });
}
async function removeTestRoot() {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    try {
      await rm(root, { recursive: true, force: true });
      return;
    } catch (error) {
      if (!['EBUSY', 'EPERM', 'ENOTEMPTY'].includes(error.code)) throw error;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  console.error(`preserved locked test root: ${root}`);
}
async function baseEnv(home, port, raftPort) {
  await mkdir(home, { recursive: true });
  return { PACIFICDB_HOME: home, PACIFICDB_ENGINE: engine, ENGINE_PORT: String(port),
    RAFT_LISTEN_PORT: String(raftPort), PACIFICDB_STARTUP_TIMEOUT_MS: '20000' };
}
try {
  const noStartHome = path.join(root, 'no-start');
  const noStartPort = await freePort();
  const noStart = await run(['--port', String(noStartPort), '--no-start', 'ping'],
    await baseEnv(noStartHome, noStartPort, await freePort()));
  assert.notEqual(noStart.code, 0);
  assert.deepEqual(await readdir(noStartHome), []);

  const home = path.join(root, 'race');
  const port = await freePort();
  const env = await baseEnv(home, port, await freePort());
  const launches = await Promise.all(Array.from({ length: 8 }, () =>
    run(['--port', String(port), 'ping'], env)));
  for (const [index, launch] of launches.entries()) {
    assert.equal(launch.code, 0, `concurrent launcher ${index}: ${launch.output}`);
    assert.match(launch.output, /pong/);
  }
  const metadata = JSON.parse(await readFile(path.join(home, 'engine.metadata.json'), 'utf8'));
  const pid = Number((await readFile(path.join(home, 'engine.pid'), 'utf8')).trim());
  assert.equal(metadata.pid, pid);
  ownedPids.add(pid);
  const healthy = await run(['--port', String(port), '--no-start', 'ping'], env);
  assert.equal(healthy.code, 0, healthy.output);
  assert.equal(Number((await readFile(path.join(home, 'engine.pid'), 'utf8')).trim()), pid);

  const staleHome = path.join(root, 'stale');
  const stalePort = await freePort();
  const staleEnv = await baseEnv(staleHome, stalePort, await freePort());
  await writeFile(path.join(staleHome, 'engine.pid'), '99999999\n');
  const stale = await run(['--port', String(stalePort), 'ping'], staleEnv);
  assert.equal(stale.code, 0, stale.output);
  const staleReplacement = Number((await readFile(path.join(staleHome, 'engine.pid'), 'utf8')).trim());
  assert.notEqual(staleReplacement, 99999999);
  ownedPids.add(staleReplacement);

  const reusedHome = path.join(root, 'reused');
  const reusedPort = await freePort();
  const reusedEnv = await baseEnv(reusedHome, reusedPort, await freePort());
  const reusedMetadata = { schema: 'pacificdb.local-engine.v1', version: 1,
    pid: process.pid, executable: process.execPath, instance_id: 'engine_unrelated',
    data_root_fingerprint: `sha256:${'0'.repeat(64)}`,
    discovery_nonce: 'unrelated-process-nonce-0000000000000000',
    port: reusedPort, started_at_ms: Date.now() };
  await writeFile(path.join(reusedHome, 'engine.pid'), `${process.pid}\n`);
  await writeFile(path.join(reusedHome, 'engine.metadata.json'),
    JSON.stringify(reusedMetadata) + '\n');
  const reused = await run(['--port', String(reusedPort), 'ping'], reusedEnv);
  assert.notEqual(reused.code, 0);
  assert.match(reused.output, /engine_pid_reused/);
  assert.equal(JSON.parse(await readFile(path.join(reusedHome,
    'engine.metadata.json'), 'utf8')).pid, process.pid);

  const earlyHome = path.join(root, 'early-exit');
  const earlyPort = await freePort();
  const early = await run(['--port', String(earlyPort), 'ping'], {
    ...await baseEnv(earlyHome, earlyPort, await freePort()),
    RAFT_NODE_ID: '', PACIFICDB_STARTUP_TIMEOUT_MS: '5000' });
  assert.notEqual(early.code, 0);
  assert.match(early.output, /engine_exited/);
  assert.match(early.output, /exit code 78/);

  const sharedData = path.join(root, 'shared-data');
  await mkdir(sharedData, { recursive: true });
  const ownerHome = path.join(root, 'lock-owner');
  const ownerPort = await freePort();
  const ownerEnv = { ...await baseEnv(ownerHome, ownerPort, await freePort()),
    DATA_ROOT: sharedData };
  const owner = await run(['--port', String(ownerPort), 'ping'], ownerEnv);
  assert.equal(owner.code, 0, owner.output);
  ownedPids.add(Number((await readFile(path.join(ownerHome, 'engine.pid'), 'utf8')).trim()));
  const contenderPort = await freePort();
  const contender = await run(['--port', String(contenderPort), 'ping'], {
    ...await baseEnv(path.join(root, 'lock-contender'), contenderPort, await freePort()),
    DATA_ROOT: sharedData });
  assert.notEqual(contender.code, 0);
  assert.match(contender.output, /engine_data_root_in_use/);

  const failpointEngine = process.env.PACIFICDB_P0_FAILPOINT_ENGINE;
  if (failpointEngine) {
    for (const delayed of [
      { name: 'delayed-start', delay: '400', timeout: '5000', success: true },
      { name: 'live-unhealthy', delay: '10000', timeout: '400', success: false },
    ]) {
      const delayedHome = path.join(root, delayed.name);
      const delayedPort = await freePort();
      const delayedResult = await run(['--port', String(delayedPort), 'ping'], {
        ...await baseEnv(delayedHome, delayedPort, await freePort()),
        PACIFICDB_ENGINE: path.resolve(failpointEngine),
        PACIFICDB_STARTUP_TIMEOUT_MS: delayed.timeout,
        PACIFICDB_TEST_MODE: 'local_engine_startup',
        PACIFICDB_TEST_FAILPOINT_CONFIRM: 'I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE',
        PACIFICDB_TEST_FAILPOINT: 'FP_SHUTDOWN_BEFORE_LISTENER_BIND',
        PACIFICDB_TEST_FAILPOINT_INDEX: '1', PACIFICDB_TEST_FAILPOINT_ACTION: 'delay',
        PACIFICDB_TEST_STARTUP_DELAY_MS: delayed.delay,
      });
      const delayedPid = Number((await readFile(path.join(delayedHome,
        'engine.pid'), 'utf8')).trim());
      ownedPids.add(delayedPid);
      assert.equal(delayedResult.code === 0, delayed.success, delayedResult.output);
      if (!delayed.success) assert.match(delayedResult.output, /engine_unhealthy/);
    }
  }

  const occupiedPort = await freePort();
  const occupiedSockets = new Set();
  const occupied = net.createServer((socket) => {
    occupiedSockets.add(socket);
    socket.once('close', () => occupiedSockets.delete(socket));
    socket.on('error', () => {});
    socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
  });
  await new Promise((resolve) => occupied.listen(occupiedPort, '127.0.0.1', resolve));
  const occupiedResult = await run(['--port', String(occupiedPort), 'ping'],
    await baseEnv(path.join(root, 'occupied'), occupiedPort, await freePort()));
  for (const socket of occupiedSockets) socket.destroy();
  await new Promise((resolve) => occupied.close(resolve));
  assert.notEqual(occupiedResult.code, 0);
  assert.match(occupiedResult.output, /not PacificDB|non-JSON response/);
  console.log(JSON.stringify({ status: 'PASS', launchers: 8,
    scenarios: ['no_start', 'concurrent_start', 'healthy_existing', 'stale_pid',
      'reused_pid', 'early_exit', 'data_root_in_use', 'unrelated_port',
      ...(failpointEngine ? ['delayed_start', 'live_unhealthy'] : [])],
    engine_pid: pid }));
} finally {
  const cleanupPids = [...ownedPids];
  if (process.platform === 'win32') {
    for (const pid of cleanupPids)
      spawnSync('taskkill.exe', ['/PID', String(pid), '/T', '/F'], { stdio: 'ignore' });
    await new Promise((resolve) => setTimeout(resolve, 300));
    ownedPids.clear();
  } else {
    for (const pid of cleanupPids) { try { process.kill(pid, 'SIGINT'); } catch {} }
  }
  for (let attempt = 0; attempt < 150 && ownedPids.size; attempt += 1) {
    for (const pid of [...ownedPids]) {
      try { process.kill(pid, 0); }
      catch { ownedPids.delete(pid); }
    }
    if (ownedPids.size) await new Promise((resolve) => setTimeout(resolve, 100));
  }
  if (process.platform !== 'win32') {
    for (const pid of cleanupPids) { try { process.kill(pid, 'SIGKILL'); } catch {} }
  }
  for (let attempt = 0; attempt < 50 && ownedPids.size; attempt += 1) {
    for (const pid of [...ownedPids]) {
      try { process.kill(pid, 0); }
      catch { ownedPids.delete(pid); }
    }
    if (ownedPids.size) await new Promise((resolve) => setTimeout(resolve, 100));
  }
  if (process.env.PACIFICDB_KEEP_P0_ROOT !== '1') {
    await removeTestRoot();
  } else {
    console.error(`preserved test root: ${root}`);
  }
}
