import assert from 'node:assert/strict';
import test from 'node:test';
import { execFileSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, readlink, rm, writeFile, symlink } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { installTerminalCommand } from '../desktop/terminal-command.mjs';

async function fixture(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-terminal-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const home = path.join(root, "home with spaces 'quotes'");
  await mkdir(home);
  const executable = path.join(root, 'Application with spaces', 'pacificdb');
  await mkdir(path.dirname(executable));
  await writeFile(executable, '#!/bin/sh\nprintf "PacificDB 1.2.0\\n"\n', { mode: 0o755 });
  return { home, executable };
}

test('Unix terminal setup installs bundled CLI and preserves shell profiles idempotently', { skip: process.platform === 'win32' }, async t => {
  const opts = { ...await fixture(t), platform: 'darwin', shell: '/bin/zsh' };
  await writeFile(path.join(opts.home, '.zshrc'), '# existing preferences\n');
  const result = await installTerminalCommand(opts);
  assert.equal(await readlink(result.command), opts.executable);
  const first = await readFile(path.join(opts.home, '.zshrc'), 'utf8');
  assert.ok(first.startsWith('# existing preferences\n'));
  await installTerminalCommand(opts);
  assert.equal(await readFile(path.join(opts.home, '.zshrc'), 'utf8'), first);
  assert.match(await readFile(path.join(opts.home, '.zprofile'), 'utf8'), /PacificDB terminal command/);
});

test('Unix setup never replaces a different command or writes through profile symlinks', { skip: process.platform === 'win32' }, async t => {
  const opts = { ...await fixture(t), platform: 'linux', shell: '/bin/bash' };
  await mkdir(path.join(opts.home, '.local/bin'), { recursive: true });
  const command = path.join(opts.home, '.local/bin/pacificdb');
  await writeFile(command, 'unrelated command');
  await assert.rejects(installTerminalCommand(opts), /already exists/);
  assert.equal(await readFile(command, 'utf8'), 'unrelated command');
  await rm(command);
  const protectedFile = path.join(opts.home, 'untouched');
  await writeFile(protectedFile, 'keep');
  await symlink(protectedFile, path.join(opts.home, '.bashrc'));
  await assert.rejects(installTerminalCommand(opts), /regular file/);
  assert.equal(await readFile(protectedFile, 'utf8'), 'keep');
});

test('Windows setup delegates to bundled PATH helper without shell interpolation', async t => {
  const opts = await fixture(t);
  const calls = [];
  const result = await installTerminalCommand({ ...opts, platform: 'win32',
    run: async (...args) => { calls.push(args); } });
  assert.equal(calls.length, 1);
  assert.equal(calls[0][1].at(-2), 'add');
  assert.equal(calls[0][1].at(-1), path.dirname(opts.executable));
  assert.equal(calls[0][2].shell, false);
  assert.equal(result.command, opts.executable);
});

test('a new POSIX shell resolves pacificdb --version with spaces and quotes in the installation path', { skip: process.platform === 'win32' }, async t => {
  const opts = { ...await fixture(t), platform: 'linux', shell: '/bin/bash' };
  await installTerminalCommand(opts);
  const output = execFileSync('/bin/sh', ['-c', '. "$1"; pacificdb --version', 'sh', path.join(opts.home, '.profile')],
    { encoding: 'utf8', env: { ...process.env, PACIFICDB_URL: 'invalid://offline-version' } });
  assert.equal(output, 'PacificDB 1.2.0\n');
});

test('bash setup preserves and updates the login profile Bash actually reads', { skip: process.platform === 'win32' }, async t => {
  for (const login of ['.bash_profile', '.bash_login']) {
    const opts = { ...await fixture(t), platform: 'linux', shell: '/bin/bash' };
    await writeFile(path.join(opts.home, login), '# existing login settings\n');
    const result = await installTerminalCommand(opts);
    assert.ok(result.profiles.includes(path.join(opts.home, login)));
    assert.ok((await readFile(path.join(opts.home, login), 'utf8')).startsWith('# existing login settings\n'));
    const output = execFileSync('/bin/bash', ['--noprofile', '-c', '. "$1"; pacificdb --version', 'bash', path.join(opts.home, login)],
      { encoding: 'utf8' });
    assert.equal(output, 'PacificDB 1.2.0\n');
  }
});
