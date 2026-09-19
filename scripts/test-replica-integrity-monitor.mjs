#!/usr/bin/env node

import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '..');
const monitor = path.join(import.meta.dirname, 'replica-integrity-monitor.mjs');

async function fakeNode(handler) {
  const sockets = new Set();
  const server = net.createServer((socket) => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    let pending = '';
    socket.on('data', (chunk) => {
      pending += chunk;
      while (pending.includes('\n')) {
        const newline = pending.indexOf('\n');
        const request = JSON.parse(pending.slice(0, newline));
        pending = pending.slice(newline + 1);
        const response = handler(request);
        if (response !== undefined) socket.write(`${JSON.stringify(response)}\n`);
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    node: { id: `node-${server.address().port}`, host: '127.0.0.1',
      port: server.address().port },
    close: async () => {
      for (const socket of sockets) socket.destroy();
      await new Promise((resolve) => server.close(resolve));
    },
  };
}

function healthyHandler({ clusterId = 'cluster-a', lastApplied = 12,
  digest = 'a'.repeat(64), schema = 'pacificdb-logical-v1' } = {}) {
  return (request) => {
    if (request.action === 'admin_raft_status') {
      return { ok: true, clusterId, nodeId: `n-${lastApplied}`, term: 4,
        commitIndex: lastApplied, lastApplied, recoveryComplete: true };
    }
    if (request.action === 'admin_replica_digest') {
      return { ok: true, clusterId, nodeId: `n-${lastApplied}`, term: 4,
        commitIndex: lastApplied, lastApplied, fence: request.fence,
        digestSchema: schema, digest, documentCount: 2,
        bounded: true, truncated: false };
    }
    return { error: 'unknown_action' };
  };
}

async function runCase(t, handlers, overrides = {}) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-integrity-monitor-'));
  const servers = await Promise.all(handlers.map((handler) => fakeNode(handler)));
  t.after(async () => {
    await Promise.all(servers.map((server) => server.close()));
    await rm(directory, { recursive: true, force: true });
  });
  const output = path.join(directory, 'result.json');
  const prometheus = path.join(directory, 'result.prom');
  const config = path.join(directory, 'config.json');
  await writeFile(config, JSON.stringify({
    userId: 'integrity-user', database: 'app', collection: 'docs',
    maxDocs: 1000, token: 'top-secret-token', timeoutMs: 250,
    lagTimeoutMs: 50, pollIntervalMs: 10,
    nodes: servers.map(({ node }) => node), ...overrides,
  }));
  const child = spawn(process.execPath,
    [monitor, '--config', config, '--output', output, '--prometheus', prometheus],
    { cwd: root });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  const exitCode = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', resolve);
  });
  const result = JSON.parse(await readFile(output, 'utf8'));
  const metrics = await readFile(prometheus, 'utf8');
  assert.equal(`${stdout}${stderr}${JSON.stringify(result)}${metrics}`
    .includes('top-secret-token'), false);
  return { exitCode, result, metrics };
}

test('reports CONSISTENT for equal canonical digests', async (t) => {
  const observed = await runCase(t, [healthyHandler(), healthyHandler(), healthyHandler()]);
  assert.equal(observed.exitCode, 0);
  assert.equal(observed.result.status, 'CONSISTENT');
  assert.equal(observed.result.fence, 12);
  assert.equal(observed.result.nodes.length, 3);
  assert.match(observed.metrics, /pacificdb_replica_integrity_state\{state="consistent"\} 1/);
  assert.match(observed.metrics,
    /pacificdb_replica_integrity_last_success_timestamp_seconds [1-9][0-9]*/);
  assert.match(observed.metrics, /pacificdb_replica_integrity_failures_total 0/);
});

test('reports DIVERGENT without document contents', async (t) => {
  const observed = await runCase(t, [healthyHandler(), healthyHandler(),
    healthyHandler({ digest: 'b'.repeat(64) })]);
  assert.equal(observed.exitCode, 2);
  assert.equal(observed.result.status, 'DIVERGENT');
  assert.equal(JSON.stringify(observed.result).includes('documents'), false);
  assert.match(observed.metrics, /pacificdb_replica_integrity_failures_total 1/);
});

test('reports LAGGING when applied indexes do not converge', async (t) => {
  const observed = await runCase(t, [healthyHandler({ lastApplied: 12 }),
    healthyHandler({ lastApplied: 11 }), healthyHandler({ lastApplied: 12 })]);
  assert.equal(observed.exitCode, 3);
  assert.equal(observed.result.status, 'LAGGING');
});

test('reports UNAVAILABLE for an unreachable member', async (t) => {
  const observed = await runCase(t, [healthyHandler(), healthyHandler()], {
    nodes: [{ id: 'unavailable', host: '127.0.0.1', port: 1 }],
  });
  assert.equal(observed.exitCode, 4);
  assert.equal(observed.result.status, 'UNAVAILABLE');
});

test('reports INCOMPATIBLE for cluster or schema mismatch', async (t) => {
  const observed = await runCase(t, [healthyHandler(),
    healthyHandler({ clusterId: 'cluster-b' }), healthyHandler()]);
  assert.equal(observed.exitCode, 5);
  assert.equal(observed.result.status, 'INCOMPATIBLE');
});

test('reports AUTHENTICATION_FAILED and redacts server detail', async (t) => {
  const auth = () => ({ error: 'authentication_required',
    message: 'token top-secret-token rejected' });
  const observed = await runCase(t, [auth, auth, auth]);
  assert.equal(observed.exitCode, 6);
  assert.equal(observed.result.status, 'AUTHENTICATION_FAILED');
  assert.equal(JSON.stringify(observed.result).includes('rejected'), false);
});
