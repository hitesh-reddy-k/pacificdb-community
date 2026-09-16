"use strict";
Object.defineProperty(exports, Symbol.toStringTag, { value: "Module" });
const electron = require("electron");
const path = require("path");
const fs = require("fs");
const events = require("events");
const net = require("net");
const tls = require("tls");
const crypto = require("crypto");
function _interopNamespaceDefault(e) {
  const n = Object.create(null, { [Symbol.toStringTag]: { value: "Module" } });
  if (e) {
    for (const k in e) {
      if (k !== "default") {
        const d = Object.getOwnPropertyDescriptor(e, k);
        Object.defineProperty(n, k, d.get ? d : {
          enumerable: true,
          get: () => e[k]
        });
      }
    }
  }
  n.default = e;
  return Object.freeze(n);
}
const path__namespace = /* @__PURE__ */ _interopNamespaceDefault(path);
const fs__namespace = /* @__PURE__ */ _interopNamespaceDefault(fs);
const net__namespace = /* @__PURE__ */ _interopNamespaceDefault(net);
const tls__namespace = /* @__PURE__ */ _interopNamespaceDefault(tls);
const crypto__namespace = /* @__PURE__ */ _interopNamespaceDefault(crypto);
class PooledConnection {
  constructor(pool) {
    this.pool = pool;
    this.socket = null;
    this.connecting = null;
    this.connected = false;
    this.current = null;
    this.responseBuffer = "";
  }
  async connect() {
    var _a, _b, _c, _d, _e, _f;
    if (this.connected && this.socket && !this.socket.destroyed) return;
    if (this.connecting) return this.connecting;
    const { profile } = this.pool;
    const socketOptions = {
      host: profile.host,
      port: profile.port
    };
    const tlsOptions = {
      host: profile.host,
      port: profile.port,
      rejectUnauthorized: ((_a = profile.tls) == null ? void 0 : _a.verifyServer) ?? true,
      ...((_b = profile.tls) == null ? void 0 : _b.caPath) ? { ca: fs__namespace.readFileSync(profile.tls.caPath) } : {},
      ...((_c = profile.tls) == null ? void 0 : _c.certPath) ? { cert: fs__namespace.readFileSync(profile.tls.certPath) } : {},
      ...((_d = profile.tls) == null ? void 0 : _d.keyPath) ? { key: fs__namespace.readFileSync(profile.tls.keyPath) } : {}
    };
    const socket = ((_e = profile.tls) == null ? void 0 : _e.enabled) ? tls__namespace.connect(tlsOptions) : net__namespace.createConnection(socketOptions);
    this.socket = socket;
    this.responseBuffer = "";
    const connectedEvent = ((_f = profile.tls) == null ? void 0 : _f.enabled) && socket instanceof tls__namespace.TLSSocket ? "secureConnect" : "connect";
    this.connecting = new Promise((resolve, reject) => {
      const cleanup = () => {
        socket.off(connectedEvent, onConnect);
        socket.off("error", onError);
        socket.off("close", onClose);
      };
      const onConnect = () => {
        cleanup();
        if (socket !== this.socket || socket.destroyed) {
          reject(new Error("PacificDB connection closed while connecting"));
          return;
        }
        this.connected = true;
        socket.unref();
        resolve();
      };
      const onError = (err) => {
        cleanup();
        reject(err);
      };
      const onClose = () => {
        cleanup();
        reject(new Error("PacificDB connection closed while connecting"));
      };
      socket.once(connectedEvent, onConnect);
      socket.once("error", onError);
      socket.once("close", onClose);
    }).finally(() => {
      this.connecting = null;
    });
    socket.on("data", (chunk) => this.onData(socket, chunk.toString("utf8")));
    socket.on("error", (err) => this.onFailure(socket, err));
    socket.on("end", () => this.onFailure(socket, new Error("PacificDB closed before returning a JSON response")));
    socket.on("close", () => {
      if (socket !== this.socket) return;
      this.connected = false;
      this.socket = null;
      this.responseBuffer = "";
    });
    return this.connecting;
  }
  onData(socket, chunk) {
    if (socket !== this.socket || !this.current) return;
    this.responseBuffer += chunk;
    const newlineIdx = this.responseBuffer.indexOf("\n");
    if (newlineIdx < 0) return;
    const wire = this.responseBuffer.slice(0, newlineIdx);
    this.responseBuffer = this.responseBuffer.slice(newlineIdx + 1);
    let value;
    try {
      value = JSON.parse(wire);
    } catch {
      this.finish(
        new Error(`PacificDB server at ${this.pool.profile.host}:${this.pool.profile.port} returned non-JSON response`),
        true
      );
      return;
    }
    if (value == null ? void 0 : value.error) {
      const code = String(value.error);
      const detail = typeof value.message === "string" && value.message !== code ? `${code}: ${value.message}` : code;
      this.finish(Object.assign(new Error(detail), { serverError: value }));
    } else {
      this.finish(null, false, value);
    }
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
  async request(wire, timeoutMs) {
    if (this.current) throw new Error("PacificDB pool: concurrent request on same socket");
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.finish(new Error(`PacificDB request timed out after ${timeoutMs}ms`), true);
      }, timeoutMs);
      const sentAt = Date.now();
      this.current = { resolve, reject, timer, sentAt, wire };
      this.connect().then(() => {
        if (!this.current || !this.socket || this.socket.destroyed) return;
        this.socket.ref();
        this.socket.write(wire, (err) => {
          if (err) this.finish(err, true);
        });
      }).catch((err) => this.finish(err, true));
    });
  }
  close(error) {
    if (this.current) this.finish(error ?? new Error("Connection closed"), true);
    if (this.socket && !this.socket.destroyed) this.socket.destroy();
    this.connected = false;
    this.socket = null;
    this.responseBuffer = "";
  }
  get isConnected() {
    return this.connected && !!this.socket && !this.socket.destroyed;
  }
}
class PacificConnectionPool {
  constructor(profile, poolSize = 16) {
    this.profile = profile;
    this.poolSize = poolSize;
    this.connections = [];
    this.idle = [];
    this.queue = [];
    this.closed = false;
  }
  newConnection() {
    const conn = new PooledConnection(this);
    this.connections.push(conn);
    return conn;
  }
  dispatch(connection, job) {
    var _a;
    const timeoutMs = ((_a = this.profile.advanced) == null ? void 0 : _a.requestTimeoutMs) ?? 3e4;
    connection.request(job.wire, timeoutMs).then(job.resolve, job.reject).finally(() => {
      setImmediate(() => {
        if (this.closed) return;
        const next = this.queue.shift();
        if (next) this.dispatch(connection, next);
        else this.idle.push(connection);
      });
    });
  }
  request(wire) {
    if (this.closed) return Promise.reject(new Error("PacificDB client is closed"));
    return new Promise((resolve, reject) => {
      const job = { wire, resolve, reject };
      const connection = this.idle.pop();
      if (connection) this.dispatch(connection, job);
      else if (this.connections.length < this.poolSize) this.dispatch(this.newConnection(), job);
      else this.queue.push(job);
    });
  }
  async connect() {
    if (this.closed) throw new Error("PacificDB client is closed");
    while (this.connections.length < this.poolSize) this.idle.push(this.newConnection());
    await Promise.all(this.connections.map((c) => c.connect()));
  }
  async ping() {
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
    const error = new Error("PacificDB client is closed");
    for (const job of this.queue.splice(0)) job.reject(error);
    for (const conn of this.connections) conn.close(error);
    this.idle.length = 0;
    this.connections.length = 0;
  }
  get isAlive() {
    return !this.closed && this.connections.some((c) => c.isConnected);
  }
}
class PacificClient extends events.EventEmitter {
  constructor(profile) {
    var _a, _b;
    super();
    this.profile = profile;
    this.token = "";
    this.database = "";
    const poolSize = Math.min(32, Math.max(1, ((_a = profile.advanced) == null ? void 0 : _a.poolSize) ?? 4));
    this.pool = new PacificConnectionPool(profile, poolSize);
    if ((_b = profile.auth) == null ? void 0 : _b.token) this.token = profile.auth.token;
    if (profile.database) this.database = profile.database;
  }
  // ── Wire framing ──────────────────────────────────────────────────────────
  request(command) {
    var _a;
    const payload = {
      userId: ((_a = this.profile.auth) == null ? void 0 : _a.userId) ?? "system",
      dbName: this.database,
      ...this.token ? { token: this.token } : {},
      ...command
    };
    const wire = JSON.stringify(payload) + "\n";
    return this.pool.request(wire);
  }
  // ── Lifecycle ─────────────────────────────────────────────────────────────
  async connect() {
    await this.pool.connect();
  }
  disconnect() {
    this.pool.close();
  }
  get isConnected() {
    return this.pool.isAlive;
  }
  // ── System ────────────────────────────────────────────────────────────────
  ping() {
    return this.request({ action: "ping" });
  }
  capabilities() {
    return this.request({ action: "community_capabilities" });
  }
  status() {
    return this.request({ action: "ping" });
  }
  // status aliases ping
  metrics() {
    return this.request({ action: "metrics" });
  }
  // ── Authentication ────────────────────────────────────────────────────────
  async authenticate(username, password) {
    const result = await this.request({ action: "security_authenticate", username, password });
    if (typeof result.token === "string") this.token = result.token;
    return result;
  }
  whoami() {
    return this.request({ action: "security_whoami" });
  }
  logout() {
    this.token = "";
    return Promise.resolve({ status: "ok" });
  }
  // ── API Keys ──────────────────────────────────────────────────────────────
  createApiKey(name, role) {
    return this.request({ action: "api_key_create", name, role });
  }
  listApiKeys() {
    return this.request({ action: "api_key_list" });
  }
  getApiKey(id) {
    return this.request({ action: "api_key_get", id });
  }
  revokeApiKey(id) {
    return this.request({ action: "api_key_revoke", id });
  }
  // ── Projects ──────────────────────────────────────────────────────────────
  createProject(name) {
    return this.request({ action: "community_project_create", name });
  }
  listProjects() {
    return this.request({ action: "community_project_list" });
  }
  getProject(id) {
    return this.request({ action: "community_project_get", id });
  }
  deleteProject(id) {
    return this.request({ action: "community_project_delete", id });
  }
  // ── Databases ─────────────────────────────────────────────────────────────
  listDatabases(projectId) {
    return projectId ? this.request({ action: "community_database_list", project_id: projectId }) : this.request({ action: "listDatabases" });
  }
  createDatabase(dbName, dbType = "binary") {
    return this.request({ action: "createDatabase", dbName, dbType });
  }
  dropDatabase(dbName) {
    return this.request({ action: "dropDatabase", dbName });
  }
  // ── Collections ───────────────────────────────────────────────────────────
  listCollections() {
    return this.request({ action: "listCollections" });
  }
  createCollection(collection) {
    return this.request({ action: "createCollection", collection });
  }
  // ── CRUD ──────────────────────────────────────────────────────────────────
  insert(collection, data) {
    return this.request({ action: "insert", collection, data });
  }
  insertMany(collection, data) {
    return this.request({ action: "insertMany", collection, data });
  }
  find(collection, filter = {}, limit = 100, offset = 0) {
    return this.request({ action: "find", collection, filter, limit, offset });
  }
  findOne(collection, filter = {}) {
    return this.request({ action: "find", collection, filter, limit: 1 });
  }
  updateOne(collection, filter, update) {
    return this.request({ action: "updateOne", collection, filter, update });
  }
  deleteOne(collection, filter) {
    return this.request({ action: "deleteOne", collection, filter });
  }
  count(collection, filter = {}) {
    return this.request({ action: "count", collection, filter });
  }
  // ── Query ─────────────────────────────────────────────────────────────────
  aggregate(collection, pipeline) {
    return this.request({ action: "aggregate", collection, pipeline });
  }
  explain(collection, filter = {}) {
    return this.request({ action: "explain", collection, filter });
  }
  // ── Secondary Indexes ─────────────────────────────────────────────────────
  createSecondaryIndex(collection, definition) {
    return this.request({ action: "createSecondaryIndex", collection, definition });
  }
  listSecondaryIndexes(collection) {
    return this.request({ action: "listSecondaryIndexes", collection });
  }
  dropSecondaryIndex(collection, name) {
    return this.request({ action: "dropSecondaryIndex", collection, name });
  }
  rebuildSecondaryIndex(collection, name) {
    return this.request({ action: "rebuildSecondaryIndex", collection, name });
  }
  // ── Vectors ───────────────────────────────────────────────────────────────
  insertVector(collection, id, vector, metadata = {}) {
    return this.request({
      action: "insertVector",
      collection,
      data: { ...metadata, id, kind: "vector", vector }
    });
  }
  queryVector(collection, vector, { k = 10, metric = "cosine", filter = {} } = {}) {
    return this.request({ action: "queryVector", collection, vector, k, metric, filter });
  }
  // ── Media ─────────────────────────────────────────────────────────────────
  mediaBegin(params) {
    return this.request({ action: "community_media_begin", ...params });
  }
  async mediaPutChunk(mediaId, index, data) {
    const sha256 = crypto__namespace.createHash("sha256").update(data).digest("hex");
    return this.request({
      action: "community_media_put_chunk",
      media_id: mediaId,
      index,
      data: data.toString("base64"),
      size_bytes: data.length,
      sha256
    });
  }
  mediaFinalize(mediaId) {
    return this.request({ action: "community_media_finalize", media_id: mediaId });
  }
  getMedia(mediaId) {
    return this.request({ action: "community_media_get", media_id: mediaId });
  }
  getMediaChunk(mediaId, index) {
    return this.request({ action: "community_media_get_chunk", media_id: mediaId, index });
  }
  listMedia(all = false) {
    return this.request({ action: "community_media_list", all });
  }
  deleteMedia(mediaId) {
    return this.request({ action: "community_media_delete", media_id: mediaId });
  }
  cleanupMedia(mediaId) {
    return this.request({ action: "community_media_cleanup", media_id: mediaId });
  }
  // ── Backups ───────────────────────────────────────────────────────────────
  createBackup(description = "manual backup") {
    return this.request({ action: "create_backup", description });
  }
  listBackups() {
    return this.request({ action: "list_backups" });
  }
  getBackup(backupId) {
    return this.request({ action: "get_backup", backup_id: backupId });
  }
  verifyBackup(backupId) {
    return this.request({ action: "verify_backup", backup_id: backupId });
  }
  restoreBackup(backupId) {
    return this.request({ action: "restore_backup", backup_id: backupId });
  }
  deleteBackup(backupId) {
    return this.request({ action: "delete_backup", backup_id: backupId });
  }
  listRestores() {
    return this.request({ action: "list_restores" });
  }
  exportBackupManifest(backupId) {
    return this.request({ action: "export_backup_manifest", backup_id: backupId });
  }
  exportBackupFileChunk(backupId, filePath, offset, maxBytes) {
    return this.request({
      action: "export_backup_file_chunk",
      backup_id: backupId,
      path: filePath,
      offset,
      max_bytes: maxBytes
    });
  }
}
class ConnectionManager extends events.EventEmitter {
  constructor() {
    super(...arguments);
    this.connections = /* @__PURE__ */ new Map();
  }
  pushStateChange(status) {
    const win = electron.BrowserWindow.getAllWindows()[0];
    if (win && !win.isDestroyed()) {
      win.webContents.send("connection:stateChange", status);
    }
    this.emit("stateChange", status);
  }
  updateState(profileId, state, extras = {}) {
    const entry = this.connections.get(profileId);
    if (!entry) return;
    Object.assign(entry.status, { state, ...extras });
    this.pushStateChange({ ...entry.status });
  }
  async connect(profile) {
    var _a, _b, _c, _d, _e, _f;
    const existing = this.connections.get(profile.id);
    if (existing) {
      existing.client.disconnect();
      this.connections.delete(profile.id);
    }
    const client = new PacificClient(profile);
    const status = {
      profileId: profile.id,
      state: "connecting"
    };
    this.connections.set(profile.id, { profile, client, status });
    this.pushStateChange({ ...status });
    try {
      await client.connect();
      this.updateState(profile.id, "connected", { connectedAt: (/* @__PURE__ */ new Date()).toISOString() });
      await client.ping();
      if (((_a = profile.auth) == null ? void 0 : _a.mode) === "password" && ((_b = profile.auth) == null ? void 0 : _b.username)) {
        this.updateState(profile.id, "authenticating");
        const creds = profile;
        const password = creds._resolvedPassword ?? "";
        const result = await client.authenticate(profile.auth.username, password);
        const username = typeof result.username === "string" ? result.username : profile.auth.username;
        this.updateState(profile.id, "authenticated", { authenticatedAs: username });
      } else if (((_c = profile.auth) == null ? void 0 : _c.mode) === "token" && ((_d = profile.auth) == null ? void 0 : _d.token)) {
        client.token = profile.auth.token;
        this.updateState(profile.id, "authenticated");
      } else if (((_e = profile.auth) == null ? void 0 : _e.mode) === "apikey" && ((_f = profile.auth) == null ? void 0 : _f.encryptedToken)) {
        const apiKeyEntry = profile;
        if (apiKeyEntry._resolvedApiKey) client.token = apiKeyEntry._resolvedApiKey;
        this.updateState(profile.id, "authenticated");
      } else {
        this.updateState(profile.id, "connected");
      }
      try {
        const cap = await client.capabilities();
        this.updateState(profile.id, this.connections.get(profile.id).status.state, {
          capabilities: cap
        });
      } catch {
      }
      return { ...this.connections.get(profile.id).status };
    } catch (error) {
      const msg = error instanceof Error ? error.message : String(error);
      client.disconnect();
      this.connections.delete(profile.id);
      const errStatus = { profileId: profile.id, state: "error", error: msg };
      this.pushStateChange(errStatus);
      throw error;
    }
  }
  async disconnect(profileId) {
    const entry = this.connections.get(profileId);
    if (!entry) return;
    entry.client.disconnect();
    this.connections.delete(profileId);
    this.pushStateChange({ profileId, state: "disconnected" });
  }
  async disconnectAll() {
    const ids = [...this.connections.keys()];
    await Promise.allSettled(ids.map((id) => this.disconnect(id)));
  }
  async test(profile) {
    const start = Date.now();
    const testClient = new PacificClient({
      ...profile,
      advanced: { ...profile.advanced, poolSize: 1, requestTimeoutMs: 1e4 }
    });
    try {
      await testClient.connect();
      await testClient.ping();
      const cap = await testClient.capabilities().catch(() => null);
      return { success: true, latencyMs: Date.now() - start, capabilities: cap };
    } catch (error) {
      return { success: false, latencyMs: Date.now() - start, error: error instanceof Error ? error.message : String(error) };
    } finally {
      testClient.disconnect();
    }
  }
  getClient(profileId) {
    var _a;
    return ((_a = this.connections.get(profileId)) == null ? void 0 : _a.client) ?? null;
  }
  getStatus(profileId) {
    const entry = this.connections.get(profileId);
    return entry ? { ...entry.status } : null;
  }
  getAllStatuses() {
    return [...this.connections.values()].map((e) => ({ ...e.status }));
  }
  isConnected(profileId) {
    return this.connections.has(profileId);
  }
}
const CURRENT_VERSION = 1;
class ProfilesStore {
  constructor(filePath, safeStorage) {
    this.filePath = filePath;
    this.safeStorage = safeStorage;
  }
  load() {
    try {
      const raw = fs__namespace.readFileSync(this.filePath, "utf8");
      const data = JSON.parse(raw);
      if (data.version !== CURRENT_VERSION) return { version: CURRENT_VERSION, profiles: [] };
      return data;
    } catch {
      return { version: CURRENT_VERSION, profiles: [] };
    }
  }
  save(data) {
    const json = JSON.stringify(data, null, 2);
    const tmp = this.filePath + ".tmp";
    fs__namespace.writeFileSync(tmp, json, { mode: 384 });
    fs__namespace.renameSync(tmp, this.filePath);
  }
  list() {
    return this.load().profiles;
  }
  get(id) {
    return this.load().profiles.find((p) => p.id === id) ?? null;
  }
  create(profile) {
    const data = this.load();
    const newProfile = {
      ...profile,
      id: crypto__namespace.randomUUID(),
      createdAt: (/* @__PURE__ */ new Date()).toISOString()
    };
    data.profiles.push(newProfile);
    this.save(data);
    return newProfile;
  }
  update(id, updates) {
    const data = this.load();
    const idx = data.profiles.findIndex((p) => p.id === id);
    if (idx < 0) throw new Error(`Profile not found: ${id}`);
    const updated = { ...data.profiles[idx], ...updates };
    data.profiles[idx] = updated;
    this.save(data);
    return updated;
  }
  delete(id) {
    const data = this.load();
    data.profiles = data.profiles.filter((p) => p.id !== id);
    this.save(data);
  }
  duplicate(id) {
    const original = this.get(id);
    if (!original) throw new Error(`Profile not found: ${id}`);
    return this.create({
      ...original,
      name: `${original.name} (copy)`,
      auth: original.auth ? { ...original.auth, encryptedPassword: void 0, token: void 0, encryptedToken: void 0 } : void 0
    });
  }
  // ── Encrypted field helpers ──────────────────────────────────────────────
  encryptSecret(plainText) {
    if (!this.safeStorage.isEncryptionAvailable()) {
      return "b64:" + Buffer.from(plainText).toString("base64");
    }
    const encrypted = this.safeStorage.encryptString(plainText);
    return "enc:" + encrypted.toString("base64");
  }
  decryptSecret(cipherText) {
    if (!cipherText) return "";
    if (cipherText.startsWith("b64:")) {
      return Buffer.from(cipherText.slice(4), "base64").toString("utf8");
    }
    if (cipherText.startsWith("enc:")) {
      const buf = Buffer.from(cipherText.slice(4), "base64");
      return this.safeStorage.decryptString(buf);
    }
    return cipherText;
  }
}
const MAX_HISTORY = 500;
const SENSITIVE = /password|token|api.?key/i;
function scrub(obj) {
  const result = {};
  for (const [k, v] of Object.entries(obj)) {
    result[k] = SENSITIVE.test(k) ? "***" : v;
  }
  return result;
}
class QueryHistoryStore {
  constructor(filePath) {
    this.filePath = filePath;
    this.history = [];
    try {
      const raw = fs__namespace.readFileSync(filePath, "utf8");
      this.history = JSON.parse(raw);
    } catch {
      this.history = [];
    }
  }
  persist() {
    try {
      const tmp = this.filePath + ".tmp";
      fs__namespace.writeFileSync(tmp, JSON.stringify(this.history.slice(-MAX_HISTORY), null, 2), { mode: 384 });
      fs__namespace.renameSync(tmp, this.filePath);
    } catch {
    }
  }
  add(entry) {
    const full = {
      ...entry,
      id: crypto__namespace.randomUUID(),
      request: scrub(entry.request)
    };
    this.history.unshift(full);
    if (this.history.length > MAX_HISTORY) this.history = this.history.slice(0, MAX_HISTORY);
    this.persist();
    return full;
  }
  list(profileId) {
    if (profileId) return this.history.filter((e) => e.profileId === profileId);
    return [...this.history];
  }
  delete(id) {
    this.history = this.history.filter((e) => e.id !== id);
    this.persist();
  }
  clear(profileId) {
    if (profileId) this.history = this.history.filter((e) => e.profileId !== profileId);
    else this.history = [];
    this.persist();
  }
}
class SavedQueriesStore {
  constructor(filePath) {
    this.filePath = filePath;
    this.queries = [];
    try {
      const raw = fs__namespace.readFileSync(filePath, "utf8");
      this.queries = JSON.parse(raw);
    } catch {
      this.queries = [];
    }
  }
  persist() {
    try {
      const tmp = this.filePath + ".tmp";
      fs__namespace.writeFileSync(tmp, JSON.stringify(this.queries, null, 2), { mode: 384 });
      fs__namespace.renameSync(tmp, this.filePath);
    } catch {
    }
  }
  list() {
    return [...this.queries];
  }
  save(query) {
    const now = (/* @__PURE__ */ new Date()).toISOString();
    const saved = { ...query, id: crypto__namespace.randomUUID(), createdAt: now, updatedAt: now };
    this.queries.unshift(saved);
    this.persist();
    return saved;
  }
  update(id, updates) {
    const idx = this.queries.findIndex((q) => q.id === id);
    if (idx < 0) throw new Error(`Saved query not found: ${id}`);
    this.queries[idx] = { ...this.queries[idx], ...updates, updatedAt: (/* @__PURE__ */ new Date()).toISOString() };
    this.persist();
    return this.queries[idx];
  }
  delete(id) {
    this.queries = this.queries.filter((q) => q.id !== id);
    this.persist();
  }
}
const DEFAULT_SETTINGS = {
  theme: "dark",
  editorFontSize: 13,
  editorTabSize: 2,
  queryLimit: 100,
  autoRefreshMs: 5e3,
  showRawInspector: false,
  confirmDestructive: true
};
function registerProtocolHandlers(deps) {
  const { profilesStore: profilesStore2, connectionManager: connectionManager2, queryHistoryStore: queryHistoryStore2, savedQueriesStore: savedQueriesStore2 } = deps;
  electron.ipcMain.handle("app:getVersion", () => electron.app.getVersion());
  electron.ipcMain.handle("settings:get", () => {
    try {
      return { ...DEFAULT_SETTINGS, ...JSON.parse(fs__namespace.readFileSync(deps.SETTINGS_FILE, "utf8")) };
    } catch {
      return { ...DEFAULT_SETTINGS };
    }
  });
  electron.ipcMain.handle("settings:set", (_ev, settings) => {
    const merged = { ...DEFAULT_SETTINGS, ...settings };
    fs__namespace.writeFileSync(deps.SETTINGS_FILE, JSON.stringify(merged, null, 2), { mode: 384 });
    return merged;
  });
  electron.ipcMain.handle("profiles:list", () => profilesStore2.list());
  electron.ipcMain.handle("profiles:get", (_ev, id) => profilesStore2.get(id));
  electron.ipcMain.handle("profiles:create", (_ev, profile) => {
    const { plainPassword, plainToken, ...rest } = profile;
    const toCreate = { ...rest };
    if (toCreate.auth) {
      if (plainPassword) {
        toCreate.auth = { ...toCreate.auth, encryptedPassword: profilesStore2.encryptSecret(plainPassword) };
      }
      if (plainToken) {
        toCreate.auth = { ...toCreate.auth, encryptedToken: profilesStore2.encryptSecret(plainToken) };
      }
    }
    return profilesStore2.create(toCreate);
  });
  electron.ipcMain.handle("profiles:update", (_ev, id, updates) => {
    const { plainPassword, plainToken, ...rest } = updates;
    const toUpdate = { ...rest };
    if (plainPassword !== void 0 && toUpdate.auth) {
      toUpdate.auth = { ...toUpdate.auth, encryptedPassword: profilesStore2.encryptSecret(plainPassword) };
    }
    if (plainToken !== void 0 && toUpdate.auth) {
      toUpdate.auth = { ...toUpdate.auth, encryptedToken: profilesStore2.encryptSecret(plainToken) };
    }
    return profilesStore2.update(id, toUpdate);
  });
  electron.ipcMain.handle("profiles:delete", (_ev, id) => {
    profilesStore2.delete(id);
    return { ok: true };
  });
  electron.ipcMain.handle("profiles:duplicate", (_ev, id) => profilesStore2.duplicate(id));
  electron.ipcMain.handle("connection:connect", async (_ev, profileId) => {
    var _a, _b;
    const profile = profilesStore2.get(profileId);
    if (!profile) throw new Error(`Profile not found: ${profileId}`);
    const resolved = { ...profile };
    if ((_a = resolved.auth) == null ? void 0 : _a.encryptedPassword) {
      resolved._resolvedPassword = profilesStore2.decryptSecret(resolved.auth.encryptedPassword);
    }
    if ((_b = resolved.auth) == null ? void 0 : _b.encryptedToken) {
      resolved._resolvedApiKey = profilesStore2.decryptSecret(resolved.auth.encryptedToken);
    }
    const status = await connectionManager2.connect(resolved);
    profilesStore2.update(profileId, { lastConnectedAt: (/* @__PURE__ */ new Date()).toISOString() });
    return status;
  });
  electron.ipcMain.handle("connection:disconnect", async (_ev, profileId) => {
    await connectionManager2.disconnect(profileId);
    return { ok: true };
  });
  electron.ipcMain.handle("connection:test", async (_ev, profileId) => {
    var _a;
    const profile = profilesStore2.get(profileId);
    if (!profile) throw new Error(`Profile not found: ${profileId}`);
    const resolved = { ...profile };
    if ((_a = resolved.auth) == null ? void 0 : _a.encryptedPassword) {
      resolved._resolvedPassword = profilesStore2.decryptSecret(resolved.auth.encryptedPassword);
    }
    return connectionManager2.test(resolved);
  });
  electron.ipcMain.handle("connection:status", (_ev, profileId) => {
    return connectionManager2.getStatus(profileId) ?? { profileId, state: "disconnected" };
  });
  electron.ipcMain.handle("connection:statusAll", () => connectionManager2.getAllStatuses());
  electron.ipcMain.handle("pacific:request", async (_ev, profileId, command) => {
    const client = connectionManager2.getClient(profileId);
    if (!client) throw new Error(`Not connected to profile: ${profileId}`);
    const sentAt = Date.now();
    try {
      const response = await client.request(command);
      const durationMs = Date.now() - sentAt;
      queryHistoryStore2.add({
        profileId,
        action: String(command.action ?? "unknown"),
        database: client.database || void 0,
        collection: typeof command.collection === "string" ? command.collection : void 0,
        request: command,
        response,
        durationMs,
        success: true,
        executedAt: (/* @__PURE__ */ new Date()).toISOString()
      });
      return { response, durationMs };
    } catch (error) {
      const durationMs = Date.now() - sentAt;
      const msg = error instanceof Error ? error.message : String(error);
      queryHistoryStore2.add({
        profileId,
        action: String(command.action ?? "unknown"),
        database: client.database || void 0,
        request: command,
        durationMs,
        success: false,
        error: msg,
        executedAt: (/* @__PURE__ */ new Date()).toISOString()
      });
      throw error;
    }
  });
  electron.ipcMain.handle("pacific:insertMany", async (_ev, profileId, collection, docs) => {
    const client = connectionManager2.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);
    const sentAt = Date.now();
    const response = await client.insertMany(collection, docs);
    return { response, durationMs: Date.now() - sentAt };
  });
  electron.ipcMain.handle("media:uploadFile", async (ev, profileId, params) => {
    const client = connectionManager2.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);
    const { filePath, collection, contentType = "application/octet-stream", resumeId } = params;
    const stat = fs__namespace.statSync(filePath);
    const CHUNK_BYTES = 512 * 1024;
    const chunkCount = Math.ceil(stat.size / CHUNK_BYTES);
    const filename = path__namespace.basename(filePath);
    const wholeHash = crypto__namespace.createHash("sha256");
    const fileStream = fs__namespace.createReadStream(filePath);
    for await (const chunk of fileStream) wholeHash.update(chunk);
    const sha256 = wholeHash.digest("hex");
    const begun = await client.mediaBegin({
      collection,
      filename,
      content_type: contentType,
      size_bytes: stat.size,
      chunk_count: chunkCount,
      sha256,
      ...resumeId ? { resume_id: resumeId } : {}
    });
    const media = begun.media;
    if (!(media == null ? void 0 : media.id)) throw new Error("Server did not return a media id");
    if (media.status === "ready") return media;
    const readStream = fs__namespace.createReadStream(filePath, { highWaterMark: CHUNK_BYTES });
    let index = 0;
    for await (const chunk of readStream) {
      await client.mediaPutChunk(media.id, index, chunk);
      ev.sender.send("media:progress", { mediaId: media.id, index, total: chunkCount });
      index++;
    }
    const finalized = await client.mediaFinalize(media.id);
    return finalized.media ?? finalized;
  });
  electron.ipcMain.handle("media:downloadFile", async (ev, profileId, mediaId, destination) => {
    const client = connectionManager2.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);
    const manifest = (await client.getMedia(mediaId)).media;
    if (!manifest || manifest.status !== "ready") throw new Error(`Ready media not found: ${mediaId}`);
    const output = fs__namespace.createWriteStream(destination, { mode: 384 });
    const wholeHash = crypto__namespace.createHash("sha256");
    let sizeBytes = 0;
    for (let i = 0; i < (manifest.chunk_count ?? 0); i++) {
      const result = await client.getMediaChunk(mediaId, i);
      const chunk = result.chunk;
      if (!(chunk == null ? void 0 : chunk.data)) throw new Error(`Media chunk missing: ${i}`);
      const bytes = Buffer.from(chunk.data, "base64");
      const checksum = crypto__namespace.createHash("sha256").update(bytes).digest("hex");
      if (checksum !== chunk.sha256) throw new Error(`Chunk checksum mismatch: ${i}`);
      wholeHash.update(bytes);
      sizeBytes += bytes.length;
      output.write(bytes);
      ev.sender.send("media:progress", { mediaId, index: i, total: manifest.chunk_count });
    }
    await new Promise((res, rej) => {
      output.end(res);
      output.once("error", rej);
    });
    return { destination, sizeBytes, sha256: wholeHash.digest("hex") };
  });
  electron.ipcMain.handle("backup:exportToFile", async (ev, profileId, backupId, destination) => {
    const client = connectionManager2.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);
    const manifest = await client.exportBackupManifest(backupId);
    if (!Array.isArray(manifest.files)) throw new Error("Invalid backup manifest");
    const output = fs__namespace.createWriteStream(destination + ".part", { mode: 384 });
    const write = (s) => new Promise((res, rej) => {
      output.write(s, (e) => e ? rej(e) : res());
    });
    await write(`{"format":${JSON.stringify(manifest.format)},"backup":${JSON.stringify(manifest.backup)},"files":[`);
    for (let fi = 0; fi < manifest.files.length; fi++) {
      const file = manifest.files[fi];
      await write(`${fi ? "," : ""}{"path":${JSON.stringify(file.path)},"size_bytes":${file.size_bytes},"sha256":${JSON.stringify(file.sha256)},"chunks":[`);
      let offset = 0, ci = 0;
      while (offset < file.size_bytes) {
        const chunk = await client.exportBackupFileChunk(backupId, file.path, offset, 1024 * 1024);
        await write(`${ci ? "," : ""}${JSON.stringify(chunk.data)}`);
        offset += chunk.size_bytes ?? 0;
        ci++;
        ev.sender.send("backup:progress", { backupId, file: fi, total: manifest.files.length, offset, fileSize: file.size_bytes });
      }
      await write("]}");
    }
    await write("]}\n");
    await new Promise((res, rej) => {
      output.end(res);
      output.once("error", rej);
    });
    fs__namespace.renameSync(destination + ".part", destination);
    return { destination, files: manifest.files.length };
  });
  electron.ipcMain.handle("history:list", (_ev, profileId) => queryHistoryStore2.list(profileId));
  electron.ipcMain.handle("history:add", (_ev, _profileId, entry) => queryHistoryStore2.add(entry));
  electron.ipcMain.handle("history:delete", (_ev, _profileId, id) => {
    queryHistoryStore2.delete(id);
    return { ok: true };
  });
  electron.ipcMain.handle("history:clear", (_ev, profileId) => {
    queryHistoryStore2.clear(profileId);
    return { ok: true };
  });
  electron.ipcMain.handle("queries:list", () => savedQueriesStore2.list());
  electron.ipcMain.handle("queries:save", (_ev, query) => savedQueriesStore2.save(query));
  electron.ipcMain.handle("queries:update", (_ev, id, updates) => savedQueriesStore2.update(id, updates));
  electron.ipcMain.handle("queries:delete", (_ev, id) => {
    savedQueriesStore2.delete(id);
    return { ok: true };
  });
  electron.ipcMain.handle("dialog:openFile", async (_ev, options) => {
    const result = await deps.dialog.showOpenDialog({ properties: ["openFile"], ...options });
    return result;
  });
  electron.ipcMain.handle("dialog:saveFile", async (_ev, options) => {
    const result = await deps.dialog.showSaveDialog(options);
    return result;
  });
  electron.ipcMain.handle("dialog:openFolder", async (_ev, options) => {
    const result = await deps.dialog.showOpenDialog({ properties: ["openDirectory"], ...options });
    return result;
  });
  electron.ipcMain.handle("nlq:compile", async (_ev, question, options) => {
    try {
      const nlqPath = path__namespace.resolve(electron.app.getAppPath(), "../../intelligence/naturalLanguageQuery.js");
      const nlq = require(nlqPath);
      const compile = nlq.compile ?? nlq.default ?? nlq;
      if (typeof compile !== "function") throw new Error("NLQ module does not export a compile function");
      return { success: true, result: compile(question, options) };
    } catch (error) {
      return { success: false, error: error instanceof Error ? error.message : String(error) };
    }
  });
}
const APP_DATA_DIR = path__namespace.join(electron.app.getPath("userData"), "pacificdb-workbench");
const CONNECTIONS_FILE = path__namespace.join(APP_DATA_DIR, "connections.json");
const HISTORY_FILE = path__namespace.join(APP_DATA_DIR, "query-history.json");
const QUERIES_FILE = path__namespace.join(APP_DATA_DIR, "saved-queries.json");
const SETTINGS_FILE = path__namespace.join(APP_DATA_DIR, "settings.json");
if (!fs__namespace.existsSync(APP_DATA_DIR)) {
  fs__namespace.mkdirSync(APP_DATA_DIR, { recursive: true, mode: 448 });
}
const profilesStore = new ProfilesStore(CONNECTIONS_FILE, electron.safeStorage);
const connectionManager = new ConnectionManager();
const queryHistoryStore = new QueryHistoryStore(HISTORY_FILE);
const savedQueriesStore = new SavedQueriesStore(QUERIES_FILE);
let mainWindow = null;
function createWindow() {
  mainWindow = new electron.BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 1100,
    minHeight: 680,
    backgroundColor: "#0d0f12",
    titleBarStyle: process.platform === "darwin" ? "hiddenInset" : "default",
    frame: process.platform !== "darwin",
    show: false,
    icon: path__namespace.join(__dirname, "../assets/icon.png"),
    webPreferences: {
      preload: path__namespace.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
      webSecurity: true
    }
  });
  if (process.env.NODE_ENV === "development") {
    mainWindow.loadURL("http://localhost:5173");
    mainWindow.webContents.openDevTools({ mode: "detach" });
  } else {
    mainWindow.loadFile(path__namespace.join(__dirname, "../dist/index.html"));
  }
  mainWindow.once("ready-to-show", () => {
    mainWindow.show();
    mainWindow.focus();
  });
  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    electron.shell.openExternal(url);
    return { action: "deny" };
  });
  mainWindow.on("closed", () => {
    mainWindow = null;
  });
}
registerProtocolHandlers({
  profilesStore,
  connectionManager,
  queryHistoryStore,
  savedQueriesStore,
  dialog: electron.dialog,
  APP_DATA_DIR,
  SETTINGS_FILE,
  safeStorage: electron.safeStorage
});
electron.app.whenReady().then(() => {
  createWindow();
  electron.app.on("activate", () => {
    if (electron.BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});
electron.app.on("window-all-closed", () => {
  connectionManager.disconnectAll().finally(() => {
    if (process.platform !== "darwin") electron.app.quit();
  });
});
electron.app.on("before-quit", () => {
  connectionManager.disconnectAll().catch(() => {
  });
});
electron.app.on("web-contents-created", (_event, contents) => {
  contents.on("will-navigate", (event, url) => {
    if (!url.startsWith("http://localhost:5173") && !url.startsWith("file://")) {
      event.preventDefault();
    }
  });
});
exports.connectionManager = connectionManager;
exports.profilesStore = profilesStore;
exports.queryHistoryStore = queryHistoryStore;
exports.savedQueriesStore = savedQueriesStore;
