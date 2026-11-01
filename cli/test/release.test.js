import assert from 'node:assert/strict';
import test from 'node:test';
import { describeRelease } from '../src/release.js';
import { validateDesktopIdentity } from '../../desktop/release-identity.mjs';

test('human release identity distinguishes base version and candidate', () => {
  assert.deepEqual(describeRelease('1.2.0-rc.2'), { version: '1.2.0', releaseVersion: '1.2.0-rc.2', channel: 'prerelease', name: 'Pre-release v1.2', label: '1.2.0 (Pre-release v1.2; 1.2.0-rc.2)' });
  assert.equal(describeRelease('1.2.0').label, '1.2.0');
  assert.throws(() => describeRelease('invalid'), /invalid release version/);
});

test('desktop staging rejects same-base stale candidate or channel', () => {
  const release = { version: '1.2.0', releaseVersion: '1.2.0-rc.2', channel: 'prerelease', tag: 'v1.2.0-rc.2', name: 'Pre-release v1.2', npmVersion: '1.2.0-rc.2' };
  const engine = { engineVersion: release.version, releaseVersion: release.releaseVersion, releaseChannel: release.channel, releaseTag: release.tag, releaseName: release.name };
  assert.doesNotThrow(() => validateDesktopIdentity(release, engine, release.npmVersion, release.npmVersion));
  for (const patch of [{ releaseVersion: '1.2.0-rc.0' }, { releaseChannel: 'stable' }, { engineVersion: '1.1.2' }, { releaseTag: 'v1.2.0' }]) {
    assert.throws(() => validateDesktopIdentity(release, { ...engine, ...patch }, release.npmVersion, release.npmVersion), /mismatch/);
  }
  assert.throws(() => validateDesktopIdentity(release, engine, '1.1.2', release.npmVersion), /mismatch/);
});
