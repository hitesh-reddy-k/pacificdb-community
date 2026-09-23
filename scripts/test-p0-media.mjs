#!/usr/bin/env node

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const engineBinary = path.join(build, process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
const cliBinary = path.join(build, process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-p0-media-'));
const chunk = 256 * 1024;
const sizes = [0, 1, chunk - 1, chunk, chunk + 1, 2 * chunk - 1,
  2 * chunk, 2 * chunk + 1, 1024 * 1024, 4 * 1024 * 1024, 10 * 1024 * 1024];
if (process.env.PACIFICDB_P0_MEDIA_100MB === '1') sizes.push(100 * 1024 * 1024);
const names = ['space name.png', 'తెలుగు-video.mp4', 'clip (final).mp4',
  `${'a'.repeat(96)}.bin`, 'archive.part.001.media.bin'];
async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => server.once('error', reject)
    .listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}
const port = await freePort();
const raftPort = await freePort();
for (const directory of ['data', 'backup', 'restore', 'files', 'downloads'])
  await mkdir(path.join(root, directory), { recursive: true, mode: 0o700 });
const env = { ...process.env, PACIFICDB_HOME: root, PACIFICDB_ENVIRONMENT: 'development',
  DATA_ROOT: path.join(root, 'data'), BACKUP_ROOT: path.join(root, 'backup'),
  RESTORE_DIR: path.join(root, 'restore'), ENGINE_BIND_HOST: '127.0.0.1',
  ENGINE_PORT: String(port), ENGINE_AUTH_REQUIRED: '0', RAFT_CLUSTER_ID: 'p0-media',
  RAFT_NODE_ID: 'node-1', RAFT_LISTEN_PORT: String(raftPort), RAFT_IS_LEADER: '1',
  MIN_QUORUM_SIZE: '1', ENGINE_CPU_CORES: '2', CONN_MIN_THREADS: '2',
  CONN_MAX_THREADS: '12', ADAPTIVE_ADMISSION: '0' };
let engine;
let log;
let pipeError;
async function start() {
  pipeError = undefined;
  log = createWriteStream(path.join(root, 'engine.log'), { flags: 'a', mode: 0o600 });
  engine = spawn(engineBinary, [], { cwd: repositoryRoot, env,
    stdio: ['ignore', 'pipe', 'pipe'] });
  engine.stdout.pipe(log, { end: false });
  engine.stderr.pipe(log, { end: false });
  if (process.platform === 'win32') {
    engine.stdout.on('error', (error) => {
      if (error.code !== 'ECONNRESET') pipeError ||= error;
    });
    engine.stderr.on('error', (error) => {
      if (error.code !== 'ECONNRESET') pipeError ||= error;
    });
  }
  const probe = new PacificDBClient({ port, timeoutMs: 500 });
  for (let attempt = 0; attempt < 240; attempt += 1) {
    if (pipeError) throw pipeError;
    if (engine.exitCode !== null) throw new Error(`engine exited with ${engine.exitCode}`);
    try { if ((await probe.request({ action: 'ping' })).status === 'pong') return; }
    catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('engine startup timed out');
}
async function stop(signal = 'SIGINT') {
  if (!engine || engine.exitCode !== null) return;
  if (signal === 'SIGKILL') {
    engine.kill('SIGKILL');
  } else {
    const stopper = spawn(cliBinary, ['--port', String(port), 'stop'], {
      cwd: repositoryRoot, env, stdio: 'ignore',
    });
    const [code] = await once(stopper, 'exit');
    if (code !== 0) throw new Error(`pacificdb stop exited ${code}`);
  }
  if (engine.exitCode === null) {
    const timer = setTimeout(() => engine.kill('SIGKILL'), 35000);
    await once(engine, 'exit');
    clearTimeout(timer);
  }
  log.end();
  await once(log, 'finish');
  if (pipeError) throw pipeError;
}
function bytesFor(size) {
  const value = Buffer.allocUnsafe(size);
  for (let index = 0; index < size; index += 1) value[index] = (index * 31 + size) & 255;
  return value;
}
const uploaded = [];
try {
  await start();
  const client = new PacificDBClient({ port, timeoutMs: 120000 });
  await client.createProject('media-test');
  await client.createDatabase('media');
  client.database = 'media';
  await client.createCollection('assets');
  for (let caseIndex = 0; caseIndex < sizes.length; caseIndex += 1) {
    const size = sizes[caseIndex];
    const filename = `${String(caseIndex).padStart(2, '0')}-${names[caseIndex % names.length]}`;
    const sourcePath = path.join(root, 'files', filename);
    const source = bytesFor(size);
    await writeFile(sourcePath, source);
    if (size === 0) {
      await assert.rejects(client.uploadMediaFile('assets', sourcePath),
        (error) => error.code === 'media_file_empty');
      continue;
    }
    const manifest = await client.uploadMediaFile('assets', sourcePath, { chunkBytes: chunk });
    assert.equal(manifest.status, 'ready');
    const destination = path.join(root, 'downloads', `first-${caseIndex}.bin`);
    await client.downloadMediaFile(manifest.id, destination);
    assert.deepEqual(await readFile(destination), source);
    uploaded.push({ id: manifest.id, size, sha256: createHash('sha256').update(source).digest('hex') });
  }
  const listed = await client.request({ action: 'community_media_list', all: true });
  assert.equal(listed.media.filter((item) => item.status === 'ready').length, uploaded.length);
  await stop('SIGKILL');
  await start();
  const recovered = new PacificDBClient({ port, database: 'media', timeoutMs: 120000 });
  for (let index = 0; index < uploaded.length; index += 1) {
    const destination = path.join(root, 'downloads', `recovered-${index}.bin`);
    const result = await recovered.downloadMediaFile(uploaded[index].id, destination);
    assert.equal(result.sizeBytes, uploaded[index].size);
    assert.equal(result.sha256, uploaded[index].sha256);
  }
  console.log(JSON.stringify({ status: 'PASS', accepted_cases: uploaded.length,
    rejected_empty: 1, maximum_bytes: Math.max(...sizes), restart: 'abrupt',
    filenames: names.length }));
} finally {
  await stop().catch(() => {});
  if (process.env.PACIFICDB_KEEP_P0_ROOT !== '1')
    await rm(root, { recursive: true, force: true });
  else console.error(`preserved test root: ${root}`);
}
