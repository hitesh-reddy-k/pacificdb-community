import { app, BrowserWindow, Menu, dialog, protocol, session, shell, clipboard } from 'electron';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createWorkbenchServer } from '../cli/src/workbench.js';
import { PacificDBClient } from '@pacificdb/client';
import { startDesktopEngine } from './engine.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
app.setName('PacificDB Workbench');
if (process.env.PACIFICDB_WORKBENCH_DATA) app.setPath('userData', path.resolve(process.env.PACIFICDB_WORKBENCH_DATA));
protocol.registerSchemesAsPrivileged([{ scheme: 'pacificdb', privileges: {
  standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true,
} }]);
let window, engine, engineStarting, workbench, closing = false, cleanedUp = false;
const startup = new AbortController();
const dataDirectory = app.getPath('userData');
const windowStateFile = path.join(dataDirectory, 'window.json');
const executable = app.isPackaged ? path.join(process.resourcesPath, 'engine',
  process.platform === 'win32' ? 'db_engine.exe' : 'db_engine') :
  path.resolve(process.env.PACIFICDB_WORKBENCH_ENGINE || path.join(here, '..', 'build',
    process.platform === 'win32' ? 'db_engine.exe' : 'db_engine'));
const cliExecutable = path.join(path.dirname(executable), process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb');

async function shutdown() {
  if (closing) return;
  closing = true;
  startup.abort();
  await engineStarting?.catch(() => {});
  if (workbench) {
    workbench.server.closeAllConnections();
    await workbench.close().catch(() => {});
  }
  await engine?.stop();
  cleanedUp = true;
  app.quit();
}
function showWindow() {
  if (!window || window.isDestroyed()) return;
  if (window.isMinimized()) window.restore();
  window.show();
  window.focus();
}
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', showWindow);
  app.on('activate', showWindow);
  app.on('before-quit', (event) => {
    if (!cleanedUp) { event.preventDefault(); void shutdown(); }
  });
  app.on('window-all-closed', () => app.quit());
  app.whenReady().then(async () => {
    await mkdir(dataDirectory, { recursive: true });
    const saved = await readFile(windowStateFile, 'utf8').then(JSON.parse).catch(() => ({}));
    const size = (number, min, max, fallback) => Number.isInteger(number) && number >= min && number <= max ? number : fallback;
    window = new BrowserWindow({
      title: 'PacificDB Workbench', width: size(saved.width, 900, 2400, 1360),
      height: size(saved.height, 650, 1800, 900), minWidth: 900, minHeight: 650,
      show: false, backgroundColor: '#081425',
      icon: path.join(here, 'icon.png'),
      webPreferences: { contextIsolation: true, nodeIntegration: false,
        sandbox: true, webSecurity: true, spellcheck: false },
    });
    window.once('ready-to-show', showWindow);
    window.on('close', () => {
      const { width, height } = window.getNormalBounds();
      void writeFile(windowStateFile, JSON.stringify({ width, height })).catch(() => {});
    });
    window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
    window.webContents.on('will-navigate', (event, url) => {
      if (url !== 'pacificdb://workbench/') event.preventDefault();
    });
    window.webContents.on('will-attach-webview', (event) => event.preventDefault());
    session.defaultSession.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
    session.defaultSession.setPermissionCheckHandler(() => false);
    session.defaultSession.on('will-download', (_event, item) => {
      item.setSaveDialogOptions({ title: 'Save file from PacificDB',
        defaultPath: path.join(app.getPath('downloads'), path.basename(item.getFilename())) });
    });
    const menu = [
      ...(process.platform === 'darwin' ? [{ role: 'appMenu' }] : []),
      { label: 'File', submenu: [{ label: 'Open data folder', click: () => shell.openPath(dataDirectory) },
        { id: 'copy-cli', label: 'Copy CLI connection command', click: async () => {
          if (!engine || closing) return;
          const quoted = process.platform === 'win32' ? `& '${cliExecutable.replaceAll("'", "''")}'` :
            `'${cliExecutable.replaceAll("'", "'\\''")}'`;
          await clipboard.writeText(`${quoted} --host 127.0.0.1 --port ${engine.port} --no-start`)
            .catch((error) => dialog.showErrorBox('Could not copy the command', error.message));
        } },
        { type: 'separator' }, { role: 'quit' }] },
      { role: 'editMenu' },
      { label: 'View', submenu: [{ role: 'reload' }, { type: 'separator' },
        { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' },
        { type: 'separator' }, { role: 'togglefullscreen' },
        ...(!app.isPackaged ? [{ role: 'toggleDevTools' }] : [])] },
      { role: 'help', submenu: [{ label: 'About PacificDB Workbench', click: () => dialog.showMessageBox(window, {
        type: 'info', title: 'PacificDB Workbench', message: `PacificDB Workbench ${app.isPackaged ? app.getVersion() : 'development'}`,
        detail: 'Local desktop database workspace.\n\nDocuments, vectors, and media on your machine.\nEngine: AGPL-3.0 · CLI and SDK: Apache-2.0',
      }) }] },
    ];
    Menu.setApplicationMenu(Menu.buildFromTemplate(menu));
    await window.loadFile(path.join(here, 'loading.html'));
    engineStarting = startDesktopEngine({ executable, directory: path.join(dataDirectory, 'database'), signal: startup.signal });
    engine = await engineStarting;
    if (closing) { await engine.stop(); return; }
    workbench = await createWorkbenchServer({ clientFactory: () => new PacificDBClient({
      host: '127.0.0.1', port: engine.port, poolSize: 1,
    }), connectionInfo: { host: '127.0.0.1', port: engine.port, tls: false,
      source: 'desktop', cliPath: cliExecutable, platform: process.platform } });
    // A stable application origin preserves display preferences across launches.
    protocol.handle('pacificdb', async (request) => {
      const url = new URL(request.url);
      if (url.hostname !== 'workbench' || !['GET', 'POST'].includes(request.method)) return new Response('Not found', { status: 404 });
      const target = new URL(workbench.url);
      target.pathname = url.pathname;
      target.search = url.search;
      const headers = new Headers(request.headers);
      headers.delete('host');
      headers.delete('content-length');
      if (headers.has('origin')) headers.set('origin', new URL(workbench.url).origin);
      try {
        return await fetch(target, { method: request.method, headers,
          body: request.method === 'POST' ? request.body : undefined,
          ...(request.method === 'POST' ? { duplex: 'half' } : {}), signal: request.signal });
      } catch { return new Response('The local workspace is unavailable.', { status: 503 }); }
    });
    await window.loadURL('pacificdb://workbench/');
    void engine.exited.then(() => {
      if (!closing) {
        dialog.showErrorBox('PacificDB engine stopped', `Reopen Workbench to reconnect. Your data is stored at:\n${dataDirectory}`);
        app.quit();
      }
    });
  }).catch((error) => {
    if (!closing) dialog.showErrorBox('PacificDB Workbench could not start', error.message);
    app.quit();
  });
}
