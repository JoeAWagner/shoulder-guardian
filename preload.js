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
  onConnected:     (cb) => ipcRenderer.on('connected',              (_, p) => cb(p)),
  onReconnecting:  (cb) => ipcRenderer.on('reconnecting',           (_, p) => cb(p)),
  onDisconnect:    (cb) => ipcRenderer.on('disconnected',           ()     => cb()),
  onTriggered:     (cb) => ipcRenderer.on('triggered',              (_, t) => cb(t)),
  onLocked:        (cb) => ipcRenderer.on('locked',                 ()     => cb()),
  onTrayToggle:    (cb) => ipcRenderer.on('tray-protection-change', (_, v) => cb(v)),
  onWeather:       (cb) => ipcRenderer.on('weather-update',         (_, d) => cb(d)),
  onEvent:         (cb) => ipcRenderer.on('event-added',            (_, e) => cb(e)),

  // App settings
  setTriggerAction:    (action) => ipcRenderer.invoke('set-trigger-action', action),
  setThreatThreshold:  (n)      => ipcRenderer.invoke('set-threat-threshold', n),
  setCloseToTray:      (v)      => ipcRenderer.invoke('set-close-to-tray', v),
  setAlwaysOnTop:      (v)      => ipcRenderer.invoke('set-always-on-top', v),
  setStartOnLogin:     (v)      => ipcRenderer.invoke('set-start-on-login', v),
  getStartOnLogin:     ()       => ipcRenderer.invoke('get-start-on-login'),
  getPrefs:            ()       => ipcRenderer.invoke('get-prefs'),
  setMiniMode:         (v)      => ipcRenderer.invoke('set-mini-mode', v),
  openLogFile:         ()       => ipcRenderer.invoke('open-log-file'),
  getLogPath:          ()       => ipcRenderer.invoke('get-log-path'),

  // Auto-update
  checkForUpdate:   ()   => ipcRenderer.invoke('check-for-update'),
  installUpdate:    ()   => ipcRenderer.invoke('install-update'),
  onUpdateStatus:   (cb) => ipcRenderer.on('update-status', (_, d) => cb(d)),

  // Weather
  refreshWeather:   ()    => ipcRenderer.invoke('refresh-weather'),
  setWeatherZip:    (zip) => ipcRenderer.invoke('set-weather-zip', zip),

  // Snooze + approach filter
  setSnoozeDur:      (sec) => ipcRenderer.invoke('set-snooze-dur', sec),
  setApproachFilter: (v)   => ipcRenderer.invoke('set-approach-filter', v),

  // Event history
  getEvents:   () => ipcRenderer.invoke('get-events'),
  clearEvents: () => ipcRenderer.invoke('clear-events'),

  // Window controls
  windowMinimize: () => ipcRenderer.invoke('window-minimize'),
  windowClose:    () => ipcRenderer.invoke('window-close'),
});
