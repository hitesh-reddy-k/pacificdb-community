import net from 'node:net';
import tls from 'node:tls';
import { createHash } from 'node:crypto';
import { createReadStream, createWriteStream, readFileSync } from 'node:fs';
import { mkdtemp, rename, rmdir, stat, unlink } from 'node:fs/promises';
import { once } from 'node:events';
import { finished } from 'node:stream/promises';
import { StringDecoder } from 'node:string_decoder';
import path from 'node:path';
import { inspect } from 'node:util';
import { parseConnectionUrl } from './connection-url.js';
export { parseConnectionUrl } from './connection-url.js';

const MAX_SOURCE_CHUNK_BYTES = 4 * 1024 * 1024;
const REQUEST_RESERVE_BYTES = 64 * 1024;
const DEFAULT_POOL_SIZE = 16;
const MAX_POOL_SIZE = 32;

function responseError(value) {
  const code = String(value.error);
  const detail = typeof value.message === 'string' && value.message !== code
    ? `${code}: ${value.message}` : code;
  return Object.assign(new Error(detail), { code, response: value });
}

class PooledConnection {
  constructor(pool) {
    this.pool = pool;
    this.socket = null;
    this.connecting = null;
    this.connected = false;
    this.current = null;
    this.response = '';
    this.decoder = new StringDecoder('utf8');
    this.responsesOnSocket = 0;
  }

  async connect() {
    if (this.connected && this.socket && !this.socket.destroyed) return;
    if (this.connecting && this.socket && !this.socket.destroyed) return this.connecting;

    const options = { host: this.pool.host, port: this.pool.port,
      ...(this.pool.ca ? { ca: this.pool.ca } : {}) };
    const socket = this.pool.useTls ? tls.connect(options) : net.createConnection(options);
    this.socket = socket;
    this.response = '';
    this.decoder = new StringDecoder('utf8');
    this.responsesOnSocket = 0;
    const connectedEvent = this.pool.useTls ? 'secureConnect' : 'connect';
    const connecting = new Promise((resolve, reject) => {
      const onConnect = () => {
        cleanup();
        if (socket !== this.socket || socket.destroyed) {
          reject(new Error('PacificDB connection closed while connecting'));
          return;
        }
        this.connected = true;
        socket.unref();
        resolve();
      };
      const onError = (error) => {
        cleanup();
        reject(error);
      };
      const onClose = () => {
        cleanup();
        reject(new Error('PacificDB connection closed while connecting'));
      };
      const cleanup = () => {
        socket.off(connectedEvent, onConnect);
        socket.off('error', onError);
        socket.off('close', onClose);
      };
      socket.once(connectedEvent, onConnect);
      socket.once('error', onError);
      socket.once('close', onClose);
    }).finally(() => { if (this.connecting === connecting) this.connecting = null; });
    this.connecting = connecting;

    socket.on('data', (chunk) => this.onData(socket, chunk));
    socket.on('error', (error) => this.onFailure(socket, error));
    socket.on('end', () => this.onFailure(socket,
      new Error('PacificDB closed before returning a JSON response')));
    socket.on('close', () => {
      if (socket !== this.socket) return;
      this.connected = false;
      this.socket = null;
      this.response = '';
      this.decoder = new StringDecoder('utf8');
    });
    return this.connecting;
  }

  onData(socket, chunk) {
    if (socket !== this.socket || !this.current) return;
    this.response += this.decoder.write(chunk);
    const newline = this.response.indexOf('\n');
    if (newline < 0) return;
    const wire = this.response.slice(0, newline);
    this.response = this.response.slice(newline + 1);
    let value;
    try {
      value = JSON.parse(wire);
    } catch (error) {
      this.finish(new Error(`PacificDB server at ${this.pool.host}:${this.pool.port} returned ` +
        'a non-JSON response; verify the host and port'), true);
      return;
    }
    this.responsesOnSocket += 1;
    // Older engines return a response before sending FIN. Treat a socket as
    // reusable only when the server explicitly advertises keep-alive; a short
    // timer cannot reliably distinguish a delayed FIN from a live connection.
    const responseObject = value !== null && typeof value === 'object' &&
      !Array.isArray(value);
    const serverWillClose = !responseObject ||
      value._pacificdb_connection_close === true ||
      value._pacificdb_connection_keepalive !== true;
    if (responseObject) {
      delete value._pacificdb_connection_close;
      delete value._pacificdb_connection_keepalive;
    }
    if (value?.error) this.finish(responseError(value), serverWillClose);
    else this.finish(null, serverWillClose, value);
  }

  onFailure(socket, error) {
    if (socket !== this.socket) return;
    this.connected = false;
    if (this.current) this.finish(error, true);
  }

  finish(error, destroy = false, value) {
    const current = this.current;
    if (!current) return;
    this.current = null;
    clearTimeout(current.timer);
    if (destroy && this.socket && !this.socket.destroyed) this.socket.destroy();
    else if (this.socket && !this.socket.destroyed) this.socket.unref();
    if (error) current.reject(error);
    else current.resolve(value);
  }

  async request(wire) {
    if (this.current) throw new Error('PacificDB pool assigned concurrent socket requests');
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.finish(
        new Error('PacificDB request timed out'), true), this.pool.timeoutMs);
      timer.unref?.();
      const current = { resolve, reject, timer };
      this.current = current;
      this.connect().then(() => {
        if (this.current !== current || !this.socket || this.socket.destroyed) return;
        this.socket.ref();
        this.socket.write(wire, (error) => {
          if (error && this.current === current) this.finish(error, true);
        });
      }).catch((error) => {
        if (this.current === current) this.finish(error, true);
      });
    });
  }

  close(error) {
    if (this.current) this.finish(error, true);
    if (this.socket && !this.socket.destroyed) this.socket.destroy();
    this.connected = false;
    this.socket = null;
    this.response = '';
    this.decoder = new StringDecoder('utf8');
  }
}

class ConnectionPool {
  constructor({ host, port, useTls, ca, timeoutMs, poolSize }) {
    Object.assign(this, { host, port, useTls, ca, timeoutMs, poolSize });
    this.connections = [];
    this.idle = [];
    this.queue = [];
    this.closed = false;
  }

  newConnection() {
    const connection = new PooledConnection(this);
    this.connections.push(connection);
    return connection;
  }

  dispatch(connection, job) {
    const release = () => {
      if (!this.closed) {
        const next = this.queue.shift();
        if (next) this.dispatch(connection, next);
        else this.idle.push(connection);
      }
    };
    connection.request(job.wire).then((value) => {
      // Response parsing already retires peers that do not explicitly promise
      // keep-alive. Return the slot before callers submit their next burst;
      // delaying release can trap every queued request on one warm connection.
      release();
      job.resolve(value);
    }, (error) => {
      release();
      job.reject(error);
    });
  }

  request(wire) {
    if (this.closed) return Promise.reject(new Error('PacificDB client is closed'));
    return new Promise((resolve, reject) => {
      const job = { wire, resolve, reject };
      const connection = this.idle.pop();
      if (connection) this.dispatch(connection, job);
      else if (this.connections.length < this.poolSize) {
        this.dispatch(this.newConnection(), job);
      } else this.queue.push(job);
    });
  }

  async connect() {
    if (this.closed) throw new Error('PacificDB client is closed');
    while (this.connections.length < this.poolSize) {
      this.idle.push(this.newConnection());
    }
    await Promise.all(this.connections.map((connection) => connection.connect()));
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    const error = new Error('PacificDB client is closed');
    for (const job of this.queue.splice(0)) job.reject(error);
    for (const connection of this.connections) connection.close(error);
    this.idle.length = 0;
  }
}

export class MediaUploadError extends Error {
  constructor(message, { code = 'media_upload_interrupted', uploadId,
    nextChunk = 0, receivedChunks = 0, receivedBytes = 0,
    resumable = true, cause } = {}) {
    super(message, cause ? { cause } : undefined);
    this.name = 'MediaUploadError';
    this.code = code;
    this.uploadId = uploadId;
    this.nextChunk = nextChunk;
    this.receivedChunks = receivedChunks;
    this.receivedBytes = receivedBytes;
    this.resumable = resumable;
  }
}

function mediaType(filename) {
  const extension = path.extname(filename).toLowerCase();
  return ({ '.gif': 'image/gif', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
    '.png': 'image/png', '.webp': 'image/webp', '.mp4': 'video/mp4',
    '.mov': 'video/quicktime', '.webm': 'video/webm', '.mp3': 'audio/mpeg',
    '.wav': 'audio/wav' })[extension] || 'application/octet-stream';
}

export class PacificDBClient {
  #credentials;
  #authentication;
  #authenticated = false;
  #secrets = new Set();

  static fromUrl(url, options = {}) {
    return new this(parseConnectionUrl(url, options));
  }

  static async connect(url, options = {}) {
    const client = this.fromUrl(url, options);
    try { await client.connect(); return client; }
    catch (error) { client.close(); throw error; }
  }

  constructor({ host = '127.0.0.1', port = 9000, userId = 'system',
                database = '', projectId = '', tls: useTls = false, ca, timeoutMs = 30000,
                poolSize = DEFAULT_POOL_SIZE, token = '', caFile, username, password } = {}) {
    if (!Number.isSafeInteger(poolSize) || poolSize < 1 || poolSize > MAX_POOL_SIZE) {
      throw new RangeError(`poolSize must be an integer between 1 and ${MAX_POOL_SIZE}`);
    }
    this.host = host;
    this.port = port;
    this.userId = userId;
    this.database = database;
    this.projectId = projectId;
    this.useTls = useTls;
    this.timeoutMs = timeoutMs;
    let currentToken = token;
    if (token) this.#secrets.add(token);
    Object.defineProperty(this, 'token', { get: () => currentToken,
      set: value => { currentToken = value; if (value) this.#secrets.add(value); } });
    if (username !== undefined || password !== undefined) {
      if (!username || !password) throw new TypeError('username and password are required together');
      this.#credentials = { username, password };
      this.#secrets.add(password);
    }
    if (caFile) {
      if (!useTls) throw new TypeError('caFile requires TLS');
      try { ca = readFileSync(caFile); }
      catch { throw new Error('could not read TLS CA file'); }
    }
    this.ca = ca;
    this.poolSize = poolSize;
    this._pool = new ConnectionPool({ host, port, useTls, ca, timeoutMs, poolSize });
  }

  request(command) {
    return this._scopedRequest()(command);
  }

  _scopedRequest() {
    const scope = { userId: this.userId, dbName: this.database,
      ...(this.token ? { token: this.token } : {}) };
    const needsAuth = !!this.#credentials && !this.#authenticated;
    const request = async command => {
      const payload = JSON.parse(JSON.stringify({ ...scope, ...command }));
      try {
        if (needsAuth && command.action !== 'security_authenticate') {
          const token = await this.#ensureAuthenticated();
          if (!Object.hasOwn(scope, 'token')) scope.token = token;
          if (!Object.hasOwn(payload, 'token')) payload.token = token;
        }
        return await this.#send(payload);
      } catch (error) { throw this.#redact(error); }
    };
    request.wireBytes = command => Buffer.byteLength(JSON.stringify({ ...scope, ...command })) + 1;
    return request;
  }

  async #send(payload) {
    const learn = value => {
      if (Array.isArray(value)) value.forEach(learn);
      else if (value && typeof value === 'object') for (const [key, item] of Object.entries(value)) {
        if (/password|token|authorization/i.test(key) && typeof item === 'string' && item) this.#secrets.add(item);
        learn(item);
      }
    };
    learn(payload);
    try {
      return await this._pool.request(JSON.stringify(payload) + '\n');
    } catch (error) { throw this.#redact(error); }
  }

  #ensureAuthenticated() {
    if (!this.#credentials) return Promise.resolve(this.token);
    if (!this.#authentication) {
      this.#authentication = this.authenticate(this.#credentials.username,
        this.#credentials.password).then(() => {
        this.#authenticated = true;
        return this.token;
      }).catch(error => { this.close(); throw error; });
    }
    return this.#authentication;
  }

  #redact(error) {
    const clean = value => {
      if (typeof value === 'string') {
        let safe = value.replace(/pacificdbs?:\/\/[^\s/]*@/g, 'pacificdb://[redacted]@');
        for (const secret of this.#secrets) safe = safe.split(secret).join('[redacted]');
        return safe;
      }
      if (Array.isArray(value)) return value.map(clean);
      if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value)
        .map(([key, val]) => [clean(key), /password|token|authorization/i.test(key) ? '[redacted]' : clean(val)]));
      return value;
    };
    const seen = new Set();
    const safeError = original => {
      if (!original || seen.has(original)) return new Error('Operation failed');
      seen.add(original);
      const message = clean(original.message || 'Operation failed');
      const result = original instanceof MediaUploadError
        ? new MediaUploadError(message, { code: clean(original.code), uploadId: clean(original.uploadId),
          nextChunk: original.nextChunk, receivedChunks: original.receivedChunks,
          receivedBytes: original.receivedBytes, resumable: original.resumable })
        : new Error(message);
      result.name = clean(original.name || 'Error');
      if (original.code !== undefined) result.code = clean(original.code);
      if (original.response !== undefined) result.response = clean(original.response);
      if (original.stack) result.stack = clean(original.stack);
      if (original.cause) result.cause = safeError(original.cause);
      return result;
    };
    return safeError(error);
  }

  async connect() {
    try { await this.#ensureAuthenticated(); await this._pool.connect(); }
    catch (error) { this.close(); throw this.#redact(error); }
  }

  toJSON() {
    return { host: this.host, port: this.port, database: this.database,
      userId: this.userId, tls: this.useTls, poolSize: this.poolSize };
  }

  [inspect.custom]() {
    return `PacificDBClient ${inspect(this.toJSON())}`;
  }

  close() {
    this._pool.close();
  }

  capabilities() {
    return this.request({ action: 'community_capabilities' });
  }

  _wireBytes(command) {
    return this._scopedRequest().wireBytes(command);
  }

  async authenticate(username, password) {
    if (typeof password === 'string' && password) this.#secrets.add(password);
    const result = await this.request({ action: 'security_authenticate', username, password });
    if (typeof result?.token !== 'string' || !result.token) throw new Error('invalid authentication response');
    this.token = result.token;
    return result;
  }

  async createProject(name) {
    const response = await this.request({ action: 'community_project_create', name });
    this.projectId = response.project.id;
    this.database = '';
    return response;
  }
  async useProject(id) {
    const response = await this.request({ action: 'community_project_get', id });
    if (!response.project?.id) throw new Error('project_not_found');
    this.projectId = response.project.id;
    this.database = '';
    return response;
  }
  async createDatabase(name = this.database, dbType = 'binary') {
    if (!name) throw new Error('database name is required');
    const request = this._scopedRequest();
    const projectId = this.projectId;
    if (!projectId) {
      const response = await request({ action: 'createDatabase', dbName: name, dbType });
      this.database = name;
      return response;
    }
    if (Buffer.byteLength(name, 'utf8') > 128)
      throw new Error('database name must be 1-128 bytes when mapped to a project');
    const project = await request({ action: 'community_project_get', id: projectId });
    if (!project.project?.id) throw new Error('project_not_found');
    const response = await request({ action: 'createDatabase', dbName: name,
      dbType, project_id: projectId });
    await request({ action: 'community_database_map', database: name,
      project_id: projectId });
    this.database = name;
    return response;
  }
  async useDatabase(name) {
    const projectId = this.projectId;
    const response = await this.request(projectId ?
      { action: 'community_database_list', project_id: projectId } : { action: 'listDatabases' });
    const names = projectId ? response.databases : response;
    if (!Array.isArray(names) || !names.includes(name)) throw new Error('database_not_found');
    this.database = name;
    return response;
  }
  async createCollection(name) {
    if (!this.database) throw new Error('select a database before creating a collection');
    const request = this._scopedRequest();
    const database = this.database;
    if (this.projectId) {
      const response = await request({ action: 'community_database_list', project_id: this.projectId });
      if (!response.databases?.includes(database)) throw new Error('database_not_found');
    }
    return request({ action: 'createCollection', collection: name });
  }
  insert(collection, data) {
    return this.request({ action: 'insert', collection, data });
  }
  insertMany(collection, data) {
    if (!Array.isArray(data)) throw new TypeError('insertMany data must be an array');
    return this.request({ action: 'insertMany', collection, data });
  }
  find(collection, filter = {}, { limit = -1, offset = 0 } = {}) {
    return this.request({ action: 'find', collection, filter, limit, offset });
  }
  updateOne(collection, filter, update) {
    return this.request({ action: 'updateOne', collection, filter, update });
  }
  deleteOne(collection, filter) {
    return this.request({ action: 'deleteOne', collection, filter });
  }

  putMedia(collection, id, data, metadata = {}) {
    if (typeof id !== 'string' || !id) throw new TypeError('media id is required');
    const bytes = Buffer.from(data);
    return this.insert(collection, { ...metadata, id, kind: 'media',
      dataBase64: bytes.toString('base64'), sizeBytes: bytes.length,
      encoding: 'base64' });
  }
  updateMedia(collection, id, data, metadata = {}) {
    return this.putMedia(collection, id, data, metadata);
  }
  async getMedia(collection, id) {
    const response = await this.find(collection, { id }, { limit: 1 });
    const document = Array.isArray(response) ? response[0] : response?.data?.[0];
    if (!document || document.kind !== 'media' || typeof document.dataBase64 !== 'string') {
      throw new Error(`media not found: ${id}`);
    }
    const { dataBase64, ...metadata } = document;
    return { data: Buffer.from(dataBase64, 'base64'), metadata };
  }
  async uploadMediaFile(collection, filename, {
    contentType = mediaType(filename), chunkBytes, resume
  } = {}) {
    if (!this.database) throw new Error('select a database before uploading media');
    if (typeof collection !== 'string' || !collection) {
      throw new TypeError('media collection is required');
    }
    const request = this._scopedRequest();
    const file = await stat(filename);
    if (!file.isFile()) throw new Error('media path must be a regular file');
    if (file.size === 0) {
      throw Object.assign(new Error('media_file_empty'), { code: 'media_file_empty' });
    }
    const capabilities = await request({ action: 'community_capabilities' });
    const maxRequestBytes = capabilities.max_request_bytes;
    if (!Number.isSafeInteger(maxRequestBytes) || maxRequestBytes <= REQUEST_RESERVE_BYTES) {
      throw new Error('engine request limit is too small for media chunks');
    }
    const safeChunkBytes = Math.floor((maxRequestBytes - REQUEST_RESERVE_BYTES) * 3 / 4);
    const mediaChunkBytes = Number.isSafeInteger(capabilities.media_chunk_source_max_bytes)
      ? capabilities.media_chunk_source_max_bytes : MAX_SOURCE_CHUNK_BYTES;
    const sourceChunkBytes = Math.min(MAX_SOURCE_CHUNK_BYTES, mediaChunkBytes,
      chunkBytes ?? safeChunkBytes, safeChunkBytes);
    if (!Number.isSafeInteger(sourceChunkBytes) || sourceChunkBytes < 64 * 1024) {
      throw new Error('media chunk size must be at least 64 KiB');
    }

    const wholeHash = createHash('sha256');
    for await (const chunk of createReadStream(filename,
      { highWaterMark: sourceChunkBytes })) wholeHash.update(chunk);
    const sha256 = wholeHash.digest('hex');
    const chunkCount = Math.ceil(file.size / sourceChunkBytes);
    const begun = await request({ action: 'community_media_begin', collection,
      filename: path.basename(filename), content_type: contentType,
      size_bytes: file.size, chunk_count: chunkCount, sha256,
      ...(resume ? { resume_id: resume } : {}) });
    const media = begun.media;
    if (!media?.id) throw new Error('engine did not return a media id');
    if (media.status === 'ready') return media;

    let received = new Set(Array.isArray(media.received_indices)
      ? media.received_indices.filter((value) => Number.isSafeInteger(value) && value >= 0)
      : []);
    let receivedBytes = Number.isSafeInteger(media.received_bytes) ? media.received_bytes : 0;
    const updateProgress = (value, index, bytes) => {
      if (Array.isArray(value?.received_indices)) {
        received = new Set(value.received_indices.filter((entry) =>
          Number.isSafeInteger(entry) && entry >= 0));
      } else if (Number.isSafeInteger(index)) {
        received.add(index);
      }
      if (Number.isSafeInteger(value?.received_bytes)) receivedBytes = value.received_bytes;
      else if (Number.isSafeInteger(index) && !received.has(index)) receivedBytes += bytes;
    };
    const interruption = (error) => {
      const nextChunk = [...Array(chunkCount).keys()].find((index) => !received.has(index))
        ?? chunkCount;
      return new MediaUploadError('media upload interrupted', {
        uploadId: media.id, nextChunk, receivedChunks: received.size,
        receivedBytes, resumable: true, cause: error
      });
    };

    try {
      let index = 0;
      for await (const chunk of createReadStream(filename,
        { highWaterMark: sourceChunkBytes })) {
        if (!received.has(index)) {
          const command = { action: 'community_media_put_chunk', media_id: media.id,
            index, data: chunk.toString('base64'), size_bytes: chunk.length,
            sha256: createHash('sha256').update(chunk).digest('hex') };
          if (request.wireBytes(command) > maxRequestBytes) {
            throw new Error('serialized media chunk exceeds engine request limit');
          }
          const stored = await request(command);
          const alreadyReceived = received.has(index);
          updateProgress(stored?.media ?? stored, index, chunk.length);
          if (!alreadyReceived && !Number.isSafeInteger((stored?.media ?? stored)?.received_bytes)) {
            receivedBytes += chunk.length;
          }
        }
        index += 1;
      }
      const finalized = await request({ action: 'community_media_finalize',
        media_id: media.id });
      return finalized.media;
    } catch (error) {
      if (error instanceof MediaUploadError) throw error;
      throw interruption(error);
    }
  }

  async downloadMediaFile(mediaId, destination) {
    const request = this._scopedRequest();
    const response = await request({ action: 'community_media_get',
      media_id: mediaId });
    const manifest = response.media;
    if (!manifest || manifest.status !== 'ready') {
      throw new Error(`ready media not found: ${mediaId}`);
    }
    // Keep an existing destination intact until every chunk and the manifest
    // have been verified. The temporary file shares its filesystem so the
    // final rename replaces the destination atomically.
    const temporaryDirectory = await mkdtemp(path.join(
      path.dirname(path.resolve(destination)), '.pacificdb-download-'));
    const partial = path.join(temporaryDirectory, 'media');
    const output = createWriteStream(partial, { flags: 'wx', mode: 0o600 });
    let outputError;
    output.on('error', (error) => { outputError = error; });
    const wholeHash = createHash('sha256');
    let sizeBytes = 0;
    try {
      for (let index = 0; index < manifest.chunk_count; index += 1) {
        const result = await request({ action: 'community_media_get_chunk',
          media_id: mediaId, index });
        const chunk = result.chunk;
        if (!chunk || typeof chunk.data !== 'string') {
          throw new Error(`media chunk missing: ${index}`);
        }
        const bytes = Buffer.from(chunk.data, 'base64');
        const checksum = createHash('sha256').update(bytes).digest('hex');
        if (checksum !== chunk.sha256) throw new Error(`media chunk checksum mismatch: ${index}`);
        wholeHash.update(bytes);
        sizeBytes += bytes.length;
        if (!output.write(bytes)) await once(output, 'drain');
        if (outputError) throw outputError;
      }
      await new Promise((resolve, reject) => {
        if (outputError) return reject(outputError);
        output.once('finish', resolve);
        output.once('error', reject);
        output.end();
      });
      const sha256 = wholeHash.digest('hex');
      if (sizeBytes !== manifest.size_bytes || sha256 !== manifest.sha256) {
        throw new Error('downloaded media does not match its manifest');
      }
      await rename(partial, destination);
      return { id: mediaId, destination, sizeBytes, sha256 };
    } catch (error) {
      output.destroy();
      await finished(output).catch(() => {});
      throw error;
    } finally {
      await unlink(partial).catch(() => {});
      await rmdir(temporaryDirectory).catch(() => {});
    }
  }
  async exportBackup(backupId, destination, { chunkBytes = 1024 * 1024 } = {}) {
    const request = this._scopedRequest();
    if (typeof backupId !== 'string' || !backupId) throw new TypeError('backup id is required');
    if (!Number.isSafeInteger(chunkBytes) || chunkBytes < 1 || chunkBytes > 1024 * 1024) {
      throw new TypeError('backup chunk size must be between 1 byte and 1 MiB');
    }
    const manifest = await request({ action: 'export_backup_manifest',
      backup_id: backupId });
    if (!Array.isArray(manifest.files)) throw new Error('invalid backup export manifest');
    const temporaryDirectory = await mkdtemp(path.join(
      path.dirname(path.resolve(destination)), '.pacificdb-export-'));
    const partial = path.join(temporaryDirectory, 'backup.json');
    const output = createWriteStream(partial, { flags: 'wx', mode: 0o600 });
    let outputError;
    output.on('error', (error) => { outputError = error; });
    const write = async (value) => {
      if (!output.write(value)) await once(output, 'drain');
      if (outputError) throw outputError;
    };
    let total = 0;
    try {
      await write(`{"format":${JSON.stringify(manifest.format)},"backup":${JSON.stringify(manifest.backup)},"files":[`);
      for (let fileIndex = 0; fileIndex < manifest.files.length; fileIndex += 1) {
        const file = manifest.files[fileIndex];
        if (typeof file.path !== 'string' || !Number.isSafeInteger(file.size_bytes) ||
            file.size_bytes < 0 || typeof file.sha256 !== 'string') {
          throw new Error('invalid backup file manifest');
        }
        await write(`${fileIndex ? ',' : ''}{"path":${JSON.stringify(file.path)},` +
          `"size_bytes":${file.size_bytes},"sha256":${JSON.stringify(file.sha256)},"chunks":[`);
        const hash = createHash('sha256');
        let offset = 0;
        let chunkIndex = 0;
        while (offset < file.size_bytes) {
          const response = await request({ action: 'export_backup_file_chunk',
            backup_id: backupId, path: file.path, offset,
            max_bytes: Math.min(chunkBytes, file.size_bytes - offset) });
          const bytes = Buffer.from(response.data || '', 'base64');
          const checksum = createHash('sha256').update(bytes).digest('hex');
          if (!bytes.length || response.offset !== offset || response.size_bytes !== bytes.length ||
              response.sha256 !== checksum) throw new Error(`invalid backup chunk: ${file.path}`);
          await write(`${chunkIndex ? ',' : ''}${JSON.stringify(response.data)}`);
          hash.update(bytes);
          offset += bytes.length;
          total += bytes.length;
          chunkIndex += 1;
        }
        if (hash.digest('hex') !== file.sha256)
          throw new Error(`backup file checksum mismatch: ${file.path}`);
        await write(']}');
      }
      await write(']}\n');
      await new Promise((resolve, reject) => {
        if (outputError) return reject(outputError);
        output.once('finish', resolve);
        output.once('error', reject);
        output.end();
      });
      await rename(partial, destination);
      return { backupId, destination, files: manifest.files.length, sizeBytes: total };
    } catch (error) {
      output.destroy();
      await finished(output).catch(() => {});
      throw error;
    } finally {
      await unlink(partial).catch(() => {});
      await rmdir(temporaryDirectory).catch(() => {});
    }
  }
  putVector(collection, id, vector, metadata = {}) {
    if (typeof id !== 'string' || !id) throw new TypeError('vector id is required');
    if (!Array.isArray(vector) || !vector.length ||
        vector.some((value) => typeof value !== 'number' || !Number.isFinite(value))) {
      throw new TypeError('vector must be a non-empty array of finite numbers');
    }
    return this.request({ action: 'insertVector', collection,
      data: { ...metadata, id, kind: 'vector', vector } });
  }
  queryVector(collection, vector, { k = 10, metric = 'cosine',
                                    filter = {}, modality } = {}) {
    if (!Array.isArray(vector) || !vector.length ||
        vector.some((value) => typeof value !== 'number' || !Number.isFinite(value))) {
      throw new TypeError('vector must be a non-empty array of finite numbers');
    }
    return this.request({ action: 'queryVector', collection, vector, k, metric,
      filter, ...(modality ? { modality } : {}) });
  }
}

export const PacificDB = PacificDBClient;
