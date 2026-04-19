const { app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, shell } = require('electron');
const path   = require('path');
const fs     = require('fs');
const { exec } = require('child_process');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const { autoUpdater } = require('electron-updater');

let mainWindow;
let tray;
let port         = null;
let parser       = null;
let protectionOn = true;
let isConnected  = false;
let flashTimer    = null;
let updateReady   = false;   // true once update-downloaded fires

// ── App-side action state (cooldown + lock timer) ────────────
let lastTriggerTime = 0;
let cooldownMs      = 5000;   // synced from Arduino STATUS
let emptyStart      = null;
let lockFired       = false;
let lockDelayMs     = 30000;  // synced from Arduino STATUS
let lockEnabled     = false;  // synced from Arduino STATUS
let triggerAction   = 'minimize';  // 'minimize' | 'lock'
let closeToTray     = true;        // false = quit on window close
let alwaysOnTop     = false;       // window floats above all others
let miniMode        = false;       // compact radar-only view
let prefsPath       = null;        // set in loadPrefs()
let threatFrames    = 0;      // consecutive frames with count >= 2
let threatThreshold = 4;      // frames needed before triggering (user-configurable)
let lastTargetCount = -1;     // tracks count changes for target appear/disappear logging

// ── Settings persistence ──────────────────────────────────────
function loadPrefs() {
  try {
    const dir = app.getPath('userData');
    prefsPath = path.join(dir, 'prefs.json');
    if (fs.existsSync(prefsPath)) {
      const data = JSON.parse(fs.readFileSync(prefsPath, 'utf8'));
      if (typeof data.alwaysOnTop     === 'boolean') alwaysOnTop     = data.alwaysOnTop;
      if (typeof data.closeToTray     === 'boolean') closeToTray     = data.closeToTray;
      if (typeof data.miniMode        === 'boolean') miniMode        = data.miniMode;
      if (data.triggerAction === 'minimize' || data.triggerAction === 'lock')
        triggerAction = data.triggerAction;
      if (typeof data.threatThreshold === 'number')
        threatThreshold = Math.max(1, Math.min(5, data.threatThreshold));
    }
  } catch (_) {}
}

function savePrefs() {
  try {
    if (!prefsPath) return;
    fs.writeFileSync(prefsPath, JSON.stringify(
      { alwaysOnTop, closeToTray, miniMode, triggerAction, threatThreshold }, null, 2
    ), 'utf8');
  } catch (_) {}
}

// ── File logger ───────────────────────────────────────────────
let logFilePath = null;

function initLogger() {
  const dir = app.getPath('userData');
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  logFilePath = path.join(dir, 'shoulder-guardian.log');
  logToFile(`\n──────────────────────────────────────────`);
  logToFile(`[${ts()}] App started (v${app.getVersion()})`);
}

function logToFile(msg) {
  if (!logFilePath) return;
  try {
    // Rotate at 500 KB
    if (fs.existsSync(logFilePath) && fs.statSync(logFilePath).size > 512 * 1024) {
      const oldPath = logFilePath.replace('.log', '.old.log');
      if (fs.existsSync(oldPath)) fs.unlinkSync(oldPath);
      fs.renameSync(logFilePath, oldPath);
    }
    // Prefix with full date for file — UI already shows time-only via ts()
    const now    = new Date();
    const date   = now.toISOString().slice(0, 10);               // 2026-04-09
    const time   = now.toTimeString().slice(0, 8);               // 14:32:11
    const prefix = `[${date} ${time}]`;
    // Replace leading [HH:MM:SS AM/PM] or [H:MM:SS AM/PM] with full prefix
    const line   = msg.replace(/^\[\d{1,2}:\d{2}:\d{2}.*?\]/, prefix);
    fs.appendFileSync(logFilePath, line + '\n', 'utf8');
  } catch (_) {}
}

// ── Auto-updater ──────────────────────────────────────────────
function initUpdater() {
  if (!app.isPackaged) return;   // skip in dev / npm start

  autoUpdater.autoDownload         = true;
  autoUpdater.autoInstallOnAppQuit = true;
  autoUpdater.allowDowngrade       = false;

  // Pipe electron-updater's internal log to our log file so we can diagnose issues
  autoUpdater.logger = {
    info:  (m) => logToFile(`[updater] ${typeof m === 'object' ? JSON.stringify(m) : m}`),
    warn:  (m) => logToFile(`[updater WARN] ${typeof m === 'object' ? JSON.stringify(m) : m}`),
    error: (m) => logToFile(`[updater ERROR] ${typeof m === 'object' ? JSON.stringify(m) : m}`),
    debug: (_) => {},   // too noisy — enable if needed
  };

  function sendStatus(data) {
    mainWindow?.webContents.send('update-status', data);
  }
  function sendLog(msg) {
    logToFile(msg);
    mainWindow?.webContents.send('log', msg);
  }

  autoUpdater.on('checking-for-update', () => {
    logToFile(`[${ts()}] Checking for updates…`);
  });

  autoUpdater.on('update-available', (info) => {
    sendLog(`[${ts()}] Update available: v${info.version} — downloading…`);
    sendStatus({ type: 'downloading', version: info.version });
    rebuildTrayMenu();
  });

  autoUpdater.on('update-not-available', () => {
    logToFile(`[${ts()}] App is up to date (v${app.getVersion()})`);
    sendStatus({ type: 'none' });
  });

  autoUpdater.on('download-progress', (p) => {
    sendStatus({ type: 'progress', version: '', percent: Math.round(p.percent) });
  });

  autoUpdater.on('update-downloaded', (info) => {
    updateReady = true;
    sendLog(`[${ts()}] Update v${info.version} ready — restart to install`);
    sendStatus({ type: 'ready', version: info.version });
    rebuildTrayMenu();
  });

  autoUpdater.on('error', (err) => {
    const msg = `[${ts()}] Update error: ${err.message}`;
    sendLog(msg);
    sendStatus({ type: 'error', message: err.message });
  });

  // Check 30 s after launch so startup is never delayed
  setTimeout(() => autoUpdater.checkForUpdates().catch((err) => {
    logToFile(`[${ts()}] Update check failed: ${err.message}`);
  }), 30000);
}

// ── Tray icon (programmatic BGRA circle) ─────────────────────
function makeCircleBuf(hex, size) {
  const buf = Buffer.alloc(size * size * 4, 0);
  const rr = parseInt(hex.slice(1,3), 16);
  const gg = parseInt(hex.slice(3,5), 16);
  const bb = parseInt(hex.slice(5,7), 16);
  const cx = (size-1)/2, cy = (size-1)/2, rad = size/2 - 1.5;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      if (Math.sqrt((x-cx)**2 + (y-cy)**2) <= rad) {
        const i = (y*size+x)*4;
        buf[i]=bb; buf[i+1]=gg; buf[i+2]=rr; buf[i+3]=255;
      }
    }
  }
  return buf;
}

function makeTrayIcon(hex = '#58a6ff') {
  const img = nativeImage.createEmpty();
  img.addRepresentation({ width:16, height:16, scaleFactor:1.0, buffer: makeCircleBuf(hex,16) });
  img.addRepresentation({ width:32, height:32, scaleFactor:2.0, buffer: makeCircleBuf(hex,32) });
  return img;
}

function flashTrayRed() {
  tray.setImage(makeTrayIcon('#f85149'));
  if (flashTimer) clearTimeout(flashTimer);
  flashTimer = setTimeout(() => {
    tray.setImage(makeTrayIcon(protectionOn && isConnected ? '#58a6ff' : '#6e7681'));
    flashTimer = null;
  }, 2500);
}

// ── Window ───────────────────────────────────────────────────
function createWindow() {
  mainWindow = new BrowserWindow({
    width: 500, height: 820,
    minWidth: 500, minHeight: 600,
    resizable: true,
    frame: false,
    show: false,
    backgroundColor: '#0d1117',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  mainWindow.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  if (alwaysOnTop) mainWindow.setAlwaysOnTop(true);
  if (miniMode) {
    mainWindow.setSize(444, 267);
    mainWindow.setResizable(false);
  }
  mainWindow.once('ready-to-show', () => mainWindow.show());
  mainWindow.on('close', (e) => {
    if (!app.isQuitting) {
      if (closeToTray) {
        e.preventDefault();
        mainWindow.hide();
        if (process.platform === 'darwin') app.dock.hide();
        rebuildTrayMenu();
      } else {
        app.isQuitting = true;
        app.quit();
      }
    }
  });
}

// ── Tray ─────────────────────────────────────────────────────
function createTray() {
  tray = new Tray(makeTrayIcon('#6e7681'));
  tray.setToolTip('Shoulder Guardian');
  rebuildTrayMenu();
  tray.on('click',        () => toggleWindow());
  tray.on('double-click', () => toggleWindow());
}

function toggleWindow() {
  if (mainWindow.isVisible() && mainWindow.isFocused()) {
    mainWindow.hide();
    if (process.platform === 'darwin') app.dock.hide();
  } else {
    mainWindow.show(); mainWindow.focus();
    if (process.platform === 'darwin') app.dock.show();
  }
  rebuildTrayMenu();
}

function rebuildTrayMenu() {
  const updateItems = updateReady
    ? [
        { type: 'separator' },
        { label: '⬆ Restart to Install Update', click: () => autoUpdater.quitAndInstall() },
      ]
    : app.isPackaged
      ? [
          { type: 'separator' },
          { label: 'Check for Updates', click: () => autoUpdater.checkForUpdates().catch(() => {}) },
        ]
      : [];

  tray.setContextMenu(Menu.buildFromTemplate([
    { label: mainWindow?.isVisible() ? 'Hide Window' : 'Show Window', click: () => toggleWindow() },
    { type: 'separator' },
    {
      label: 'Protection Enabled', type: 'checkbox', checked: protectionOn,
      click: (item) => {
        protectionOn = item.checked;
        if (port?.isOpen) port.write((protectionOn ? 'ENABLE' : 'DISABLE') + '\n');
        tray.setImage(makeTrayIcon(protectionOn && isConnected ? '#58a6ff' : '#6e7681'));
        mainWindow?.webContents.send('tray-protection-change', protectionOn);
        rebuildTrayMenu();
      },
    },
    { type: 'separator' },
    { label: `Status: ${isConnected ? 'Connected' : 'Disconnected'}`, enabled: false },
    { type: 'separator' },
    { label: 'Open Log File', click: () => shell.openPath(logFilePath || '') },
    ...updateItems,
    { type: 'separator' },
    { label: 'Quit', click: () => { app.isQuitting = true; app.quit(); } },
  ]));
}

// ── App lifecycle ─────────────────────────────────────────────
app.whenReady().then(() => { loadPrefs(); initLogger(); createWindow(); createTray(); initUpdater(); });
app.on('window-all-closed', () => {});
app.on('before-quit', () => {
  app.isQuitting = true;
  logToFile(`[${ts()}] App quit`);
});
app.on('activate', () => {
  if (!mainWindow.isVisible()) {
    mainWindow.show();
    if (process.platform === 'darwin') app.dock.show();
  }
});

// ── IPC: Window controls ──────────────────────────────────────
ipcMain.handle('set-trigger-action',   (_, action) => { triggerAction   = action; savePrefs(); });
ipcMain.handle('set-threat-threshold', (_, n)      => { threatThreshold = Math.max(1, Math.min(5, n)); savePrefs(); });
ipcMain.handle('set-close-to-tray',    (_, v)      => { closeToTray = Boolean(v); savePrefs(); });
ipcMain.handle('set-always-on-top',    (_, v)      => {
  alwaysOnTop = Boolean(v);
  mainWindow.setAlwaysOnTop(alwaysOnTop);
  savePrefs();
});
ipcMain.handle('set-start-on-login',   (_, v)      => {
  app.setLoginItemSettings({ openAtLogin: Boolean(v) });
});
ipcMain.handle('get-start-on-login',   ()          => {
  return app.getLoginItemSettings().openAtLogin;
});
ipcMain.handle('get-prefs', () => ({ alwaysOnTop, closeToTray, miniMode, triggerAction, threatThreshold }));
ipcMain.handle('set-mini-mode', (_, mini) => {
  miniMode = Boolean(mini);
  if (miniMode) {
    mainWindow.setSize(444, 267);
    mainWindow.setResizable(false);
  } else {
    mainWindow.setSize(500, 820);
    mainWindow.setResizable(true);
  }
  savePrefs();
});
ipcMain.handle('check-for-update', () => {
  if (app.isPackaged) autoUpdater.checkForUpdates().catch(() => {});
});
ipcMain.handle('install-update', () => autoUpdater.quitAndInstall());
ipcMain.handle('window-minimize', () => mainWindow.minimize());
ipcMain.handle('window-close', () => {
  if (closeToTray) {
    mainWindow.hide();
    if (process.platform === 'darwin') app.dock.hide();
    rebuildTrayMenu();
  } else {
    app.isQuitting = true;
    app.quit();
  }
});
ipcMain.handle('open-log-file', () => {
  if (logFilePath) shell.openPath(logFilePath);
});
ipcMain.handle('get-log-path', () => logFilePath || '');

// ── IPC: Serial ports ─────────────────────────────────────────
ipcMain.handle('list-ports', async () => {
  const ports = await SerialPort.list();
  return ports.map(p => p.path);
});

// ── IPC: Connect ──────────────────────────────────────────────
ipcMain.handle('connect', async (event, portPath) => {
  if (port?.isOpen) port.close();
  resetLockState();

  return new Promise((resolve, reject) => {
    port   = new SerialPort({ path: portPath, baudRate: 115200 });
    parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));

    parser.on('data', (line) => {
      line = line.trim();
      if (!line.startsWith('STATUS:')) {
        if (line.startsWith('OK:')) {
          const msg = `[${ts()}] ${line}`;
          mainWindow.webContents.send('log', msg);
          logToFile(msg);
        }
        return;
      }

      const status = parseStatus(line);

      // ── Log new/lost targets ──────────────────────────────
      if (status.count !== lastTargetCount) {
        const diff = status.count - lastTargetCount;
        const msg  = diff > 0
          ? `[${ts()}] Target detected — ${status.count} in view`
          : `[${ts()}] Target lost — ${status.count} in view`;
        mainWindow.webContents.send('log', msg);
        logToFile(msg);
        lastTargetCount = status.count;
      }

      // Sync app-side state from Arduino settings
      cooldownMs  = (status.cool  || 5)  * 1000;
      lockDelayMs = (status.lockdly || 30) * 1000;
      lockEnabled = Boolean(status.locken);

      // Sync protection state — keeps tray & trigger logic in step with the UI toggle
      const newProtection = Boolean(status.enabled);
      if (newProtection !== protectionOn) {
        protectionOn = newProtection;
        tray.setImage(makeTrayIcon(protectionOn && isConnected ? '#58a6ff' : '#6e7681'));
        rebuildTrayMenu();
      }

      // ── Action: Shoulder surfer (2+ targets, debounced) ──────
      if (protectionOn && status.count >= 2) {
        threatFrames++;
        if (threatFrames >= threatThreshold) {
          const now = Date.now();
          if (now - lastTriggerTime >= cooldownMs) {
            lastTriggerTime = now;
            threatFrames    = 0;
            if (triggerAction === 'lock') lockScreen(); else minimizeAll();
            const trigMsg = `[${ts()}] TRIGGERED — ${status.count} targets detected`;
            mainWindow.webContents.send('triggered', status.targets);
            mainWindow.webContents.send('log', trigMsg);
            logToFile(trigMsg);
            flashTrayRed();
          }
        }
      } else {
        threatFrames = 0;
      }

      // ── Action: Lock on empty ─────────────────────────────
      if (protectionOn && lockEnabled) {
        if (status.count === 0) {
          if (emptyStart === null) emptyStart = Date.now();
          const elapsed = Date.now() - emptyStart;
          if (!lockFired && elapsed >= lockDelayMs) {
            lockFired = true;
            lockScreen();
            const lockMsg = `[${ts()}] Screen locked — no presence for ${Math.round(elapsed/1000)}s`;
            mainWindow.webContents.send('locked');
            mainWindow.webContents.send('log', lockMsg);
            logToFile(lockMsg);
          }
        } else {
          emptyStart = null;
          lockFired  = false;
        }
      }

      mainWindow.webContents.send('status-update', status);
    });

    port.on('open', () => {
      isConnected = true;
      tray.setImage(makeTrayIcon(protectionOn ? '#58a6ff' : '#6e7681'));
      rebuildTrayMenu();
      const msg = `[${ts()}] Connected to ${portPath}`;
      logToFile(msg);
      resolve(true);
    });
    port.on('error', (err) => {
      logToFile(`[${ts()}] Serial error: ${err.message}`);
      reject(err.message);
    });
    port.on('close', () => {
      isConnected = false;
      resetLockState();
      tray.setImage(makeTrayIcon('#6e7681'));
      rebuildTrayMenu();
      logToFile(`[${ts()}] Disconnected from ${portPath}`);
      mainWindow.webContents.send('disconnected');
    });
  });
});

// ── IPC: Disconnect ───────────────────────────────────────────
ipcMain.handle('disconnect', () => {
  if (port?.isOpen) port.close();
  port = null;
  resetLockState();
  return true;
});

// ── IPC: Send command ─────────────────────────────────────────
ipcMain.handle('send-command', (event, cmd) => {
  if (port?.isOpen) { port.write(cmd + '\n'); return true; }
  return false;
});

// ── OS actions ────────────────────────────────────────────────
function minimizeAll() {
  if (process.platform === 'darwin') {
    exec(`osascript -e 'tell application "System Events" to set visible of every process whose visible is true to false'`);
  } else if (process.platform === 'win32') {
    const ps = [
      `Add-Type -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);' -Name KB -Namespace N -EA 0`,
      `[N.KB]::keybd_event(0x5B,0,0,[UIntPtr]::Zero)`,
      `[N.KB]::keybd_event(0x44,0,0,[UIntPtr]::Zero)`,
      `Start-Sleep -Milliseconds 50`,
      `[N.KB]::keybd_event(0x44,0,2,[UIntPtr]::Zero)`,
      `[N.KB]::keybd_event(0x5B,0,2,[UIntPtr]::Zero)`,
    ].join(';');
    exec(`powershell -NoProfile -WindowStyle Hidden -EncodedCommand ${Buffer.from(ps, 'utf16le').toString('base64')}`);
  }
}

function lockScreen() {
  if (process.platform === 'darwin') {
    exec('/System/Library/CoreServices/Menu\\ Extras/User.menu/Contents/Resources/CGSession -suspend', (err) => {
      if (err) exec(`osascript -e 'tell application "System Events" to keystroke "q" using {command down, control down}'`);
    });
  } else if (process.platform === 'win32') {
    exec('rundll32.exe user32.dll,LockWorkStation');
  }
}

// ── Helpers ───────────────────────────────────────────────────
function parseStatus(line) {
  const obj = {};
  line.replace('STATUS:', '').split(',').forEach(pair => {
    const [k, v] = pair.split('=');
    if (k) obj[k.trim()] = isNaN(v) ? v : Number(v);
  });

  obj.targets = [];
  for (let i = 0; i < 3; i++) {
    const x = obj[`t${i}x`] || 0;
    const y = obj[`t${i}y`] || 0;
    const s = obj[`t${i}s`] || 0;
    if (x !== 0 || y !== 0) obj.targets.push({ x, y, speed: s });
  }

  return obj;
}

function resetLockState() {
  emptyStart      = null;
  lockFired       = false;
  lastTargetCount = -1;
}

function ts() { return new Date().toLocaleTimeString(); }
