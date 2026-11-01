import assert from 'node:assert/strict';

export function validateDesktopIdentity(release, engine, workbenchVersion, cliVersion) {
  assert.equal(workbenchVersion, release.npmVersion, 'Workbench release version mismatch');
  assert.equal(cliVersion, release.npmVersion, 'CLI release version mismatch');
  for (const [field, expected] of Object.entries({ engineVersion: release.version,
    releaseVersion: release.releaseVersion, releaseChannel: release.channel,
    releaseTag: release.tag, releaseName: release.name })) {
    assert.equal(engine[field], expected, `bundled engine ${field} mismatch`);
  }
}
