import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createWriteStream } from 'node:fs';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { captureEngineLogs } from './lib/engine-log-capture.mjs';

test('lifecycle JSON stays intact when stdout arrives before its stderr newline', async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-log-capture-'));
  try {
    const stderrPath = path.join(root, 'stderr.log');
    const stdoutPath = path.join(root, 'stdout.log');
    const stderrLog = createWriteStream(stderrPath);
    const stdoutLog = createWriteStream(stdoutPath);
    const child = spawn(process.execPath, ['-e', `
      process.stderr.write(JSON.stringify({ code: 'engine_shutdown_complete', fields: { clean_shutdown: true } }), () => {
        process.stdout.write('[MAIN] Shutdown complete\\n', () => {
          setTimeout(() => process.stderr.write('\\n'), 50);
        });
      });
    `], { stdio: ['ignore', 'pipe', 'pipe'] });
    await captureEngineLogs(child, stderrLog, stdoutLog)();
    assert.equal(child.exitCode, 0);
    const eventText = await readFile(stderrPath, 'utf8');
    assert.deepEqual(JSON.parse(eventText), {
      code: 'engine_shutdown_complete', fields: { clean_shutdown: true },
    });
    assert.equal(await readFile(stdoutPath, 'utf8'), '[MAIN] Shutdown complete\n');
    assert.throws(() => JSON.parse(eventText.trimEnd() + '[MAIN] Shutdown complete\n'), SyntaxError,
      'the old combined-stream fixture must remain invalid JSON, not be accepted by a looser parser');
  } finally { await rm(root, { recursive: true, force: true }); }
});
