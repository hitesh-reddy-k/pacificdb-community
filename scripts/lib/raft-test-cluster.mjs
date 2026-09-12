import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, rm } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../../sdk/node/src/index.js';

const pause = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

async function freePort(host) {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, host, resolve);
  });
  const selected = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return selected;
}

class DirectedProxy {
  constructor(source, destination, host, port, targetHost, targetPort) {
    Object.assign(this, { source, destination, host, port, targetHost, targetPort });
    this.enabled = true;
    this.connections = new Set();
    this.server = net.createServer((incoming) => this.accept(incoming));
  }

  accept(incoming) {
    if (!this.enabled) {
      incoming.destroy();
      return;
    }
    const outgoing = net.createConnection({ host: this.targetHost, port: this.targetPort });
    incoming.setNoDelay(true);
    outgoing.setNoDelay(true);
    const pair = { incoming, outgoing };
    this.connections.add(pair);
    const close = () => {
      incoming.destroy();
      outgoing.destroy();
      this.connections.delete(pair);
    };
    incoming.once('error', close);
    incoming.once('close', close);
    outgoing.once('error', close);
    outgoing.once('close', close);
    incoming.pipe(outgoing);
    outgoing.pipe(incoming);
  }

  async start() {
    await new Promise((resolve, reject) => {
      this.server.once('error', reject);
      this.server.listen(this.port, this.host, resolve);
    });
  }

  setEnabled(enabled) {
    this.enabled = enabled;
    if (!enabled) {
      for (const pair of [...this.connections]) {
        pair.incoming.destroy();
        pair.outgoing.destroy();
      }
    }
  }

  async close() {
    this.setEnabled(false);
    if (!this.server.listening) return;
    await new Promise((resolve) => this.server.close(resolve));
  }
}

export class RaftTestCluster {
  static async create(options = {}) {
    const cluster = new RaftTestCluster(options);
    await cluster.initialize();
    return cluster;
  }

  constructor(options) {
    this.build = path.resolve(options.build);
    this.binary = path.join(this.build, 'db_engine');
    this.clusterId = options.clusterId || `community-rf3-${process.pid}-${Date.now()}`;
    this.rootPrefix = options.rootPrefix || 'pacificdb-rf3-';
    this.authRequired = Boolean(options.authRequired);
    this.allEligible = options.allEligible !== false;
    this.useProxies = options.useProxies !== false;
    this.keep = options.keep || process.env.KEEP_RF3 === '1';
    this.hosts = ['127.0.0.2', '127.0.0.3', '127.0.0.4'];
    this.proxyHost = '127.0.1.1';
    this.nodes = Array(3).fill(null);
    this.proxies = new Map();
  }

  async initialize() {
    this.root = await mkdtemp(path.join(os.tmpdir(), this.rootPrefix));
    this.enginePorts = await Promise.all(this.hosts.map((host) => freePort(host)));
    this.raftPorts = await Promise.all(this.hosts.map((host) => freePort(host)));
    if (this.useProxies) {
      for (let source = 0; source < 3; source += 1) {
        for (let destination = 0; destination < 3; destination += 1) {
          if (source === destination) continue;
          const port = await freePort(this.proxyHost);
          const proxy = new DirectedProxy(source, destination, this.proxyHost, port,
            this.hosts[destination], this.raftPorts[destination]);
          this.proxies.set(`${source}->${destination}`, proxy);
        }
      }
    }
  }

  peerList(source) {
    return this.hosts.map((host, destination) => {
      if (source === destination) return null;
      const proxy = this.proxies.get(`${source}->${destination}`);
      return `node-${destination + 1}@${proxy ? proxy.host : host}:${proxy ? proxy.port : this.raftPorts[destination]}`;
    }).filter(Boolean).join(',');
  }

  nodeEnvironment(index) {
    const directory = path.join(this.root, `node-${index + 1}`);
    return {
      ...process.env,
      PACIFICDB_ENVIRONMENT: 'development', DATA_ROOT: directory,
      BACKUP_ROOT: path.join(directory, 'backups'), RESTORE_DIR: path.join(directory, 'restores'),
      TMP_DIR: path.join(directory, 'tmp'), LOG_DIR: path.join(directory, 'logs'),
      ENGINE_BIND_HOST: this.hosts[index], ENGINE_HOST: this.hosts[index],
      ENGINE_PORT: String(this.enginePorts[index]),
      RAFT_BIND_HOST: this.hosts[index], RAFT_LISTEN_PORT: String(this.raftPorts[index]),
      RAFT_NODE_ID: `node-${index + 1}`, RAFT_CLUSTER_ID: this.clusterId,
      RAFT_PEERS: this.peerList(index), RAFT_IS_LEADER: index === 0 ? '1' : '0',
      RAFT_LEADER_ELIGIBLE: this.allEligible || index === 0 ? '1' : '0',
      MIN_QUORUM_SIZE: '2', RAFT_REPLICATION_MODE: 'sync',
      RAFT_HEARTBEAT_INTERVAL_MS: '100', RAFT_ELECTION_TIMEOUT_MS: '1000',
      RAFT_APPEND_TIMEOUT_MS: '2500', RAFT_PEER_RPC_TIMEOUT_MS: '400',
      RAFT_REPLICATOR_RPC_TIMEOUT_MS: '400', RAFT_QUORUM_WAIT_TIMEOUT_MS: '3000',
      RAFT_SCHEMA_OP_TIMEOUT_MS: '4000', RAFT_LEADER_NOOP_TIMEOUT_MS: '3000',
      RAFT_MIN_WORKERS: '4', RAFT_MAX_WORKERS: '16', ENGINE_CPU_CORES: '4',
      CONN_MIN_THREADS: '4', CONN_MAX_THREADS: '32', DBQ_SHARDS: '4',
      DBQ_WORKERS_PER_SHARD: '2', ADAPTIVE_ADMISSION: '0', ENGINE_AUTH_REQUIRED: this.authRequired ? '1' : '0'
    };
  }

  client(index, options = {}) {
    return new PacificDBClient({ host: this.hosts[index], port: this.enginePorts[index],
      timeoutMs: options.timeoutMs || 5000, token: options.token });
  }

  async start() {
    if (this.useProxies) await Promise.all([...this.proxies.values()].map((proxy) => proxy.start()));
    await Promise.all([this.startNode(1), this.startNode(2)]);
    await this.startNode(0);
  }

  async startNode(index) {
    const environment = this.nodeEnvironment(index);
    await mkdir(environment.DATA_ROOT, { recursive: true });
    const child = spawn(this.binary, [], {
      cwd: path.resolve(import.meta.dirname, '../..'), env: environment,
      stdio: ['ignore', 'ignore', 'ignore']
    });
    this.nodes[index] = { child, environment };
    const probe = this.client(index, { timeoutMs: 300 });
    let lastError;
    for (let attempt = 0; attempt < 200; attempt += 1) {
      if (child.exitCode !== null) throw new Error(`node ${index + 1} exited with ${child.exitCode}`);
      try {
        if ((await probe.request({ action: 'ping' })).status === 'pong') return;
      } catch (error) { lastError = error; }
      await pause(100);
    }
    throw new Error(`node ${index + 1} failed to start: ${lastError?.message}`);
  }

  async stopNode(index, signal = 'SIGINT') {
    const node = this.nodes[index];
    if (!node?.child || node.child.exitCode !== null) return;
    node.child.kill(signal);
    const timer = setTimeout(() => node.child.kill('SIGKILL'), 10_000);
    await once(node.child, 'exit');
    clearTimeout(timer);
  }

  setLink(source, destination, enabled) {
    const proxy = this.proxies.get(`${source}->${destination}`);
    assert.ok(proxy, `no directed proxy ${source}->${destination}`);
    proxy.setEnabled(enabled);
  }

  isolate(index) {
    for (let peer = 0; peer < 3; peer += 1) {
      if (peer === index) continue;
      this.setLink(index, peer, false);
      this.setLink(peer, index, false);
    }
  }

  heal() {
    for (const proxy of this.proxies.values()) proxy.setEnabled(true);
  }

  async pings() {
    return Promise.all(this.hosts.map(async (_host, index) => {
      try { return await this.client(index, { timeoutMs: 800 }).request({ action: 'ping' }); }
      catch (error) { return { error: error.message }; }
    }));
  }

  async waitForLeader(indices = [0, 1, 2], timeoutMs = 20_000, excluded = []) {
    const deadline = Date.now() + timeoutMs;
    let last = [];
    while (Date.now() < deadline) {
      last = await this.pings();
      const leaders = indices.filter((index) => last[index]?.isLeader && !excluded.includes(index));
      if (leaders.length === 1) return { index: leaders[0], ping: last[leaders[0]], pings: last };
      await pause(200);
    }
    throw new Error(`expected one leader among ${indices.join(',')}: ${JSON.stringify(last)}`);
  }

  async retry(operation, label, attempts = 120, delayMs = 100) {
    let lastError;
    for (let attempt = 0; attempt < attempts; attempt += 1) {
      try { return await operation(); }
      catch (error) { lastError = error; await pause(delayMs); }
    }
    throw new Error(`${label}: ${lastError?.message}`);
  }

  async close() {
    await Promise.all([0, 1, 2].map((index) => this.stopNode(index, 'SIGKILL').catch(() => {})));
    await Promise.all([...this.proxies.values()].map((proxy) => proxy.close().catch(() => {})));
    if (!this.keep) await rm(this.root, { recursive: true, force: true });
  }
}

export { pause };
