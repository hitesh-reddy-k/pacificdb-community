import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const script = fileURLToPath(new URL('./workbench-preview-evidence.mjs', import.meta.url));
const release = { schemaVersion: 1, version: '1.2.0', releaseVersion: '1.2.0-rc.1',
  name: 'Pre-release v1.2', channel: 'prerelease', tag: 'v1.2.0-rc.1', npmVersion: '1.2.0-rc.1' };
test('preview staging binds native bytes and identity without claiming stable qualification', async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), 'workbench-preview-contract-'));
  try {
    execFileSync('git', ['init', '-q'], { cwd: root });
    await writeFile(path.join(root, 'tracked'), 'candidate\n');
    execFileSync('git', ['add', 'tracked'], { cwd: root });
    execFileSync('git', ['-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'candidate'], { cwd: root });
    const revision = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim();
    for (const name of ['desktop/stage', 'desktop/resources/engine', 'cli']) await mkdir(path.join(root, name), { recursive: true });
    for (const name of ['release.json', 'desktop/stage/package.json', 'cli/package.json']) await writeFile(path.join(root, name), JSON.stringify(name === 'release.json' ? release : { version: release.npmVersion }));
    const native = process.platform === 'win32' ? ['db_engine.exe', 'pacificdb.exe'] : ['db_engine', 'pacificdb'];
    for (const name of native) await writeFile(path.join(root, 'desktop/resources/engine', name), name);
    const hash = text => createHash('sha256').update(text).digest('hex');
    const manifest = { release, sourceRevision: revision, engineIdentity: { engineVersion: release.version,
      releaseVersion: release.releaseVersion, releaseChannel: release.channel, releaseName: release.name,
      releaseTag: release.tag, gitCommit: revision }, engineSha256: hash(native[0]), cliSha256: hash(native[1]) };
    const manifestFile = path.join(root, 'desktop/resources/engine/manifest.json');
    await writeFile(manifestFile, JSON.stringify(manifest));
    await writeFile(path.join(root, 'candidate.deb'), 'preview artifact bytes');
    const run = () => spawnSync(process.execPath, [script, 'linux-x64', 'candidate.deb', 'evidence/preview.json'], { cwd: root, encoding: 'utf8' });
    let result = run();
    assert.equal(result.status, 0, result.stderr);
    const evidence = JSON.parse(await readFile(path.join(root, 'evidence/preview.json'), 'utf8'));
    assert.equal(evidence.status, 'STAGED');
    assert.equal(evidence.sourceDirty, true);
    assert.equal(evidence.stableQualification.status, 'NOT_QUALIFIED');
    assert.equal(evidence.stableQualification.physicalPower, 'NOT_TESTED');
    assert.equal(evidence.artifact.sha256, hash('preview artifact bytes'));
    await writeFile(path.join(root, 'desktop/resources/engine', native[0]), 'tampered');
    result = run();
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /digest mismatch/);
    await writeFile(path.join(root, 'desktop/resources/engine', native[0]), native[0]);
    manifest.engineIdentity.releaseVersion = release.version;
    await writeFile(manifestFile, JSON.stringify(manifest));
    result = run();
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /releaseVersion mismatch/);
  } finally { await rm(root, { recursive: true, force: true }); }
});
