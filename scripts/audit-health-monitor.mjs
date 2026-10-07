#!/usr/bin/env node
import assert from 'node:assert/strict';
import { lstat, readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { requestNode, atomicWrite } from './replica-integrity-monitor.mjs';

const loopback = host => ['127.0.0.1', '::1', '[::1]', 'localhost'].includes(host);
async function privateFile(filename) {
  const info = await lstat(filename);
  assert.ok(info.isFile() && !info.isSymbolicLink(), 'private input must be a regular file');
  if (process.platform !== 'win32') assert.equal(info.mode & 0o077, 0, 'private input must have mode 0600');
  return readFile(filename, 'utf8');
}

export async function monitor(config, output) {
  assert.notEqual(process.env.NODE_TLS_REJECT_UNAUTHORIZED, '0', 'TLS verification must not be disabled');
  assert.match(config.instance, /^[A-Za-z0-9_.:-]{1,128}$/, 'bounded instance label required');
  const node = config.engine;
  assert.ok(node && typeof node.host === 'string' && node.host.length < 254);
  assert.ok(Number.isSafeInteger(node.port) && node.port > 0 && node.port < 65536);
  assert.ok(node.tls === true || loopback(node.host), 'remote engines require TLS');
  if (node.tls === true) assert.ok(node.caFile && node.certFile && node.keyFile, 'mTLS files required');
  const destination = new URL(config.alertmanagerUrl);
  assert.ok(destination.protocol === 'https:' ||
    (destination.protocol === 'http:' && loopback(destination.hostname)), 'remote alerts require HTTPS');
  assert.ok(!destination.username && !destination.password && !destination.hash, 'URL credentials are forbidden');
  assert.equal(destination.pathname, '/api/v2/alerts', 'Alertmanager API v2 endpoint required');
  const token = (await privateFile(node.tokenFile)).trim();
  assert.match(token, /^[A-Za-z0-9_-]{1,1024}$/, 'invalid engine credential format');
  let status = 'UNAVAILABLE';
  let failures = 0, evictions = 0;
  try {
    const value = await requestNode(node, { action: 'security_metrics', token }, 5000);
    if (value?.success === true && typeof value.auditHealthy === 'boolean' &&
        typeof value.auditLoggingEnabled === 'boolean' &&
        Number.isSafeInteger(value.auditWriteFailures) && value.auditWriteFailures >= 0 &&
        Number.isSafeInteger(value.auditBufferEvictions) && value.auditBufferEvictions >= 0) {
      status = value.auditHealthy && value.auditLoggingEnabled ? 'HEALTHY' : 'UNHEALTHY';
      failures = value.auditWriteFailures; evictions = value.auditBufferEvictions;
    }
  } catch { /* Unavailable/auth/TLS errors are alertable, never healthy. */ }
  const now = Date.now();
  const alert = [{ labels: { alertname: 'PacificDBAuditPersistenceUnavailable',
    instance: config.instance, severity: 'critical' },
    annotations: { summary: `PacificDB audit status ${status}`,
      runbook_url: 'docs/OPERATIONS.md#security-audit-failure-policy' },
    startsAt: new Date(now - 1000).toISOString(),
    endsAt: new Date(status === 'HEALTHY' ? now : now + 180000).toISOString() }];
  let delivered = false;
  try {
    const authorization = config.alertTokenFile
      ? `Bearer ${(await privateFile(config.alertTokenFile)).trim()}` : undefined;
    const response = await fetch(destination, { method: 'POST', redirect: 'error',
      signal: AbortSignal.timeout(5000), headers: { 'content-type': 'application/json',
        ...(authorization ? { authorization } : {}) }, body: JSON.stringify(alert) });
    delivered = response.ok;
    await response.body?.cancel();
  } catch { /* Delivery failures remain observable and get retried next timer run. */ }
  const result = { schemaVersion: 1, instance: config.instance, status,
    alertAccepted: delivered, checkedAt: new Date(now).toISOString(), failures, evictions };
  const metrics = [
    `pacificdb_audit_healthy ${status === 'HEALTHY' ? 1 : 0}`,
    `pacificdb_audit_write_failures_total ${failures}`,
    `pacificdb_audit_buffer_evictions_total ${evictions}`,
    `pacificdb_audit_alert_delivery_ok ${delivered ? 1 : 0}`,
    `pacificdb_audit_monitor_last_check_timestamp_seconds ${Math.floor(now / 1000)}`,
  ].join('\n') + '\n';
  await atomicWrite(`${output}.prom`, metrics);
  await atomicWrite(output, JSON.stringify(result, null, 2) + '\n');
  return result;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  try {
    const [configFile, output] = process.argv.slice(2);
    assert.ok(configFile && output, 'usage: audit-health-monitor.mjs PRIVATE_CONFIG.json RESULT.json');
    const result = await monitor(JSON.parse(await privateFile(configFile)), output);
    console.log(JSON.stringify(result));
    process.exitCode = !result.alertAccepted ? 2 : result.status === 'HEALTHY' ? 0 : 1;
  } catch { console.error('audit monitor configuration/output failure; check private files and TLS setup'); process.exitCode = 2; }
}
