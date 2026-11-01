import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { patchBuildToolCompat } from './patch-build-tool-compat.mjs';

const root = process.env.PACIFICDB_DEPENDENCY_ROOT || fileURLToPath(new URL('../', import.meta.url));
const require = createRequire(path.join(root, 'package.json'));

async function fixture(t) {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-build-compat-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  for (const [name, content] of Object.entries({
    'source/main.js': 'export const value = 42;\n',
    'source/native/addon.node': 'native fixture',
    'source/native/skip.txt': 'skip fixture',
    'source/.hidden.json': '{}\n',
    'source/sub/data.json': '{"value":42}\n',
  })) {
    await mkdir(path.dirname(path.join(dir, name)), { recursive: true });
    await writeFile(path.join(dir, name), content);
  }
  return dir;
}

// These exercise the actual package operations affected by minimatch's export change.
test('ASAR packaging preserves unpack globs with the patched matcher', async t => {
  const dir = await fixture(t);
  const asar = require('@electron/asar');
  const archive = path.join(dir, 'app.asar');
  await asar.createPackageWithOptions(path.join(dir, 'source'), archive, { unpack: '*.node', unpackDir: '**/sub' });
  assert.equal(asar.extractFile(archive, 'main.js').toString(), 'export const value = 42;\n');
  assert.equal(asar.statFile(archive, 'native/addon.node').unpacked, true);
  assert.equal(asar.statFile(archive, 'sub/data.json').unpacked, true);
  assert.equal(await readFile(`${archive}.unpacked/native/addon.node`, 'utf8'), 'native fixture');
  assert.ok(asar.listPackage(archive).map(file => file.replaceAll('\\', '/')).includes('/.hidden.json'));
});

test('directory comparison preserves include and exclude glob filters', async t => {
  const dir = await fixture(t);
  const compare = require('dir-compare');
  await mkdir(path.join(dir, 'other'), { recursive: true });
  await writeFile(path.join(dir, 'other', 'main.js'), 'export const value = 42;\n');
  const result = await compare.compare(path.join(dir, 'source'), path.join(dir, 'other'), {
    compareContent: true, includeFilter: '*.js', excludeFilter: 'native,sub,.hidden.json',
  });
  assert.equal(result.same, true);
  assert.equal(result.equal, 1);
});

test('legacy glob and filelist retain build file enumeration', async t => {
  const dir = await fixture(t);
  const glob = require('glob');
  const options = { cwd: path.join(dir, 'source'), dot: true, nodir: true };
  const expected = ['.hidden.json', 'sub/data.json'];
  assert.deepEqual(glob.sync('**/*.json', options).sort(), expected);
  assert.deepEqual((await new Promise((resolve, reject) => glob('**/*.json', options,
    (error, files) => error ? reject(error) : resolve(files)))).sort(), expected);
  assert.deepEqual(glob.sync('sub/../sub/*.json', options), ['sub/../sub/data.json']);
  const files = spawnSync(process.execPath, ['-e', `const { FileList } = require(${JSON.stringify(require.resolve('filelist'))});
    console.log(JSON.stringify(new FileList('*.js').toArray()));`], { cwd: path.join(dir, 'source'), encoding: 'utf8' });
  assert.equal(files.status, 0, files.stderr);
  assert.deepEqual(JSON.parse(files.stdout), ['main.js']);
});

test('builder file matching retains negative and dotfile patterns', () => {
  const { FileMatcher } = require('app-builder-lib/out/fileMatcher');
  const { createFilter } = require('app-builder-lib/out/util/filter');
  const source = path.resolve('build-tool-fixture-source');
  const matcher = new FileMatcher(source, path.resolve('build-tool-fixture-destination'), value => value,
    ['**/*', '!**/*.map']);
  const patterns = [];
  matcher.computeParsedPatterns(patterns);
  const filter = createFilter(source, patterns, null);
  const stat = { isDirectory: () => false };
  assert.equal(filter(path.join(source, '.hidden.json'), stat), true);
  assert.equal(filter(path.join(source, 'sub/main.js'), stat), true);
  assert.equal(filter(path.join(source, 'sub/main.js.map'), stat), false);
});

// Universal ASAR merging is a macOS operation with POSIX archive paths.
test('mac universal ASAR merge retains single-architecture glob rules', { skip: process.platform === 'win32' }, async t => {
  const dir = await fixture(t);
  const asar = require('@electron/asar');
  const { mergeASARs } = require('@electron/universal/dist/cjs/asar-utils.js');
  for (const arch of ['x64', 'arm64']) {
    const source = path.join(dir, arch);
    await mkdir(source);
    await writeFile(path.join(source, 'main.js'), 'shared code');
    await writeFile(path.join(source, `${arch}.json`), arch);
    await asar.createPackage(source, path.join(dir, `${arch}.asar`));
  }
  const options = { x64AsarPath: path.join(dir, 'x64.asar'), arm64AsarPath: path.join(dir, 'arm64.asar'),
    outputAsarPath: path.join(dir, 'merged.asar') };
  await assert.rejects(mergeASARs(options), /not covered by allowList/);
  await mergeASARs({ ...options, singleArchFiles: '*.json' });
  assert.deepEqual(asar.listPackage(options.outputAsarPath).sort(), ['/arm64.json', '/main.js', '/x64.json']);
});

async function bridgeFixture(t) {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-build-patches-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  for (const [name, files] of Object.entries({
    '@electron/asar': ['lib/asar.js'], 'dir-compare': ['build/src/FilterHandler/defaultFilterHandler.js'],
    glob: ['glob.js', 'sync.js', 'common.js'], minimatch: [],
  })) {
    const target = path.join(dir, 'node_modules', name);
    await mkdir(target, { recursive: true });
    const manifest = JSON.parse(await readFile(require.resolve(`${name}/package.json`), 'utf8'));
    if (name === 'minimatch') manifest.version = '11.0.3';
    await writeFile(path.join(target, 'package.json'), JSON.stringify(manifest));
    for (const file of files) {
      await mkdir(path.dirname(path.join(target, file)), { recursive: true });
      await writeFile(path.join(target, file), await readFile(require.resolve(`${name}/${file}`)));
    }
  }
  return dir;
}

test('install bridge is idempotent and permits runtime-only installs', async t => {
  const dir = await bridgeFixture(t);
  await patchBuildToolCompat(dir);
  assert.deepEqual(await patchBuildToolCompat(dir), { patched: 0, omitted: false });
  await rm(path.join(dir, 'node_modules'), { recursive: true });
  assert.deepEqual(await patchBuildToolCompat(dir), { patched: 0, omitted: true });
});

test('install bridge rejects unreviewed source before writing any patches', async t => {
  const dir = await bridgeFixture(t);
  const asar = path.join(dir, 'node_modules/@electron/asar/lib/asar.js');
  const original = await readFile(asar, 'utf8');
  await writeFile(path.join(dir, 'node_modules/glob/sync.js'), '// changed upstream source');
  await assert.rejects(patchBuildToolCompat(dir), /unreviewed build dependency source/);
  assert.equal(await readFile(asar, 'utf8'), original);
});

test('install bridge rejects unreviewed package and matcher versions', async t => {
  const dir = await bridgeFixture(t);
  const manifest = path.join(dir, 'node_modules/glob/package.json');
  await writeFile(manifest, JSON.stringify({ version: '8.0.0' }));
  await assert.rejects(patchBuildToolCompat(dir), /unreviewed build dependency: glob/);
  await writeFile(path.join(dir, 'node_modules/minimatch/package.json'), JSON.stringify({ version: '10.2.6' }));
  await assert.rejects(patchBuildToolCompat(dir), /reviewed minimatch version/);
});
