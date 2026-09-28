import { randomBytes } from 'node:crypto';
import { createReadStream, createWriteStream } from 'node:fs';
import { mkdtemp, readFile, rm, stat } from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { pipeline } from 'node:stream/promises';
import { PacificDBClient } from '@pacificdb/client';

const JSON_LIMIT = 1024 * 1024;
const MEDIA_LIMIT = 64 * 1024 * 1024;
const assets = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/app.css': ['app.css', 'text/css; charset=utf-8'],
  '/logo.png': ['logo.png', 'image/png'],
};

function string(value, label, max = 128) {
  if (typeof value !== 'string' || !value.trim() ||
      Buffer.byteLength(value, 'utf8') > max) {
    throw new Error(`${label} must be 1-${max} bytes`);
  }
  return value;
}

function object(value, label) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`${label} must be a JSON object`);
  }
  return value;
}

function page(body, maxLimit = 1000) {
  const limit = body.limit ?? 100;
  const offset = body.offset ?? 0;
  if (!Number.isSafeInteger(limit) || limit < 1 || limit > maxLimit ||
      !Number.isSafeInteger(offset) || offset < 0) {
    throw new Error(`page requires limit 1-${maxLimit} and non-negative offset`);
  }
  return { limit, offset };
}

function send(res, code, value, contentType = 'application/json; charset=utf-8') {
  const body = Buffer.isBuffer(value) || typeof value === 'string' ? value : JSON.stringify(value);
  res.writeHead(code, {
    'Content-Type': contentType,
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'no-referrer',
    'Cross-Origin-Resource-Policy': 'same-origin',
    'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' blob:; object-src 'none'; frame-ancestors 'none'; base-uri 'none'",
  });
  res.end(body);
}

async function readJson(req) {
  if (!req.headers['content-type']?.startsWith('application/json')) {
    throw new Error('Content-Type must be application/json');
  }
  let size = 0;
  const chunks = [];
  for await (const chunk of req) {
    size += chunk.length;
    if (size > JSON_LIMIT) throw new Error('request exceeds 1 MiB');
    chunks.push(chunk);
  }
  return object(JSON.parse(Buffer.concat(chunks).toString('utf8')), 'request');
}

async function project(client, projectId) {
  const id = string(projectId, 'projectId');
  const result = await client.request({ action: 'community_project_get', id });
  if (result.project?.id !== id) throw new Error('project_not_found');
  return id;
}

async function database(client, projectId, databaseName) {
  const id = await project(client, projectId);
  const name = string(databaseName, 'database');
  const response = await client.request({ action: 'community_database_list',
    project_id: id });
  if (!response.databases?.includes(name)) throw new Error('database_not_found');
  client.database = name;
  return name;
}

async function collection(client, body) {
  await database(client, body.projectId, body.database);
  const name = string(body.collection, 'collection', 251);
  const response = await client.request({ action: 'listCollections' });
  const collections = (Array.isArray(response) ? response : response.collections || [])
    .map((item) => typeof item === 'string' ? item : item.name);
  if (!collections?.includes(name)) throw new Error('collection_not_found');
  return name;
}

async function execute(client, body) {
  const op = string(body.op, 'op', 64);
  switch (op) {
    case 'engine.status': {
      const started = performance.now();
      const ping = await client.request({ action: 'ping' });
      const latencyMs = Math.round(performance.now() - started);
      const health = await client.request({ action: 'health_check' });
      const cluster = await client.request({ action: 'get_cluster_status' });
      return { ping, latencyMs, health, cluster };
    }
    case 'engine.metrics':
      return client.request({ action: 'get_metrics' });
    case 'projects.list':
      return client.request({ action: 'community_project_list', ...page(body) });
    case 'projects.create':
      return client.request({ action: 'community_project_create',
        name: string(body.name, 'name') });
    case 'databases.list':
      await project(client, body.projectId);
      return client.request({ action: 'community_database_list',
        project_id: body.projectId });
    case 'databases.create': {
      await project(client, body.projectId);
      const name = string(body.name, 'name');
      const response = await client.request({ action: 'createDatabase', dbName: name,
        project_id: body.projectId });
      await client.request({ action: 'community_database_map', database: name,
        project_id: body.projectId });
      return response;
    }
    case 'collections.list':
      await database(client, body.projectId, body.database);
      {
        const response = await client.request({ action: 'listCollections' });
        return { collections: (Array.isArray(response) ? response : response.collections || [])
          .map((item) => typeof item === 'string' ? item : item.name) };
      }
    case 'collections.create':
      await database(client, body.projectId, body.database);
      return client.request({ action: 'createCollection',
        collection: string(body.name, 'name', 251) });
    case 'collections.summary': {
      const name = await collection(client, body);
      const count = await client.request({ action: 'count', collection: name, filter: {} });
      const indexes = await client.request({ action: 'listIndexes', collection: name });
      return { count: count.count, indexes: Array.isArray(indexes.indexes) ? indexes.indexes.length : null };
    }
    case 'indexes.list':
    case 'indexes.validate':
    case 'indexes.rebuild':
    case 'indexes.delete':
    case 'indexes.create': {
      const name = await collection(client, body);
      const actions = { list: 'listIndexes', validate: 'validateIndex',
        rebuild: 'rebuildIndex', delete: 'dropIndex', create: 'createIndex' };
      const operation = op.split('.')[1];
      const values = operation === 'create' ? {
        name: string(body.name, 'index name'),
        fields: { [string(body.field, 'field')]: body.order === -1 ? -1 : 1 },
        sparse: body.sparse === true, unique: false,
      } : ['rebuild', 'delete'].includes(operation) ? { name: string(body.name, 'index name') } : {};
      return client.request({ action: actions[operation], collection: name, ...values });
    }
    case 'projects.delete':
      await project(client, body.projectId);
      return client.request({ action: 'community_project_delete', id: body.projectId });
    case 'databases.delete':
      await database(client, body.projectId, body.database);
      return client.request({ action: 'dropDatabase', dbName: body.database, project_id: body.projectId });
    case 'collections.delete':
      return client.request({ action: 'dropCollection', collection: await collection(client, body),
        dbName: body.database, project_id: body.projectId });
    case 'media.delete': {
      await collection(client, body);
      const id = string(body.mediaId, 'mediaId');
      const lookup = await client.request({ action: 'community_media_get', media_id: id });
      if (lookup.media?.database !== body.database || lookup.media?.collection !== body.collection) {
        throw new Error('media_not_found');
      }
      return client.request({ action: 'community_media_delete', media_id: id });
    }
    case 'documents.find': {
      const name = await collection(client, body);
      const { limit, offset } = page({ limit: body.limit ?? 50,
        offset: body.offset }, 500);
      return client.find(name, object(body.filter ?? {}, 'filter'), { limit, offset });
    }
    case 'documents.insert':
      return client.insert(await collection(client, body), object(body.data, 'data'));
    case 'documents.update': {
      const name = await collection(client, body);
      const filter = object(body.filter, 'filter');
      if (!Object.keys(filter).length) throw new Error('update filter cannot be empty');
      return client.updateOne(name, filter, object(body.update, 'update'));
    }
    case 'documents.delete': {
      const name = await collection(client, body);
      const filter = object(body.filter, 'filter');
      if (!Object.keys(filter).length) throw new Error('delete filter cannot be empty');
      return client.deleteOne(name, filter);
    }
    case 'vectors.put':
      return client.putVector(await collection(client, body),
        string(body.id, 'id'), body.vector, object(body.metadata ?? {}, 'metadata'));
    case 'vectors.query':
      return client.queryVector(await collection(client, body), body.vector,
        { k: body.k ?? 10, metric: body.metric ?? 'cosine', filter: object(body.filter ?? {}, 'filter') });
    case 'media.list':
      await database(client, body.projectId, body.database);
      return client.request({ action: 'community_media_list',
        dbName: body.database, collection: body.collection || '',
        ...page(body) });
    default:
      throw new Error('unsupported workbench operation');
  }
}

async function withClient(clientFactory, fn) {
  const client = clientFactory();
  try { return await fn(client); }
  finally { client.close?.(); }
}

async function receiveFile(req, destination) {
  let size = 0;
  req.on('data', (chunk) => {
    size += chunk.length;
    if (size > MEDIA_LIMIT) req.destroy(new Error('media exceeds 64 MiB'));
  });
  await pipeline(req, createWriteStream(destination, { flags: 'wx', mode: 0o600 }));
  if (!size) throw new Error('media file is empty');
}

export async function createWorkbenchServer({ clientFactory, port = 0, connectionInfo = null } = {}) {
  if (typeof clientFactory !== 'function') throw new TypeError('clientFactory is required');
  if (connectionInfo && (typeof connectionInfo.host !== 'string' ||
      !Number.isSafeInteger(connectionInfo.port) || connectionInfo.port < 1 ||
      connectionInfo.port > 65535)) throw new TypeError('connectionInfo requires a host and port');
  const token = randomBytes(32).toString('hex');
  const server = http.createServer(async (req, res) => {
    try {
      const portNow = server.address()?.port;
      const host = req.headers.host;
      if (host !== `127.0.0.1:${portNow}` && host !== `localhost:${portNow}`) {
        send(res, 403, { error: 'invalid_host' });
        return;
      }
      const pathname = new URL(req.url, `http://${host}`).pathname;
      if (req.method === 'GET' && assets[pathname]) {
        const [filename, contentType] = assets[pathname];
        let content = await readFile(new URL(`../workbench/${filename}`, import.meta.url),
          contentType === 'image/png' ? undefined : 'utf8');
        if (pathname === '/') content = content.replace('__SESSION_TOKEN__', token);
        send(res, 200, content, contentType);
        return;
      }
      if (req.method !== 'POST' || !pathname.startsWith('/api/')) {
        send(res, 404, { error: 'not_found' });
        return;
      }
      const origin = req.headers.origin;
      if ((origin && origin !== `http://${host}`) ||
          req.headers['x-pacificdb-workbench-token'] !== token) {
        send(res, 403, { error: 'forbidden' });
        return;
      }
      if (pathname === '/api/execute') {
        const body = await readJson(req);
        const result = body.op === 'connection.info' ? connectionInfo :
          await withClient(clientFactory, (client) => execute(client, body));
        send(res, 200, { result });
      } else if (pathname === '/api/media/upload') {
        const projectId = string(req.headers['x-project-id'], 'projectId');
        const databaseName = string(req.headers['x-database'], 'database');
        const collectionName = string(req.headers['x-collection'], 'collection', 251);
        const filename = path.basename(string(decodeURIComponent(
          req.headers['x-filename'] || ''), 'filename', 255));
        const temporary = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-workbench-'));
        try {
          const source = path.join(temporary, filename);
          await receiveFile(req, source);
          const result = await withClient(clientFactory, async (client) => {
            await collection(client, { projectId, database: databaseName,
              collection: collectionName });
            return client.uploadMediaFile(collectionName, source);
          });
          send(res, 200, { result });
        } finally { await rm(temporary, { recursive: true, force: true }); }
      } else if (pathname === '/api/media/download') {
        const body = await readJson(req);
        const result = await withClient(clientFactory, async (client) => {
          await database(client, body.projectId, body.database);
          const id = string(body.mediaId, 'mediaId');
          const lookup = await client.request({ action: 'community_media_get', media_id: id });
          if (lookup.media?.database !== body.database || lookup.media?.status !== 'ready') {
            throw new Error('media_not_found');
          }
          const temporary = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-workbench-'));
          try {
            const destination = path.join(temporary, 'download');
            await client.downloadMediaFile(id, destination);
            const info = await stat(destination);
            res.writeHead(200, {
              'Content-Type': 'application/octet-stream',
              'Content-Length': info.size,
              'Content-Disposition': `attachment; filename="${id.replace(/[^A-Za-z0-9._-]/g, '_')}"`,
              'Cache-Control': 'no-store',
              'X-Content-Type-Options': 'nosniff',
            });
            await pipeline(createReadStream(destination), res);
          } finally { await rm(temporary, { recursive: true, force: true }); }
        });
        void result;
      } else send(res, 404, { error: 'not_found' });
    } catch (error) {
      if (!res.headersSent) send(res, 400, { error: error.message || 'request_failed' });
      else res.destroy(error);
    }
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', resolve);
  });
  return {
    server,
    url: `http://127.0.0.1:${server.address().port}/`,
    close: () => new Promise((resolve, reject) =>
      server.close((error) => error ? reject(error) : resolve())),
  };
}

export async function startWorkbench(options, output, ensureConnection) {
  const probe = new PacificDBClient(options);
  try {
    await ensureConnection(probe);
    const response = await probe.request({ action: 'ping' });
    if (response.status !== 'pong') throw new Error('PacificDB engine is not ready');
  }
  finally { probe.close(); }
  const workbench = await createWorkbenchServer({
    clientFactory: () => new PacificDBClient(options),
    port: options.uiPort ?? 0,
    connectionInfo: { host: options.host || '127.0.0.1', port: options.port || 9000,
      tls: options.tls === true, source: 'cli' },
  });
  output.write(`PacificDB Workbench: ${workbench.url}\n`);
  return workbench;
}
