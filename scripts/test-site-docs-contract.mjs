import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { promisify } from 'node:util';

const execute = promisify(execFile);
async function fixture(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-site-contract-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, 'scripts')); await mkdir(path.join(root, 'cli'));
  await cp('scripts/test-site-docs.mjs', path.join(root, 'scripts/test-site-docs.mjs'));
  await cp('cli/package.json', path.join(root, 'cli/package.json'));
  await cp('site', path.join(root, 'site'), { recursive: true });
  return { root, script: path.join(root, 'scripts/test-site-docs.mjs'), indexPath: path.join(root, 'site/index.html') };
}

test('website validation rejects missing files, fragments, assets, versions and download links', async t => {
  const { root, script, indexPath } = await fixture(t);
  const index = await readFile(indexPath, 'utf8');
  const identity = JSON.parse(await readFile(path.join(root, 'site/release.json'), 'utf8'));
  const version = identity.releaseVersion || identity.candidate, tag = identity.tag;
  assert.match((await execute(process.execPath, [script])).stdout, /checks passed/);
  for (const [extra, expected] of [
    ['<a href="missing.txt">missing file</a>', 'ENOENT'],
    ['<a href="missing.html#anchor">missing page</a>', 'ENOENT'],
    ['<a href="docs.html#missing-anchor">missing fragment</a>', 'missing fragment target'],
    ['<!-- hitesh-reddy-k.github.io/pacificdb-community/docs.html -->', 'AssertionError'],
    ['<p>Install version 1.1.1</p>', 'current guides must use the published release'],
    [`<a href="https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/${tag}/unknown.exe">unpublished</a>`, 'unpublished release download'],
    ['<a href="https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v0.0.0/SHA256SUMS">wrong tag</a>', 'download tags must match'],
  ]) {
    await writeFile(indexPath, index + extra);
    await assert.rejects(execute(process.execPath, [script]), e => e.code === 1 && e.stderr.includes(expected));
  }
  for (const [original, replacement, expected] of [
    [`${tag}/PacificDB-Workbench-${version}-linux-amd64.deb`, `${tag}/missing-linux.deb`, 'Workbench must link the published linux-amd64.deb installer'],
    [`${tag}/PacificDB-Workbench-${version}-win-x64.exe`, `${tag}/missing-win.exe`, 'Workbench must link the published win-x64.exe installer'],
    [`${tag}/SHA256SUMS`, `${tag}/wrong-checksums.txt`, 'Workbench must link its release checksum manifest'],
    [`${tag}/PacificDB-Workbench-${version}-mac-arm64.dmg`, `${tag}/missing-arm64.dmg`, 'Workbench must link the published mac-arm64.dmg installer'],
    [`${tag}/PacificDB-Workbench-${version}-mac-x64.dmg`, `${tag}/missing-x64.dmg`, 'Workbench must link the published mac-x64.dmg installer'],
    ['<select id="mac-arch">', '<select id="mac-arch" disabled>', 'AssertionError'],
  ]) {
    const changed = index.replaceAll(original, replacement);
    assert.notEqual(changed, index, 'fixture must change the rendered HTML');
    await writeFile(indexPath, changed);
    await assert.rejects(execute(process.execPath, [script]), e => e.code === 1 && e.stderr.includes(expected));
  }
  await writeFile(indexPath, index);
  await rm(path.join(root, 'site/assets/pacificdb-logo-symbol.png'));
  await assert.rejects(execute(process.execPath, [script]), e => e.code === 1 && e.stderr.includes('ENOENT'));
});

test('stable website identity passes while a mismatched tag or channel still fails', async t => {
  const { root, script } = await fixture(t);
  const metadataPath = path.join(root, 'site/release.json');
  const original = JSON.parse(await readFile(metadataPath, 'utf8'));
  const oldVersion = original.releaseVersion || original.candidate;
  const version = original.version;
  const stable = { ...original, releaseVersion: version, candidate: version, channel: 'stable', tag: `v${version}`,
    assets: original.assets.map(name => name.replaceAll(oldVersion, version)) };
  for (const name of ['index.html', 'docs.html', `release-${original.version}.html`]) {
    const file = path.join(root, 'site', name);
    await writeFile(file, (await readFile(file, 'utf8')).replaceAll(oldVersion, version)
      .replace(/pre-release|prerelease/gi, 'release'));
  }
  await writeFile(metadataPath, JSON.stringify(stable));
  assert.match((await execute(process.execPath, [script])).stdout, /checks passed/);
  for (const changes of [{tag:'v0.0.0'}, {channel:'unknown'}, {releaseVersion:version+'-rc.9'}]) {
    await writeFile(metadataPath, JSON.stringify({ ...stable, ...changes }));
    await assert.rejects(execute(process.execPath, [script]));
  }
});
