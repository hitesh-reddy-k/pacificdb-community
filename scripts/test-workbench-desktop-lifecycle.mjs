import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import test from 'node:test';
import { closeDesktopApplication } from './lib/workbench-desktop-lifecycle.mjs';

test('cleanup returns when its desktop process has already exited', { timeout: 10_000 }, async () => {
  const child = spawn(process.execPath, ['-e', 'process.exit(0)']);
  await once(child, 'exit');
  await closeDesktopApplication({ process: () => child,
    close: async () => { throw new Error('already exited'); } }, 100);
});

test('cleanup rejects and kills its own stuck desktop process', { timeout: 10_000 }, async () => {
  const child = spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { detached: true });
  const exited = once(child, 'exit');
  try {
    await assert.rejects(closeDesktopApplication({ process: () => child,
      close: () => new Promise(() => {}) }, 100), /did not complete within/);
    await exited;
    assert.ok(child.exitCode !== null || child.signalCode !== null);
  } finally { if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL'); }
});

test('cleanup lets the desktop close API shut its process down cleanly', { timeout: 10_000 }, async () => {
  const child = spawn(process.execPath, ['-e', "process.on('message', () => process.exit(0))"],
    { stdio: ['ignore', 'ignore', 'ignore', 'ipc'] });
  const exited = once(child, 'exit');
  try {
    await closeDesktopApplication({ process: () => child, close: async () => {
      child.send('quit');
      await exited;
    } }, 1000);
    assert.equal(child.exitCode, 0);
  } finally { if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL'); }
});
