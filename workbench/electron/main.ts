import { app, BrowserWindow, ipcMain, shell, dialog, safeStorage } from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import { ConnectionManager } from './connection-manager';
import { ProfilesStore } from './storage/profiles';
import { QueryHistoryStore } from './storage/query-history';
import { SavedQueriesStore } from './storage/saved-queries';
import { registerProtocolHandlers } from './ipc-handlers';

// ─── App directories ──────────────────────────────────────────────────────────
const APP_DATA_DIR = path.join(app.getPath('userData'), 'pacificdb-workbench');
const CONNECTIONS_FILE = path.join(APP_DATA_DIR, 'connections.json');
const HISTORY_FILE = path.join(APP_DATA_DIR, 'query-history.json');
const QUERIES_FILE = path.join(APP_DATA_DIR, 'saved-queries.json');
const SETTINGS_FILE = path.join(APP_DATA_DIR, 'settings.json');

// Ensure data directory exists before anything uses it
if (!fs.existsSync(APP_DATA_DIR)) {
  fs.mkdirSync(APP_DATA_DIR, { recursive: true, mode: 0o700 });
}

// ─── Singleton services (all run in main process) ─────────────────────────────
export const profilesStore = new ProfilesStore(CONNECTIONS_FILE, safeStorage);
export const connectionManager = new ConnectionManager();
export const queryHistoryStore = new QueryHistoryStore(HISTORY_FILE);
export const savedQueriesStore = new SavedQueriesStore(QUERIES_FILE);

// ─── Window ───────────────────────────────────────────────────────────────────
let mainWindow: BrowserWindow | null = null;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 1100,
    minHeight: 680,
    backgroundColor: '#0d0f12',
    titleBarStyle: process.platform === 'darwin' ? 'hiddenInset' : 'default',
    frame: process.platform !== 'darwin',
    show: false,
    icon: path.join(__dirname, '../assets/icon.png'),
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
      webSecurity: true,
    },
  });

  // Load app
  if (process.env.NODE_ENV === 'development') {
    mainWindow.loadURL('http://localhost:5173');
    mainWindow.webContents.openDevTools({ mode: 'detach' });
  } else {
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'));
  }

  mainWindow.once('ready-to-show', () => {
    mainWindow!.show();
    mainWindow!.focus();
  });

  // External links open in browser, not in the app
  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url);
    return { action: 'deny' };
  });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

// ─── Register IPC handlers ────────────────────────────────────────────────────
registerProtocolHandlers({
  profilesStore,
  connectionManager,
  queryHistoryStore,
  savedQueriesStore,
  dialog,
  APP_DATA_DIR,
  SETTINGS_FILE,
  safeStorage,
});

// ─── App lifecycle ────────────────────────────────────────────────────────────
app.whenReady().then(() => {
  createWindow();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  // Disconnect all active PacificDB connections gracefully on exit
  connectionManager.disconnectAll().finally(() => {
    if (process.platform !== 'darwin') app.quit();
  });
});

app.on('before-quit', () => {
  // Best-effort disconnect
  connectionManager.disconnectAll().catch(() => {});
});

// Security: prevent new webview windows
app.on('web-contents-created', (_event, contents) => {
  contents.on('will-navigate', (event, url) => {
    if (!url.startsWith('http://localhost:5173') && !url.startsWith('file://')) {
      event.preventDefault();
    }
  });
});
