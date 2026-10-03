#!/usr/bin/env node
// Run after: npm ci && npx playwright install chromium
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, rm } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PassThrough } from 'node:stream';
import { chromium } from 'playwright';
import { main } from '../cli/src/cli.js';
import { PacificDBClient } from '../sdk/node/src/index.js';

const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-browser-'));
const screenshots = process.env.WORKBENCH_SCREENSHOTS;
let engine, workbench, browser, client, page;
const errors = [];
async function freePort() {
  const server = net.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}
try {
  const port = await freePort();
  const raftPort = await freePort();
  await Promise.all(['data', 'backup', 'restore'].map((name) => mkdir(path.join(root, name))));
  engine = spawn(path.resolve(process.argv[2] || 'build', 'db_engine'), [], {
    stdio: 'ignore', env: { ...process.env, PACIFICDB_ENVIRONMENT: 'development',
      PACIFICDB_HOME: root, DATA_ROOT: path.join(root, 'data'),
      BACKUP_ROOT: path.join(root, 'backup'), RESTORE_DIR: path.join(root, 'restore'),
      ENGINE_AUTH_REQUIRED: '0', ENGINE_BIND_HOST: '127.0.0.1', ENGINE_PORT: String(port),
      RAFT_LISTEN_PORT: String(raftPort), RAFT_CLUSTER_ID: 'workbench-browser',
      RAFT_NODE_ID: 'node-1', RAFT_IS_LEADER: '1', MIN_QUORUM_SIZE: '1', ENGINE_CPU_CORES: '2' },
  });
  client = new PacificDBClient({ port, poolSize: 1, timeoutMs: 500 });
  let ready = false;
  for (let attempt = 0; attempt < 100; attempt++) {
    try { ready = (await client.request({ action: 'ping' })).status === 'pong'; } catch {}
    if (ready) break;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  assert.ok(ready, 'engine must start');
  workbench = await main(['workbench', '--no-start', '--port', String(port)], {
    input: new PassThrough(), output: new PassThrough(),
  });
  browser = await chromium.launch({ headless: true, channel: 'chromium' });
  page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
  page.on('pageerror', (error) => errors.push(error.message));
  const requests = [];
  page.on('request', req => { if (req.url().endsWith('/api/execute')) requests.push(req.postDataJSON()); });
  await page.goto(workbench.url);
  await page.waitForFunction(() => document.querySelector('#connection').textContent.includes('Connected'));
  assert.equal(await page.locator('#overview').isVisible(), true);
  assert.equal(await page.locator('.brand img').evaluate((image) => image.complete && image.naturalWidth === 210), true);
  if (screenshots) await mkdir(screenshots, { recursive: true });
  await page.locator('#environment-open').click();
  await page.locator('#connection-dialog').waitFor({ state: 'visible' });
  assert.equal(await page.locator('#copy-connection').isDisabled(), true);
  await page.locator('#close-connection').click();
  await page.locator('#nav-explorer').click();
  await page.locator('#welcome-title').filter({ hasText: 'No databases yet' }).waitFor();
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'empty.png') });

  async function create(kind, name) {
    await page.locator(kind === 'database' ? '#overview-create' : `#add-${kind}`).click();
    await page.locator('#create-name').fill(name);
    await page.locator('#submit-create').click();
    await page.waitForFunction(() => !document.querySelector('#create-dialog').open);
    await page.locator('#notice').filter({ hasText: `Created ${kind} ${name}.` }).waitFor();
  }
  await create('database', 'commerce');
  await create('collection', 'customers');
  assert.equal(await page.locator('#databases > .tree-node > .tree-children .tree-item').textContent().then(value => value.includes('customers')), true,
    'collections must be nested directly under their database');
  const disclosure = page.locator('#databases > .tree-node').first().getByRole('button', { name: 'Collapse database' });
  await disclosure.click();
  assert.equal(await page.locator('#databases > .tree-node > .tree-children').first().isVisible(), false);
  await page.locator('#databases > .tree-node').first().getByRole('button', { name: 'Expand database' }).click();
  await page.locator('#environment-open').click();
  await page.locator('#connection-dialog').waitFor({ state: 'visible' });
  await page.locator('#connection-url').click();
  assert.equal(await page.locator('#connection-example').textContent(), `pacificdb://127.0.0.1:${port}/commerce`);
  for (const [format, pattern] of [['cli', /--url .*commerce/], ['node', /PacificDB.connect/], ['python', /with PacificDB.connect/], ['java', /try \(var db = PacificDB.connect/]]) {
    await page.locator(`#connection-${format}`).click();
    const example = await page.locator('#connection-example').textContent();
    assert.match(example, pattern); assert.doesNotMatch(example, /useProject|projectId|useDatabase/);
  }
  // An authenticated endpoint never generates inline credential examples.
  await page.route('**/api/execute', async route => {
    if (route.request().postDataJSON().op === 'connection.info') await route.fulfill({ json: { result: { host: '127.0.0.1', port, tls: true, userId: 'alice', authenticationRequired: true } } });
    else await route.continue();
  });
  await page.locator('#close-connection').click();
  await page.locator('#environment-open').click();
  await page.locator('#connection-dialog').waitFor({ state: 'visible' });
  for (const format of ['url', 'cli', 'node', 'python', 'java']) {
    await page.locator(`#connection-${format}`).click();
    assert.match(await page.locator('#connection-example').textContent(), /PACIFICDB_URL/);
    assert.doesNotMatch(await page.locator('#connection-example').textContent(), /password|token=|userProject/);
  }
  await page.unrouteAll({ behavior: 'wait' });
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'connection.png') });
  await page.locator('#close-connection').click();
  await create('database', 'second');
  await page.locator('#databases .tree-item').filter({ hasText: 'commerce' }).click();
  await page.locator('#breadcrumb').filter({ hasText: 'commerce' }).waitFor();
  await client.useDatabase('commerce');
  // Seed through the same single-document operation offered by Workbench.
  for (let index = 0; index < 131; index++) {
    await client.insert('customers', {
      id: `customer-${String(index).padStart(3, '0')}`, name: `Customer ${index + 1}`,
      status: index % 2 ? 'active' : 'pending', orders: index + 1,
    });
  }
  assert.equal((await client.find('customers')).data.length, 131, 'fixture must contain all rows');
  for (const [name, count] of [['projects', 4], ['tasks', 3], ['messages', 2], ['vectors', 1], ['files', 0]]) {
    await client.createCollection(name);
    for (let index = 0; index < count; index++) await client.insert(name, { id: `${name}-${index}` });
  }
  const beforeHidden = requests.filter(req => req.op === 'collections.summary').length;
  for (const view of ['query', 'media', 'explorer']) await page.locator(`#nav-${view}`).click();
  await page.locator('#refresh-docs').click();
  await page.waitForFunction(() => !document.querySelector('#refresh-docs').disabled);
  assert.equal(requests.filter(req => req.op === 'collections.summary').length, beforeHidden, 'hidden overview must not count collections');
  let active = 0, maxActive = 0;
  await page.route('**/api/execute', async route => {
    if (route.request().postDataJSON().op !== 'collections.summary') return route.continue();
    active++; maxActive = Math.max(maxActive, active);
    try { const response = await route.fetch(); await new Promise(resolve => setTimeout(resolve, 50)); await route.fulfill({ response }); }
    finally { active--; }
  });
  await page.locator('#nav-overview').click();
  await page.locator('#overview-refresh').click();
  await page.locator('#total-documents').filter({ hasText: '141' }).waitFor();
  await page.unrouteAll({ behavior: 'wait' });
  assert.ok(maxActive > 0 && maxActive <= 4, `summary concurrency ${maxActive} must be bounded at four`);
  // Hold old database summaries while navigation changes; neither stale values nor
  // new summary jobs may leak into a hidden Overview.
  let releaseSummaries, summariesStarted;
  const heldSummaries = new Promise(resolve => { releaseSummaries = resolve; });
  const startedSummaries = new Promise(resolve => { summariesStarted = resolve; });
  await page.route('**/api/execute', async route => {
    if (route.request().postDataJSON().op !== 'collections.summary') return route.continue();
    summariesStarted(); await heldSummaries;
    await route.fulfill({ json: { result: { count: 999999, indexes: 999 } } });
  });
  await page.locator('#overview-refresh').click();
  await startedSummaries;
  assert.match(await page.locator('#collection-rows').textContent(), /—/, 'unloaded counts remain unknown');
  await page.locator('#databases .tree-item').filter({ hasText: 'second' }).click();
  await page.locator('#welcome-title').filter({ hasText: 'No collections yet' }).waitFor();
  releaseSummaries();
  await page.unrouteAll({ behavior: 'wait' });
  assert.doesNotMatch(await page.locator('#total-documents').textContent(), /999999/);
  await page.locator('#databases .tree-item').filter({ hasText: 'commerce' }).click();
  await page.locator('#document-list .record').first().waitFor();
  await page.locator('#nav-overview').click();
  await page.locator('#total-documents').filter({ hasText: '141' }).waitFor();
  assert.equal(await page.locator('#collection-rows tr').count(), 6);
  assert.deepEqual(await page.locator('.collection-table th').allTextContents(), ['Name ↕', 'Type', 'Documents', 'Indexes']);
  assert.equal(await page.locator('#top-new-query').count(), 0);
  for (const id of ['open-databases', 'open-collections']) {
    await page.locator(`#${id}`).click();
    assert.equal(await page.locator('#nav-explorer').getAttribute('aria-current'), 'page');
    await page.locator('#nav-overview').click();
  }
  await page.locator('.quick-grid [data-open-tab="documents"]').click();
  assert.equal(await page.locator('#nav-explorer').getAttribute('aria-current'), 'page');
  await page.locator('#nav-overview').click();
  await page.locator('.quick-grid [data-open-tab="vectors"]').click();
  assert.equal(await page.locator('#nav-vectors').getAttribute('aria-current'), 'page');
  await page.locator('#nav-overview').click();
  await page.locator('.quick-grid [data-open-tab="media"]').click();
  assert.equal(await page.locator('#nav-media').getAttribute('aria-current'), 'page');
  await page.locator('#nav-overview').click();
  await page.locator('#quick-cli').click();
  await page.locator('#connection-dialog').waitFor({ state: 'visible' });
  await page.locator('#close-connection').click();
  await page.locator('#nav-search').fill('commerce');
  assert.equal(await page.locator('#databases > .tree-node').count(), 1);
  await page.locator('#nav-search').fill('');
  assert.match(await page.locator('#collection-rows tr').filter({ hasText: 'customers' }).textContent(), /131.*1/s);
  await page.locator('#sort-collections').click();
  assert.match(await page.locator('#collection-rows tr').first().textContent(), /vectors/);
  await page.locator('#sort-collections').click();
  await page.locator('#collection-search').fill('missing');
  assert.equal(await page.locator('#collection-rows tr').count(), 0);
  await page.locator('#collection-search').fill('customers');
  await page.locator('#collection-type').selectOption('populated');
  assert.equal(await page.locator('#collection-rows tr').count(), 1);
  await page.locator('#collection-type').selectOption('empty');
  assert.equal(await page.locator('#collection-rows tr').count(), 0);
  await page.locator('#collection-search').fill('');
  assert.equal(await page.locator('#collection-rows tr').count(), 1);
  await page.locator('#collection-type').selectOption('all');
  await page.locator('#collection-grid-view').click();
  assert.equal(await page.locator('#collection-rows').evaluate((element) => element.classList.contains('grid-layout')), true);
  await page.locator('#collection-list-view').click();
  await page.locator('#collection-search').fill('');
  await page.setViewportSize({ width: 1058, height: 770 });
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  assert.equal(await page.locator('#nav-overview').evaluate((element) => parseFloat(getComputedStyle(element).fontSize) >= 12), true);
  assert.equal(await page.locator('.collection-table td').first().evaluate((element) => parseFloat(getComputedStyle(element).fontSize) >= 12), true);
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'dashboard-reference.png') });
  await page.setViewportSize({ width: 1440, height: 1100 });
  await page.locator('#quick-query').click();
  assert.equal(await page.locator('#nav-query').getAttribute('aria-current'), 'page');
  assert.equal(await page.locator('#title').textContent(), 'Query Workbench');
  assert.equal(await page.locator('#new-document').isVisible(), false);
  assert.equal(await page.locator('#database-identity').isVisible(), false);
  assert.equal(await page.locator('#document-editor').isVisible(), false);
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'query.png') });
  await page.locator('#document-list button').first().click();
  assert.equal(await page.locator('#nav-explorer').getAttribute('aria-current'), 'page');
  assert.equal(await page.locator('#document-editor').isVisible(), true);
  await page.locator('#document-editor').evaluate((element) => { element.open = false; });
  await page.locator('#nav-monitoring').click();
  await page.locator('#monitor-status').filter({ hasText: 'healthy' }).waitFor();
  await page.locator('#monitoring-refresh').click();
  await page.locator('#monitor-metrics').filter({ hasText: 'pacificdb_' }).waitFor();
  await page.locator('#monitor-status').filter({ hasText: 'healthy' }).waitFor();
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'monitoring.png') });
  await page.keyboard.press('Control+k');
  await page.locator('#command-input').fill('Data Explorer');
  await page.keyboard.press('Enter');
  assert.equal(await page.locator('#explorer').isVisible(), true);
  assert.equal(await page.locator('#title').textContent(), 'Documents');
  assert.equal(await page.locator('#new-document').isVisible(), true);
  await page.locator('#refresh-docs').click();
  await page.waitForFunction(() => document.querySelectorAll('#document-list .record').length === 50);
  assert.doesNotMatch(await page.locator('#document-list .record').first().textContent(), /_mvcc_version|_raft_commit_index/);
  await page.locator('#next-docs').click();
  await page.locator('#document-page').filter({ hasText: '51–100 shown' }).waitFor();
  await page.locator('#next-docs').click();
  await page.locator('#document-page').filter({ hasText: '101–131 shown' }).waitFor();
  assert.equal(await page.locator('#next-docs').isDisabled(), true);
  assert.equal(await page.locator('#document-list .record').count(), 31);
  await page.locator('#page-size').selectOption('25');
  await page.locator('#document-page').filter({ hasText: '1–25 shown' }).waitFor();

  await page.locator('#new-document').click();
  await page.locator('#document-json').fill('{"id":"ui-record","name":"Created in Workbench"}');
  await page.locator('#insert-doc').click();
  await page.locator('#notice').filter({ hasText: 'Document inserted' }).waitFor();
  await page.locator('#filter').fill('{"id":"ui-record"}');
  await page.locator('#find-docs').click();
  await page.locator('#document-list .record').filter({ hasText: 'Created in Workbench' }).waitFor();
  await page.locator('#document-list').getByRole('button', { name: 'Edit', exact: true }).click();
  await page.locator('#document-json').fill('{"name":"Updated in Workbench"}');
  await page.locator('#update-doc').click();
  await page.locator('#document-list .record').filter({ hasText: 'Updated in Workbench' }).waitFor();
  page.once('dialog', (dialog) => dialog.accept());
  await page.locator('#delete-doc').click();
  await page.locator('#document-list').filter({ hasText: 'No documents match' }).waitFor();
  await page.locator('#filter').fill('not json');
  await page.locator('#find-docs').click();
  await page.locator('#notice.error').filter({ hasText: 'valid JSON' }).waitFor();

  // A slow old query must never overwrite a newer selection/filter result.
  let releaseOld;
  const delayed = new Promise((resolve) => { releaseOld = resolve; });
  let intercepted;
  const started = new Promise((resolve) => { intercepted = resolve; });
  await page.route('**/api/execute', async (route) => {
    const body = route.request().postDataJSON();
    if (body.op === 'documents.find' && body.filter?.id === 'slow-query') {
      intercepted();
      await delayed;
      await route.fulfill({ json: { result: { data: [{ id: 'stale-result' }] } } }).catch(() => {});
    } else await route.continue();
  });
  await page.locator('#filter').fill('{"id":"slow-query"}');
  await page.locator('#find-docs').click();
  await started;
  await page.locator('#filter').fill('{"id":"customer-000"}');
  await page.locator('#find-docs').click();
  await page.locator('#document-list .record').filter({ hasText: 'customer-000' }).waitFor();
  releaseOld();
  await page.unrouteAll({ behavior: 'wait' });
  assert.equal(await page.locator('#document-list').textContent().then((text) => text.includes('stale-result')), false);

  await page.locator('#nav-vectors').click();
  assert.equal(await page.locator('#title').textContent(), 'Vector Search');
  assert.equal(await page.locator('#new-document').isVisible(), false);
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'vectors.png') });
  await page.locator('#vector-id').fill('sample-vector');
  await page.locator('#vector-filter').fill('not JSON');
  await page.locator('#put-vector').click();
  await page.waitForFunction(() => !document.querySelector('#put-vector').disabled);
  assert.match(await page.locator('#notice').textContent(), /Vector stored/, 'search filter must not affect vector storage');
  await page.locator('#vector-filter').fill('{}');
  await page.locator('#notice').filter({ hasText: 'Vector stored' }).waitFor();
  await page.locator('#query-vector').click();
  await page.locator('#vector-result').filter({ hasText: 'sample-vector' }).waitFor();
  for (const metric of ['l2', 'dot']) {
    await page.locator('#vector-metric').selectOption(metric);
    await page.locator('#query-vector').click();
    await page.locator('#vector-result').filter({ hasText: 'sample-vector' }).waitFor();
  }
  await page.locator('#vector-filter').fill('{"id":"missing-vector"}');
  await page.locator('#query-vector').click();
  await page.waitForFunction(() => JSON.parse(document.querySelector('#vector-result').textContent).data.length === 0);
  await page.locator('#vector-filter').fill('{}');
  await page.locator('#nav-media').click();
  assert.equal(await page.locator('#title').textContent(), 'Media Library');
  assert.equal(await page.locator('#new-document').isVisible(), false);
  await page.locator('#media-page').filter({ hasText: '0 files' }).waitFor();
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'media.png') });
  await page.locator('#media-file').setInputFiles({ name: 'sample.txt', mimeType: 'text/plain', buffer: Buffer.from('workbench browser upload') });
  await page.locator('#upload-media').click();
  await page.locator('#media-list .record').filter({ hasText: 'sample.txt' }).waitFor();
  await page.locator('#refresh-media').click();
  await page.locator('#media-list .record').filter({ hasText: 'sample.txt' }).waitFor();
  await page.locator('#media-search').fill('missing');
  assert.equal(await page.locator('#media-list .record:visible').count(), 0);
  await page.locator('#media-search').fill('sample');
  assert.equal(await page.locator('#media-list .record:visible').count(), 1);
  await page.locator('#media-search').fill('');
  await page.locator('#media-list').getByRole('button', { name: 'Preview', exact: true }).click();
  await page.locator('#media-preview-text').filter({ hasText: 'workbench browser upload' }).waitFor();
  await page.locator('#close-preview').click();
  await page.locator('#media-file').setInputFiles({ name: 'second.txt', mimeType: 'text/plain', buffer: Buffer.from('newer preview') });
  await page.locator('#upload-media').click();
  const firstCard = page.locator('#media-list .record').filter({ hasText: 'sample.txt' });
  const secondCard = page.locator('#media-list .record').filter({ hasText: 'second.txt' });
  await secondCard.waitFor();
  let releasePreview, previewStarted;
  const heldPreview = new Promise((resolve) => { releasePreview = resolve; });
  const startedPreview = new Promise((resolve) => { previewStarted = resolve; });
  const firstMediaId = (await client.request({ action: 'community_media_list', dbName: 'commerce', collection: 'customers' })).media.find((file) => file.filename === 'sample.txt').id;
  await page.route('**/api/media/download', async (route) => {
    if (route.request().postDataJSON().mediaId === firstMediaId) {
      const response = await route.fetch(); previewStarted(); await heldPreview;
      await route.fulfill({ response });
    } else await route.continue();
  });
  await firstCard.getByRole('button', { name: 'Preview', exact: true }).click();
  await startedPreview;
  await secondCard.getByRole('button', { name: 'Preview', exact: true }).click();
  await page.locator('#media-preview-title').filter({ hasText: 'second.txt' }).waitFor();
  releasePreview();
  await page.waitForFunction(() => ![...document.querySelectorAll('#media-list .record')].find((card) => card.dataset.search.includes('sample.txt')).querySelector('[aria-busy="true"]'));
  assert.equal(await page.locator('#media-preview-title').textContent(), 'second.txt', 'a slow old preview must not overwrite the newer selection');
  await page.locator('#close-preview').click();
  await page.unrouteAll({ behavior: 'wait' });
  await secondCard.getByRole('button', { name: 'Delete', exact: true }).click();
  await page.locator('#confirm-submit').click();
  await page.locator('#media-page').filter({ hasText: '1 file' }).waitFor();
  const downloaded = page.waitForEvent('download');
  await page.locator('#media-list').getByRole('button', { name: 'Download', exact: true }).click();
  assert.equal((await downloaded).suggestedFilename(), 'sample.txt');
  await page.locator('#media-list').getByRole('button', { name: 'Delete', exact: true }).click();
  await page.locator('#confirm-cancel').click();
  assert.equal(await page.locator('#media-list .record').count(), 1);
  await page.locator('#media-list').getByRole('button', { name: 'Delete', exact: true }).click();
  await page.locator('#confirm-submit').click();
  await page.locator('#media-page').filter({ hasText: '0 files' }).waitFor();

  await page.locator('#nav-explorer').click();
  await page.locator('#filter').fill('{}');
  await page.locator('#find-docs').click();
  await page.waitForFunction(() => document.querySelectorAll('#document-list .record').length === 25);
  await page.locator('#document-editor').evaluate((element) => { element.open = false; });
  await page.evaluate(() => scrollTo(0, 0));
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'explorer.png') });
  // Index controls must operate on real engine indexes, not display fixtures.
  assert.equal(await page.locator('#tab-indexes').count(), 1, 'collection index management must be available');
  await page.locator('#tab-indexes').click();
  await page.locator('#create-index').click();
  await page.locator('#index-name').fill('status_idx');
  await page.locator('#index-field').fill('status');
  await page.locator('#submit-index').click();
  await page.locator('#index-list tr').filter({ hasText: 'status_idx' }).waitFor();
  const actualIndexes = await client.request({ action: 'listIndexes', collection: 'customers' });
  assert.ok(actualIndexes.indexes.some((index) => index.name === 'status_idx'), 'index must exist in engine');
  await page.locator('#index-list tr').filter({ hasText: 'status_idx' }).getByRole('button', { name: 'Validate' }).click();
  await page.locator('#notice').filter({ hasText: 'Index validation' }).waitFor();
  await page.locator('#index-list tr').filter({ hasText: 'status_idx' }).getByRole('button', { name: 'Rebuild' }).click();
  await page.locator('#notice').filter({ hasText: 'Index rebuilt' }).waitFor();
  await page.locator('#index-list tr').filter({ hasText: 'status_idx' }).getByRole('button', { name: 'Delete' }).click();
  await page.locator('#confirm-submit').click();
  await page.locator('#notice').filter({ hasText: 'Index deleted' }).waitFor();
  assert.ok(!(await client.request({ action: 'listIndexes', collection: 'customers' })).indexes.some((index) => index.name === 'status_idx'));
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'indexes.png') });
  await page.locator('#tab-documents').click();
  await page.locator('#nav-query').click();
  await page.locator('#filter').fill('{"id":"customer-000"}');
  await page.keyboard.press('Control+Enter');
  await page.locator('#document-result-count').filter({ hasText: '1' }).waitFor();
  await page.locator('#query-history').selectOption('{"id":"customer-000"}');
  assert.equal(await page.locator('#filter').inputValue(), '{"id":"customer-000"}');
  await page.locator('#nav-explorer').click();
  await page.locator('#filter').fill('{}');
  await page.locator('#find-docs').click();
  await page.locator('#format-filter').click();
  assert.equal(await page.locator('#filter').inputValue(), '{}');
  await page.locator('#view-table').click();
  assert.equal(await page.locator('#document-list table tbody tr').count(), 25);
  await page.locator('#view-cards').click();
  await page.locator('#new-document').click();
  await page.locator('#document-json').fill('{invalid');
  await page.locator('#format-document').click();
  await page.locator('#notice.error').filter({ hasText: 'valid JSON' }).waitFor();
  await page.locator('#document-json').fill('{"name":"Polished editor"}');
  await page.locator('#format-document').click();
  assert.match(await page.locator('#document-json').inputValue(), /\n/);
  assert.equal(await page.locator('#json-lines').textContent(), '1\n2\n3');
  assert.ok(await page.locator('#json-preview .json-key').count() > 0);
  await page.locator('#cancel-editor').click();
  // External removal during refresh must not carry an old edit into a new scope.
  await client.createCollection('replacement');
  await page.locator('#document-list button').first().click();
  await page.route('**/api/execute', async (route) => {
    if (route.request().postDataJSON().op === 'collections.list') {
      await route.fulfill({ json: { result: { collections: ['replacement'] } } });
    } else await route.continue();
  });
  await page.locator('#refresh-all').click();
  await page.locator('#welcome-title').filter({hasText: 'Choose a collection'}).waitFor({timeout: 5000});
  assert.equal(await page.locator('#collection-name').textContent(), '');
  assert.equal(await page.locator('#document-editor').evaluate((element) => element.open), false);
  assert.equal(await page.locator('#document-json').inputValue(), '{}');
  assert.equal(await page.locator('#write-filter').inputValue(), '{}');
  await page.unrouteAll({ behavior: 'wait' });
  await page.locator('#nav-overview').click();
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'overview.png') });
  await page.locator('#open-settings').click();
  await page.locator('#theme-select').selectOption('dark');
  await page.locator('#density-select').selectOption('compact');
  await page.locator('#settings-done').click();
  await page.reload();
  await page.waitForFunction(() => document.documentElement.dataset.theme === 'dark');
  assert.equal(await page.locator('#page-size').inputValue(), '25');
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'dark.png') });
  await page.locator('#open-settings').click();
  await page.locator('#theme-select').selectOption('light');
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'light');
  await page.locator('#theme-select').selectOption('system');
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'system');
  await page.locator('#theme-select').selectOption('dark');
  await page.locator('#settings-done').click();
  await page.setViewportSize({ width: 390, height: 844 });
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  await page.locator('#mobile-menu').click();
  await page.locator('#nav-explorer').click();
  assert.equal(await page.locator('#nav-scrim').isVisible(), false);
  await page.waitForFunction(() => document.querySelector('#sidebar').getBoundingClientRect().right <= 0);
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  if (screenshots) await page.screenshot({ path: path.join(screenshots, 'mobile.png') });
  await page.setViewportSize({ width: 1365, height: 900 });
  await create('collection', 'do_not_choose');
  await page.locator('#delete-collection').click();
  await page.locator('#confirm-submit').click();
  await page.locator('#welcome-title').filter({hasText: 'Choose a collection'}).waitFor({timeout: 5000});
  assert.equal(await page.locator('#collection-name').textContent(), '');
  await create('database', 'removal_test');
  await create('collection', 'temporary');
  await page.locator('#delete-collection').click();
  await page.locator('#confirm-submit').click();
  await page.locator('#welcome-title').filter({ hasText: 'No collections yet' }).waitFor();
  await page.locator('#delete-database').click();
  await page.locator('#confirm-submit').click();
  await page.locator('#notice').filter({ hasText: 'Database deleted' }).waitFor();
  assert.ok(!(await client.request({ action: 'listDatabases' })).includes('removal_test'));
  assert.equal(await page.locator('#top-database').textContent(), 'Workspace',
    'deleting the selected database must not switch into an unrelated database');
  await create('database', 'd'.repeat(129)); await create('collection', 'long_namespace');
  await create('database', '数据库'); await create('collection', '文件');
  await page.locator('#nav-media').click();
  await page.locator('#media-file').setInputFiles({name: '文件.txt', mimeType: 'text/plain', buffer: Buffer.from('unicode scope upload')});
  await page.locator('#upload-media').click();
  await page.locator('#media-list .record').filter({hasText: '文件.txt'}).waitFor({timeout: 5000});
  // A URL-selected missing database is not permission to choose a different one.
  const beforeMissing = requests.length;
  await page.route('**/api/execute', async route => {
    if (route.request().postDataJSON().op === 'connection.info') await route.fulfill({json: {result: {
      host: '127.0.0.1', port, tls: false, userId: 'system', authenticationRequired: false, database: 'requested-missing'}}});
    else await route.continue();
  });
  await page.reload();
  await page.locator('#notice').filter({hasText: 'requested-missing'}).waitFor({timeout: 5000});
  assert.equal(await page.locator('#top-database').textContent(), 'Workspace');
  assert.equal(requests.slice(beforeMissing).some(value => value.op === 'collections.list'), false);
  assert.deepEqual(errors, []);
  console.log('WORKBENCH_BROWSER_PASS: CRUD/deletion, indexes, query history/shortcuts, table/JSON editor, vector metrics/filters, media preview races, preferences, responsive layout');
} catch (error) {
  console.error('Browser diagnostics:', JSON.stringify({ errors, notice: await page?.locator('#notice').textContent(), summary: await page?.locator('#document-summary').textContent(), records: await page?.locator('#document-list .record').count(), page: await page?.locator('#document-page').textContent() }));
  if (page && screenshots) await page.screenshot({ path: path.join(screenshots, 'failure.png') });
  throw error;
} finally {
  await browser?.close();
  client?.close();
  if (workbench) await workbench.close();
  if (engine && engine.exitCode === null) {
    const exited = once(engine, 'exit');
    engine.kill('SIGINT');
    const timeout = setTimeout(() => engine.kill('SIGKILL'), 10_000);
    await exited;
    clearTimeout(timeout);
  }
  await rm(root, { recursive: true, force: true });
}
