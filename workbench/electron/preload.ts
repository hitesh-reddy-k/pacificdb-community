import { contextBridge, ipcRenderer } from 'electron';

// ─── Typed IPC channels ───────────────────────────────────────────────────────
// The preload script is the ONLY bridge between the sandboxed renderer and main.
// All database communication goes through these channels.

const pacificAPI = {
  // ── Connection Profile Management ─────────────────────────────────────────
  profiles: {
    list:      ()         => ipcRenderer.invoke('profiles:list'),
    get:       (id: string) => ipcRenderer.invoke('profiles:get', id),
    create:    (profile: unknown) => ipcRenderer.invoke('profiles:create', profile),
    update:    (id: string, updates: unknown) => ipcRenderer.invoke('profiles:update', id, updates),
    delete:    (id: string) => ipcRenderer.invoke('profiles:delete', id),
    duplicate: (id: string) => ipcRenderer.invoke('profiles:duplicate', id),
  },

  // ── Connection Lifecycle ──────────────────────────────────────────────────
  connection: {
    connect:    (profileId: string) => ipcRenderer.invoke('connection:connect', profileId),
    disconnect: (profileId: string) => ipcRenderer.invoke('connection:disconnect', profileId),
    test:       (profileId: string) => ipcRenderer.invoke('connection:test', profileId),
    status:     (profileId: string) => ipcRenderer.invoke('connection:status', profileId),
    statusAll:  ()                   => ipcRenderer.invoke('connection:statusAll'),

    // Listen for connection state changes pushed from main
    onStateChange: (callback: (event: unknown) => void) => {
      ipcRenderer.on('connection:stateChange', (_ev, data) => callback(data));
      return () => ipcRenderer.removeAllListeners('connection:stateChange');
    },
  },

  // ── PacificDB Protocol Requests ───────────────────────────────────────────
  // Every request goes through main process TCP socket — never direct from renderer.
  request: (profileId: string, command: unknown) =>
    ipcRenderer.invoke('pacific:request', profileId, command),

  // ── Batch / Streaming operations ─────────────────────────────────────────
  insertMany: (profileId: string, collection: string, docs: unknown[]) =>
    ipcRenderer.invoke('pacific:insertMany', profileId, collection, docs),

  // ── Media operations (chunked, main-process only) ─────────────────────────
  media: {
    uploadFile:   (profileId: string, params: unknown) => ipcRenderer.invoke('media:uploadFile', profileId, params),
    downloadFile: (profileId: string, mediaId: string, destination: string) =>
      ipcRenderer.invoke('media:downloadFile', profileId, mediaId, destination),
    onProgress: (callback: (event: unknown) => void) => {
      ipcRenderer.on('media:progress', (_ev, data) => callback(data));
      return () => ipcRenderer.removeAllListeners('media:progress');
    },
  },

  // ── Backup export (streaming) ────────────────────────────────────────────
  backup: {
    exportToFile: (profileId: string, backupId: string, destination: string) =>
      ipcRenderer.invoke('backup:exportToFile', profileId, backupId, destination),
    onProgress: (callback: (event: unknown) => void) => {
      ipcRenderer.on('backup:progress', (_ev, data) => callback(data));
      return () => ipcRenderer.removeAllListeners('backup:progress');
    },
  },

  // ── Query History ─────────────────────────────────────────────────────────
  history: {
    list:   (profileId: string) => ipcRenderer.invoke('history:list', profileId),
    add:    (profileId: string, entry: unknown) => ipcRenderer.invoke('history:add', profileId, entry),
    delete: (profileId: string, id: string) => ipcRenderer.invoke('history:delete', profileId, id),
    clear:  (profileId: string) => ipcRenderer.invoke('history:clear', profileId),
  },

  // ── Saved Queries ─────────────────────────────────────────────────────────
  queries: {
    list:   () => ipcRenderer.invoke('queries:list'),
    save:   (query: unknown) => ipcRenderer.invoke('queries:save', query),
    update: (id: string, updates: unknown) => ipcRenderer.invoke('queries:update', id, updates),
    delete: (id: string) => ipcRenderer.invoke('queries:delete', id),
  },

  // ── Settings ──────────────────────────────────────────────────────────────
  settings: {
    get: () => ipcRenderer.invoke('settings:get'),
    set: (settings: unknown) => ipcRenderer.invoke('settings:set', settings),
  },

  // ── File dialogs (desktop-only capability) ────────────────────────────────
  dialog: {
    openFile:   (options: unknown) => ipcRenderer.invoke('dialog:openFile', options),
    saveFile:   (options: unknown) => ipcRenderer.invoke('dialog:saveFile', options),
    openFolder: (options: unknown) => ipcRenderer.invoke('dialog:openFolder', options),
  },

  // ── NLQ Compiler (runs in main, offline/deterministic) ───────────────────
  nlq: {
    compile: (question: string, options?: unknown) =>
      ipcRenderer.invoke('nlq:compile', question, options),
  },

  // ── App info ──────────────────────────────────────────────────────────────
  app: {
    getVersion: () => ipcRenderer.invoke('app:getVersion'),
    getPlatform: () => process.platform,
  },
};

contextBridge.exposeInMainWorld('pacific', pacificAPI);

// TypeScript type export for renderer
export type PacificAPI = typeof pacificAPI;
