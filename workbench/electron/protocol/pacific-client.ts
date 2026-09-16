/**
 * PacificDB TCP Protocol Client
 *
 * Replicates the wire protocol from @pacificdb/client (sdk/node/src/index.js).
 * Communication is newline-delimited JSON over TCP (optionally TLS).
 *
 * Verified against:
 *  - sdk/node/src/index.js (ConnectionPool, PooledConnection)
 *  - engine/src/server.cpp (action dispatch)
 *  - cli/src/shell.js (action names and payloads)
 */

import * as net from 'net';
import * as tls from 'tls';
import * as fs from 'fs';
import * as crypto from 'crypto';
import { EventEmitter } from 'events';
import type { ConnectionProfile } from '../storage/profiles';

// ─── Types ────────────────────────────────────────────────────────────────────

export interface RequestCommand {
  action: string;
  [key: string]: unknown;
}

export interface PacificResponse {
  status?: string;
  error?: string;
  message?: string;
  [key: string]: unknown;
}

export interface RequestTiming {
  sentAt: number;
  receivedAt: number;
  totalMs: number;
}

export interface RequestRecord {
  id: string;
  command: RequestCommand;
  response: PacificResponse;
  timing: RequestTiming;
  error?: string;
}

interface PendingRequest {
  resolve: (value: PacificResponse) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
  sentAt: number;
  wire: string;
}

// ─── PooledConnection — mirrors sdk/node/src/index.js PooledConnection ────────

class PooledConnection {
  private socket: net.Socket | tls.TLSSocket | null = null;
  private connecting: Promise<void> | null = null;
  private connected = false;
  private current: PendingRequest | null = null;
  private responseBuffer = '';

  constructor(private readonly pool: PacificConnectionPool) {}

  async connect(): Promise<void> {
    if (this.connected && this.socket && !this.socket.destroyed) return;
    if (this.connecting) return this.connecting;

    const { profile } = this.pool;

    const socketOptions: net.TcpNetConnectOpts = {
      host: profile.host,
      port: profile.port,
    };

    const tlsOptions: tls.ConnectionOptions = {
      host: profile.host,
      port: profile.port,
      rejectUnauthorized: profile.tls?.verifyServer ?? true,
      ...(profile.tls?.caPath ? { ca: fs.readFileSync(profile.tls.caPath) } : {}),
      ...(profile.tls?.certPath ? { cert: fs.readFileSync(profile.tls.certPath) } : {}),
      ...(profile.tls?.keyPath ? { key: fs.readFileSync(profile.tls.keyPath) } : {}),
    };

    const socket = profile.tls?.enabled
      ? tls.connect(tlsOptions)
      : net.createConnection(socketOptions);

    this.socket = socket;
    this.responseBuffer = '';

    const connectedEvent = (profile.tls?.enabled && socket instanceof tls.TLSSocket)
      ? 'secureConnect' : 'connect';

    this.connecting = new Promise<void>((resolve, reject) => {
      const cleanup = () => {
        socket.off(connectedEvent as 'connect', onConnect);
        socket.off('error', onError);
        socket.off('close', onClose);
      };
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
      const onError = (err: Error) => { cleanup(); reject(err); };
      const onClose = () => { cleanup(); reject(new Error('PacificDB connection closed while connecting')); };
      socket.once(connectedEvent as 'connect', onConnect);
      socket.once('error', onError);
      socket.once('close', onClose);
    }).finally(() => { this.connecting = null; });

    socket.on('data', (chunk: Buffer) => this.onData(socket, chunk.toString('utf8')));
    socket.on('error', (err: Error) => this.onFailure(socket, err));
    socket.on('end', () =>
      this.onFailure(socket, new Error('PacificDB closed before returning a JSON response')));
    socket.on('close', () => {
      if (socket !== this.socket) return;
      this.connected = false;
      this.socket = null;
      this.responseBuffer = '';
    });

    return this.connecting;
  }

  private onData(socket: net.Socket | tls.TLSSocket, chunk: string) {
    if (socket !== this.socket || !this.current) return;
    this.responseBuffer += chunk;
    const newlineIdx = this.responseBuffer.indexOf('\n');
    if (newlineIdx < 0) return;

    const wire = this.responseBuffer.slice(0, newlineIdx);
    this.responseBuffer = this.responseBuffer.slice(newlineIdx + 1);

    let value: PacificResponse;
    try {
      value = JSON.parse(wire) as PacificResponse;
    } catch {
      this.finish(
        new Error(`PacificDB server at ${this.pool.profile.host}:${this.pool.profile.port} returned non-JSON response`),
        true,
      );
      return;
    }

    if (value?.error) {
      const code = String(value.error);
      const detail = typeof value.message === 'string' && value.message !== code
        ? `${code}: ${value.message}` : code;
      this.finish(Object.assign(new Error(detail), { serverError: value }));
    } else {
      this.finish(null, false, value);
    }
  }

  private onFailure(socket: net.Socket | tls.TLSSocket, error: Error) {
    if (socket !== this.socket) return;
    this.connected = false;
    if (this.current) this.finish(error, true);
  }

  private finish(error: Error | null, destroy = false, value?: PacificResponse) {
    const current = this.current;
    if (!current) return;
    this.current = null;
    clearTimeout(current.timer);
    if (destroy && this.socket && !this.socket.destroyed) this.socket.destroy();
    else if (this.socket && !this.socket.destroyed) this.socket.unref();
    if (error) current.reject(error);
    else current.resolve(value!);
  }

  async request(wire: string, timeoutMs: number): Promise<PacificResponse> {
    if (this.current) throw new Error('PacificDB pool: concurrent request on same socket');
    return new Promise<PacificResponse>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.finish(new Error(`PacificDB request timed out after ${timeoutMs}ms`), true);
      }, timeoutMs);
      const sentAt = Date.now();
      this.current = { resolve, reject, timer, sentAt, wire };
      this.connect().then(() => {
        if (!this.current || !this.socket || this.socket.destroyed) return;
        this.socket.ref();
        this.socket.write(wire, (err) => { if (err) this.finish(err, true); });
      }).catch((err) => this.finish(err, true));
    });
  }

  close(error?: Error) {
    if (this.current) this.finish(error ?? new Error('Connection closed'), true);
    if (this.socket && !this.socket.destroyed) this.socket.destroy();
    this.connected = false;
    this.socket = null;
    this.responseBuffer = '';
  }

  get isConnected() { return this.connected && !!this.socket && !this.socket.destroyed; }
}

// ─── PacificConnectionPool — mirrors sdk/node/src/index.js ConnectionPool ─────

export class PacificConnectionPool {
  private connections: PooledConnection[] = [];
  private idle: PooledConnection[] = [];
  private queue: Array<{ wire: string; resolve: (v: PacificResponse) => void; reject: (e: Error) => void }> = [];
  private closed = false;

  constructor(
    public readonly profile: ConnectionProfile,
    private readonly poolSize: number = 16,
  ) {}

  private newConnection(): PooledConnection {
    const conn = new PooledConnection(this);
    this.connections.push(conn);
    return conn;
  }

  private dispatch(connection: PooledConnection, job: { wire: string; resolve: (v: PacificResponse) => void; reject: (e: Error) => void }) {
    const timeoutMs = this.profile.advanced?.requestTimeoutMs ?? 30000;
    connection.request(job.wire, timeoutMs).then(job.resolve, job.reject).finally(() => {
      setImmediate(() => {
        if (this.closed) return;
        const next = this.queue.shift();
        if (next) this.dispatch(connection, next);
        else this.idle.push(connection);
      });
    });
  }

  request(wire: string): Promise<PacificResponse> {
    if (this.closed) return Promise.reject(new Error('PacificDB client is closed'));
    return new Promise<PacificResponse>((resolve, reject) => {
      const job = { wire, resolve, reject };
      const connection = this.idle.pop();
      if (connection) this.dispatch(connection, job);
      else if (this.connections.length < this.poolSize) this.dispatch(this.newConnection(), job);
      else this.queue.push(job);
    });
  }

  async connect(): Promise<void> {
    if (this.closed) throw new Error('PacificDB client is closed');
    while (this.connections.length < this.poolSize) this.idle.push(this.newConnection());
    await Promise.all(this.connections.map(c => c.connect()));
  }

  async ping(): Promise<boolean> {
    try {
      const conn = new PooledConnection(this);
      await conn.connect();
      conn.close();
      return true;
    } catch {
      return false;
    }
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    const error = new Error('PacificDB client is closed');
    for (const job of this.queue.splice(0)) job.reject(error);
    for (const conn of this.connections) conn.close(error);
    this.idle.length = 0;
    this.connections.length = 0;
  }

  get isAlive() { return !this.closed && this.connections.some(c => c.isConnected); }
}

// ─── PacificClient — high-level client with all documented actions ─────────────

export class PacificClient extends EventEmitter {
  private pool: PacificConnectionPool;
  public token: string = '';
  public database: string = '';

  constructor(private profile: ConnectionProfile) {
    super();
    const poolSize = Math.min(32, Math.max(1, profile.advanced?.poolSize ?? 4));
    this.pool = new PacificConnectionPool(profile, poolSize);

    // Restore session token from profile if stored
    if (profile.auth?.token) this.token = profile.auth.token;
    if (profile.database) this.database = profile.database;
  }

  // ── Wire framing ──────────────────────────────────────────────────────────
  request(command: RequestCommand): Promise<PacificResponse> {
    const payload: Record<string, unknown> = {
      userId: this.profile.auth?.userId ?? 'system',
      dbName: this.database,
      ...(this.token ? { token: this.token } : {}),
      ...command,
    };
    const wire = JSON.stringify(payload) + '\n';
    return this.pool.request(wire);
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────────
  async connect(): Promise<void> { await this.pool.connect(); }
  disconnect(): void { this.pool.close(); }
  get isConnected(): boolean { return this.pool.isAlive; }

  // ── System ────────────────────────────────────────────────────────────────
  ping()         { return this.request({ action: 'ping' }); }
  capabilities() { return this.request({ action: 'community_capabilities' }); }
  status()       { return this.request({ action: 'ping' }); } // status aliases ping
  metrics()      { return this.request({ action: 'metrics' }); }

  // ── Authentication ────────────────────────────────────────────────────────
  async authenticate(username: string, password: string): Promise<PacificResponse> {
    const result = await this.request({ action: 'security_authenticate', username, password });
    if (typeof result.token === 'string') this.token = result.token;
    return result;
  }
  whoami()    { return this.request({ action: 'security_whoami' }); }
  logout()    { this.token = ''; return Promise.resolve({ status: 'ok' } as PacificResponse); }

  // ── API Keys ──────────────────────────────────────────────────────────────
  createApiKey(name: string, role: string) {
    return this.request({ action: 'api_key_create', name, role });
  }
  listApiKeys() { return this.request({ action: 'api_key_list' }); }
  getApiKey(id: string) { return this.request({ action: 'api_key_get', id }); }
  revokeApiKey(id: string) { return this.request({ action: 'api_key_revoke', id }); }

  // ── Projects ──────────────────────────────────────────────────────────────
  createProject(name: string) { return this.request({ action: 'community_project_create', name }); }
  listProjects()              { return this.request({ action: 'community_project_list' }); }
  getProject(id: string)      { return this.request({ action: 'community_project_get', id }); }
  deleteProject(id: string)   { return this.request({ action: 'community_project_delete', id }); }

  // ── Databases ─────────────────────────────────────────────────────────────
  listDatabases(projectId?: string) {
    return projectId
      ? this.request({ action: 'community_database_list', project_id: projectId })
      : this.request({ action: 'listDatabases' });
  }
  createDatabase(dbName: string, dbType = 'binary') {
    return this.request({ action: 'createDatabase', dbName, dbType });
  }
  dropDatabase(dbName: string) { return this.request({ action: 'dropDatabase', dbName }); }

  // ── Collections ───────────────────────────────────────────────────────────
  listCollections() { return this.request({ action: 'listCollections' }); }
  createCollection(collection: string) { return this.request({ action: 'createCollection', collection }); }

  // ── CRUD ──────────────────────────────────────────────────────────────────
  insert(collection: string, data: Record<string, unknown>) {
    return this.request({ action: 'insert', collection, data });
  }
  insertMany(collection: string, data: Record<string, unknown>[]) {
    return this.request({ action: 'insertMany', collection, data });
  }
  find(collection: string, filter: Record<string, unknown> = {}, limit = 100, offset = 0) {
    return this.request({ action: 'find', collection, filter, limit, offset });
  }
  findOne(collection: string, filter: Record<string, unknown> = {}) {
    return this.request({ action: 'find', collection, filter, limit: 1 });
  }
  updateOne(collection: string, filter: Record<string, unknown>, update: Record<string, unknown>) {
    return this.request({ action: 'updateOne', collection, filter, update });
  }
  deleteOne(collection: string, filter: Record<string, unknown>) {
    return this.request({ action: 'deleteOne', collection, filter });
  }
  count(collection: string, filter: Record<string, unknown> = {}) {
    return this.request({ action: 'count', collection, filter });
  }

  // ── Query ─────────────────────────────────────────────────────────────────
  aggregate(collection: string, pipeline: unknown[]) {
    return this.request({ action: 'aggregate', collection, pipeline });
  }
  explain(collection: string, filter: Record<string, unknown> = {}) {
    return this.request({ action: 'explain', collection, filter });
  }

  // ── Secondary Indexes ─────────────────────────────────────────────────────
  createSecondaryIndex(collection: string, definition: Record<string, unknown>) {
    return this.request({ action: 'createSecondaryIndex', collection, definition });
  }
  listSecondaryIndexes(collection: string) {
    return this.request({ action: 'listSecondaryIndexes', collection });
  }
  dropSecondaryIndex(collection: string, name: string) {
    return this.request({ action: 'dropSecondaryIndex', collection, name });
  }
  rebuildSecondaryIndex(collection: string, name: string) {
    return this.request({ action: 'rebuildSecondaryIndex', collection, name });
  }

  // ── Vectors ───────────────────────────────────────────────────────────────
  insertVector(
    collection: string,
    id: string,
    vector: number[],
    metadata: Record<string, unknown> = {},
  ) {
    return this.request({
      action: 'insertVector',
      collection,
      data: { ...metadata, id, kind: 'vector', vector },
    });
  }
  queryVector(
    collection: string,
    vector: number[],
    { k = 10, metric = 'cosine', filter = {} }: { k?: number; metric?: string; filter?: Record<string, unknown> } = {},
  ) {
    return this.request({ action: 'queryVector', collection, vector, k, metric, filter });
  }

  // ── Media ─────────────────────────────────────────────────────────────────
  mediaBegin(params: {
    collection: string; filename: string; content_type: string;
    size_bytes: number; chunk_count: number; sha256: string; resume_id?: string;
  }) {
    return this.request({ action: 'community_media_begin', ...params });
  }

  async mediaPutChunk(mediaId: string, index: number, data: Buffer): Promise<PacificResponse> {
    const sha256 = crypto.createHash('sha256').update(data).digest('hex');
    return this.request({
      action: 'community_media_put_chunk',
      media_id: mediaId,
      index,
      data: data.toString('base64'),
      size_bytes: data.length,
      sha256,
    });
  }

  mediaFinalize(mediaId: string) {
    return this.request({ action: 'community_media_finalize', media_id: mediaId });
  }
  getMedia(mediaId: string)      { return this.request({ action: 'community_media_get', media_id: mediaId }); }
  getMediaChunk(mediaId: string, index: number) {
    return this.request({ action: 'community_media_get_chunk', media_id: mediaId, index });
  }
  listMedia(all = false)         { return this.request({ action: 'community_media_list', all }); }
  deleteMedia(mediaId: string)   { return this.request({ action: 'community_media_delete', media_id: mediaId }); }
  cleanupMedia(mediaId: string)  { return this.request({ action: 'community_media_cleanup', media_id: mediaId }); }

  // ── Backups ───────────────────────────────────────────────────────────────
  createBackup(description = 'manual backup') {
    return this.request({ action: 'create_backup', description });
  }
  listBackups()                  { return this.request({ action: 'list_backups' }); }
  getBackup(backupId: string)    { return this.request({ action: 'get_backup', backup_id: backupId }); }
  verifyBackup(backupId: string) { return this.request({ action: 'verify_backup', backup_id: backupId }); }
  restoreBackup(backupId: string){ return this.request({ action: 'restore_backup', backup_id: backupId }); }
  deleteBackup(backupId: string) { return this.request({ action: 'delete_backup', backup_id: backupId }); }
  listRestores()                 { return this.request({ action: 'list_restores' }); }
  exportBackupManifest(backupId: string) {
    return this.request({ action: 'export_backup_manifest', backup_id: backupId });
  }
  exportBackupFileChunk(backupId: string, filePath: string, offset: number, maxBytes: number) {
    return this.request({
      action: 'export_backup_file_chunk',
      backup_id: backupId,
      path: filePath,
      offset,
      max_bytes: maxBytes,
    });
  }
}
