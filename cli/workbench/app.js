'use strict';

const $ = (id) => document.getElementById(id);
const token = document.querySelector('meta[name="pacificdb-token"]').content;
// Keep the same public document view as the native CLI. Engine bookkeeping
// stays in storage and never appears in the document editor.
const internalDocumentFields = new Set(['_mvcc_commit_ms', '_mvcc_version',
  '_raft_commit_index', '_raft_term', '_visibility_floor', '_visibility_state',
  '_logicalWritePayloadHash', 'created_at_ms', 'created_txn', 'deleted_at_ms',
  'deleted_txn', 'version', 'committed', 'tenant_id']);
const state = {
  database: null, collection: null, databases: [],
  collections: [], view: 'overview', navQuery: '',
  navigation: 0, documentRequest: null, mediaRequest: null, pageSize: 50,
  documentView: 'cards', documentOffset: 0, documentHasNext: false,
  documentFilter: {}, mediaOffset: 0, mediaOffsets: [], mediaNext: null,
  createKind: null, createScope: null, writePending: false,
  connectionInfo: null, connectionFormat: 'cli',
  collectionSummaries: new Map(), summaryGeneration: 0, summaryScope: '', summaryTask: null,
  collectionSort: 'asc', collectionLayout: 'list', activity: [],
  documents: [], queryHistory: [], collapsed: new Set(), indexGeneration: 0, indexScope: null, indexes: [],
};

function scope() {
  return { database: state.database, collection: state.collection };
}
function scopeKey(value = scope()) { return JSON.stringify(value); }
function notice(message, error = false) {
  $('notice').textContent = message;
  $('notice').classList.toggle('error', error);
}
function connection(connected, failed = false) {
  $('environment-open').dataset.state = connected ? 'connected' : failed ? 'error' : 'disconnected';
  $('connection').classList.toggle('offline', !connected);
  $('connection').replaceChildren();
  const dot = document.createElement('span');
  dot.className = 'status-dot';
  $('connection').append(dot, connected ? 'Connected' : 'Connection unavailable');
  $('environment-state').textContent = connected ? 'Connected' : failed ? 'Error' : 'Disconnected';
  $('environment-open').classList.toggle('offline', !connected);
}
async function copyText(value) {
  try { await navigator.clipboard.writeText(value); }
  catch {
    const field = document.createElement('textarea');
    field.value = value;
    field.style.cssText = 'position:fixed;left:-9999px;top:0;opacity:0';
    document.body.append(field);
    field.select();
    const copied = document.execCommand('copy');
    field.remove();
    if (!copied) throw new Error('Copy failed. Select the text and copy it manually.');
  }
}
function connectionUrl() {
  const info = state.connectionInfo;
  if (!info || !state.database) return null;
  const host = info.host.includes(':') && !info.host.startsWith('[') ? `[${info.host}]` : info.host;
  const user = info.userId && info.userId !== 'system' ? `?userId=${encodeURIComponent(info.userId)}` : '';
  return `${info.tls ? 'pacificdbs' : 'pacificdb'}://${host}:${info.port}/${encodeURIComponent(state.database)}${user}`;
}
function connectionExample() {
  const info = state.connectionInfo;
  const url = connectionUrl();
  if (!url) return 'Select or create a database to copy a connection example.';
  const authenticated = info.authenticationRequired;
  const collection = JSON.stringify(state.collection || 'records');
  if (state.connectionFormat === 'url') return authenticated ?
    'Set PACIFICDB_URL privately with your credentials.\nDatabase endpoint (credentials omitted):\n' + url : url;
  const value = JSON.stringify(url);
  if (state.connectionFormat === 'node') return "import { PacificDB } from '@pacificdb/client';\n\n" +
    `const db = await PacificDB.connect(${authenticated ? 'process.env.PACIFICDB_URL' : value});\n` +
    `try {\n  console.log(await db.find(${collection}, {}));\n} finally {\n  db.close();\n}`;
  if (state.connectionFormat === 'python') return 'from pacificdb import PacificDB\n' +
    (authenticated ? 'import os\n' : '') + `\nwith PacificDB.connect(${authenticated ? 'os.environ["PACIFICDB_URL"]' : value}) as db:\n` +
    `    print(db.find(${collection}, {}))`;
  if (state.connectionFormat === 'java') return 'import io.pacificdb.PacificDB;\nimport java.util.Map;\n\n' +
    'public class Connect {\n  public static void main(String[] args) throws Exception {\n' +
    `    try (var db = PacificDB.connect(${authenticated ? 'System.getenv("PACIFICDB_URL")' : value})) {\n` +
    `      System.out.println(db.find(${collection}, Map.of()));\n    }\n  }\n}`;
  const cli = info.cliPath ? info.platform === 'win32' ?
    `& '${info.cliPath.replaceAll("'", "''")}'` :
    `'${info.cliPath.replaceAll("'", "'\\''")}'` : 'pacificdb';
  return authenticated ? `${cli} --no-start\n# Reads PACIFICDB_URL from your environment.` :
    `${cli} --url ${value} --no-start`;
}
function renderConnection() {
  $('connection-endpoint').textContent = state.connectionInfo ?
    `${state.connectionInfo.host}:${state.connectionInfo.port}` : 'Unavailable';
  $('connection-database').textContent = state.database || 'Select a database';
  $('connection-example').textContent = connectionExample();
  $('copy-connection').disabled = !connectionUrl();
  for (const format of ['url', 'cli', 'node', 'python', 'java']) {
    const button = $(`connection-${format}`);
    const selected = state.connectionFormat === format;
    button.classList.toggle('active', selected);
    button.setAttribute('aria-selected', String(selected));
  }
}
async function openConnection() {
  state.connectionInfo = await request('connection.info');
  renderConnection();
  $('connection-dialog').showModal();
}
async function run(fn, button) {
  if (button?.disabled) return;
  const label = button?.textContent;
  if (button) {
    button.disabled = true; button.setAttribute('aria-busy', 'true');
    button.textContent = button.id.startsWith('refresh') || button.id.endsWith('refresh') ? 'Refreshing…' :
      button.id === 'upload-media' ? 'Uploading…' : button.id === 'query-vector' ? 'Searching…' :
      button.id === 'put-vector' ? 'Saving…' : 'Working…';
  }
  notice('');
  try { await fn(); }
  catch (error) {
    if (error.name !== 'AbortError') notice(error.message || String(error), true);
  } finally { if (button) { button.disabled = false; button.removeAttribute('aria-busy'); button.textContent = label; } }
}
async function request(op, values = {}, signal) {
  const controller = new AbortController();
  const abort = () => controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) controller.abort();
  const timer = setTimeout(abort, 30_000);
  try {
    const response = await fetch('/api/execute', {
      method: 'POST', signal: controller.signal,
      headers: { 'Content-Type': 'application/json', 'X-PacificDB-Workbench-Token': token },
      body: JSON.stringify({ op, ...scope(), ...values }),
    });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || `Request failed (${response.status})`);
    if (op !== 'connection.info') connection(true);
    return payload.result;
  } catch (error) {
    if (signal?.aborted) throw error;
    if (error.name === 'AbortError') throw new Error('The request timed out. Try refreshing.');
    if (error instanceof TypeError) connection(false);
    throw error;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
  }
}
function jsonField(id, label, array = false) {
  let value;
  try { value = JSON.parse($(id).value); }
  catch { throw new Error(`${label} must be valid JSON.`); }
  if (array ? !Array.isArray(value) : !value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`${label} must be a JSON ${array ? 'array' : 'object'}.`);
  }
  return value;
}
function applyPreferences() {
  document.documentElement.dataset.theme = $('theme-select').value;
  document.documentElement.dataset.density = $('density-select').value;
  $('page-size').value = String(state.pageSize);
  renderDocuments();
  $('document-list').classList.toggle('compact', state.documentView === 'compact');
  for (const view of ['cards', 'compact', 'table']) {
    $(`view-${view}`).classList.toggle('active', state.documentView === view);
    $(`view-${view}`).setAttribute('aria-pressed', String(state.documentView === view));
  }
}
function savePreferences() {
  applyPreferences();
  try {
    localStorage.setItem('pacificdb.workbench.preferences', JSON.stringify({
      theme: $('theme-select').value, density: $('density-select').value,
      pageSize: state.pageSize, documentView: state.documentView,
    }));
  } catch { /* Settings still work when browser storage is unavailable. */ }
}
function loadPreferences() {
  try {
    const saved = JSON.parse(localStorage.getItem('pacificdb.workbench.preferences') || '{}');
    $('theme-select').value = ['light', 'dark', 'system'].includes(saved.theme) ? saved.theme : 'dark';
    $('density-select').value = saved.density === 'compact' ? 'compact' : 'comfortable';
    state.pageSize = [25, 50, 100].includes(saved.pageSize) ? saved.pageSize : 50;
    state.documentView = ['compact', 'table'].includes(saved.documentView) ? saved.documentView : 'cards';
  } catch { /* Use defaults when preferences are missing or invalid. */ }
  applyPreferences();
}
function closeNavigation() {
  $('sidebar').classList.remove('open');
  $('nav-scrim').hidden = true;
  $('mobile-menu').setAttribute('aria-expanded', 'false');
}
function setView(view) {
  if (state.view === 'overview' && view !== 'overview') state.summaryGeneration++;
  state.view = view;
  if (view === 'explorer') selectTab('documents');
  $('overview').hidden = view !== 'overview';
  $('monitoring').hidden = view !== 'monitoring';
  $('explorer').hidden = !['explorer', 'query', 'vectors', 'media'].includes(view);
  for (const name of ['overview', 'explorer', 'query', 'vectors', 'media', 'monitoring']) {
    $(`nav-${name}`).classList.toggle('active', view === name);
    if (name === view) $(`nav-${name}`).setAttribute('aria-current', 'page');
    else $(`nav-${name}`).removeAttribute('aria-current');
  }
  $('top-location').textContent = ({ overview: 'Overview', explorer: 'Data Explorer',
    query: 'Query Workbench', vectors: 'Vectors', media: 'Media', monitoring: 'Monitoring' })[view];
  renderPageHeader();
  closeNavigation();
  if (view === 'overview') void loadCollectionSummaries().catch(error => notice(error.message, true));
}
function renderPageHeader() {
  const view = state.view;
  const context = [state.database, state.collection].filter(Boolean).join(' / ');
  const page = {
    explorer: ['DATA EXPLORER', state.collection ? 'Documents' : state.database || 'Data Explorer'],
    query: ['QUERY WORKBENCH', 'Query Workbench'],
    vectors: ['VECTOR SEARCH', 'Vector Search'],
    media: ['MEDIA LIBRARY', 'Media Library'],
  }[view] || ['DATA EXPLORER', 'Data Explorer'];
  $('page-kicker').textContent = page[0];
  $('title').textContent = page[1];
  $('breadcrumb').textContent = context || 'Create a database to begin';
  $('database-identity').hidden = !state.database || view !== 'explorer';
  $('new-document').hidden = !state.collection || view !== 'explorer';
  $('delete-database').hidden = !state.database;
  $('explorer').dataset.view = view;
  $('query-kicker').textContent = view === 'query' ? 'QUERY EDITOR' : 'DOCUMENT FILTER';
  $('query-heading').textContent = view === 'query' ? 'Run a document query' : 'Filter documents';
  $('query-description').textContent = view === 'query' ?
    'Write a JSON filter, run it, and inspect the matching records.' :
    'Narrow the records shown in this collection.';
  $('document-results-title').textContent = view === 'query' ? 'Matching records' : 'Documents';
}
function selectTab(name) {
  for (const item of document.querySelectorAll('.tab')) {
    const active = item.dataset.tab === name;
    item.classList.toggle('active', active);
    item.setAttribute('aria-selected', String(active));
    item.tabIndex = active ? 0 : -1;
  }
  for (const panel of document.querySelectorAll('.tab-panel')) panel.hidden = panel.id !== name;
}
function tab() { return document.querySelector('.tab.active').dataset.tab; }
function invalidateSelection(incrementNavigation = true) {
  if (incrementNavigation) state.navigation++;
  state.indexGeneration++;
  state.documents = [];
  state.indexes = [];
  $('index-list').replaceChildren();
  $('index-report').hidden = true;
  state.documentRequest?.abort();
  state.mediaRequest?.abort();
  state.documentOffset = 0;
  state.documentFilter = {};
  state.mediaOffset = 0;
  state.mediaOffsets = [];
  state.mediaNext = null;
  state.documentHasNext = false;
  $('filter').value = '{}';
  $('document-list').replaceChildren();
  $('media-list').replaceChildren();
  $('vector-result').textContent = '';
  $('document-editor').open = false;
  $('document-json').value = '{}';
  $('write-filter').value = '{}';
  $('document-result-count').textContent = '';
  updateView();
}
function treeRow(name, kind, selected, onClick, { id, addKind } = {}) {
  const node = document.createElement('div');
  const key = `${kind}:${id || name}`;
  node.className = 'tree-node';
  const row = document.createElement('div');
  row.className = 'tree-row';
  row.classList.toggle('selected', selected);
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'tree-item';
  button.title = `${kind}: ${name}${id ? ` · ${id}` : ''}`;
  button.setAttribute('role', 'treeitem');
  button.setAttribute('aria-selected', String(selected));
  if (kind !== 'collection') button.setAttribute('aria-expanded', String(selected));
  const icon = document.createElement('span');
  icon.className = 'tree-icon';
  icon.textContent = kind === 'database' ? '▤' : '▦';
  const label = document.createElement('span');
  label.className = 'tree-name';
  label.textContent = name;
  if (id) {
    const identifier = document.createElement('small');
    identifier.className = 'tree-id';
    identifier.textContent = id;
    label.append(identifier);
  }
  button.append(icon, label);
  if (kind !== 'collection') {
    const toggle = document.createElement('button');
    toggle.type = 'button'; toggle.className = 'tree-toggle';
    const expanded = selected && !state.collapsed.has(key);
    toggle.textContent = expanded ? '⌄' : '›';
    toggle.setAttribute('aria-label', `${expanded ? 'Collapse' : 'Expand'} ${kind}`);
    toggle.setAttribute('aria-expanded', String(expanded));
    toggle.addEventListener('click', () => {
      const children = node.querySelector(':scope > .tree-children');
      if (!children) { state.collapsed.delete(key); void run(onClick); return; }
      children.hidden = !children.hidden;
      if (children.hidden) state.collapsed.add(key); else state.collapsed.delete(key);
      toggle.textContent = children.hidden ? '›' : '⌄';
      toggle.setAttribute('aria-label', `${children.hidden ? 'Expand' : 'Collapse'} ${kind}`);
      toggle.setAttribute('aria-expanded', String(!children.hidden));
      button.setAttribute('aria-expanded', String(!children.hidden));
    });
    button.setAttribute('aria-expanded', String(expanded));
    button.addEventListener('keydown', (event) => {
      if ((event.key === 'ArrowRight' && toggle.getAttribute('aria-expanded') === 'false') ||
          (event.key === 'ArrowLeft' && toggle.getAttribute('aria-expanded') === 'true')) {
        event.preventDefault(); toggle.click();
      }
    });
    row.append(toggle);
  }
  button.addEventListener('click', () => { state.collapsed.delete(key); void run(onClick); });
  row.append(button);
  if (addKind) {
    const add = document.createElement('button');
    add.id = `add-${addKind}`;
    add.type = 'button';
    add.className = 'tree-add';
    add.textContent = '+';
    add.title = `Create ${addKind} in ${name}`;
    add.setAttribute('aria-label', `Create ${addKind} in ${name}`);
    add.addEventListener('click', () => run(() => openCreate(addKind)));
    row.append(add);
  }
  node.append(row);
  return node;
}
function renderTree() {
  const fragment = document.createDocumentFragment();
  const query = state.navQuery;
  const matches = name => name.toLowerCase().includes(query);
  for (const database of state.databases) {
    const active = database === state.database;
    const dbMatch = matches(database);
    if (query && !dbMatch && !(active && state.collections.some(matches))) continue;
    const node = treeRow(database, 'database', active, async () => {
      state.database = database;
      state.collection = null;
      state.collections = [];
      invalidateSelection();
      setView('explorer');
      await refreshCollections(state.navigation, true);
    }, { addKind: active ? 'collection' : undefined });
    if (active) {
      const children = document.createElement('div');
      children.className = 'tree-children';
      children.hidden = state.collapsed.has(`database:${database}`);
      children.setAttribute('role', 'group');
      if (!state.collections.length) {
        const empty = document.createElement('div'); empty.className = 'tree-empty';
        empty.textContent = 'No collections yet'; children.append(empty);
      }
      for (const collection of state.collections) {
        if (query && !dbMatch && !matches(collection)) continue;
        children.append(treeRow(collection, 'collection', collection === state.collection, async () => {
          state.collection = collection;
          invalidateSelection();
          setView('explorer');
          await refreshActive();
        }));
      }
      node.append(children);
    }
    fragment.append(node);
  }
  if (!fragment.childElementCount) {
    const empty = document.createElement('div'); empty.className = 'empty-nav';
    empty.textContent = state.databases.length ? 'No matching databases or collections' : 'No databases yet';
    fragment.append(empty);
  }
  $('databases').replaceChildren(fragment);
}
function ensureSummaryScope() {
  const database = state.database || '';
  if (state.summaryScope !== database) {
    state.summaryGeneration++;
    state.summaryScope = database;
    state.collectionSummaries.clear();
  }
}
function invalidateSummary(selected = scope()) {
  if (selected.database !== state.database) return;
  state.summaryGeneration++;
  state.collectionSummaries.delete(selected.collection);
  renderCollections();
  if (state.view === 'overview') void loadCollectionSummaries().catch(error => notice(error.message, true));
}
function updateView() {
  ensureSummaryScope();
  renderTree();
  $('welcome').hidden = Boolean(state.collection);
  $('workspace').hidden = !state.collection;
  $('database-identity-name').textContent = state.database || '';
  renderPageHeader();
  renderHistory();
  if ($('connection-dialog').open) renderConnection();
  $('collection-name').textContent = state.collection || '';
  $('welcome-title').textContent = state.database ? (state.collections.length ? 'Choose a collection' : 'No collections yet') :
    (state.databases.length ? 'Choose a database' : 'No databases yet');
  $('welcome-description').textContent = state.database ? 'Create a collection to store documents, vectors, and media.' :
    (state.databases.length ? 'Select a database from the sidebar to explore its collections.' : 'Create your first database, then add a collection and your data.');
  $('welcome-create').textContent = state.database ? 'Create a collection' : 'Create a database';
  $('database-count').textContent = String(state.databases.length);
  $('collection-count').textContent = String(state.collections.length);
  $('database-context').textContent = 'Accessible databases';
  $('collection-context').textContent = state.database ? `In ${state.database}` : 'Choose a database';
  $('top-database').textContent = state.database || 'Workspace';
  $('collections-subtitle').textContent = state.database ? `In ${state.database}` : 'Choose a database to explore.';
  renderCollections();
}
async function refreshDatabases(version, selectInitial = false) {
  if (version === undefined) version = ++state.navigation;
  const result = await request('databases.list');
  if (version !== state.navigation) return;
  state.databases = result.databases || [];
  if (!state.databases.includes(state.database)) {
    const missing = Boolean(state.database);
    if (missing) notice(`Database ${state.database} is unavailable. Choose a database.`, true);
    state.database = selectInitial && !missing ? (state.databases[0] || null) : null;
    state.collection = null;
    state.collections = [];
    invalidateSelection(false);
  }
  await refreshCollections(version, selectInitial);
}
async function refreshCollections(version = state.navigation, selectInitial = false) {
  const result = state.database ? await request('collections.list') : { collections: [] };
  if (version !== state.navigation) return;
  state.collections = result.collections || [];
  if (!state.collections.includes(state.collection)) {
    const missing = Boolean(state.collection);
    state.collection = selectInitial && !missing ? (state.collections[0] || null) : null;
    invalidateSelection(false);
  }
  updateView();
  if (state.view === 'overview') await loadCollectionSummaries();
  await refreshActive();
}
async function refreshActive() {
  if (!['explorer', 'query', 'vectors', 'media'].includes(state.view) || !state.collection) return;
  if (tab() === 'documents') await loadDocuments(state.documentOffset);
  if (tab() === 'media') await loadMedia(state.mediaOffset);
  if (tab() === 'indexes') await loadIndexes();
}
function record(title, body, buttonText, onClick) {
  const card = document.createElement('div');
  card.className = 'record';
  const head = document.createElement('div');
  head.className = 'record-head';
  const strong = document.createElement('strong');
  strong.textContent = title;
  head.append(strong);
  if (buttonText) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = buttonText;
    button.addEventListener('click', () => run(onClick, button));
    head.append(button);
  }
  const pre = document.createElement('pre');
  pre.textContent = typeof body === 'string' ? body : JSON.stringify(body, null, 2);
  card.append(head, pre);
  return card;
}
function visibleDocument(doc) {
  return Object.fromEntries(Object.entries(doc).filter(([name]) =>
    !internalDocumentFields.has(name)));
}
function openEditor(doc = {}) {
  if (state.view === 'query') setView('explorer');
  const visible = visibleDocument(doc);
  $('document-json').value = JSON.stringify(visible, null, 2);
  const field = doc.id !== undefined ? 'id' : doc._id !== undefined ? '_id' : null;
  $('write-filter').value = JSON.stringify(field ? { [field]: doc[field] } : {});
  $('document-editor').open = true;
  $('document-editor').scrollIntoView({ block: 'nearest' });
  updateJsonEditor();
  $('document-json').focus({ preventScroll: true });
}
async function loadDocuments(offset = 0, newFilter = state.documentFilter) {
  if (!state.collection) return;
  state.documentRequest?.abort();
  const controller = state.documentRequest = new AbortController();
  const key = scopeKey();
  const started = performance.now();
  const list = $('document-list');
  list.setAttribute('aria-busy', 'true');
  $('previous-docs').disabled = $('next-docs').disabled = true;
  $('document-summary').textContent = 'Loading documents…';
  try {
    // One extra record detects the next page without a separate count query.
    const result = await request('documents.find', { filter: newFilter,
      limit: state.pageSize + 1, offset }, controller.signal);
    if (controller.signal.aborted || key !== scopeKey()) return;
    const all = Array.isArray(result) ? result : result.data || [];
    const docs = all.slice(0, state.pageSize);
    state.documents = docs;
    renderDocuments();
    state.documentOffset = offset;
    state.documentFilter = newFilter;
    state.documentHasNext = all.length > state.pageSize;
    $('document-result-count').textContent = String(docs.length);
    $('document-summary').textContent = `${docs.length} record${docs.length === 1 ? '' : 's'} · ${Math.round(performance.now() - started)} ms`;
    $('document-page').textContent = docs.length ? `${offset + 1}–${offset + docs.length} shown` : '0 records';
  } catch (error) {
    if (controller.signal.aborted || key !== scopeKey()) return;
    state.documents = [];
    list.textContent = 'Could not load documents. Check the filter and try again.';
    $('document-summary').textContent = 'Query failed';
    $('document-result-count').textContent = '';
    $('document-page').textContent = '';
    state.documentHasNext = false;
    throw error;
  } finally {
    if (state.documentRequest === controller) {
      list.setAttribute('aria-busy', 'false');
      $('previous-docs').disabled = state.documentOffset === 0;
      $('next-docs').disabled = !state.documentHasNext;
    }
  }
}
async function loadMedia(offset = 0) {
  if (!state.collection) return;
  state.mediaRequest?.abort();
  const controller = state.mediaRequest = new AbortController();
  const key = scopeKey();
  const selected = scope();
  const list = $('media-list');
  list.setAttribute('aria-busy', 'true');
  $('previous-media').disabled = $('next-media').disabled = true;
  try {
    const result = await request('media.list', { limit: 50, offset }, controller.signal);
    if (controller.signal.aborted || key !== scopeKey()) return;
    const files = (result.media || []).filter((file) => file.collection === selected.collection);
    const fragment = document.createDocumentFragment();
    for (const file of files) {
      const card = record(file.filename || file.id, {
        id: file.id, status: file.status, sizeBytes: file.size_bytes, contentType: file.content_type,
      }, file.status === 'ready' ? 'Download' : '', () => downloadMedia(file, selected));
      card.dataset.search = `${file.filename || ''} ${file.id}`.toLowerCase();
      if (file.status === 'ready') actionButton(card.querySelector('.record-head'), 'Preview', () => previewMedia(file, selected));
      actionButton(card.querySelector('.record-head'), 'Delete', () => confirmAction('Delete file?',
        `Permanently delete ${file.filename || file.id}? This action cannot be undone.`, async () => {
          await request('media.delete', { ...selected, mediaId: file.id });
          invalidateSummary(selected);
          if (scopeKey(selected) === scopeKey()) await loadMedia(state.mediaOffset);
          notice('File deleted.');
        }), 'danger');
      fragment.append(card);
    }
    list.replaceChildren(fragment);
    if (!files.length) list.textContent = offset ? 'No files on this page. Return to the previous page or upload a file.' : 'No files yet. Upload a file to store media in this collection.';
    filterMedia();
    state.mediaOffset = offset;
    state.mediaNext = result.has_more ? result.next_offset : null;
    $('media-page').textContent = `${files.length} file${files.length === 1 ? '' : 's'} on this page`;
  } catch (error) {
    if (controller.signal.aborted || key !== scopeKey()) return;
    list.textContent = 'Could not load files. Try refreshing.';
    throw error;
  } finally {
    if (state.mediaRequest === controller) {
      list.setAttribute('aria-busy', 'false');
      $('previous-media').disabled = !state.mediaOffsets.length;
      $('next-media').disabled = state.mediaNext === null;
    }
  }
}
async function mediaBlob(file, selected) {
  const response = await fetch('/api/media/download', {
    method: 'POST', headers: { 'Content-Type': 'application/json',
      'X-PacificDB-Workbench-Token': token },
    body: JSON.stringify({ ...selected, mediaId: file.id }),
  });
  if (!response.ok) throw new Error((await response.json()).error || 'Download failed');
  return response.blob();
}
async function downloadMedia(file, selected) {
  const url = URL.createObjectURL(await mediaBlob(file, selected));
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = file.filename || file.id;
  anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
  notice(`Downloaded ${file.filename || file.id}.`);
}
async function uploadMedia() {
  const file = $('media-file').files[0];
  if (!file) throw new Error('Choose a file first.');
  if (!file.size || file.size > 64 * 1024 * 1024) throw new Error('Choose a nonempty file up to 64 MiB.');
  const selected = scope();
  notice(`Uploading ${file.name}…`);
  const response = await fetch('/api/media/upload', {
    method: 'POST', headers: { 'Content-Type': 'application/octet-stream',
      'X-PacificDB-Workbench-Token': token,
      'X-Database': encodeURIComponent(selected.database), 'X-Collection': encodeURIComponent(selected.collection),
      'X-Filename': encodeURIComponent(file.name) }, body: file,
  });
  const payload = await response.json();
  if (!response.ok) throw new Error(payload.error || 'Upload failed');
  invalidateSummary(selected);
  notice(`Uploaded ${file.name} to ${selected.collection}.`);
  addActivity('write', 'Media uploaded', `${selected.collection} · ${file.name}`);
  if (scopeKey(selected) === scopeKey()) {
    $('media-file').value = '';
    state.mediaOffsets = [];
    await loadMedia(0);
  }
}
function openCreate(kind) {
  if (!['database', 'collection'].includes(kind)) throw new Error('Unknown resource type.');
  if (kind === 'collection' && !state.database) throw new Error('Create or select a database first.');
  state.createKind = kind;
  state.createScope = scope();
  $('create-title').textContent = `Create ${kind}`;
  $('create-description').textContent = kind === 'database' ? 'Create a database for your application.' : `Add a collection to ${state.database}.`;
  $('create-name-label').textContent = `${kind[0].toUpperCase() + kind.slice(1)} name`;
  $('submit-create').textContent = `Create ${kind}`;
  $('create-name').placeholder = `my-${kind}`;
  $('create-guidance').textContent = `Names must be 1–${kind === 'collection' ? 251 : 255} UTF-8 bytes.`;
  $('create-name').value = '';
  $('create-name').maxLength = kind === 'collection' ? 251 : 255;
  $('create-error').textContent = '';
  $('create-dialog').showModal();
  $('create-name').focus();
}
async function createResource() {
  const kind = state.createKind;
  const name = $('create-name').value.trim();
  if (!name) throw new Error('Enter a name.');
  if (new TextEncoder().encode(name).length > (kind === 'collection' ? 251 : 255)) throw new Error('Name exceeds the UTF-8 byte limit.');
  const selected = { ...state.createScope };
  const navigation = state.navigation;
  await request(`${kind === 'database' ? 'databases' : 'collections'}.create`, { ...selected, name });
  $('create-dialog').close();
  if (navigation === state.navigation && scopeKey(selected) === scopeKey()) {
    setView('explorer');
    state.navQuery = ''; $('nav-search').value = '';
    if (kind === 'database') {
      state.database = name; state.collection = null; state.collections = [];
      invalidateSelection();
      await refreshDatabases(state.navigation);
    } else {
      state.collection = name;
      invalidateSummary({ ...selected, collection: name });
      invalidateSelection();
      await refreshCollections(state.navigation);
    }
  }
  notice(`Created ${kind} ${name}.`);
  addActivity('workspace', `${kind[0].toUpperCase()}${kind.slice(1)} created`, name);
}
async function writeDocument(operation) {
  if (state.writePending) return;
  const selected = scope();
  const values = { ...selected };
  if (operation === 'insert') values.data = jsonField('document-json', 'Document');
  else {
    values.filter = jsonField('write-filter', 'Filter');
    if (!Object.keys(values.filter).length) throw new Error('Enter a specific filter before updating or deleting.');
    if (operation === 'update') values.update = jsonField('document-json', 'Document');
    if (operation === 'delete' && !confirm(`Delete one matching document from ${selected.collection}?`)) return;
  }
  const writeButton = $(`${operation}-doc`);
  const writeLabel = writeButton.textContent;
  writeButton.textContent = { insert: 'Inserting…', update: 'Saving…', delete: 'Deleting…' }[operation];
  state.writePending = true;
  for (const id of ['insert-doc', 'update-doc', 'delete-doc']) $(id).disabled = true;
  try {
    await request(`documents.${operation}`, values);
    addActivity('write', `Document ${operation === 'insert' ? 'inserted' : operation === 'update' ? 'updated' : 'deleted'}`, selected.collection);
    invalidateSummary(selected);
    if (scopeKey(selected) === scopeKey()) await loadDocuments(0);
    notice(`Document ${operation === 'insert' ? 'inserted' : operation === 'update' ? 'updated' : 'deleted'} in ${selected.collection}.`);
  } finally {
    state.writePending = false;
    writeButton.textContent = writeLabel;
    for (const id of ['insert-doc', 'update-doc', 'delete-doc']) $(id).disabled = false;
  }
}
const number = (value) => Number.isFinite(value) ? value.toLocaleString() : '—';
function addActivity(type, label, detail) {
  state.activity.unshift({ type, label, detail, at: Date.now() });
  state.activity.length = Math.min(state.activity.length, 50);
  renderActivity();
}
function renderActivity() {
  const cutoff = $('activity-range').value === 'hour' ? Date.now() - 3_600_000 :
    $('activity-range').value === 'day' ? Date.now() - 86_400_000 : 0;
  const kind = $('activity-filter').value;
  const entries = state.activity.filter((item) => item.at >= cutoff && (kind === 'all' || item.type === kind));
  const fragment = document.createDocumentFragment();
  for (const item of entries.slice(0, 8)) {
    const row = document.createElement('div');
    row.className = `activity-row ${item.type}`;
    const dot = document.createElement('span'); dot.className = 'activity-dot';
    const label = document.createElement('span'); label.textContent = item.label;
    const detail = document.createElement('span'); detail.textContent = item.detail;
    const time = document.createElement('time'); time.dateTime = new Date(item.at).toISOString();
    time.textContent = new Date(item.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    row.append(dot, label, detail, time); fragment.append(row);
  }
  $('activity-list').replaceChildren(fragment);
  if (!entries.length) $('activity-list').textContent = 'No activity in this period.';
}
function renderStorage() {
  const known = state.collections.map((name) => ({ name, count: state.collectionSummaries.get(name)?.count }))
    .filter((item) => Number.isFinite(item.count) && item.count > 0).sort((a, b) => b.count - a.count);
  const total = known.reduce((sum, item) => sum + item.count, 0);
  $('storage-total').firstChild.textContent = state.database && state.collections.every(name => Number.isFinite(state.collectionSummaries.get(name)?.count)) ? number(total) : '—';
  const visible = known.slice(0, 5);
  if (known.length > 5) visible.push({ name: 'Other', count: known.slice(5).reduce((sum, item) => sum + item.count, 0) });
  const colors = ['#a873ef', '#49a8ff', '#31cbbd', '#58d47c', '#efbb5a', '#9ab1cf'];
  let offset = 0;
  const stops = [];
  const fragment = document.createDocumentFragment();
  for (const [index, item] of visible.entries()) {
    const start = offset; offset += item.count / total * 100;
    stops.push(`${colors[index]} ${start}% ${offset}%`);
    const row = document.createElement('div');
    const swatch = document.createElement('i'); swatch.style.backgroundColor = colors[index];
    const label = document.createElement('span'); label.textContent = item.name;
    const count = document.createElement('strong'); count.textContent = number(item.count);
    const ratio = document.createElement('small'); ratio.textContent = `${(item.count / total * 100).toFixed(1)}%`;
    row.append(swatch, label, count, ratio); fragment.append(row);
  }
  $('storage-donut').style.background = total ? `conic-gradient(${stops.join(', ')})` : 'conic-gradient(#29405d 0 100%)';
  $('storage-legend').replaceChildren(fragment);
  if (!total) $('storage-legend').textContent = state.collections.length ? 'No documents counted yet.' : 'No collections in this database.';
}
function renderCollections() {
  const search = $('collection-search').value.trim().toLowerCase();
  const filter = $('collection-type').value;
  const rows = state.collections.filter((name) => {
    const count = state.collectionSummaries.get(name)?.count;
    return name.toLowerCase().includes(search) && (filter === 'all' ||
      filter === 'populated' && count > 0 || filter === 'empty' && count === 0);
  }).sort((a, b) => state.collectionSort === 'asc' ? a.localeCompare(b) : b.localeCompare(a));
  const fragment = document.createDocumentFragment();
  for (const name of rows) {
    const summary = state.collectionSummaries.get(name);
    const row = document.createElement('tr');
    const nameCell = document.createElement('td');
    const open = document.createElement('button'); open.type = 'button'; open.className = 'collection-name';
    open.textContent = `▤  ${name}`; open.addEventListener('click', () => run(() => openCollection(name)));
    nameCell.append(open);
    const type = document.createElement('td'); type.textContent = 'Collection';
    const count = document.createElement('td'); count.textContent = summary ? number(summary.count) : '—';
    const indexes = document.createElement('td'); indexes.textContent = summary ? number(summary.indexes) : '—';
    row.append(nameCell, type, count, indexes); fragment.append(row);
  }
  $('collection-rows').replaceChildren(fragment);
  $('collection-empty').hidden = rows.length > 0;
  $('collection-metadata').textContent = state.collection ? `Documents: ${number(state.collectionSummaries.get(state.collection)?.count)} · Indexes: ${number(state.collectionSummaries.get(state.collection)?.indexes)}` : '';
  $('collection-empty').textContent = state.database ? state.collections.length ? 'No collections match this filter.' :
    'No collections yet. Create one from the object explorer.' : 'Choose a database to see collections.';
  const summaries = state.collections.map((name) => state.collectionSummaries.get(name));
  const complete = Boolean(state.database) && summaries.every((item) => item !== undefined);
  const valid = summaries.every((item) => item && Number.isFinite(item.count));
  $('total-documents').textContent = complete && valid ? number(summaries.reduce((sum, item) => sum + item.count, 0)) : '—';
  $('document-context').textContent = !state.database ? 'Choose a database' : complete && !valid ? 'Some counts unavailable' :
    complete ? `In ${state.database}` : state.summaryTask && state.view === 'overview' ? 'Counting documents…' : 'Counts not loaded';
  renderStorage();
}
let pendingSummaryRender = false;
function scheduleSummaryRender() {
  if (pendingSummaryRender) return;
  pendingSummaryRender = true;
  requestAnimationFrame(() => { pendingSummaryRender = false; renderCollections(); });
}
async function loadCollectionSummaries() {
  if (state.view !== 'overview' || !state.database) return;
  ensureSummaryScope();
  if (state.summaryTask) {
    await state.summaryTask;
    return loadCollectionSummaries();
  }
  const generation = ++state.summaryGeneration;
  const selected = scope();
  const names = state.collections.filter(name => !state.collectionSummaries.has(name));
  if (!names.length) return;
  let cursor = 0;
  const current = () => generation === state.summaryGeneration && state.view === 'overview' && selected.database === state.database;
  // One existing four-worker loader at a time, including obsolete in-flight requests.
  const task = Promise.all(Array.from({ length: Math.min(4, names.length) }, async () => {
    while (cursor < names.length && current()) {
      const name = names[cursor++];
      let summary;
      try { summary = await request('collections.summary', { ...selected, collection: name }); }
      catch { summary = { count: null, indexes: null }; }
      if (!current()) return;
      state.collectionSummaries.set(name, summary);
      scheduleSummaryRender();
    }
  }));
  state.summaryTask = task;
  renderCollections();
  try { await task; }
  finally {
    if (state.summaryTask === task) state.summaryTask = null;
    if (current()) renderCollections();
  }
}
async function openCollection(name, selectedTab = 'documents') {
  if (!state.database || !state.collections.includes(name)) return;
  state.collection = name;
  invalidateSelection();
  selectTab(selectedTab);
  setView(selectedTab === 'documents' ? 'explorer' : selectedTab);
  await refreshActive();
}
function showHealth(data) {
  const health = data?.health || {};
  const cluster = data?.cluster || {};
  const status = health.status || 'Unavailable';
  $('health-open').textContent = `${status} ›`;
  $('health-open').dataset.status = status;
  $('health-status').textContent = $('monitor-status').textContent = status;
  $('health-latency').textContent = $('monitor-latency').textContent = Number.isFinite(data?.latencyMs) ? `${data.latencyMs} ms` : '—';
  $('health-memory').textContent = $('monitor-memory').textContent = Number.isFinite(health.memory_usage_ratio) ?
    `${(health.memory_usage_ratio * 100).toFixed(1)}%` : '—';
  $('health-workers').textContent = $('monitor-workers').textContent = number(health.connection_pool_active);
  $('monitor-queue').textContent = number(health.connection_pool_queue);
  $('health-role').textContent = $('monitor-role').textContent = cluster.role || health.role || '—';
  $('health-term').textContent = $('monitor-term').textContent = number(cluster.raft_current_term);
  $('health-commit').textContent = $('monitor-commit').textContent = number(cluster.raft_commit_index);
  $('health-applied').textContent = number(cluster.raft_last_applied);
  $('health-queue').textContent = number(cluster.raft_apply_queue_depth);
  $('monitor-applied').textContent = number(cluster.raft_last_applied);
  $('monitor-raft-queue').textContent = number(cluster.raft_apply_queue_depth);
}
async function refreshEngine() {
  try { showHealth(await request('engine.status')); }
  catch { showHealth(null); connection(false, true); }
}
function renderCommands() {
  const query = $('command-input').value.trim().toLowerCase();
  const commands = [
    ['Overview', () => setView('overview')],
    ['Data Explorer', () => setView('explorer')],
    ['Query Workbench', () => openTool('query')],
    ['Vectors', () => openTool('vectors')],
    ['Media', () => openTool('media')],
    ['Monitoring', () => openTool('monitoring')],
    ['New database', () => openCreate('database')],
    ['Connection options', openConnection],
  ];
  for (const name of state.collections) commands.push([`Collection · ${name}`, () => openCollection(name)]);
  const fragment = document.createDocumentFragment();
  const matches = commands.filter(([label]) => label.toLowerCase().includes(query)).slice(0, 12);
  for (const [label, action] of matches) {
    const button = document.createElement('button'); button.type = 'button'; button.textContent = label;
    button.addEventListener('click', () => { $('command-dialog').close(); void run(action); });
    fragment.append(button);
  }
  $('command-results').replaceChildren(fragment);
  if (!matches.length) $('command-results').textContent = 'No matching commands or collections.';
}
async function openTool(view) {
  if (view === 'monitoring') { setView(view); await refreshMonitoring(); return; }
  if (!state.collection) { setView('explorer'); notice('Choose or create a collection first.'); return; }
  selectTab(view === 'query' ? 'documents' : view);
  setView(view);
  await refreshActive();
  if (view === 'query') $('filter').focus();
}
function click(id, fn, disable = false) {
  $(id).addEventListener('click', () => run(fn, disable ? $(id) : undefined));
}
for (const id of ['environment-open', 'quick-cli']) click(id, openConnection);
click('close-connection', () => $('connection-dialog').close());
click('copy-connection', async () => {
  await copyText($('connection-example').textContent);
  notice('Connection example copied.');
});
for (const format of ['url', 'cli', 'node', 'python', 'java']) click(`connection-${format}`, () => {
  state.connectionFormat = format;
  renderConnection();
});
click('welcome-create', () => openCreate(state.database ? 'collection' : 'database'));
click('overview-create', () => openCreate('database'));
click('nav-overview', () => setView('overview'));
for (const view of ['query', 'vectors', 'media', 'monitoring']) click(`nav-${view}`, () => openTool(view));
click('quick-query', () => openTool('query'));
click('health-open', () => openTool('monitoring'));
for (const id of ['open-databases', 'open-collections']) click(id, () => setView('explorer'));
for (const id of ['nav-explorer']) click(id, async () => {
  setView('explorer');
  await refreshActive();
});
for (const id of ['overview-refresh', 'refresh-all']) click(id, async () => {
  if (state.view === 'overview') { state.summaryGeneration++; state.collectionSummaries.clear(); }
  await Promise.all([refreshDatabases(), refreshEngine()]);
}, true);
click('refresh-collections', async () => { state.summaryGeneration++; state.collectionSummaries.clear(); await refreshCollections(); }, true);
click('monitoring-refresh', refreshMonitoring, true);
for (const id of ['collection-search', 'collection-type']) $(id).addEventListener(id === 'collection-search' ? 'input' : 'change', renderCollections);
click('sort-collections', () => { state.collectionSort = state.collectionSort === 'asc' ? 'desc' : 'asc'; renderCollections(); });
for (const layout of ['list', 'grid']) click(`collection-${layout}-view`, () => {
  state.collectionLayout = layout;
  $('collection-rows').classList.toggle('grid-layout', layout === 'grid');
  for (const name of ['list', 'grid']) {
    $(`collection-${name}-view`).classList.toggle('active', name === layout);
    $(`collection-${name}-view`).setAttribute('aria-pressed', String(name === layout));
  }
});
for (const id of ['activity-range', 'activity-filter']) $(id).addEventListener('change', renderActivity);
click('global-search', () => { $('command-input').value = ''; renderCommands(); $('command-dialog').showModal(); $('command-input').focus(); });
$('command-input').addEventListener('input', renderCommands);
$('command-input').addEventListener('keydown', (event) => {
  if (event.key === 'Enter') { event.preventDefault(); $('command-results').querySelector('button')?.click(); }
});
$('nav-search').addEventListener('input', () => {
  state.navQuery = $('nav-search').value.trim().toLowerCase();
  updateView();
});
click('mobile-menu', () => {
  const open = $('sidebar').classList.toggle('open');
  $('nav-scrim').hidden = !open;
  $('mobile-menu').setAttribute('aria-expanded', String(open));
});
click('nav-scrim', closeNavigation);
click('open-settings', () => $('settings-dialog').showModal());
for (const id of ['close-settings', 'settings-done']) click(id, () => $('settings-dialog').close());
for (const id of ['theme-select', 'density-select']) $(id).addEventListener('change', savePreferences);
for (const id of ['close-create', 'cancel-create']) click(id, () => $('create-dialog').close());
$('create-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if ($('submit-create').disabled) return;
  $('submit-create').disabled = true;
  $('submit-create').textContent = 'Creating…';
  $('create-error').textContent = '';
  try { await createResource(); }
  catch (error) { $('create-error').textContent = error.message; }
  finally { $('submit-create').disabled = false; $('submit-create').textContent = `Create ${state.createKind}`; }
});
click('find-docs', executeQuery);
click('refresh-docs', () => loadDocuments(state.documentOffset));
click('previous-docs', () => loadDocuments(Math.max(0, state.documentOffset - state.pageSize)));
click('next-docs', () => loadDocuments(state.documentOffset + state.pageSize));
$('page-size').addEventListener('change', () => run(async () => {
  state.pageSize = Number($('page-size').value);
  savePreferences();
  await loadDocuments(0);
}));
for (const view of ['cards', 'compact', 'table']) click(`view-${view}`, () => {
  state.documentView = view;
  savePreferences();
});
click('new-document', async () => { selectTab('documents'); await refreshActive(); openEditor(); });
for (const operation of ['insert', 'update', 'delete']) click(`${operation}-doc`, () => writeDocument(operation));
for (const operation of ['put', 'query']) click(`${operation}-vector`, async () => {
  const selected = scope();
  const vector = jsonField('vector-values', 'Vector', true);
  if (!vector.length || !vector.every(Number.isFinite)) throw new Error('Vector must contain finite numbers.');
  const k = Number($('vector-k').value);
  if (operation === 'query' && (!Number.isInteger(k) || k < 1 || k > 100)) throw new Error('Results must be between 1 and 100.');
  const result = await request(`vectors.${operation}`, { ...selected, vector, k,
    ...(operation === 'put' ? { id: $('vector-id').value, metadata: jsonField('vector-metadata', 'Metadata') } :
      { metric: $('vector-metric').value, filter: jsonField('vector-filter', 'Vector filter') }) });
  if (operation === 'put') invalidateSummary(selected);
  if (scopeKey(selected) === scopeKey()) $('vector-result').textContent = JSON.stringify(result, null, 2);
  addActivity(operation === 'put' ? 'write' : 'query', operation === 'put' ? 'Vector stored' : 'Vector search', selected.collection);
  notice(operation === 'put' ? 'Vector stored.' : 'Vector search complete.');
}, true);
click('refresh-media', () => loadMedia(state.mediaOffset));
click('upload-media', uploadMedia, true);
click('next-media', async () => {
  if (state.mediaNext === null) return;
  const previous = state.mediaOffset;
  const key = scopeKey();
  await loadMedia(state.mediaNext);
  if (key !== scopeKey()) return;
  if (state.mediaOffset !== previous) state.mediaOffsets.push(previous);
  $('previous-media').disabled = !state.mediaOffsets.length;
});
click('previous-media', async () => {
  const previous = state.mediaOffsets.at(-1);
  if (previous === undefined) return;
  const key = scopeKey();
  await loadMedia(previous);
  if (key !== scopeKey()) return;
  if (state.mediaOffset === previous) state.mediaOffsets.pop();
  $('previous-media').disabled = !state.mediaOffsets.length;
});
for (const button of document.querySelectorAll('.tab, [data-open-tab]')) {
  button.addEventListener('click', () => run(async () => {
    const selected = button.dataset.tab || button.dataset.openTab;
    selectTab(selected);
    setView(['documents', 'indexes'].includes(selected) ? 'explorer' : selected);
    selectTab(selected);
    await refreshActive();
  }));
}
document.querySelector('.tabs').addEventListener('keydown', (event) => {
  if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
  const tabs = [...document.querySelectorAll('.tab')];
  const index = tabs.indexOf(document.activeElement);
  if (index < 0) return;
  event.preventDefault();
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 :
    (index + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
  tabs[next].focus();
  tabs[next].click();
});
document.addEventListener('keydown', (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
    event.preventDefault();
    if (!$('command-dialog').open) $('global-search').click();
    return;
  }
  if (document.querySelector('dialog[open]')) return;
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter' && ['explorer', 'query'].includes(state.view) && tab() === 'documents' && state.collection) {
    event.preventDefault();
    void run(executeQuery);
  }
  if (event.key === '/' && !/INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName)) {
    event.preventDefault();
    if (matchMedia('(max-width:760px)').matches && !$('sidebar').classList.contains('open')) $('mobile-menu').click();
    $('nav-search').focus();
  }
  if (event.key === 'Escape') closeNavigation();
});

function actionButton(parent, label, action, className = 'subtle-button') {
  const button = document.createElement('button');
  button.type = 'button'; button.textContent = label; button.className = className;
  button.addEventListener('click', () => run(action, button));
  parent.append(button);
  return button;
}
function renderDocuments() {
  const list = $('document-list');
  const docs = state.documents;
  list.replaceChildren();
  if (!docs.length) {
    list.classList.add('empty-state');
    list.textContent = 'No documents match this filter. Insert a document or adjust your filter to get started.';
    return;
  }
  list.classList.remove('empty-state');
  const selected = scope();
  const editable = (doc) => scopeKey(selected) === scopeKey() && openEditor(doc);
  if (state.documentView === 'table') {
    const wrap = document.createElement('div'); wrap.className = 'data-table-wrap';
    const table = document.createElement('table'); table.className = 'data-table';
    const fields = [...new Set(docs.flatMap((doc) => Object.keys(visibleDocument(doc))))];
    const head = table.createTHead().insertRow();
    for (const field of [...fields, 'Actions']) { const th = document.createElement('th'); th.textContent = field; head.append(th); }
    const body = table.createTBody();
    for (const doc of docs) {
      const row = body.insertRow();
      for (const field of fields) {
        const cell = row.insertCell(); cell.textContent = doc[field] === undefined ? '—' : JSON.stringify(doc[field]);
        cell.title = cell.textContent;
      }
      actionButton(row.insertCell(), 'Edit', () => editable(doc));
    }
    wrap.append(table); list.append(wrap); return;
  }
  for (const doc of docs) {
    const visible = visibleDocument(doc);
    const card = record(String(doc.id ?? doc._id ?? 'Document'), visible,
      state.view === 'query' ? 'Edit in Documents' : 'Edit', () => editable(doc));
    const head = card.querySelector('.record-head');
    actionButton(head, 'Copy JSON', async () => { await copyText(JSON.stringify(visible, null, 2)); notice('Document JSON copied.'); });
    const idField = doc.id !== undefined ? 'id' : doc._id !== undefined ? '_id' : null;
    if (idField) actionButton(head, 'Delete', () => confirmAction('Delete document?',
      `Delete ${doc[idField]} from ${selected.collection}? This action cannot be undone.`, async () => {
        await request('documents.delete', { ...selected, filter: { [idField]: doc[idField] } });
        invalidateSummary(selected);
        if (scopeKey(selected) === scopeKey()) { await loadDocuments(state.documentOffset); }
        notice('Document deleted.');
      }), 'danger');
    const details = document.createElement('details'); details.className = 'document-fields';
    const summary = document.createElement('summary'); summary.textContent = 'Explore fields'; details.append(summary);
    details.addEventListener('toggle', () => {
      if (details.open && details.childElementCount === 1) details.append(jsonFields(visible));
    });
    card.append(details); list.append(card);
  }
}
function jsonFields(value) {
  const container = document.createElement('div'); container.className = 'json-fields';
  for (const [key, item] of Object.entries(value)) {
    if (item && typeof item === 'object') {
      const details = document.createElement('details'); const summary = document.createElement('summary');
      summary.textContent = `${key} · ${Array.isArray(item) ? 'Array' : 'Object'} (${Object.keys(item).length})`;
      details.append(summary);
      details.addEventListener('toggle', () => { if (details.open && details.childElementCount === 1) details.append(jsonFields(item)); });
      container.append(details);
    } else {
      const row = document.createElement('div'); row.className = 'json-field';
      const name = document.createElement('code'); name.textContent = key;
      const content = document.createElement('code'); content.textContent = JSON.stringify(item);
      row.append(name, content);
      actionButton(row, 'Copy value', () => copyText(JSON.stringify(item)));
      container.append(row);
    }
  }
  return container;
}
function updateJsonEditor() {
  const text = $('document-json').value;
  $('json-lines').textContent = Array.from({ length: text.split('\n').length }, (_, i) => i + 1).join('\n');
  try { jsonField('document-json', 'Document'); $('json-validation').textContent = 'Valid JSON object'; }
  catch (error) { $('json-validation').textContent = error.message; }
  const preview = $('json-preview'); preview.replaceChildren();
  const tokens = /("(?:\\.|[^"\\])*"\s*:?)|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|\b(true|false|null)\b/g;
  let position = 0;
  for (const match of text.matchAll(tokens)) {
    preview.append(document.createTextNode(text.slice(position, match.index)));
    const span = document.createElement('span');
    span.className = match[1] ? match[1].endsWith(':') ? 'json-key' : 'json-string' : 'json-literal';
    span.textContent = match[0]; preview.append(span); position = match.index + match[0].length;
  }
  preview.append(document.createTextNode(text.slice(position)));
}
async function executeQuery() {
  const selected = scope(); const filter = jsonField('filter', 'Filter');
  await loadDocuments(0, filter);
  if (scopeKey(selected) !== scopeKey() || $('document-summary').textContent === 'Query failed') return;
  const text = JSON.stringify(filter);
  state.queryHistory = [{ key: scopeKey(selected), text }, ...state.queryHistory.filter((item) => item.key !== scopeKey(selected) || item.text !== text)].slice(0, 20);
  renderHistory(); addActivity('query', 'Query executed', selected.collection);
}
function renderHistory() {
  const select = $('query-history'); select.replaceChildren();
  const entries = state.queryHistory.filter((item) => item.key === scopeKey());
  const option = document.createElement('option'); option.value = ''; option.textContent = entries.length ? 'Query history · this session' : 'No query history yet'; select.append(option);
  for (const item of entries) { const option = document.createElement('option'); option.value = item.text; option.textContent = item.text; select.append(option); }
}
function confirmAction(title, description, action) {
  $('confirm-title').textContent = title; $('confirm-description').textContent = description;
  $('confirm-error').textContent = ''; $('confirm-submit').textContent = title.replace('?', '');
  $('confirm-submit').onclick = async () => {
    if ($('confirm-submit').disabled) return;
    $('confirm-submit').disabled = true; $('confirm-cancel').disabled = true;
    try { await action(); $('confirm-dialog').close(); }
    catch (error) { $('confirm-error').textContent = error.message; }
    finally { $('confirm-submit').disabled = false; $('confirm-cancel').disabled = false; }
  };
  $('confirm-dialog').showModal(); $('confirm-cancel').focus();
}
async function loadIndexes() {
  const selected = scope(); const generation = ++state.indexGeneration;
  $('index-list').replaceChildren(); $('index-empty').hidden = false;
  $('index-empty').textContent = 'Loading indexes…'; $('index-list').setAttribute('aria-busy', 'true');
  try {
    const result = await request('indexes.list', selected);
    if (generation !== state.indexGeneration || scopeKey(selected) !== scopeKey()) return;
    state.indexes = result.indexes || [];
    for (const index of state.indexes) {
      const row = document.createElement('tr');
      for (const value of [index.name, index.field || Object.keys(index.fields || {}).join(', ') || '—',
        index.type || '—', index.status || '—', index.managed ? 'Engine managed' : `${typeof index.unique === 'boolean' ? index.unique ? 'Unique' : 'Non-unique' : '—'}${index.sparse ? ' · Sparse' : ''}`]) {
        const td = document.createElement('td'); td.textContent = value; row.append(td);
      }
      const actions = document.createElement('td'); actions.className = 'table-actions';
      actionButton(actions, 'Validate', async () => {
        const result = await request('indexes.validate', selected);
        if (scopeKey(selected) !== scopeKey()) return;
        $('index-validation').textContent = JSON.stringify(result, null, 2); $('index-report').hidden = false;
        notice('Index validation complete. Review the engine report below.');
      });
      if (index.type === 'btree') {
        actionButton(actions, 'Rebuild', async () => {
          await request('indexes.rebuild', { ...selected, name: index.name });
          if (scopeKey(selected) === scopeKey()) await loadIndexes(); notice('Index rebuilt.');
        });
        actionButton(actions, 'Delete', () => confirmAction('Delete index?',
          `Permanently delete ${index.name} on ${selected.collection}? Documents are preserved.`, async () => {
            await request('indexes.delete', { ...selected, name: index.name });
            invalidateSummary(selected);
            if (scopeKey(selected) === scopeKey()) { await loadIndexes(); }
            notice('Index deleted.');
          }), 'danger');
      }
      row.append(actions); $('index-list').append(row);
    }
    $('index-empty').hidden = state.indexes.length > 0;
    $('index-empty').textContent = 'No indexes yet. Create an index to speed up filters on a field.';
  } catch (error) {
    if (generation !== state.indexGeneration || scopeKey(selected) !== scopeKey()) return;
    $('index-empty').textContent = 'Unable to load indexes. Refresh to retry.'; throw error;
  } finally { if (generation === state.indexGeneration) $('index-list').setAttribute('aria-busy', 'false'); }
}
click('confirm-cancel', () => $('confirm-dialog').close());
$('confirm-dialog').addEventListener('cancel', (event) => { if ($('confirm-submit').disabled) event.preventDefault(); });
click('collection-query', () => openTool('query'));
click('collection-indexes', async () => { selectTab('indexes'); await loadIndexes(); });
click('collection-refresh', () => refreshCollections(), true);
click('refresh-indexes', loadIndexes, true);
click('create-index', () => {
  state.indexScope = scope(); $('index-form').reset(); $('index-error').textContent = '';
  $('index-dialog').showModal(); $('index-name').focus();
});
for (const id of ['close-index', 'cancel-index']) click(id, () => { if (!$('submit-index').disabled) $('index-dialog').close(); });
$('index-dialog').addEventListener('cancel', (event) => { if ($('submit-index').disabled) event.preventDefault(); });
$('index-form').addEventListener('submit', async (event) => {
  event.preventDefault(); if ($('submit-index').disabled) return;
  const selected = { ...state.indexScope };
  $('submit-index').disabled = true; $('submit-index').textContent = 'Creating…'; $('index-error').textContent = '';
  try {
    await request('indexes.create', { ...selected, name: $('index-name').value.trim(),
      field: $('index-field').value.trim(), order: Number($('index-order').value), sparse: $('index-sparse').checked });
    invalidateSummary(selected);
    $('index-dialog').close();
    if (scopeKey(selected) === scopeKey()) { await loadIndexes(); }
    notice('Index created.');
  } catch (error) { $('index-error').textContent = error.message; }
  finally { $('submit-index').disabled = false; $('submit-index').textContent = 'Create index'; }
});
for (const kind of ['database', 'collection']) click(`delete-${kind}`, () => {
  const selected = scope(); const name = selected[kind];
  const summary = state.collectionSummaries.get(selected.collection);
  const description = kind === 'collection' ? `Documents: ${number(summary?.count)}. Indexes: ${number(summary?.indexes)}. All collection data will be removed.` : 'All collections and their data will be removed.';
  confirmAction(`Delete ${kind}?`, `${name}. ${description} This action cannot be undone.`, async () => {
    await request(`${kind}s.delete`, selected);
    if (scopeKey(selected) === scopeKey()) {
      invalidateSummary(selected);
      invalidateSelection(); await refreshDatabases(state.navigation);
    }
    notice(`${kind[0].toUpperCase() + kind.slice(1)} deleted.`);
  });
});
for (const [id, field, label] of [['format-filter', 'filter', 'Filter'], ['format-document', 'document-json', 'Document']]) click(id, () => {
  $(field).value = JSON.stringify(jsonField(field, label), null, 2); if (field === 'document-json') updateJsonEditor();
});
for (const [id, field] of [['copy-filter', 'filter'], ['copy-document', 'document-json']]) click(id, async () => { await copyText($(field).value); notice('JSON copied.'); });
click('clear-filter', () => { $('filter').value = '{}'; });
click('cancel-editor', () => { $('document-editor').open = false; });
$('document-json').addEventListener('input', updateJsonEditor);
$('document-json').addEventListener('scroll', () => { $('json-lines').scrollTop = $('document-json').scrollTop; });
$('query-history').addEventListener('change', () => { if ($('query-history').value) $('filter').value = $('query-history').value; });
$('vector-values').addEventListener('input', () => {
  try { $('vector-dimensions').textContent = `${jsonField('vector-values', 'Vector', true).length} dimensions`; }
  catch { $('vector-dimensions').textContent = 'Enter a valid vector'; }
});


async function refreshMonitoring() {
  await refreshEngine();
  $('monitor-metrics').textContent = 'Loading engine counters…';
  try { $('monitor-metrics').textContent = (await request('engine.metrics')).metrics || 'The engine has not reported counters yet.'; }
  catch (error) { $('monitor-metrics').textContent = `Unable to load engine counters: ${error.message}`; throw error; }
}
function filterMedia() {
  const query = $('media-search').value.trim().toLowerCase();
  for (const card of $('media-list').querySelectorAll('.record')) card.hidden = !card.dataset.search.includes(query);
}
let previewUrl, previewGeneration = 0;
async function previewMedia(file, selected) {
  const generation = ++previewGeneration;
  const blob = await mediaBlob(file, selected);
  if (generation !== previewGeneration || scopeKey(selected) !== scopeKey()) return;
  const extension = (file.filename || '').split('.').pop().toLowerCase();
  const images = { png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', webp: 'image/webp' };
  const imageType = images[extension];
  const text = !imageType && blob.size <= 1024 * 1024 &&
    (file.content_type?.startsWith('text/') || ['txt', 'json', 'csv', 'md', 'log'].includes(extension)) ?
    await blob.text() : 'Preview supports images and text files up to 1 MiB. Download this file to view its contents.';
  if (generation !== previewGeneration || scopeKey(selected) !== scopeKey()) return;
  if (previewUrl) URL.revokeObjectURL(previewUrl);
  previewUrl = null;
  $('media-preview-title').textContent = file.filename || file.id;
  $('media-preview-image').hidden = !imageType;
  $('media-preview-text').textContent = imageType ? '' : text;
  if (imageType) {
    previewUrl = URL.createObjectURL(new Blob([blob], { type: imageType }));
    $('media-preview-image').src = previewUrl;
    $('media-preview-image').alt = file.filename || 'Media image';
  }
  $('media-preview').showModal();
}
$('media-search').addEventListener('input', filterMedia);
click('close-preview', () => $('media-preview').close());
$('media-preview').addEventListener('close', () => { previewGeneration++; if (previewUrl) URL.revokeObjectURL(previewUrl); previewUrl = null; $('media-preview-image').removeAttribute('src'); });

loadPreferences();
selectTab('documents');
updateView();
renderActivity();
void run(async () => {
  state.connectionInfo = await request('connection.info');
  if (state.connectionInfo?.database) state.database = state.connectionInfo.database;
  if (state.connectionInfo) $('environment-endpoint').textContent = `${state.connectionInfo.host}:${state.connectionInfo.port}`;
  await refreshDatabases(undefined, true);
});
void refreshEngine();
setInterval(() => { if (document.visibilityState === 'visible') void refreshEngine(); }, 20_000);
