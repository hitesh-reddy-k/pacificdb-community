import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);

test('website validation still rejects missing files, fragments, assets and obsolete links', async (t) => {
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-site-contract-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, 'scripts'));
  await mkdir(path.join(root, 'cli'));
  await cp('scripts/test-site-docs.mjs', path.join(root, 'scripts/test-site-docs.mjs'));
  await cp('cli/package.json', path.join(root, 'cli/package.json'));
  await cp('site', path.join(root, 'site'), { recursive: true });
  const script = path.join(root, 'scripts/test-site-docs.mjs');
  const indexPath = path.join(root, 'site/index.html');
  const index = await readFile(indexPath, 'utf8');
  assert.match((await execFileAsync(process.execPath, [script])).stdout, /checks passed/);
  for (const [extra, expected] of [
    ['<a href="missing.txt">missing file</a>', 'ENOENT'],
    ['<a href="missing.html#anchor">missing page</a>', 'ENOENT'],
    ['<a href="docs.html#missing-anchor">missing fragment</a>', 'missing fragment target'],
    ['<!-- hitesh-reddy-k.github.io/pacificdb-community/docs.html -->', 'AssertionError'],
    ['<p>Install version 1.1.1</p>', 'current guides must use the RC2 release'],
    ['<a href="https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/pacificdb-community-1.2.0-rc.2-windows-x64.exe">unpublished</a>', 'unpublished release download'],
    ['<a href="https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0/SHA256SUMS">wrong tag</a>', 'download tags must match'],
  ]) {
    await writeFile(indexPath, index + extra);
    await assert.rejects(execFileAsync(process.execPath, [script]), (error) =>
      error.code === 1 && error.stderr.includes(expected));
  }
  for (const [changed, expected] of [
    [index.replace('v1.2.0-rc.2/PacificDB-Workbench-1.2.0-rc.2-linux-amd64.deb',
      'v1.2.0-rc.2/pacificdb-community-1.2.0-rc.2-linux-x64.deb'), 'Workbench must link the published linux-amd64.deb installer'],
    [index.replace('v1.2.0-rc.2/PacificDB-Workbench-1.2.0-rc.2-win-x64.exe',
      'v1.2.0-rc.2/missing-win-x64.exe'), 'Workbench must link the published win-x64.exe installer'],
    [index.replace('v1.2.0-rc.2/SHA256SUMS', 'v1.2.0-rc.2/wrong-checksums.txt'),
      'Workbench must link its release checksum manifest'],
    [index.replace('v1.2.0-rc.2/PacificDB-Workbench-1.2.0-rc.2-mac-arm64.dmg',
      'v1.2.0-rc.2/missing-mac-arm64.dmg'), 'Workbench must link the published mac-arm64.dmg installer'],
    [index.replace('v1.2.0-rc.2/PacificDB-Workbench-1.2.0-rc.2-mac-x64.dmg',
      'v1.2.0-rc.2/missing-mac-x64.dmg'), 'Workbench must link the published mac-x64.dmg installer'],
    [index.replace('<select id="mac-arch">', '<select id="mac-arch" disabled>'),
      'AssertionError'],
  ]) {
    assert.notEqual(changed, index, 'download regression fixture must change the rendered HTML');
    await writeFile(indexPath, changed);
    await assert.rejects(execFileAsync(process.execPath, [script]), (error) =>
      error.code === 1 && error.stderr.includes(expected));
  }
  await writeFile(indexPath, index);
  await rm(path.join(root, 'site/assets/pacificdb-logo-symbol.png'));
  await assert.rejects(execFileAsync(process.execPath, [script]), (error) =>
    error.code === 1 && error.stderr.includes('ENOENT'));
});
