#!/usr/bin/env node
import { cp, mkdir, readFile, rm, writeFile, access, chmod } from 'node:fs/promises';
import { constants } from 'node:fs';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const workspace = JSON.parse(await readFile(path.join(root, 'package.json'), 'utf8'));
const cli = JSON.parse(await readFile(path.join(root, 'cli/package.json'), 'utf8'));
const executableName = process.platform === 'win32' ? 'db_engine.exe' : 'db_engine';
const executable = path.resolve(process.env.PACIFICDB_WORKBENCH_ENGINE || path.join(root, 'build', executableName));
await access(executable, process.platform === 'win32' ? constants.F_OK : constants.X_OK);
const cliName = process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb';
const cliExecutable = path.join(path.dirname(executable), cliName);
await access(cliExecutable, process.platform === 'win32' ? constants.F_OK : constants.X_OK);
const engineIdentity = JSON.parse(execFileSync(executable, ['--build-info'], { encoding: 'utf8' }));
const sourceRevision = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim();
assert.equal(engineIdentity.engineVersion, cli.version, 'bundled engine component version mismatch');
if (process.env.PACIFICDB_DESKTOP_RELEASE === '1') {
  execFileSync('git', ['diff', '--exit-code', 'HEAD'], { cwd: root, stdio: 'pipe' });
  assert.equal(engineIdentity.gitCommit, sourceRevision, 'release engine must match the source revision');
}
const digest = async file => createHash('sha256').update(await readFile(file)).digest('hex');
const stage = path.join(root, 'desktop/stage');
const resources = path.join(root, 'desktop/resources');
await rm(stage, { recursive: true, force: true });
await rm(resources, { recursive: true, force: true });
await mkdir(path.join(stage, 'desktop'), { recursive: true });
await mkdir(path.join(stage, 'cli/src'), { recursive: true });
await mkdir(path.join(resources, 'engine'), { recursive: true });
for (const name of ['main.mjs', 'engine.mjs', 'icon.png', 'logo.png', 'loading.html', 'loading.css']) {
  await cp(path.join(root, 'desktop', name), path.join(stage, 'desktop', name));
}
await cp(path.join(root, 'cli/src/workbench.js'), path.join(stage, 'cli/src/workbench.js'));
await cp(path.join(root, 'cli/workbench'), path.join(stage, 'cli/workbench'), { recursive: true });
const sdk = path.join(stage, 'node_modules/@pacificdb/client');
await mkdir(sdk, { recursive: true });
for (const name of ['src', 'package.json', 'LICENSE']) {
  await cp(path.join(root, 'sdk/node', name), path.join(sdk, name), { recursive: true });
}
for (const name of ['LICENSE', 'LICENSES', 'THIRD_PARTY_NOTICES.md']) {
  await cp(path.join(root, name), path.join(stage, name), { recursive: true });
}
await cp(executable, path.join(resources, 'engine', executableName));
await cp(cliExecutable, path.join(resources, 'engine', cliName));
await writeFile(path.join(resources, 'engine', 'manifest.json'), JSON.stringify({
  schemaVersion: 1, sourceRevision, engineIdentity,
  engineSha256: await digest(executable), cliSha256: await digest(cliExecutable),
}, null, 2) + '\n');
if (process.platform !== 'win32') await chmod(path.join(resources, 'engine', executableName), 0o755);
// Windows builds must ship the runtime DLLs from the native engine build.
if (process.env.PACIFICDB_WORKBENCH_ENGINE_LIBS) {
  await cp(path.resolve(process.env.PACIFICDB_WORKBENCH_ENGINE_LIBS), path.join(resources, 'engine'), { recursive: true });
}
await writeFile(path.join(stage, 'package.json'), JSON.stringify({
  name: 'pacificdb-workbench', version: workspace.version, private: true, type: 'module',
  desktopName: 'pacificdb-workbench.desktop',
  description: 'PacificDB desktop database workspace', main: 'desktop/main.mjs',
  author: 'PacificDB Community', homepage: 'https://pacificdb.in',
  license: 'AGPL-3.0', dependencies: { '@pacificdb/client': cli.version },
}, null, 2) + '\n');
console.log(`Prepared desktop ${workspace.version} with PacificDB ${cli.version} from ${executable}`);
