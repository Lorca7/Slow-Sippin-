/* Preload: the only bridge between the page and macOS. */
'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('native', {
  platform: process.platform,
  /** Native "Load sample" dialog → { name, path, data: Uint8Array } or null. */
  openSample: () => ipcRenderer.invoke('open-sample'),
  /** Native save dialog; writes the bytes. Resolves to the path, or null if cancelled. */
  saveFile: (name, data) => ipcRenderer.invoke('save-file', { name, data }),
  reveal: (p) => ipcRenderer.invoke('reveal-file', p),
  /** Files opened from Finder / the File menu. */
  onOpenSample: (cb) => ipcRenderer.on('open-sample', (_e, s) => cb(s)),
  /** Menu commands: 'render' | 'play' | 'audition' | 'export' | 'clear-region'. */
  onMenu: (cb) => ipcRenderer.on('menu', (_e, cmd) => cb(cmd)),
});
