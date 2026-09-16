/**
 * IPC Handlers — registered in the main process.
 * Every channel corresponds to an entry in preload.ts.
 *
 * Data flow:
 *   Renderer → IPC → Handler → PacificClient (TCP) → PacificDB Server
 *   PacificDB Server → TCP → PacificClient → IPC → Renderer
 */

import { ipcMain, dialog as electronDialog, app } from 'electron';
import * as fs from 'fs';
import * as path from 'path';
import * as crypto from 'crypto';
import type { ConnectionManager } from './connection-manager';
import type { ProfilesStore } from './storage/profiles';
import type { QueryHistoryStore } from './storage/query-history';
import type { SavedQueriesStore } from './storage/saved-queries';
import type { ConnectionProfile } from './protocol/types';

interface HandlerDeps {
  profilesStore: ProfilesStore;
  connectionManager: ConnectionManager;
  queryHistoryStore: QueryHistoryStore;
  savedQueriesStore: SavedQueriesStore;
  dialog: typeof electronDialog;
  APP_DATA_DIR: string;
  SETTINGS_FILE: string;
  safeStorage: Electron.SafeStorage;
}

const DEFAULT_SETTINGS = {
  theme: 'dark',
  editorFontSize: 13,
  editorTabSize: 2,
  queryLimit: 100,
  autoRefreshMs: 5000,
  showRawInspector: false,
  confirmDestructive: true,
};

export function registerProtocolHandlers(deps: HandlerDeps) {
  const { profilesStore, connectionManager, queryHistoryStore, savedQueriesStore } = deps;

  // ── App info ────────────────────────────────────────────────────────────
  ipcMain.handle('app:getVersion', () => app.getVersion());

  // ── Settings ────────────────────────────────────────────────────────────
  ipcMain.handle('settings:get', () => {
    try {
      return { ...DEFAULT_SETTINGS, ...JSON.parse(fs.readFileSync(deps.SETTINGS_FILE, 'utf8')) };
    } catch { return { ...DEFAULT_SETTINGS }; }
  });

  ipcMain.handle('settings:set', (_ev, settings: unknown) => {
    const merged = { ...DEFAULT_SETTINGS, ...(settings as object) };
    fs.writeFileSync(deps.SETTINGS_FILE, JSON.stringify(merged, null, 2), { mode: 0o600 });
    return merged;
  });

  // ── Connection Profiles ──────────────────────────────────────────────────
  ipcMain.handle('profiles:list', () => profilesStore.list());
  ipcMain.handle('profiles:get', (_ev, id: string) => profilesStore.get(id));

  ipcMain.handle('profiles:create', (_ev, profile: Omit<ConnectionProfile, 'id' | 'createdAt'> & { plainPassword?: string; plainToken?: string }) => {
    const { plainPassword, plainToken, ...rest } = profile;
    const toCreate = { ...rest };
    if (toCreate.auth) {
      if (plainPassword) {
        toCreate.auth = { ...toCreate.auth, encryptedPassword: profilesStore.encryptSecret(plainPassword) };
      }
      if (plainToken) {
        toCreate.auth = { ...toCreate.auth, encryptedToken: profilesStore.encryptSecret(plainToken) };
      }
    }
    return profilesStore.create(toCreate);
  });

  ipcMain.handle('profiles:update', (_ev, id: string, updates: Partial<ConnectionProfile> & { plainPassword?: string; plainToken?: string }) => {
    const { plainPassword, plainToken, ...rest } = updates;
    const toUpdate = { ...rest };
    if (plainPassword !== undefined && toUpdate.auth) {
      toUpdate.auth = { ...toUpdate.auth, encryptedPassword: profilesStore.encryptSecret(plainPassword) };
    }
    if (plainToken !== undefined && toUpdate.auth) {
      toUpdate.auth = { ...toUpdate.auth, encryptedToken: profilesStore.encryptSecret(plainToken) };
    }
    return profilesStore.update(id, toUpdate);
  });

  ipcMain.handle('profiles:delete', (_ev, id: string) => { profilesStore.delete(id); return { ok: true }; });
  ipcMain.handle('profiles:duplicate', (_ev, id: string) => profilesStore.duplicate(id));

  // ── Connection Lifecycle ─────────────────────────────────────────────────
  ipcMain.handle('connection:connect', async (_ev, profileId: string) => {
    const profile = profilesStore.get(profileId);
    if (!profile) throw new Error(`Profile not found: ${profileId}`);

    // Resolve encrypted credentials at connect time
    const resolved = { ...profile };
    if (resolved.auth?.encryptedPassword) {
      (resolved as ConnectionProfile & { _resolvedPassword: string })._resolvedPassword =
        profilesStore.decryptSecret(resolved.auth.encryptedPassword);
    }
    if (resolved.auth?.encryptedToken) {
      (resolved as ConnectionProfile & { _resolvedApiKey: string })._resolvedApiKey =
        profilesStore.decryptSecret(resolved.auth.encryptedToken);
    }

    const status = await connectionManager.connect(resolved);
    // Update last-connected-at
    profilesStore.update(profileId, { lastConnectedAt: new Date().toISOString() });
    return status;
  });

  ipcMain.handle('connection:disconnect', async (_ev, profileId: string) => {
    await connectionManager.disconnect(profileId);
    return { ok: true };
  });

  ipcMain.handle('connection:test', async (_ev, profileId: string) => {
    const profile = profilesStore.get(profileId);
    if (!profile) throw new Error(`Profile not found: ${profileId}`);
    const resolved = { ...profile };
    if (resolved.auth?.encryptedPassword) {
      (resolved as ConnectionProfile & { _resolvedPassword: string })._resolvedPassword =
        profilesStore.decryptSecret(resolved.auth.encryptedPassword);
    }
    return connectionManager.test(resolved);
  });

  ipcMain.handle('connection:status', (_ev, profileId: string) => {
    return connectionManager.getStatus(profileId) ?? { profileId, state: 'disconnected' };
  });

  ipcMain.handle('connection:statusAll', () => connectionManager.getAllStatuses());

  // ── PacificDB Protocol Requests ──────────────────────────────────────────
  ipcMain.handle('pacific:request', async (_ev, profileId: string, command: Record<string, unknown>) => {
    const client = connectionManager.getClient(profileId);
    if (!client) throw new Error(`Not connected to profile: ${profileId}`);

    const sentAt = Date.now();
    try {
      const response = await client.request(command as { action: string; [key: string]: unknown });
      const durationMs = Date.now() - sentAt;

      // Record in history
      queryHistoryStore.add({
        profileId,
        action: String(command.action ?? 'unknown'),
        database: client.database || undefined,
        collection: typeof command.collection === 'string' ? command.collection : undefined,
        request: command,
        response: response as Record<string, unknown>,
        durationMs,
        success: true,
        executedAt: new Date().toISOString(),
      });

      return { response, durationMs };
    } catch (error) {
      const durationMs = Date.now() - sentAt;
      const msg = error instanceof Error ? error.message : String(error);

      queryHistoryStore.add({
        profileId,
        action: String(command.action ?? 'unknown'),
        database: client.database || undefined,
        request: command,
        durationMs,
        success: false,
        error: msg,
        executedAt: new Date().toISOString(),
      });

      throw error;
    }
  });

  ipcMain.handle('pacific:insertMany', async (_ev, profileId: string, collection: string, docs: Record<string, unknown>[]) => {
    const client = connectionManager.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);
    const sentAt = Date.now();
    const response = await client.insertMany(collection, docs);
    return { response, durationMs: Date.now() - sentAt };
  });

  // ── Media Upload (chunked, progress events to renderer) ─────────────────
  ipcMain.handle('media:uploadFile', async (ev, profileId: string, params: {
    filePath: string; collection: string; contentType?: string; resumeId?: string;
  }) => {
    const client = connectionManager.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);

    const { filePath, collection, contentType = 'application/octet-stream', resumeId } = params;
    const stat = fs.statSync(filePath);
    const CHUNK_BYTES = 512 * 1024; // 512 KB chunks
    const chunkCount = Math.ceil(stat.size / CHUNK_BYTES);
    const filename = path.basename(filePath);

    // Compute full SHA-256 first
    const wholeHash = crypto.createHash('sha256');
    const fileStream = fs.createReadStream(filePath);
    for await (const chunk of fileStream) wholeHash.update(chunk as Buffer);
    const sha256 = wholeHash.digest('hex');

    // Begin upload
    const begun = await client.mediaBegin({
      collection, filename, content_type: contentType,
      size_bytes: stat.size, chunk_count: chunkCount, sha256,
      ...(resumeId ? { resume_id: resumeId } : {}),
    });

    const media = (begun as { media?: { id?: string; status?: string } }).media;
    if (!media?.id) throw new Error('Server did not return a media id');
    if (media.status === 'ready') return media;

    // Upload chunks
    const readStream = fs.createReadStream(filePath, { highWaterMark: CHUNK_BYTES });
    let index = 0;
    for await (const chunk of readStream) {
      await client.mediaPutChunk(media.id, index, chunk as Buffer);
      ev.sender.send('media:progress', { mediaId: media.id, index, total: chunkCount });
      index++;
    }

    const finalized = await client.mediaFinalize(media.id);
    return (finalized as { media?: unknown }).media ?? finalized;
  });

  ipcMain.handle('media:downloadFile', async (ev, profileId: string, mediaId: string, destination: string) => {
    const client = connectionManager.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);

    const manifest = (await client.getMedia(mediaId) as { media?: { status?: string; chunk_count?: number; sha256?: string; size_bytes?: number } }).media;
    if (!manifest || manifest.status !== 'ready') throw new Error(`Ready media not found: ${mediaId}`);

    const output = fs.createWriteStream(destination, { mode: 0o600 });
    const wholeHash = crypto.createHash('sha256');
    let sizeBytes = 0;

    for (let i = 0; i < (manifest.chunk_count ?? 0); i++) {
      const result = await client.getMediaChunk(mediaId, i) as { chunk?: { data?: string; sha256?: string } };
      const chunk = result.chunk;
      if (!chunk?.data) throw new Error(`Media chunk missing: ${i}`);
      const bytes = Buffer.from(chunk.data, 'base64');
      const checksum = crypto.createHash('sha256').update(bytes).digest('hex');
      if (checksum !== chunk.sha256) throw new Error(`Chunk checksum mismatch: ${i}`);
      wholeHash.update(bytes);
      sizeBytes += bytes.length;
      output.write(bytes);
      ev.sender.send('media:progress', { mediaId, index: i, total: manifest.chunk_count });
    }

    await new Promise<void>((res, rej) => { output.end(res); output.once('error', rej); });
    return { destination, sizeBytes, sha256: wholeHash.digest('hex') };
  });

  // ── Backup Export ────────────────────────────────────────────────────────
  ipcMain.handle('backup:exportToFile', async (ev, profileId: string, backupId: string, destination: string) => {
    const client = connectionManager.getClient(profileId);
    if (!client) throw new Error(`Not connected: ${profileId}`);

    const manifest = await client.exportBackupManifest(backupId) as { files?: { path: string; size_bytes: number; sha256: string }[]; format?: string; backup?: unknown };
    if (!Array.isArray(manifest.files)) throw new Error('Invalid backup manifest');

    const output = fs.createWriteStream(destination + '.part', { mode: 0o600 });
    const write = (s: string) => new Promise<void>((res, rej) => { output.write(s, e => e ? rej(e) : res()); });

    await write(`{"format":${JSON.stringify(manifest.format)},"backup":${JSON.stringify(manifest.backup)},"files":[`);
    for (let fi = 0; fi < manifest.files.length; fi++) {
      const file = manifest.files[fi];
      await write(`${fi ? ',' : ''}{"path":${JSON.stringify(file.path)},"size_bytes":${file.size_bytes},"sha256":${JSON.stringify(file.sha256)},"chunks":[`);
      let offset = 0, ci = 0;
      while (offset < file.size_bytes) {
        const chunk = await client.exportBackupFileChunk(backupId, file.path, offset, 1024 * 1024) as { data?: string; size_bytes?: number };
        await write(`${ci ? ',' : ''}${JSON.stringify(chunk.data)}`);
        offset += chunk.size_bytes ?? 0;
        ci++;
        ev.sender.send('backup:progress', { backupId, file: fi, total: manifest.files.length, offset, fileSize: file.size_bytes });
      }
      await write(']}');
    }
    await write(']}\n');
    await new Promise<void>((res, rej) => { output.end(res); output.once('error', rej); });
    fs.renameSync(destination + '.part', destination);
    return { destination, files: manifest.files.length };
  });

  // ── Query History ────────────────────────────────────────────────────────
  ipcMain.handle('history:list', (_ev, profileId?: string) => queryHistoryStore.list(profileId));
  ipcMain.handle('history:add', (_ev, _profileId: string, entry: Parameters<typeof queryHistoryStore.add>[0]) => queryHistoryStore.add(entry));
  ipcMain.handle('history:delete', (_ev, _profileId: string, id: string) => { queryHistoryStore.delete(id); return { ok: true }; });
  ipcMain.handle('history:clear', (_ev, profileId?: string) => { queryHistoryStore.clear(profileId); return { ok: true }; });

  // ── Saved Queries ────────────────────────────────────────────────────────
  ipcMain.handle('queries:list', () => savedQueriesStore.list());
  ipcMain.handle('queries:save', (_ev, query: Parameters<typeof savedQueriesStore.save>[0]) => savedQueriesStore.save(query));
  ipcMain.handle('queries:update', (_ev, id: string, updates: Parameters<typeof savedQueriesStore.update>[1]) => savedQueriesStore.update(id, updates));
  ipcMain.handle('queries:delete', (_ev, id: string) => { savedQueriesStore.delete(id); return { ok: true }; });

  // ── File Dialogs ─────────────────────────────────────────────────────────
  ipcMain.handle('dialog:openFile', async (_ev, options: Electron.OpenDialogOptions) => {
    const result = await deps.dialog.showOpenDialog({ properties: ['openFile'], ...options });
    return result;
  });
  ipcMain.handle('dialog:saveFile', async (_ev, options: Electron.SaveDialogOptions) => {
    const result = await deps.dialog.showSaveDialog(options);
    return result;
  });
  ipcMain.handle('dialog:openFolder', async (_ev, options: Electron.OpenDialogOptions) => {
    const result = await deps.dialog.showOpenDialog({ properties: ['openDirectory'], ...options });
    return result;
  });

  // ── NLQ (Natural Language Query Compiler — offline/deterministic) ─────────
  ipcMain.handle('nlq:compile', async (_ev, question: string, options?: unknown) => {
    try {
      // The NLQ compiler is a pure-JS deterministic module from intelligence/naturalLanguageQuery.js
      // We require it dynamically from the parent repo
      const nlqPath = path.resolve(app.getAppPath(), '../../intelligence/naturalLanguageQuery.js');
      // eslint-disable-next-line @typescript-eslint/no-var-requires
      const nlq = require(nlqPath);
      const compile = nlq.compile ?? nlq.default ?? nlq;
      if (typeof compile !== 'function') throw new Error('NLQ module does not export a compile function');
      return { success: true, result: compile(question, options) };
    } catch (error) {
      return { success: false, error: error instanceof Error ? error.message : String(error) };
    }
  });
}
