#!/usr/bin/env node
import assert from 'node:assert/strict';
import { chmod, mkdir, mkdtemp, readFile, rm, readdir, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { startDesktopEngine } from '../desktop/engine.mjs';
import { PacificDBClient } from '../sdk/node/src/index.js';

const build = path.resolve(process.argv[2] || 'build');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-desktop-isolation-'));
const names = ['BACKUP_DIR', 'SST_DIR', 'SNAPSHOT_DIR', 'TMP_DIR', 'LOG_DIR', 'RAFT_LOG_PATH'];
const saved = Object.fromEntries(names.map((name) => [name, process.env[name]]));
let engine;
try {
  for (const name of names) process.env[name] = path.join(root, 'external', name);
  engine = await startDesktopEngine({ executable: path.join(build,
    process.platform === 'win32' ? 'db_engine.exe' : 'db_engine'), directory: path.join(root, 'desktop') });
  const raftReady = /\[RAFTCORE\] Listener ready on 127\.0\.0\.1:/;
  const listenerDeadline = Date.now() + 5000;
  let startupLog;
  do {
    startupLog = await readFile(engine.logPath, 'utf8');
    if (raftReady.test(startupLog)) break;
    await new Promise(resolve => setTimeout(resolve, 25));
  } while (Date.now() < listenerDeadline);
  assert.match(startupLog, /\[SERVER\] Listening on 127\.0\.0\.1:/);
  assert.match(startupLog, raftReady);
  const client = new PacificDBClient({ port: engine.port, poolSize: 1 });
  try {
    const storage = await client.request({ action: 'storage_stats' });
    assert.equal(storage.success, true);
    assert.equal(typeof storage.sst_files, 'number');
    const wal = await client.request({ action: 'admin_wal_status' });
    assert.equal(wal.success, true);
    assert.equal(wal.checksum_validation_expected, true);
    const verified = await client.request({ action: 'verify_integrity' });
    assert.equal(verified.success, true);
    assert.equal(verified.readOnly, true);
    const repaired = await client.request({ action: 'storage_repair' });
    assert.equal(repaired.success, true);
    assert.equal(repaired.repairPerformed, true);
    console.log('DESKTOP_STORAGE_DISPATCH_PASS');
  } finally { client.close(); }
  await engine.stop();
  assert.match(await readFile(engine.logPath, 'utf8'), /Clean shutdown marker v2 written/);
  assert.ok(!(await readdir(root)).includes('external'), 'desktop must not write inherited engine paths');
  console.log('DESKTOP_ENGINE_ISOLATION_AND_CLEAN_SHUTDOWN_PASS');
  if (process.platform !== 'win32') {
    const descriptorsBefore = process.platform === 'linux' ? (await readdir('/proc/self/fd')).length : null;
    const executable = path.join(root, 'failing-engine');
    for (const symlink of [false, true]) {
      const failedRoot = path.join(root, symlink ? 'failed-symlink' : 'failed-startup');
      await writeFile(executable, `#!/usr/bin/env node
const { renameSync, symlinkSync, writeFileSync, writeSync } = require('node:fs');
const path = require('node:path');
writeSync(2, '中'.repeat(2100) + 'original engine startup failure\\n');
const logPath = path.join(process.env.PACIFICDB_HOME, 'engine.log');
renameSync(logPath, logPath + '.original');
const replacement = ${symlink} ? logPath + '.replacement' : logPath;
writeFileSync(replacement, 'replacement log must not become diagnostics\\n');
if (${symlink}) symlinkSync(replacement, logPath);
process.exit(1);
`, { mode: 0o700 });
      await assert.rejects(startDesktopEngine({ executable, directory: failedRoot }), (error) => {
        assert.ok(error.message.endsWith('中'.repeat(1968) + 'original engine startup failure\n'));
        assert.doesNotMatch(error.message, /replacement log must not become diagnostics/);
        return true;
      });
    }
    await assert.rejects(startDesktopEngine({ executable: path.join(root, 'missing-engine'),
      directory: path.join(root, 'failed-spawn') }), /ENOENT/);
    await writeFile(executable, '#!/usr/bin/env node\nsetInterval(() => {}, 1000);\n', { mode: 0o700 });
    await assert.rejects(startDesktopEngine({ executable, directory: path.join(root, 'timed-out'),
      timeoutMs: 500 }), /did not become ready/);
    const cancel = new AbortController();
    cancel.abort();
    await assert.rejects(startDesktopEngine({ executable, directory: path.join(root, 'canceled'),
      signal: cancel.signal }), /Startup canceled/);
    if (descriptorsBefore !== null) {
      assert.ok((await readdir('/proc/self/fd')).length <= descriptorsBefore,
        'startup failures must close their parent log descriptors');
    }
    console.log('DESKTOP_ENGINE_ORIGINAL_LOG_DIAGNOSTICS_PASS');
    if (process.getuid() !== 0) {
      const directory = path.join(root, 'write-only-log');
      await mkdir(directory);
      const originalLog = path.join(directory, 'engine.log');
      await writeFile(originalLog, 'existing write-only log\n', { mode: 0o200 });
      const writeOnlyEngine = await startDesktopEngine({ executable: path.join(build, 'db_engine'), directory });
      try {
        assert.notEqual(writeOnlyEngine.logPath, originalLog,
          'an unreadable original log must use a new exclusive diagnostic log');
      } finally { await writeOnlyEngine.stop(); }
      assert.match(await readFile(writeOnlyEngine.logPath, 'utf8'), /Clean shutdown marker v2 written/);
      await writeFile(executable, '#!/usr/bin/env node\nprocess.stderr.write("exclusive fallback failure\\n");\nprocess.exit(1);\n', { mode: 0o700 });
      await assert.rejects(startDesktopEngine({ executable, directory }), (error) => {
        assert.ok(error.message.endsWith('exclusive fallback failure\n'));
        assert.match(error.message, /Engine log: .*engine-[0-9a-f-]+\.log\n/);
        return true;
      });
      await chmod(originalLog, 0o600);
      assert.equal(await readFile(originalLog, 'utf8'), 'existing write-only log\n');
      console.log('DESKTOP_ENGINE_WRITE_ONLY_LOG_COMPATIBILITY_PASS');
    }
  }
} finally {
  await engine?.stop();
  for (const name of names) {
    if (saved[name] === undefined) delete process.env[name];
    else process.env[name] = saved[name];
  }
  await rm(root, { recursive: true, force: true });
}
