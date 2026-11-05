/*
 * Slow Sippin — macOS app shell (Electron main process).
 *
 * The instrument itself is the HTML/JS in the project root. It is served
 * from a private app:// scheme (so Web Workers, fonts and storage behave
 * like a normal secure origin), and given native Open / Save dialogs and a
 * Mac menu through a small, isolated preload bridge.
 */
'use strict';

const { app, BrowserWindow, Menu, dialog, ipcMain, protocol, net, shell } = require('electron');
const path = require('path');
const fs = require('fs');
const { pathToFileURL } = require('url');

const ROOT = path.join(__dirname, '..');
const SCHEME = 'app';
const HOST = 'slow-sippin';
const AUDIO_EXT = ['wav', 'wave', 'aif', 'aiff', 'aifc', 'mp3', 'flac'];

protocol.registerSchemesAsPrivileged([{
  scheme: SCHEME,
  privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true },
}]);

let win = null;
let pendingOpen = null;    // file opened from Finder before the window exists

function serveApp() {
  protocol.handle(SCHEME, (req) => {
    const url = new URL(req.url);
    let rel = decodeURIComponent(url.pathname);
    if (rel === '/' || rel === '') rel = '/index.html';
    const file = path.normalize(path.join(ROOT, rel));
    // never serve anything outside the app folder
    if (file !== ROOT && !file.startsWith(ROOT + path.sep)) {
      return new Response('Not found', { status: 404 });
    }
    return net.fetch(pathToFileURL(file).toString());
  });
}

function createWindow() {
  win = new BrowserWindow({
    width: 1480,
    height: 1000,
    minWidth: 900,
    minHeight: 640,
    title: 'Slow Sippin',
    backgroundColor: '#121211',
    show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      spellcheck: false,
      backgroundThrottling: false,   // keep playback/rendering smooth when in the background
    },
  });
  win.once('ready-to-show', () => win.show());
  win.loadURL(`${SCHEME}://${HOST}/index.html`);

  // Stay on the instrument: no navigation, no popups.
  win.webContents.on('will-navigate', (e) => e.preventDefault());
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:/.test(url)) shell.openExternal(url);
    return { action: 'deny' };
  });
  win.webContents.on('did-finish-load', () => {
    if (pendingOpen) { sendFile(pendingOpen); pendingOpen = null; }
  });
  win.on('closed', () => { win = null; });
}

function readSample(filePath) {
  const data = fs.readFileSync(filePath);
  return { name: path.basename(filePath), path: filePath, data: new Uint8Array(data.buffer, data.byteOffset, data.byteLength) };
}

function sendFile(filePath) {
  if (!win) { pendingOpen = filePath; return; }
  try {
    win.webContents.send('open-sample', readSample(filePath));
    app.addRecentDocument(filePath);
  } catch (e) {
    dialog.showErrorBox('Could not open file', String(e.message || e));
  }
}

async function showOpenDialog() {
  const res = await dialog.showOpenDialog(win, {
    title: 'Load sample',
    properties: ['openFile'],
    filters: [
      { name: 'Audio', extensions: AUDIO_EXT },
      { name: 'All files', extensions: ['*'] },
    ],
  });
  if (res.canceled || !res.filePaths.length) return null;
  app.addRecentDocument(res.filePaths[0]);
  return readSample(res.filePaths[0]);
}

// ------------------------------------------------------------------ IPC --

ipcMain.handle('open-sample', () => showOpenDialog());

ipcMain.handle('save-file', async (_e, { name, data }) => {
  const ext = path.extname(name).slice(1) || 'wav';
  const res = await dialog.showSaveDialog(win, {
    title: 'Export rendered audio',
    defaultPath: path.join(app.getPath('music'), name),
    filters: [{ name: ext.toUpperCase(), extensions: [ext] }],
  });
  if (res.canceled || !res.filePath) return null;
  await fs.promises.writeFile(res.filePath, Buffer.from(data.buffer, data.byteOffset, data.byteLength));
  return res.filePath;
});

ipcMain.handle('reveal-file', (_e, p) => { if (typeof p === 'string') shell.showItemInFolder(p); });

// ----------------------------------------------------------------- menu --

function command(cmd) {
  if (win) win.webContents.send('menu', cmd);
}

function buildMenu() {
  const template = [
    {
      label: app.name,
      submenu: [
        { role: 'about' },
        { type: 'separator' },
        { role: 'services' },
        { type: 'separator' },
        { role: 'hide' },
        { role: 'hideOthers' },
        { role: 'unhide' },
        { type: 'separator' },
        { role: 'quit' },
      ],
    },
    {
      label: 'File',
      submenu: [
        { label: 'Load Sample…', accelerator: 'CmdOrCtrl+O', click: async () => { const s = await showOpenDialog(); if (s && win) win.webContents.send('open-sample', s); } },
        { role: 'recentDocuments', submenu: [{ role: 'clearRecentDocuments' }] },
        { type: 'separator' },
        { label: 'Export…', accelerator: 'CmdOrCtrl+E', click: () => command('export') },
        { type: 'separator' },
        { role: 'close' },
      ],
    },
    { role: 'editMenu' },
    {
      label: 'Sound',
      submenu: [
        { label: 'Render / Cancel', accelerator: 'CmdOrCtrl+R', click: () => command('render') },
        { label: 'Play / Stop', accelerator: 'CmdOrCtrl+P', click: () => command('play') },
        { label: 'Audition', accelerator: 'CmdOrCtrl+L', click: () => command('audition') },
        { type: 'separator' },
        { label: 'Clear Selection', accelerator: 'CmdOrCtrl+D', click: () => command('clear-region') },
      ],
    },
    {
      label: 'View',
      submenu: [
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'togglefullscreen' },
        ...(app.isPackaged ? [] : [{ type: 'separator' }, { role: 'reload' }, { role: 'toggleDevTools' }]),
      ],
    },
    { role: 'windowMenu' },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

// ------------------------------------------------------------ lifecycle --

app.setName('Slow Sippin');
app.setAboutPanelOptions({
  applicationName: 'Slow Sippin',
  applicationVersion: app.getVersion(),
  credits: 'An extreme time-stretch instrument for textures, pads and drones.',
});

// Double-click / drag onto the Dock icon / "Open With…" in Finder.
app.on('open-file', (e, filePath) => {
  e.preventDefault();
  if (app.isReady() && win) { sendFile(filePath); if (win.isMinimized()) win.restore(); win.focus(); }
  else pendingOpen = filePath;
  if (app.isReady() && !win) createWindow();
});

app.whenReady().then(() => {
  serveApp();
  buildMenu();
  createWindow();
  app.on('activate', () => { if (!win) createWindow(); });
});

app.on('window-all-closed', () => app.quit());
