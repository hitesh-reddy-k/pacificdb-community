import assert from 'node:assert/strict';
import test from 'node:test';
import { checkAdvisories } from './check-build-tool-advisories.mjs';

const fixture = () => ({
  full: { metadata: { vulnerabilities: { total: 1 } }, vulnerabilities: {
    'sprintf-js': { severity: 'moderate', via: [{ name: 'sprintf-js', severity: 'moderate', url: 'known-advisory' }], nodes: ['node_modules/sprintf-js'] },
  } },
  runtime: { metadata: { vulnerabilities: { total: 0 } }, vulnerabilities: {} },
  lock: { packages: { 'node_modules/sprintf-js': { version: '1.1.3', dev: true } } },
  policy: { schemaVersion: 1, expiresOn: '2026-11-06', advisory: 'known-advisory', packages: { 'sprintf-js': '1.1.3' } },
});
const check = f => checkAdvisories(f.full, f.runtime, f.lock, f.policy, new Date('2026-10-07'));
test('accepts only the reviewed build-only advisory', () => assert.equal(check(fixture()).acceptedEntries, 1));
test('rejects runtime, new/high advisories, changed versions and non-dev packages', () => {
  for (const change of [
    f => { f.runtime.metadata.vulnerabilities.total = 1; },
    f => { f.runtime.vulnerabilities.unreviewed = {}; },
    f => { f.full.vulnerabilities['sprintf-js'].severity = 'high'; },
    f => { f.full.vulnerabilities['sprintf-js'].via[0].url = 'new-advisory'; },
    f => { f.lock.packages['node_modules/sprintf-js'].version = '1.1.2'; },
    f => { f.lock.packages['node_modules/sprintf-js'].dev = false; },
  ]) { const f = fixture(); change(f); assert.throws(() => check(f)); }
});
test('exception expires and malformed evidence fails closed', () => {
  const f = fixture();
  assert.throws(() => checkAdvisories(f.full, f.runtime, f.lock, f.policy, new Date('2026-11-06')));
  delete f.full.metadata; assert.throws(() => check(f));
});
test('no waiver is needed once the entire dependency audit is clean', () => {
  const f = fixture(); f.full.vulnerabilities = {}; f.full.metadata.vulnerabilities.total = 0;
  assert.equal(checkAdvisories(f.full, f.runtime, f.lock, f.policy, new Date('2027-01-01')).acceptedEntries, 0);
});
