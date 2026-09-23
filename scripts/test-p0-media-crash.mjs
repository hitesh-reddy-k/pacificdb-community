#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { MediaUploadError, PacificDBClient } from '../sdk/node/src/index.js';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build-p0-failpoints'));
const binary = path.join(build, process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const confirmation = 'I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE';
async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => server.once('error', reject)
    .listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}
async function crashCase(failpoint) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-p0-media-crash-'));
  const port = await freePort();
  const raftPort = await freePort();
  for (const directory of ['data', 'backup', 'restore'])
    await mkdir(path.join(root, directory), { recursive: true, mode: 0o700 });
  const baseEnv = { ...process.env, PACIFICDB_HOME: root,
    PACIFICDB_ENVIRONMENT: 'development', DATA_ROOT: path.join(root, 'data'),
    BACKUP_ROOT: path.join(root, 'backup'), RESTORE_DIR: path.join(root, 'restore'),
    ENGINE_BIND_HOST: '127.0.0.1', ENGINE_PORT: String(port), ENGINE_AUTH_REQUIRED: '0',
    RAFT_CLUSTER_ID: `p0-${failpoint.toLowerCase()}`, RAFT_NODE_ID: 'node-1',
    RAFT_LISTEN_PORT: String(raftPort), RAFT_IS_LEADER: '1', MIN_QUORUM_SIZE: '1',
    ENGINE_CPU_CORES: '2', CONN_MIN_THREADS: '2', CONN_MAX_THREADS: '8',
    ADAPTIVE_ADMISSION: '0' };
  let engine;
  let log;
  async function closeLog() {
    if (!log) return;
    const current = log;
    log = null;
    if (!current.writableFinished && !current.destroyed) {
      await new Promise((resolve) => current.end(resolve));
    }
  }
  async function start(armed) {
    log = createWriteStream(path.join(root, 'engine.log'), { flags: 'a', mode: 0o600 });
    const env = armed ? { ...baseEnv, PACIFICDB_TEST_MODE: 'media_upload_recovery',
      PACIFICDB_TEST_FAILPOINT_CONFIRM: confirmation,
      PACIFICDB_TEST_FAILPOINT: failpoint, PACIFICDB_TEST_FAILPOINT_INDEX: '1',
      PACIFICDB_TEST_FAILPOINT_ACTION: 'crash' } : baseEnv;
    engine = spawn(binary, [], { cwd: repositoryRoot, env,
      stdio: ['ignore', 'pipe', 'pipe'] });
    engine.stdout.pipe(log, { end: false });
    engine.stderr.pipe(log, { end: false });
    const probe = new PacificDBClient({ port, timeoutMs: 300 });
    for (let attempt = 0; attempt < 240; attempt += 1) {
      if (engine.exitCode !== null) throw new Error(`engine exited during startup: ${engine.exitCode}`);
      try { if ((await probe.request({ action: 'ping' })).status === 'pong') return; }
      catch {}
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    throw new Error('engine startup timed out');
  }
  async function stop() {
    if (engine && engine.exitCode === null) {
      engine.kill('SIGINT');
      const timer = setTimeout(() => engine.kill('SIGKILL'), 15000);
      await once(engine, 'exit');
      clearTimeout(timer);
    }
    await closeLog();
  }
  try {
    const source = Buffer.alloc(150_000, 73);
    const filename = path.join(root, 'crash media.bin');
    await writeFile(filename, source);
    await start(true);
    const client = new PacificDBClient({ port, timeoutMs: 10000 });
    await client.createProject('media-crash');
    await client.createDatabase('media');
    client.database = 'media';
    await client.createCollection('assets');
    let knownId = '';
    let failure;
    try { await client.uploadMediaFile('assets', filename, { chunkBytes: 65_536 }); }
    catch (error) { failure = error; knownId = error.uploadId || ''; }
    assert.ok(failure, `${failpoint} did not terminate the engine`);
    if (failpoint === 'FP_MEDIA_AFTER_MANIFEST') {
      assert.equal(failure instanceof MediaUploadError, false);
      assert.equal(knownId, '');
    } else {
      assert.ok(failure instanceof MediaUploadError);
      assert.ok(knownId.startsWith('media_'));
    }
    if (engine.exitCode === null) await once(engine, 'exit');
    assert.equal(engine.exitCode, 86);
    await closeLog();
    await start(false);
    const recovered = new PacificDBClient({ port, database: 'media', timeoutMs: 30000 });
    if (!knownId) {
      const listing = await recovered.request({ action: 'community_media_list', all: true });
      assert.equal(listing.media.length, 1);
      assert.equal(listing.media[0].status, 'uploading');
      await recovered.request({ action: 'community_media_cleanup', media_id: listing.media[0].id });
    } else {
      const ready = await recovered.uploadMediaFile('assets', filename,
        { chunkBytes: 65_536, resume: knownId });
      assert.equal(ready.id, knownId);
      assert.equal(ready.status, 'ready');
      const destination = path.join(root, 'recovered.bin');
      await recovered.downloadMediaFile(knownId, destination);
      assert.deepEqual(await readFile(destination), source);
    }
    await stop();
    return { failpoint, known_id: Boolean(knownId) };
  } finally {
    await stop().catch(() => {});
    if (process.env.PACIFICDB_KEEP_P0_ROOT !== '1')
      await rm(root, { recursive: true, force: true });
  }
}

const results = [];
for (const failpoint of ['FP_MEDIA_AFTER_MANIFEST', 'FP_MEDIA_AFTER_CHUNK',
  'FP_MEDIA_VERIFYING', 'FP_MEDIA_BEFORE_READY']) results.push(await crashCase(failpoint));
console.log(JSON.stringify({ status: 'PASS', cases: results }));
