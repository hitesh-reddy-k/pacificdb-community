"use strict";
const electron = require("electron");
const pacificAPI = {
  // ── Connection Profile Management ─────────────────────────────────────────
  profiles: {
    list: () => electron.ipcRenderer.invoke("profiles:list"),
    get: (id) => electron.ipcRenderer.invoke("profiles:get", id),
    create: (profile) => electron.ipcRenderer.invoke("profiles:create", profile),
    update: (id, updates) => electron.ipcRenderer.invoke("profiles:update", id, updates),
    delete: (id) => electron.ipcRenderer.invoke("profiles:delete", id),
    duplicate: (id) => electron.ipcRenderer.invoke("profiles:duplicate", id)
  },
  // ── Connection Lifecycle ──────────────────────────────────────────────────
  connection: {
    connect: (profileId) => electron.ipcRenderer.invoke("connection:connect", profileId),
    disconnect: (profileId) => electron.ipcRenderer.invoke("connection:disconnect", profileId),
    test: (profileId) => electron.ipcRenderer.invoke("connection:test", profileId),
    status: (profileId) => electron.ipcRenderer.invoke("connection:status", profileId),
    statusAll: () => electron.ipcRenderer.invoke("connection:statusAll"),
    // Listen for connection state changes pushed from main
    onStateChange: (callback) => {
      electron.ipcRenderer.on("connection:stateChange", (_ev, data) => callback(data));
      return () => electron.ipcRenderer.removeAllListeners("connection:stateChange");
    }
  },
  // ── PacificDB Protocol Requests ───────────────────────────────────────────
  // Every request goes through main process TCP socket — never direct from renderer.
  request: (profileId, command) => electron.ipcRenderer.invoke("pacific:request", profileId, command),
  // ── Batch / Streaming operations ─────────────────────────────────────────
  insertMany: (profileId, collection, docs) => electron.ipcRenderer.invoke("pacific:insertMany", profileId, collection, docs),
  // ── Media operations (chunked, main-process only) ─────────────────────────
  media: {
    uploadFile: (profileId, params) => electron.ipcRenderer.invoke("media:uploadFile", profileId, params),
    downloadFile: (profileId, mediaId, destination) => electron.ipcRenderer.invoke("media:downloadFile", profileId, mediaId, destination),
    onProgress: (callback) => {
      electron.ipcRenderer.on("media:progress", (_ev, data) => callback(data));
      return () => electron.ipcRenderer.removeAllListeners("media:progress");
    }
  },
  // ── Backup export (streaming) ────────────────────────────────────────────
  backup: {
    exportToFile: (profileId, backupId, destination) => electron.ipcRenderer.invoke("backup:exportToFile", profileId, backupId, destination),
    onProgress: (callback) => {
      electron.ipcRenderer.on("backup:progress", (_ev, data) => callback(data));
      return () => electron.ipcRenderer.removeAllListeners("backup:progress");
    }
  },
  // ── Query History ─────────────────────────────────────────────────────────
  history: {
    list: (profileId) => electron.ipcRenderer.invoke("history:list", profileId),
    add: (profileId, entry) => electron.ipcRenderer.invoke("history:add", profileId, entry),
    delete: (profileId, id) => electron.ipcRenderer.invoke("history:delete", profileId, id),
    clear: (profileId) => electron.ipcRenderer.invoke("history:clear", profileId)
  },
  // ── Saved Queries ─────────────────────────────────────────────────────────
  queries: {
    list: () => electron.ipcRenderer.invoke("queries:list"),
    save: (query) => electron.ipcRenderer.invoke("queries:save", query),
    update: (id, updates) => electron.ipcRenderer.invoke("queries:update", id, updates),
    delete: (id) => electron.ipcRenderer.invoke("queries:delete", id)
  },
  // ── Settings ──────────────────────────────────────────────────────────────
  settings: {
    get: () => electron.ipcRenderer.invoke("settings:get"),
    set: (settings) => electron.ipcRenderer.invoke("settings:set", settings)
  },
  // ── File dialogs (desktop-only capability) ────────────────────────────────
  dialog: {
    openFile: (options) => electron.ipcRenderer.invoke("dialog:openFile", options),
    saveFile: (options) => electron.ipcRenderer.invoke("dialog:saveFile", options),
    openFolder: (options) => electron.ipcRenderer.invoke("dialog:openFolder", options)
  },
  // ── NLQ Compiler (runs in main, offline/deterministic) ───────────────────
  nlq: {
    compile: (question, options) => electron.ipcRenderer.invoke("nlq:compile", question, options)
  },
  // ── App info ──────────────────────────────────────────────────────────────
  app: {
    getVersion: () => electron.ipcRenderer.invoke("app:getVersion"),
    getPlatform: () => process.platform
  }
};
electron.contextBridge.exposeInMainWorld("pacific", pacificAPI);
