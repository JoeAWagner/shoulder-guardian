const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('arduino', {
  // Serial
  listPorts:   ()      => ipcRenderer.invoke('list-ports'),
  connect:     (path)  => ipcRenderer.invoke('connect', path),
  disconnect:  ()      => ipcRenderer.invoke('disconnect'),
  sendCommand: (cmd)   => ipcRenderer.invoke('send-command', cmd),

  // Events from main process
  onStatus:        (cb) => ipcRenderer.on('status-update',          (_, d) => cb(d)),
  onLog:           (cb) => ipcRenderer.on('log',                    (_, m) => cb(m)),
  onDisconnect:    (cb) => ipcRenderer.on('disconnected',           ()     => cb()),
  onTriggered:     (cb) => ipcRenderer.on('triggered',              (_, t) => cb(t)),
  onLocked:        (cb) => ipcRenderer.on('locked',                 ()     => cb()),
  onTrayToggle:    (cb) => ipcRenderer.on('tray-protection-change', (_, v) => cb(v)),

  // App settings
  setTriggerAction:    (action) => ipcRenderer.invoke('set-trigger-action', action),
  setThreatThreshold:  (n)      => ipcRenderer.invoke('set-threat-threshold', n),
  setCloseToTray:      (v)      => ipcRenderer.invoke('set-close-to-tray', v),
  setAlwaysOnTop:      (v)      => ipcRenderer.invoke('set-always-on-top', v),
  setStartOnLogin:     (v)      => ipcRenderer.invoke('set-start-on-login', v),
  getStartOnLogin:     ()       => ipcRenderer.invoke('get-start-on-login'),
  getPrefs:            ()       => ipcRenderer.invoke('get-prefs'),
  openLogFile:         ()       => ipcRenderer.invoke('open-log-file'),
  getLogPath:          ()       => ipcRenderer.invoke('get-log-path'),

  // Window controls
  windowMinimize: () => ipcRenderer.invoke('window-minimize'),
  windowClose:    () => ipcRenderer.invoke('window-close'),
});
