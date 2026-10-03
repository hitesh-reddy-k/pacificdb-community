#!/usr/bin/env node
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, readFile, writeFile, rm, stat } from 'node:fs/promises';
import net from 'node:net';
import tls from 'node:tls';
import os from 'node:os';
import path from 'node:path';
import { PacificDB } from '../sdk/node/src/index.js';

const build = path.resolve(process.argv[2] || 'build');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-direct-cli-e2e-'));
const commands = [[path.join(build, process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb'), []],
  [process.execPath, [path.resolve('cli/bin/pacificdb.js')]]];
const sockets = new Set(), servers = new Set();
let engine;
async function run(command, args, input = '', env = {}) {
  const childEnv = {
    ...process.env, PACIFICDB_URL: '', PACIFICDB_CLI_HOME: path.join(root, 'context'),
    PACIFICDB_HOME: path.join(root, 'managed'), ...env,
  };
  if (childEnv.PACIFICDB_URL === '') delete childEnv.PACIFICDB_URL;
  const child = spawn(command[0], [...command[1], ...args], { env: childEnv, stdio: ['pipe', 'pipe', 'pipe'] });
  let output = ''; child.stdout.on('data', b => { output += b; }); child.stderr.on('data', b => { output += b; });
  const timer = setTimeout(() => child.kill('SIGKILL'), 10000);
  child.stdin.end(input);
  const [code, signal] = await once(child, 'exit'); clearTimeout(timer);
  assert.equal(signal, null, output);
  return { code, output };
}
async function peer(handler, secure = null) {
  const frames = [];
  const listener = socket => {
    sockets.add(socket); socket.on('close', () => sockets.delete(socket)); socket.on('error', () => {});
    let wire = '';
    socket.on('data', async b => {
      wire += b;
      while (wire.includes('\n')) {
        const index = wire.indexOf('\n'), frame = JSON.parse(wire.slice(0, index)); wire = wire.slice(index+1);
        frames.push(frame);
        const result = await handler(frame);
        if (result !== undefined && !socket.destroyed) socket.write(JSON.stringify(result)+'\n');
      }
    });
  };
  const server = secure ? tls.createServer(secure, listener) : net.createServer(listener);
  server.on('tlsClientError', () => {});
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); servers.add(server);
  return { port: server.address().port, frames };
}
async function freePort() {
  const s = net.createServer(); await new Promise(r => s.listen(0, '127.0.0.1', r));
  const port = s.address().port; await new Promise(r => s.close(r)); return port;
}
try {
  const p = await peer(frame => {
    if (frame.action === 'listDatabases') return ['app'];
    if (frame.action === 'dropDatabase' || frame.dbName === 'denied') return { error: 'permission_denied' };
    if (frame.action === 'security_authenticate') return { token: 'dummy-token' };
    return { status: 'ok' };
  });
  for (const [i, command] of commands.entries()) {
    const home = path.join(root, `context-${i}`); await mkdir(home);
    await writeFile(path.join(home, 'context.json'), JSON.stringify({ database: 'app', projectId: 'legacy', token: 'old-secret' }));
    const start = p.frames.length;
    const r = await run(command, ['shell', '--port', String(p.port), '--no-start'],
      'create collection users\nfind users {}\ncreate database denied\ndrop database app\ncontext show\nuse missing\nrequest {"action":"community_project_list"}\nlist projects\npacificdb://demo:dummy-secret@localhost/app\nrequest {"token":"dummy-secret", nope}\nexit\n',
      { PACIFICDB_CLI_HOME: home, PACIFICDB_URL: '' });
    assert.equal(r.code, 0, r.output);
    assert.deepEqual(p.frames.slice(start).map(f => f.action), ['listDatabases', 'createCollection', 'find', 'createDatabase', 'dropDatabase', 'listDatabases', 'community_project_list']);
    assert.ok(p.frames.slice(start).every(f => !('project_id' in f)));
    assert.match(r.output, /permission_denied/); assert.match(r.output, /database_not_found/);
    assert.match(r.output, /unknown command/); assert.doesNotMatch(r.output, /old-secret|dummy-secret|"projectId"/);
    assert.deepEqual(JSON.parse(await readFile(path.join(home, 'context.json'))), { database: 'app' });
    assert.doesNotMatch(await readFile(path.join(home, 'history'), 'utf8'), /old-secret|dummy-secret/);
    if (process.platform !== 'win32') assert.equal((await stat(path.join(home, 'context.json'))).mode & 0o777, 0o600);
    const url = `pacificdb://demo:dummy-secret@127.0.0.1:${p.port}/app`;
    const before = p.frames.length;
    const auth = await run(command, ['ping', '--url', url, '--no-start']);
    assert.equal(auth.code, 0, auth.output);
    assert.deepEqual(p.frames.slice(before).map(f => f.action), ['security_authenticate', 'ping']);
    assert.equal(p.frames.at(-1).token, 'dummy-token'); assert.equal(p.frames.at(-1).dbName, 'app');
    assert.doesNotMatch(auth.output, /dummy-secret|dummy-token/);
    const short = await run(command, ['ping', '--url', `pacificdb://demo:a@127.0.0.1:${p.port}/app`, '--no-start']);
    assert.equal(short.code, 0, short.output);
    assert.equal(JSON.parse(short.output).status, 'ok', 'successful responses must retain field names and data');
    const afterShort = p.frames.length;
    const bad = await run(command, ['ping', '--url', url, '--database', 'other']);
    assert.equal(bad.code, 1); assert.match(bad.output, /invalid_connection_url/);
    assert.equal(p.frames.length, afterShort);
    for (const flag of ['--url', '--port', '--host', '--database']) {
      const missing = await run(command, ['ping', flag]); assert.equal(missing.code, 1); assert.match(missing.output, /requires a value/);
    }
  }
  const privatePeer = await peer(frame => ({error: 'permission_denied',
    message: frame.password+' '+frame.token,
    nested: {authorization: 'server-private-authorization', password: 'server-private-password'}}));
  for (const command of commands) {
    const failure = await run(command, ['request', JSON.stringify({action: 'security_authenticate',
      username: 'demo', password: 'raw-secret-password', token: 'raw-secret-token'}),
      '--port', String(privatePeer.port), '--no-start']);
    assert.equal(failure.code, 1);
    assert.doesNotMatch(failure.output, /raw-secret-password|raw-secret-token|server-private/);
    assert.match(failure.output, /permission_denied/);
  }
  const key = path.join(root, 'key.pem'), cert = path.join(root, 'cert.pem');
  execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
    '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'], { stdio: 'ignore' });
  const secure = await peer(() => ({ status: 'pong' }), { key: await readFile(key), cert: await readFile(cert) });
  const stalled = await peer(() => undefined);
  const handshakeStall = net.createServer(socket => {
    sockets.add(socket); socket.on('close', () => sockets.delete(socket)); socket.on('error', () => {});
  });
  await new Promise(r => handshakeStall.listen(0, '127.0.0.1', r)); servers.add(handshakeStall);
  for (const command of commands) {
    const trusted = await run(command, ['ping', '--url', `pacificdbs://localhost:${secure.port}/app?caFile=${encodeURIComponent(cert)}`]);
    assert.equal(trusted.code, 0, trusted.output); assert.match(trusted.output, /pong/);
    const before = secure.frames.length;
    const wrong = await run(command, ['ping', '--url', `pacificdbs://demo:dummy-secret@127.0.0.1:${secure.port}/app?caFile=${encodeURIComponent(cert)}`]);
    assert.equal(wrong.code, 1); assert.equal(secure.frames.length, before); assert.doesNotMatch(wrong.output, /dummy-secret/);
    const untrusted = await run(command, ['ping', '--url', `pacificdbs://localhost:${secure.port}/app`]);
    assert.equal(untrusted.code, 1); assert.equal(secure.frames.length, before);
    const start = Date.now();
    const timeout = await run(command, ['ping', '--url', `pacificdb://127.0.0.1:${stalled.port}/app?timeoutMs=50`, '--no-start']);
    assert.equal(timeout.code, 1); assert.ok(Date.now()-start < 3000);
    const handshake = await run(command, ['ping', '--url', `pacificdbs://127.0.0.1:${handshakeStall.address().port}/app?timeoutMs=50`]);
    assert.equal(handshake.code, 1);
  }
  // Engine-backed CRUD and actual process restart, separate from protocol peers.
  const port = await freePort(), raft = await freePort();
  for (const dir of ['data', 'backup', 'restore']) await mkdir(path.join(root, dir));
  const env = { ...process.env, PACIFICDB_ENVIRONMENT: 'development', ENGINE_CPU_CORES: '2', ENGINE_AUTH_REQUIRED: '0',
    ENGINE_BIND_HOST: '127.0.0.1', ENGINE_PORT: String(port), RAFT_LISTEN_PORT: String(raft),
    RAFT_IS_LEADER: '1', RAFT_NODE_ID: 'node-1', RAFT_CLUSTER_ID: 'database-first-test',
    MIN_QUORUM_SIZE: '1', DATA_ROOT: path.join(root, 'data'),
    BACKUP_ROOT: path.join(root, 'backup'), RESTORE_DIR: path.join(root, 'restore') };
  const startEngine = async () => {
    let diagnostics = '';
    engine = spawn(path.join(build, process.platform === 'win32' ? 'db_engine.exe' : 'db_engine'), [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    engine.stdout.on('data', b => { diagnostics = (diagnostics + b).slice(-4000); });
    engine.stderr.on('data', b => { diagnostics = (diagnostics + b).slice(-4000); });
    const client = new PacificDB({ port, timeoutMs: 200, poolSize: 1 });
    try {
      for (let i = 0; i < 200; ++i) {
        try { if ((await client.request({ action: 'ping' })).status === 'pong') return; } catch {}
        if (engine.exitCode !== null) throw new Error(`engine exited (${engine.exitCode}): ${diagnostics}`);
        await new Promise(r => setTimeout(r, 100));
      }
      throw new Error('engine startup timeout');
    } finally { client.close(); }
  };
  await startEngine();
  for (const [i, command] of commands.entries()) {
    const r = await run(command, ['shell', '--port', String(port), '--no-start'],
      `create database app${i}\ncreate collection users\ninsert users {"id":"ada","name":"Ada"}\nfind users {"id":"ada"}\nexit\n`,
      { PACIFICDB_CLI_HOME: path.join(root, `live-${i}`), PACIFICDB_URL: '' });
    assert.equal(r.code, 0, r.output); assert.doesNotMatch(r.output, /error:/); assert.match(r.output, /Ada/);
  }
  const exit = once(engine, 'exit'); engine.kill('SIGKILL'); await exit;
  await startEngine();
  for (const [i, command] of commands.entries()) {
    const r = await run(command, ['request', '{"action":"find","collection":"users","filter":{"id":"ada"}}',
      '--url', `pacificdb://127.0.0.1:${port}/app${i}`, '--no-start']);
    assert.equal(r.code, 0, r.output); assert.match(r.output, /Ada/);
  }
  console.log('DATABASE_FIRST_CLI_TCP_TLS_CONTEXT_CRUD_RESTART_PASS');
} finally {
  if (engine && engine.exitCode === null) { const exit = once(engine, 'exit'); engine.kill('SIGKILL'); await exit; }
  for (const s of sockets) s.destroy();
  await Promise.all([...servers].map(s => new Promise(r => s.close(r))));
  await rm(root, { recursive: true, force: true });
}
