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
  ]) {
    await writeFile(indexPath, index + extra);
    await assert.rejects(execFileAsync(process.execPath, [script]), (error) =>
      error.code === 1 && error.stderr.includes(expected));
  }
  await writeFile(indexPath, index);
  await rm(path.join(root, 'site/assets/pacificdb-logo-symbol.png'));
  await assert.rejects(execFileAsync(process.execPath, [script]), (error) =>
    error.code === 1 && error.stderr.includes('ENOENT'));
});
