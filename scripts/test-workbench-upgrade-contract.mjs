#!/usr/bin/env node
import assert from 'node:assert/strict';
import { access, mkdtemp, readdir, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const executable = path.resolve(process.env.PACIFICDB_TEST_DESKTOP ||
  'dist/desktop/linux-unpacked/pacificdb-workbench');
await access(executable);
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-upgrade-contract-'));
const data = path.join(root, 'owned-data');

try {
  const result = spawnSync(process.execPath, ['scripts/test-workbench-desktop.mjs'], {
    cwd: path.resolve('.'), encoding: 'utf8', timeout: 120_000,
    env: { ...process.env, PACIFICDB_TEST_DESKTOP: executable,
      PACIFICDB_TEST_DESKTOP_PREVIOUS: executable,
      PACIFICDB_TEST_DESKTOP_DATA: data },
  });
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /WORKBENCH_DESKTOP_UPGRADE_PASS/);
  assert.ok((await readdir(data)).includes('database'),
    'caller-owned data must contain the upgraded database');
  console.log('WORKBENCH_UPGRADE_CONTRACT_PASS');
} finally {
  await rm(root, { recursive: true, force: true });
}
