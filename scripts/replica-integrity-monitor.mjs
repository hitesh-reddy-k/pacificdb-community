#!/usr/bin/env node

import net from 'node:net';
import tls from 'node:tls';
import { readFile, rename, writeFile } from 'node:fs/promises';
import path from 'node:path';

const EXIT = {
  CONSISTENT: 0,
  DIVERGENT: 2,
  LAGGING: 3,
  UNAVAILABLE: 4,
  INCOMPATIBLE: 5,
  AUTHENTICATION_FAILED: 6,
  ERROR: 7,
};
const AUTH_ERRORS = new Set([
  'authentication_required', 'authentication_failed', 'invalid_token',
  'permission_denied', 'forbidden',
]);

function argumentsFor(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 1) {
    const key = argv[index];
    if (!key.startsWith('--') || index + 1 >= argv.length) {
      throw new Error('usage: replica-integrity-monitor.mjs --config FILE --output FILE [--prometheus FILE]');
    }
    result[key.slice(2)] = argv[++index];
  }
  if (!result.config || !result.output) throw new Error('config and output are required');
  return result;
}

function positiveInteger(value, fallback, name) {
  const selected = value ?? fallback;
  if (!Number.isSafeInteger(selected) || selected <= 0) {
    throw new Error(`${name} must be a positive integer`);
  }
  return selected;
}

function validateConfig(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('config must be an object');
  }
  for (const field of ['userId', 'database', 'collection']) {
    if (typeof value[field] !== 'string' || !value[field]) {
      throw new Error(`${field} is required`);
    }
  }
  if (!Array.isArray(value.nodes) || value.nodes.length < 1) {
    throw new Error('nodes must contain at least one member');
  }
  const ids = new Set();
  const nodes = value.nodes.map((node, index) => {
    if (!node || typeof node !== 'object' || typeof node.host !== 'string' ||
        !node.host || !Number.isSafeInteger(node.port) || node.port < 1 || node.port > 65535) {
      throw new Error(`nodes[${index}] has an invalid host or port`);
    }
    const id = typeof node.id === 'string' && node.id ? node.id : `${node.host}:${node.port}`;
    if (ids.has(id)) throw new Error(`duplicate node id: ${id}`);
    ids.add(id);
    return { id, host: node.host, port: node.port, tls: node.tls === true,
      servername: node.servername, caFile: node.caFile };
  });
  return {
    userId: value.userId,
    database: value.database,
    collection: value.collection,
    token: typeof value.token === 'string' ? value.token : '',
    maxDocs: positiveInteger(value.maxDocs, 100000, 'maxDocs'),
    timeoutMs: positiveInteger(value.timeoutMs, 5000, 'timeoutMs'),
    lagTimeoutMs: positiveInteger(value.lagTimeoutMs, 30000, 'lagTimeoutMs'),
    pollIntervalMs: positiveInteger(value.pollIntervalMs, 500, 'pollIntervalMs'),
    nodes,
  };
}

async function requestNode(node, payload, timeoutMs) {
  let ca;
  if (node.caFile) ca = await readFile(node.caFile);
  return new Promise((resolve, reject) => {
    const socket = node.tls
      ? tls.connect({ host: node.host, port: node.port, ca,
        ...(node.servername ? { servername: node.servername } : {}) })
      : net.createConnection({ host: node.host, port: node.port });
    let response = '';
    let settled = false;
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.destroy();
      if (error) reject(error);
      else resolve(value);
    };
    const timer = setTimeout(() => finish(new Error('request_timeout')), timeoutMs);
    timer.unref?.();
    socket.once(node.tls ? 'secureConnect' : 'connect', () => {
      socket.write(`${JSON.stringify(payload)}\n`);
    });
    socket.on('data', (chunk) => {
      response += chunk;
      const newline = response.indexOf('\n');
      if (newline < 0) return;
      try {
        finish(null, JSON.parse(response.slice(0, newline)));
      } catch {
        finish(new Error('invalid_json_response'));
      }
    });
    socket.once('error', (error) => finish(error));
    socket.once('end', () => finish(new Error('response_closed')));
  });
}

function requestPayload(config, action, extra = {}) {
  return { action, userId: config.userId, dbName: config.database,
    ...(config.token ? { token: config.token } : {}), ...extra };
}

function safeNode(node, response = {}) {
  return {
    id: node.id,
    nodeId: typeof response.nodeId === 'string' ? response.nodeId : '',
    term: Number.isSafeInteger(response.term) ? response.term :
      (Number.isSafeInteger(response.currentTerm) ? response.currentTerm : 0),
    commitIndex: Number.isSafeInteger(response.commitIndex) ? response.commitIndex : 0,
    lastApplied: Number.isSafeInteger(response.lastApplied) ? response.lastApplied : 0,
  };
}

function classifyFailure(results) {
  if (results.some(({ response }) => AUTH_ERRORS.has(String(response?.error ?? '')))) {
    return 'AUTHENTICATION_FAILED';
  }
  if (results.some(({ error, response }) => error || response?.error || response?.ok !== true)) {
    return 'UNAVAILABLE';
  }
  return null;
}

async function probeStatuses(config) {
  return Promise.all(config.nodes.map(async (node) => {
    try {
      const response = await requestNode(node,
        requestPayload(config, 'admin_raft_status'), config.timeoutMs);
      return { node, response };
    } catch (error) {
      return { node, error };
    }
  }));
}

function sameApplied(results) {
  return new Set(results.map(({ response }) => response.lastApplied)).size === 1;
}

async function inspect(config) {
  const startedAt = new Date().toISOString();
  let statuses = await probeStatuses(config);
  let failure = classifyFailure(statuses);
  if (failure) return result(failure, startedAt, statuses);

  const statusClusters = new Set(statuses.map(({ response }) => response.clusterId)
    .filter((value) => typeof value === 'string' && value));
  if (statusClusters.size > 1) return result('INCOMPATIBLE', startedAt, statuses);

  const lagDeadline = Date.now() + config.lagTimeoutMs;
  while (!sameApplied(statuses) && Date.now() < lagDeadline) {
    await new Promise((resolve) => setTimeout(resolve, config.pollIntervalMs));
    statuses = await probeStatuses(config);
    failure = classifyFailure(statuses);
    if (failure) return result(failure, startedAt, statuses);
  }
  if (!sameApplied(statuses)) return result('LAGGING', startedAt, statuses);

  const fence = Math.min(...statuses.map(({ response }) => response.lastApplied));
  const digests = await Promise.all(config.nodes.map(async (node) => {
    try {
      const response = await requestNode(node, requestPayload(config,
        'admin_replica_digest', { collection: config.collection, fence,
          maxDocs: config.maxDocs }), config.timeoutMs);
      return { node, response };
    } catch (error) {
      return { node, error };
    }
  }));
  failure = classifyFailure(digests);
  if (failure) return result(failure, startedAt, digests, fence);

  const clusters = new Set(digests.map(({ response }) => response.clusterId));
  const schemas = new Set(digests.map(({ response }) => response.digestSchema));
  const fences = new Set(digests.map(({ response }) => response.fence));
  if (clusters.size !== 1 || schemas.size !== 1 ||
      schemas.values().next().value !== 'pacificdb-logical-v1' ||
      fences.size !== 1 || fences.values().next().value !== fence) {
    return result('INCOMPATIBLE', startedAt, digests, fence);
  }
  const values = new Set(digests.map(({ response }) => response.digest));
  return result(values.size === 1 ? 'CONSISTENT' : 'DIVERGENT',
    startedAt, digests, fence, schemas.values().next().value);
}

function result(status, startedAt, observations, fence, digestSchema) {
  return {
    schemaVersion: 1,
    status,
    startedAt,
    finishedAt: new Date().toISOString(),
    ...(Number.isSafeInteger(fence) ? { fence } : {}),
    ...(digestSchema ? { digestSchema } : {}),
    nodes: observations.map(({ node, response }) => ({
      ...safeNode(node, response),
      ...(typeof response?.digest === 'string' ? { digest: response.digest } : {}),
      ...(Number.isSafeInteger(response?.documentCount)
        ? { documentCount: response.documentCount } : {}),
      outcome: response?.ok === true ? 'OK' : 'FAILED',
    })),
  };
}

async function atomicWrite(filename, content) {
  const temporary = `${filename}.tmp-${process.pid}`;
  await writeFile(temporary, content, { mode: 0o600 });
  await rename(temporary, filename);
}

function prometheus(resultValue) {
  const states = Object.keys(EXIT).filter((state) => state !== 'ERROR');
  const lines = [
    '# HELP pacificdb_replica_integrity_state Current replica integrity state.',
    '# TYPE pacificdb_replica_integrity_state gauge',
  ];
  for (const state of states) {
    lines.push(`pacificdb_replica_integrity_state{state="${state.toLowerCase()}"} ${resultValue.status === state ? 1 : 0}`);
  }
  if (Number.isSafeInteger(resultValue.fence)) {
    lines.push('# TYPE pacificdb_replica_integrity_fence gauge');
    lines.push(`pacificdb_replica_integrity_fence ${resultValue.fence}`);
  }
  lines.push('# TYPE pacificdb_replica_integrity_failures_total gauge');
  lines.push(`pacificdb_replica_integrity_failures_total ${resultValue.status === 'CONSISTENT' ? 0 : 1}`);
  lines.push('# TYPE pacificdb_replica_integrity_last_success_timestamp_seconds gauge');
  lines.push(`pacificdb_replica_integrity_last_success_timestamp_seconds ${resultValue.status === 'CONSISTENT' ? Math.floor(Date.parse(resultValue.finishedAt) / 1000) : 0}`);
  return `${lines.join('\n')}\n`;
}

export async function main(argv = process.argv.slice(2)) {
  const args = argumentsFor(argv);
  const config = validateConfig(JSON.parse(await readFile(args.config, 'utf8')));
  const observed = await inspect(config);
  await atomicWrite(path.resolve(args.output), `${JSON.stringify(observed, null, 2)}\n`);
  if (args.prometheus) await atomicWrite(path.resolve(args.prometheus), prometheus(observed));
  process.stdout.write(`${observed.status}\n`);
  return EXIT[observed.status] ?? EXIT.ERROR;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  main().then((code) => { process.exitCode = code; }).catch((error) => {
    process.stderr.write(`replica integrity monitor failed: ${error.message}\n`);
    process.exitCode = EXIT.ERROR;
  });
}
