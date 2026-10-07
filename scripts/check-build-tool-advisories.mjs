#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

export function checkAdvisories(full, runtime, lock, policy, now = new Date()) {
  assert.equal(runtime?.metadata?.vulnerabilities?.total, 0, 'runtime advisories are never accepted');
  assert.ok(runtime?.vulnerabilities && typeof runtime.vulnerabilities === 'object' && !Array.isArray(runtime.vulnerabilities));
  assert.equal(Object.keys(runtime.vulnerabilities).length, 0, 'runtime findings are never accepted');
  assert.equal(policy.schemaVersion, 1);
  const expiry = Date.parse(`${policy.expiresOn}T00:00:00Z`);
  assert.ok(full?.vulnerabilities && typeof full.vulnerabilities === 'object' && !Array.isArray(full.vulnerabilities));
  assert.equal(full.metadata?.vulnerabilities?.total, Object.keys(full.vulnerabilities).length);
  if (Object.keys(full.vulnerabilities).length > 0)
    assert.ok(Number.isFinite(expiry) && now.getTime() < expiry, 'build-tool exception expired or invalid');
  let matchedAdvisory = false;
  for (const [name, finding] of Object.entries(full.vulnerabilities)) {
    assert.equal(finding.severity, 'moderate', `unaccepted severity: ${name}`);
    assert.ok(Object.hasOwn(policy.packages, name), `unaccepted package: ${name}`);
    assert.ok(Array.isArray(finding.via) && finding.via.length > 0);
    for (const via of finding.via) {
      if (typeof via === 'string') {
        assert.ok(Object.hasOwn(full.vulnerabilities, via), `unresolved advisory chain: ${via}`);
      } else {
        assert.equal(via.url, policy.advisory, 'new advisory requires review');
        assert.equal(via.name, 'sprintf-js');
        assert.equal(via.severity, 'moderate');
        matchedAdvisory = true;
      }
    }
    assert.ok(Array.isArray(finding.nodes) && finding.nodes.length > 0);
    for (const node of finding.nodes) {
      assert.equal(lock.packages[node]?.dev, true, `not build-only: ${node}`);
      assert.equal(lock.packages[node]?.version, policy.packages[name], `unreviewed version: ${node}`);
    }
  }
  assert.ok(Object.keys(full.vulnerabilities).length === 0 || matchedAdvisory, 'missing underlying reviewed advisory');
  return { status: Object.keys(full.vulnerabilities).length ? 'PASS_WITH_BUILD_ONLY_EXCEPTION' : 'PASS', acceptedEntries: Object.keys(full.vulnerabilities).length,
    runtimeAdvisories: 0, expiresOn: policy.expiresOn, advisory: policy.advisory };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  try {
    const [fullFile, runtimeFile] = process.argv.slice(2);
    assert.ok(Boolean(fullFile) === Boolean(runtimeFile), 'supply both FULL.json and RUNTIME.json or neither');
    const json = async file => JSON.parse(await readFile(file, 'utf8'));
    const audit = args => {
      const command = spawnSync(process.platform === 'win32' ? 'npm.cmd' : 'npm',
        ['audit', '--json', ...args], { encoding: 'utf8', shell: process.platform === 'win32' });
      assert.ok(!command.error && [0, 1].includes(command.status), 'dependency audit failed to execute');
      return JSON.parse(command.stdout);
    };
    const result = checkAdvisories(fullFile ? await json(fullFile) : audit([]),
      runtimeFile ? await json(runtimeFile) : audit(['--omit=dev']),
      await json(new URL('../package-lock.json', import.meta.url)),
      await json(new URL('../docs/security/build-tool-advisory-exceptions.json', import.meta.url)));
    console.log(JSON.stringify(result));
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
