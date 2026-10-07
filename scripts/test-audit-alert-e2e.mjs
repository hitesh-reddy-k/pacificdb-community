#!/usr/bin/env node
// Linux fixture only: real mTLS engine -> monitor -> pinned Alertmanager -> receiver.
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, writeFile, readFile, rename, rm, rmdir } from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { requestNode } from './replica-integrity-monitor.mjs';
import { monitor } from './audit-health-monitor.mjs';

const image = 'prom/alertmanager@sha256:e9733bafb1bdef9b00e25a21f8f99dc26a22224bf16641ad754d1649f4c3357a';
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-audit-alert-'));
const build = path.resolve(process.argv[2] || 'build');
const binary = path.join(build, 'db_engine');
const container = `pacificdb-audit-alert-${process.pid}`;
let engine, alertmanager, receiver;
const receipts = [];
let receiverRejects = true;
let engineLogs = '', monitorLogs = '';
const password = 'disposable-audit-fixture-password';
async function port() {
  const server = net.createServer();
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const value = server.address().port;
  await new Promise(resolve => server.close(resolve)); return value;
}
async function until(check, message) {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    try { const value = await check(); if (value) return value; } catch { /* startup/retry */ }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw new Error(message);
}
async function stop(child) {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const closed = once(child, 'close'); child.kill('SIGTERM');
  const force = setTimeout(() => child.kill('SIGKILL'), 15000);
  await closed; clearTimeout(force);
}
try {
  execFileSync('docker', ['pull', image], { stdio: 'pipe' });
  const tlsRoot = path.join(root, 'tls');
  const data = path.join(root, 'data');
  await mkdir(tlsRoot); await mkdir(data);
  const openssl = args => execFileSync('openssl', args, { cwd: tlsRoot, stdio: 'pipe' });
  openssl(['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
    '-subj', '/CN=PacificDB-disposable-test-CA', '-keyout', 'ca.key', '-out', 'ca.crt']);
  for (const [name, usage] of [['server', 'serverAuth'], ['health', 'clientAuth']]) {
    openssl(['req', '-newkey', 'rsa:2048', '-nodes', '-subj', `/CN=${name}`,
      '-keyout', `${name}.key`, '-out', `${name}.csr`,
      '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.2', '-addext', `extendedKeyUsage=${usage}`]);
    openssl(['x509', '-req', '-in', `${name}.csr`, '-CA', 'ca.crt', '-CAkey', 'ca.key',
      '-CAcreateserial', '-days', '2', '-copy_extensions', 'copy', '-out', `${name}.crt`]);
  }
  const enginePort = await port(), raftPort = await port(), amPort = await port();
  const node = { host: '127.0.0.2', port: enginePort, tls: true, servername: 'localhost',
    caFile: path.join(tlsRoot, 'ca.crt'), certFile: path.join(tlsRoot, 'health.crt'),
    keyFile: path.join(tlsRoot, 'health.key') };
  engine = spawn(binary, [], { env: { ...process.env,
    PACIFICDB_ENVIRONMENT: 'development', DATA_ROOT: data,
    BACKUP_ROOT: path.join(root, 'backup'), RESTORE_DIR: path.join(root, 'restore'),
    ENGINE_BIND_HOST: node.host, ENGINE_PORT: String(enginePort), ENGINE_AUTH_REQUIRED: '1',
    PACIFICDB_ENGINE_ADMIN_USERNAME: 'admin', PACIFICDB_ENGINE_ADMIN_PASSWORD: password,
    TLS_ENABLED: '1', TLS_REQUIRE_CLIENT_CERT: '1', TLS_CERT_PATH: path.join(tlsRoot, 'server.crt'),
    TLS_KEY_PATH: path.join(tlsRoot, 'server.key'), TLS_CA_PATH: node.caFile,
    RAFT_BIND_HOST: node.host, RAFT_LISTEN_PORT: String(raftPort), RAFT_PEERS: '',
    RAFT_TLS_ENABLED: '1', RAFT_TLS_CERT_PATH: path.join(tlsRoot, 'server.crt'),
    RAFT_TLS_KEY_PATH: path.join(tlsRoot, 'server.key'), RAFT_TLS_CA_PATH: node.caFile,
    RAFT_CLUSTER_ID: 'audit-alert-fixture', RAFT_NODE_ID: 'node-1', RAFT_IS_LEADER: '1',
    MIN_QUORUM_SIZE: '1', ENGINE_CPU_CORES: '2', CONN_MIN_THREADS: '2', CONN_MAX_THREADS: '4',
    DBQ_SHARDS: '2', DBQ_WORKERS_PER_SHARD: '1', ADAPTIVE_ADMISSION: '0',
  }, stdio: ['ignore', 'pipe', 'pipe'] });
  engine.stdout.on('data', chunk => { engineLogs = (engineLogs + chunk).slice(-65536); });
  engine.stderr.on('data', chunk => { engineLogs = (engineLogs + chunk).slice(-65536); });
  await until(async () => (await requestNode(node, { action: 'ping' }, 1000)).status === 'pong', 'mTLS engine startup');
  const admin = await requestNode(node, { action: 'security_authenticate', username: 'admin', password }, 3000);
  assert.ok(admin.token);
  const key = await requestNode(node, { action: 'api_key_create', token: admin.token,
    name: 'audit-monitor', role: 'metrics' }, 3000);
  assert.ok(key.key);
  const tokenFile = path.join(root, 'metrics.token');
  await writeFile(tokenFile, key.key, { mode: 0o600 });
  assert.equal((await requestNode(node, { action: 'insert', token: key.key,
    dbName: 'no-db', collection: 'docs', data: { id: 'cannot-write' } }, 3000)).error, 'permission_denied');
  assert.equal((await requestNode(node, { action: 'find', token: key.key,
    dbName: 'no-db', collection: 'docs' }, 3000)).error, 'permission_denied');
  await assert.rejects(requestNode({ ...node, certFile: undefined, keyFile: undefined }, { action: 'ping' }, 1000));
  await assert.rejects(requestNode({ ...node, servername: 'wrong.invalid' }, { action: 'security_metrics', token: key.key }, 1000));

  receiver = http.createServer((request, response) => {
    let body = '';
    request.on('data', chunk => { body += chunk; if (body.length > 16384) request.destroy(); });
    request.on('end', () => {
      const statusCode = receiverRejects ? 503 : 200;
      receipts.push({ statusCode, value: JSON.parse(body) });
      response.writeHead(statusCode); response.end();
    });
  });
  await new Promise(resolve => receiver.listen(0, '127.0.0.1', resolve));
  const amConfig = path.join(root, 'alertmanager.yml');
  await writeFile(amConfig, `global:\n  resolve_timeout: 2m\nroute:\n  receiver: fixture\n  group_by: [alertname, instance]\n  group_wait: 0s\n  group_interval: 1s\n  repeat_interval: 1m\nreceivers:\n  - name: fixture\n    webhook_configs:\n      - url: http://127.0.0.1:${receiver.address().port}/alerts\n        send_resolved: true\n`, { mode: 0o644 });
  alertmanager = spawn('docker', ['run', '--rm', '--name', container, '--network', 'host',
    '--read-only', '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
    '--tmpfs', '/alertmanager:rw,noexec,nosuid,size=8m',
    '--mount', `type=bind,src=${amConfig},dst=/etc/alertmanager/alertmanager.yml,readonly`,
    image, '--config.file=/etc/alertmanager/alertmanager.yml', '--storage.path=/alertmanager',
    `--web.listen-address=127.0.0.1:${amPort}`, '--cluster.listen-address='], { stdio: 'ignore' });
  await until(async () => (await fetch(`http://127.0.0.1:${amPort}/-/ready`)).ok, 'Alertmanager startup');
  const config = { instance: 'disposable-audit-fixture', engine: { ...node, tokenFile },
    alertmanagerUrl: `http://127.0.0.1:${amPort}/api/v2/alerts` };
  const configFile = path.join(root, 'monitor.json'), output = path.join(root, 'result.json');
  await writeFile(configFile, JSON.stringify(config), { mode: 0o600 });
  async function runMonitor(value = config) {
    await writeFile(configFile, JSON.stringify(value), { mode: 0o600 });
    const child = spawn(process.execPath, [path.resolve('scripts/audit-health-monitor.mjs'), configFile, output]);
    child.stdout.on('data', chunk => { monitorLogs += chunk; });
    child.stderr.on('data', chunk => { monitorLogs += chunk; });
    const [code] = await once(child, 'close');
    return { code, ...JSON.parse(await readFile(output, 'utf8')) };
  }
  async function probe(kind) {
    const child = spawn('sh', [path.resolve('deploy/docker/healthcheck.sh'), kind], { env: {
      ...process.env, PACIFICDB_ENVIRONMENT: 'production', ENGINE_BIND_HOST: node.host,
      POD_IP: node.host, ENGINE_PORT: String(enginePort), PACIFICDB_TLS_SERVER_NAME: node.servername,
      PACIFICDB_HEALTH_TLS_DIR: tlsRoot, PACIFICDB_HEALTH_TOKEN_FILE: tokenFile,
    }, stdio: 'ignore' });
    return (await once(child, 'close'))[0];
  }
  assert.equal((await runMonitor()).code, 0);
  assert.equal(await probe('readiness'), 0);
  const audit = path.join(data, 'security', 'audit.log');
  await rename(audit, `${audit}.saved`); await mkdir(audit);
  await requestNode(node, { action: 'find', dbName: 'missing', collection: 'docs' }, 3000);
  const failing = await runMonitor();
  assert.equal(failing.code, 1); assert.equal(failing.status, 'UNHEALTHY');
  assert.equal(failing.alertAccepted, true); assert.ok(failing.failures > 0);
  assert.notEqual(await probe('readiness'), 0);
  assert.equal(await probe('liveness'), 0, 'audit failures must not cause restart loops');
  await until(() => receipts.some(r => r.statusCode === 503 && r.value.status === 'firing'), 'receiver rejection observed');
  receiverRejects = false;
  await until(() => receipts.some(r => r.statusCode === 200 && r.value.status === 'firing'), 'firing alert delivered after receiver recovery');
  const failedDelivery = await runMonitor({ ...config, alertmanagerUrl: `http://127.0.0.1:${await port()}/api/v2/alerts` });
  assert.equal(failedDelivery.code, 2); assert.equal(failedDelivery.alertAccepted, false);
  assert.match(await readFile(`${output}.prom`, 'utf8'), /pacificdb_audit_alert_delivery_ok 0/);
  await rmdir(audit); await rename(`${audit}.saved`, audit);
  await requestNode(node, { action: 'find', dbName: 'missing', collection: 'docs' }, 3000);
  const recovered = await runMonitor();
  assert.equal(recovered.code, 0); assert.equal(recovered.status, 'HEALTHY');
  assert.equal(await probe('readiness'), 0);
  await until(() => receipts.some(r => r.statusCode === 200 && r.value.status === 'resolved'), 'resolved alert delivered');
  await assert.rejects(monitor({ ...config, engine: { ...config.engine, host: 'remote.invalid', tls: false } }, output));
  await assert.rejects(monitor({ ...config, alertmanagerUrl: 'http://remote.invalid/api/v2/alerts' }, output));
  const report = { status: 'PASS', engineIdentity: JSON.parse(execFileSync(binary, ['--build-info'], { encoding: 'utf8' })),
    alertmanagerImage: image, mTLS: true, leastPrivilege: true, readinessRejectsAuditFailure: true,
    livenessSurvivesAuditFailure: true, receiverRejectedThenRecovered: true,
    firingReceived: true, resolvedReceived: true, deliveryFailureDetected: true,
    scope: 'Owned loopback fixture, not production operator notification delivery' };
  assert.doesNotMatch(JSON.stringify(receipts) + monitorLogs, new RegExp(password));
  for (const secret of [key.key, admin.token]) assert.ok(!(JSON.stringify(receipts) + monitorLogs).includes(secret));
  assert.match(engineLogs, /audit_write_failed/); assert.match(engineLogs, /audit_write_recovered/);
  console.log(JSON.stringify(report, null, 2));
  if (process.argv[3]) await writeFile(path.resolve(process.argv[3]), JSON.stringify(report, null, 2) + '\n');
} finally {
  // Only the exact named disposable container and newly owned fixture are removed.
  try { execFileSync('docker', ['rm', '-f', container], { stdio: 'ignore' }); } catch { /* never started */ }
  await stop(alertmanager); await stop(engine);
  if (receiver) await new Promise(resolve => receiver.close(resolve));
  await rm(root, { recursive: true, force: true });
}
