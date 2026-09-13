#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { access, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const packageRoot = path.resolve(process.argv[2] || '');
assert.ok(process.argv[2], 'usage: test-p0-installed.mjs PACKAGE_ROOT [EVIDENCE_JSON]');
const executable = process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb';
const candidates = [packageRoot, path.join(packageRoot, 'bin'),
  path.join(packageRoot, 'usr', 'bin'), path.join(packageRoot, 'Release')];
let binaryRoot;
for (const candidate of candidates) {
  try { await access(path.join(candidate, executable)); binaryRoot = candidate; break; }
  catch {}
}
assert.ok(binaryRoot, `PacificDB executable not found below ${packageRoot}`);
const temporaryBase = await mkdtemp(path.join(os.tmpdir(), 'PacificDB P0 తెలుగు '));
const isolatedTemp = path.join(temporaryBase, 'space root');
await mkdir(isolatedTemp, { recursive: true, mode: 0o700 });
const includeSoak = process.env.PACIFICDB_P0_SOAK === '1';
const environment = { ...process.env, TMPDIR: isolatedTemp, TMP: isolatedTemp, TEMP: isolatedTemp,
  PACIFICDB_P0_WRITES: process.env.PACIFICDB_P0_WRITES || '10000',
  PACIFICDB_P0_CLIENTS: process.env.PACIFICDB_P0_CLIENTS || '8',
  PACIFICDB_P0_SOAK: '0' };
function run(command, args, env = environment) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd: repositoryRoot, env });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk; });
    child.stderr.on('data', (chunk) => { output += chunk; });
    child.once('error', reject);
    child.once('close', (code) => code === 0 ? resolve(output) :
      reject(new Error(`${command} exited ${code}\n${output}`)));
  });
}
async function removeTemporaryBase() {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    try {
      await rm(temporaryBase, { recursive: true, force: true });
      return;
    } catch (error) {
      if (!['EBUSY', 'EPERM', 'ENOTEMPTY'].includes(error.code)) throw error;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  console.error(`preserved locked installed-suite root: ${temporaryBase}`);
}
const evidence = { schema: 'pacificdb.p0-evidence.v1', platform: process.platform,
  architecture: process.arch, package_root: packageRoot, binary_root: binaryRoot,
  executable: path.join(binaryRoot, executable), started_at: new Date().toISOString(), tests: [] };
try {
  const version = (await run(path.join(binaryRoot, executable), ['--version'])).trim();
  assert.match(version, /^PacificDB 0\.1\.0-beta\.12$/);
  evidence.version = version;
  const cases = [
    ['protocol', 'test-p0-protocol-errors.mjs'],
    ['discovery', 'test-p0-discovery.mjs'],
    ['lifecycle', 'test-p0-client-lifecycle.mjs'],
    ['load', 'test-p0-load.mjs'],
    ['media', 'test-p0-media.mjs'],
  ];
  if (includeSoak) cases.push(['soak', 'test-p0-load.mjs', { PACIFICDB_P0_SOAK: '1' }]);
  for (const [name, script, overrides = {}] of cases) {
    console.error(`[p0-installed] starting ${name}`);
    const began = Date.now();
    const output = await run(process.execPath,
      [path.join(repositoryRoot, 'scripts', script), binaryRoot],
      { ...environment, ...overrides });
    evidence.tests.push({ name, exit_code: 0, elapsed_ms: Date.now() - began,
      result: output.trim().split('\n').at(-1) });
    console.error(`[p0-installed] passed ${name}`);
  }
  evidence.status = 'PASS';
  evidence.completed_at = new Date().toISOString();
  const evidencePath = path.resolve(process.argv[3] ||
    path.join(repositoryRoot, `p0-evidence-${process.platform}-${process.arch}.json`));
  await writeFile(evidencePath, JSON.stringify(evidence, null, 2) + '\n', { mode: 0o600 });
  console.log(JSON.stringify({ status: 'PASS', evidence: evidencePath,
    tests: evidence.tests.length, version }));
} finally {
  if (process.env.PACIFICDB_KEEP_P0_ROOT !== '1') {
    await removeTemporaryBase();
  } else {
    console.error(`preserved installed-suite root: ${temporaryBase}`);
  }
}
