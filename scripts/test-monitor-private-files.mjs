#!/usr/bin/env node
import assert from 'node:assert/strict';
import test from 'node:test';
import { mkdtemp, writeFile, readFile, chmod, symlink, rm, readdir, mkdir, stat } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import os from 'node:os';
import path from 'node:path';
import { privateFile } from './audit-health-monitor.mjs';
import { atomicWrite } from './replica-integrity-monitor.mjs';

test('monitor validates the opened private file and rejects public files, links and FIFOs',
  { skip: process.platform === 'win32' }, async t => {
    const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-private-input-'));
    t.after(() => rm(root, { recursive: true, force: true }));
    const token = path.join(root, 'token');
    await writeFile(token, 'fixture-token', { mode: 0o600 });
    assert.equal(await privateFile(token), 'fixture-token');
    await chmod(token, 0o644);
    await assert.rejects(privateFile(token), /mode 0600/);
    await chmod(token, 0o600);
    const link = path.join(root, 'link'); await symlink(token, link);
    await assert.rejects(privateFile(link), /ELOOP|symbolic link/);
    const fifo = path.join(root, 'fifo'); execFileSync('mkfifo', [fifo]);
    await assert.rejects(privateFile(fifo), /regular file/);
    await assert.rejects(privateFile(root), /regular file/);
  });

test('monitor output uses private exclusive temporary files and cleans failed publications', async t => {
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-monitor-output-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const output = path.join(root, 'result.json');
  const sentinel = path.join(root, 'sentinel');
  await writeFile(sentinel, 'preserve');
  // The previous predictable name must never be followed or truncated.
  const previous = `${output}.tmp-${process.pid}`;
  if (process.platform !== 'win32') await symlink(sentinel, previous);
  else await writeFile(previous, 'preserve');
  await Promise.all(Array.from({ length: 12 }, (_, id) => atomicWrite(output, JSON.stringify({ id }))));
  assert.ok(Number.isInteger(JSON.parse(await readFile(output, 'utf8')).id));
  assert.equal(await readFile(sentinel, 'utf8'), 'preserve');
  if (process.platform !== 'win32') assert.equal((await stat(output)).mode & 0o077, 0);
  const directory = path.join(root, 'directory'); await mkdir(directory);
  await assert.rejects(atomicWrite(directory, 'cannot replace directory'));
  assert.deepEqual((await readdir(root)).filter(name => name.includes('.tmp-')), [path.basename(previous)]);
});
