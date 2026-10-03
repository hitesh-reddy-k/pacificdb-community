#!/usr/bin/env node
// Executes the website's exact complete examples against a disposable engine.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, writeFile, mkdir, rm } from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { startDesktopEngine } from '../desktop/engine.mjs';

const repository = path.resolve(import.meta.dirname, '..');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-release-examples-'));
const build = path.resolve(process.argv[2] || 'build');
const decode = text => text.replace(/&#x27;/g, "'").replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const html = await readFile(path.join(repository, 'site/docs.html'), 'utf8');
function example(id) {
  const match = html.match(new RegExp(`<code id="${id}">([\\s\\S]*?)</code>`));
  assert.ok(match, `missing example ${id}`); return decode(match[1]);
}
async function run(command, args, env = {}, cwd = root) {
  const child = spawn(command, args, { cwd, env: { ...process.env, ...env }, stdio: ['ignore', 'pipe', 'pipe'] });
  let output = ''; child.stdout.on('data', b => { output += b; }); child.stderr.on('data', b => { output += b; });
  const timer = setTimeout(() => child.kill('SIGKILL'), 180000);
  const [code, signal] = await once(child, 'exit'); clearTimeout(timer);
  assert.equal(signal, null, output); assert.equal(code, 0, output); console.log(output.trim());
  return output;
}
let engine;
try {
  await run('npm', ['install', '--ignore-scripts', '--no-audit', '--no-fund',
    path.join(repository, 'release-sdk/npm/pacificdb-client-1.1.1.tgz'),
    path.join(repository, 'release-sdk/npm/pacificdb-cli-1.1.1.tgz')]);
  await run('node', [path.join(root, 'node_modules/@pacificdb/cli/bin/pacificdb.js'), '--version']);
  await run('python3', ['-m', 'venv', path.join(root, 'venv')]);
  const python = path.join(root, 'venv/bin/python');
  await run(python, ['-m', 'pip', 'install', '--no-deps', path.join(repository, 'release-sdk/python/pacificdb-1.1.1-py3-none-any.whl')]);
  await writeFile(path.join(root, 'hello.mjs'), example('node-code'));
  await writeFile(path.join(root, 'hello.py'), example('python-code'));
  await writeFile(path.join(root, 'Hello.java'), example('java-code'));
  // Arbitrary nonempty bytes are valid for the file-transfer API, regardless of extension.
  const media = Buffer.from('PacificDB 1.1.1 website transfer fixture\n'.repeat(100));
  await writeFile(path.join(root, 'demo.mp4'), media);
  await run('mvn', ['-B', '-q', '-f', path.join(repository, 'sdk/java/pom.xml'), 'dependency:build-classpath', `-Dmdep.outputFile=${path.join(root, 'classpath.txt')}`]);
  const classpath = path.join(repository, 'sdk/java/target/pacificdb-client-1.1.1.jar') + path.delimiter + (await readFile(path.join(root, 'classpath.txt'), 'utf8')).trim();
  await mkdir(path.join(root, 'classes'));
  await run('javac', ['--release', '11', '-cp', classpath, '-d', path.join(root, 'classes'), path.join(root, 'Hello.java')]);
  engine = await startDesktopEngine({ executable: path.join(build, 'db_engine'), directory: path.join(root, 'engine') });
  for (const [language, command, args] of [
    ['node', 'node', [path.join(root, 'hello.mjs')]],
    ['python', python, [path.join(root, 'hello.py')]],
    ['java', 'java', ['-cp', path.join(root, 'classes') + path.delimiter + classpath, 'Hello']]
  ]) {
    await run(command, args, { PACIFICDB_URL: `pacificdb://127.0.0.1:${engine.port}/${language}_demo` });
    assert.deepEqual(await readFile(path.join(root, `downloaded-${language}.mp4`)), media);
  }
  console.log(JSON.stringify({ status: 'PASS', version: '1.1.1', npm: 'installed exact local tarballs',
    python: 'installed exact local wheel', java: 'compiled against exact local packaged JAR',
    examples: ['Node CRUD/vector/media/close', 'Python CRUD/vector/media/close', 'Java CRUD/vector/media/close'], media: 'byte-for-byte equality' }));
} finally { await engine?.stop(); await rm(root, { recursive: true, force: true }); }
