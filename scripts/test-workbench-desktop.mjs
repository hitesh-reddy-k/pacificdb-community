#!/usr/bin/env node
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { _electron as electron } from 'playwright';

const directory = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-desktop-test-'));
const executablePath = process.env.PACIFICDB_TEST_DESKTOP;
const screenshot = process.env.PACIFICDB_DESKTOP_SCREENSHOT;
let desktop;
const env = { ...process.env, PACIFICDB_WORKBENCH_DATA: directory };
delete env.ELECTRON_RUN_AS_NODE;
const errors = [];
async function launch() {
  const started = Date.now();
  desktop = await electron.launch({
    ...(executablePath ? { executablePath } : {}),
    args: [...(!executablePath ? [path.resolve('desktop/main.mjs')] : [])],
    env, timeout: 60_000,
  });
  desktop.process().stderr.on('data', (chunk) => {
    const text = String(chunk);
    if (/Error:|SyntaxError|ERR_/.test(text)) errors.push(text);
  });
  const page = await desktop.firstWindow();
  page.on('pageerror', (error) => errors.push(error.message));
  await page.waitForURL('pacificdb://workbench/', { timeout: 40_000 });
  await page.waitForFunction(() => document.querySelector('#connection').textContent.includes('Connected'));
  assert.equal(await page.locator('.brand img').evaluate((image) => image.complete && image.naturalWidth === 210), true);
  console.log(`Desktop ready in ${Date.now() - started} ms`);
  return page;
}
try {
  let page = await launch();
  assert.equal(await page.evaluate(() => typeof process), 'undefined');
  const preferences = await desktop.evaluate(({ BrowserWindow }) => {
    const p = BrowserWindow.getAllWindows()[0].webContents.getLastWebPreferences();
    return { sandbox: p.sandbox, contextIsolation: p.contextIsolation, nodeIntegration: p.nodeIntegration };
  });
  assert.deepEqual(preferences, { sandbox: true, contextIsolation: true, nodeIntegration: false });
  async function create(kind, name) {
    await page.locator(kind === 'database' ? '#overview-create' : `#add-${kind}`).click();
    await page.locator('#create-name').fill(name);
    await page.locator('#submit-create').click();
    await page.locator('#notice').filter({ hasText: `Created ${kind} ${name}.` }).waitFor();
  }
  await create('database', 'workspace');
  await create('collection', 'notes');
  assert.equal(await page.locator('#databases > .tree-node > .tree-children .tree-item').count(), 1);
  await page.locator('#environment-open').click();
  await page.locator('#connection-dialog').waitFor();
  assert.match(await page.locator('#connection-endpoint').textContent(), /^127\.0\.0\.1:\d+$/);
  assert.match(await page.locator('#connection-example').textContent(), /pacificdb(?:\.exe)?['"]? --url "pacificdb:\/\/127\.0\.0\.1:\d+\/workspace" --no-start/);
  await page.locator('#connection-node').click();
  assert.match(await page.locator('#connection-example').textContent(), /PacificDB.connect.*workspace/);
  await page.locator('#close-connection').click();
  await page.locator('#new-document').click();
  await page.locator('#document-json').fill('{"id":"desktop-note","message":"Saved in the desktop app"}');
  await page.locator('#insert-doc').click();
  await page.locator('#document-list .record').filter({ hasText: 'Saved in the desktop app' }).waitFor();
  const command = await desktop.evaluate(async ({ Menu, clipboard }) => {
    const previous = await clipboard.readText();
    await Menu.getApplicationMenu().getMenuItemById('copy-cli').click();
    const value = await clipboard.readText();
    await clipboard.writeText(previous);
    return value;
  });
  assert.match(command, /--host 127\.0\.0\.1 --port \d+ --no-start$/);
  const nativeDirectory = executablePath ? process.platform === 'darwin' ?
    path.resolve(path.dirname(executablePath), '../Resources/engine') :
    path.join(path.dirname(executablePath), 'resources/engine') :
    path.dirname(path.resolve(process.env.PACIFICDB_WORKBENCH_ENGINE || path.join('build',
      process.platform === 'win32' ? 'db_engine.exe' : 'db_engine')));
  const cli = path.join(nativeDirectory, process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb');
  const version = spawnSync(cli, ['--version'], { encoding: 'utf8' });
  assert.equal(version.status, 0, version.stderr);
  assert.match(version.stdout, /PacificDB/);
  const shell = spawnSync(cli, ['--host', '127.0.0.1', '--port',
    command.match(/--port (\d+)/)[1], '--no-start'], {
    encoding: 'utf8', input: 'list databases\nquit\n', timeout: 10_000,
    env: { ...process.env, PACIFICDB_CLI_HOME: path.join(directory, 'cli') },
  });
  assert.equal(shell.status, 0, shell.stderr);
  assert.match(shell.stdout, /workspace/);
  await page.locator('#open-settings').click();
  await page.locator('#theme-select').selectOption('dark');
  await page.locator('#settings-done').click();
  if (screenshot) await page.screenshot({ path: screenshot });
  // Verify native save-dialog support is enabled without writing a user file.
  await page.locator('#nav-media').click();
  await page.locator('#media-file').setInputFiles({ name: 'desktop.txt', mimeType: 'text/plain', buffer: Buffer.from('desktop upload') });
  await page.locator('#upload-media').click();
  await page.locator('#media-list .record').filter({ hasText: 'desktop.txt' }).waitFor();
  const exit = new Promise((resolve) => desktop.process().once('exit', resolve));
  await desktop.evaluate(({ app }) => { app.quit(); });
  await exit;
  desktop = null;
  page = await launch();
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark');
  await page.locator('#nav-explorer').click();
  await page.locator('#document-list .record').filter({ hasText: 'desktop-note' }).waitFor();
  // Navigation out of the trusted application must be denied.
  await page.evaluate(() => { location.href = 'https://example.com'; });
  assert.equal(page.url(), 'pacificdb://workbench/');
  assert.deepEqual(errors, []);
  console.log('WORKBENCH_DESKTOP_PASS: native window, sandbox, bundled engine, CRUD, media, shutdown/restart persistence');
} finally {
  if (desktop) {
    const exited = new Promise((resolve) => desktop.process().once('exit', resolve));
    await desktop.evaluate(({ app }) => app.quit()).catch(() => {});
    await exited;
  }
  await rm(directory, { recursive: true, force: true });
}
