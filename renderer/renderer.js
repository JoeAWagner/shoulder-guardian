// ── Element refs ──────────────────────────────────────────────
const portSelect      = document.getElementById('portSelect');
const refreshBtn      = document.getElementById('refreshBtn');
const connectBtn      = document.getElementById('connectBtn');
const connDot         = document.getElementById('connDot');
const connStatus      = document.getElementById('connStatus');
const radarCard       = document.getElementById('radarCard');
const radarCanvas     = document.getElementById('radarCanvas');
const countBadge      = document.getElementById('countBadge');
const statusBadge     = document.getElementById('statusBadge');
const lockCountdown   = document.getElementById('lockCountdown');
const targetList      = document.getElementById('targetList');
const lockEnToggle    = document.getElementById('lockEnToggle');
const lockDlySlider   = document.getElementById('lockDlySlider');
const lockDlyVal      = document.getElementById('lockDlyVal');
const coolSlider      = document.getElementById('coolSlider');
const coolVal         = document.getElementById('coolVal');
const maxRangeSlider  = document.getElementById('maxRangeSlider');
const maxRangeVal     = document.getElementById('maxRangeVal');
const maxXSlider      = document.getElementById('maxXSlider');
const maxXVal         = document.getElementById('maxXVal');
const applyBtn        = document.getElementById('applyBtn');
const logBox          = document.getElementById('logBox');
const clearLogBtn     = document.getElementById('clearLogBtn');
const openLogBtn      = document.getElementById('openLogBtn');
const enableToggle    = document.getElementById('enableToggle');
const enableText      = document.getElementById('enableText');
const actionMinimize    = document.getElementById('actionMinimize');
const actionLock        = document.getElementById('actionLock');
const exitOnCloseToggle  = document.getElementById('exitOnCloseToggle');
const alwaysOnTopToggle  = document.getElementById('alwaysOnTopToggle');
const startOnLoginToggle = document.getElementById('startOnLoginToggle');
const pollingSlider   = document.getElementById('pollingSlider');
const pollingVal      = document.getElementById('pollingVal');
const smoothSlider    = document.getElementById('smoothSlider');
const smoothVal       = document.getElementById('smoothVal');
const advancedToggle  = document.getElementById('advancedToggle');
const advancedBody    = document.getElementById('advancedBody');
const advancedChevron = document.getElementById('advancedChevron');
const weatherZipInput = document.getElementById('weatherZipInput');
const weatherZipStatus = document.getElementById('weatherZipStatus');
const minimizeBtn     = document.getElementById('minimizeBtn');
const closeBtn        = document.getElementById('closeBtn');
const miniBtn         = document.getElementById('miniBtn');

const ctx = radarCanvas.getContext('2d');

// Radar canvas dimensions — recomputed whenever mini mode toggles.
// Normal mode uses a large canvas to fill the 3-wide window; mini mode
// uses the compact 444-wide size that fits the mini window.
let W, H, SENSOR_X, SENSOR_Y, PLOT_R;

function sizeRadarCanvas(mini) {
  radarCanvas.width  = mini ? 444 : 664;
  radarCanvas.height = mini ? 220 : 260;
  W = radarCanvas.width;
  H = radarCanvas.height;
  SENSOR_X = W / 2;
  SENSOR_Y = H - 8;
  PLOT_R   = Math.min(W / 2 - 4, H - 14);   // max radar radius in pixels
}
sizeRadarCanvas(false);   // normal mode by default

let connected           = false;
let currentMaxRange     = 2000;
let currentMaxX         = 1000;
let currentLockDelay    = 30;
let emptyStartTime      = null;
let settingsInitialized = false;  // sliders only synced on first status after connect

// ── Coordinate smoothing (EMA) ────────────────────────────────
let SMOOTH_ALPHA = 0.35;   // 0=frozen, 1=raw — lower = smoother, set by smoothSlider
let smoothed = [{x:0,y:0},{x:0,y:0},{x:0,y:0}];

function smoothTargets(targets) {
  return targets.map((t, i) => {
    if (!smoothed[i] || (smoothed[i].x === 0 && smoothed[i].y === 0)) {
      smoothed[i] = { x: t.x, y: t.y };   // snap on first appearance
    } else {
      smoothed[i].x = SMOOTH_ALPHA * t.x + (1 - SMOOTH_ALPHA) * smoothed[i].x;
      smoothed[i].y = SMOOTH_ALPHA * t.y + (1 - SMOOTH_ALPHA) * smoothed[i].y;
    }
    return { ...t, x: smoothed[i].x, y: smoothed[i].y };
  });
}

// ── Threat action selector ────────────────────────────────────
[actionMinimize, actionLock].forEach(btn => {
  btn.addEventListener('click', () => {
    actionMinimize.classList.toggle('active', btn === actionMinimize);
    actionLock.classList.toggle('active',     btn === actionLock);
    window.arduino.setTriggerAction(btn === actionLock ? 'lock' : 'minimize');
  });
});

// ── Exit on close toggle ──────────────────────────────────────
exitOnCloseToggle.addEventListener('change', () => {
  window.arduino.setCloseToTray(!exitOnCloseToggle.checked);
});

// ── Always on top toggle ──────────────────────────────────────
alwaysOnTopToggle.addEventListener('change', () => {
  window.arduino.setAlwaysOnTop(alwaysOnTopToggle.checked);
  addLog(`[${ts()}] Always on top ${alwaysOnTopToggle.checked ? 'enabled' : 'disabled'}`, 'ok');
});

// ── Start on login toggle ─────────────────────────────────────
startOnLoginToggle.addEventListener('change', () => {
  window.arduino.setStartOnLogin(startOnLoginToggle.checked);
  addLog(`[${ts()}] Start on login ${startOnLoginToggle.checked ? 'enabled' : 'disabled'}`, 'ok');
});

// Sync start-on-login state from OS on load
window.arduino.getStartOnLogin().then(v => { startOnLoginToggle.checked = v; });

// Restore persisted settings (alwaysOnTop, closeToTray, miniMode, triggerAction, threatThreshold)
window.arduino.getPrefs().then(prefs => {
  alwaysOnTopToggle.checked  = prefs.alwaysOnTop;
  exitOnCloseToggle.checked  = !prefs.closeToTray;
  if (prefs.miniMode) applyMiniMode(true);
  const isLock = prefs.triggerAction === 'lock';
  actionMinimize.classList.toggle('active', !isLock);
  actionLock.classList.toggle('active',      isLock);
  if (prefs.threatThreshold !== undefined) {
    pollingSlider.value    = prefs.threatThreshold;
    pollingVal.textContent = prefs.threatThreshold + ' frames';
  }
  if (typeof prefs.weatherZip === 'string') {
    weatherZipInput.value = prefs.weatherZip;
  }
});

// ── Weather ZIP code ──────────────────────────────────────────
// Strip non-digits as the user types; commit on Enter or blur.
weatherZipInput.addEventListener('input', () => {
  weatherZipInput.value = weatherZipInput.value.replace(/\D/g, '').slice(0, 5);
});

function commitWeatherZip() {
  const zip = weatherZipInput.value.trim();
  if (zip.length !== 0 && zip.length !== 5) {
    weatherZipStatus.textContent = 'need 5 digits';
    return;
  }
  weatherZipStatus.textContent = '';
  window.arduino.setWeatherZip(zip);
  addLog(`[${ts()}] Weather location set to ${zip || 'auto-detect'}`, 'ok');
}

weatherZipInput.addEventListener('blur', commitWeatherZip);
weatherZipInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { weatherZipInput.blur(); }
});

// ── Advanced section toggle ───────────────────────────────────
advancedToggle.addEventListener('click', () => {
  const open = advancedBody.classList.toggle('open');
  advancedChevron.classList.toggle('open', open);
});

// ── Titlebar ──────────────────────────────────────────────────
minimizeBtn.addEventListener('click', () => window.arduino.windowMinimize());
closeBtn.addEventListener('click',    () => window.arduino.windowClose());

// ── Mini mode toggle ──────────────────────────────────────────
function applyMiniMode(mini) {
  document.body.classList.toggle('mini', mini);
  miniBtn.textContent = mini ? '⊞' : '⊡';
  miniBtn.title       = mini ? 'Expand' : 'Mini mode';
  sizeRadarCanvas(mini);
  drawRadar([], currentMaxRange, currentMaxX);
}

miniBtn.addEventListener('click', () => {
  const mini = !document.body.classList.contains('mini');
  applyMiniMode(mini);
  window.arduino.setMiniMode(mini);
});

// ── Ports ─────────────────────────────────────────────────────
async function refreshPorts() {
  const ports = await window.arduino.listPorts();
  portSelect.innerHTML = '<option value="">— Port —</option>';
  ports.forEach(p => {
    const opt = document.createElement('option');
    opt.value = opt.textContent = p;
    portSelect.appendChild(opt);
  });
  if (!ports.length) addLog('No serial ports found.', 'info');
}
refreshBtn.addEventListener('click', refreshPorts);
refreshPorts();

// ── Connect / Disconnect ──────────────────────────────────────
connectBtn.addEventListener('click', async () => {
  if (connected) {
    await window.arduino.disconnect();
    setConnected(false);
    return;
  }
  const path = portSelect.value;
  if (!path) { addLog('Select a port first.', 'info'); return; }
  connectBtn.disabled = true;
  connStatus.textContent = 'Connecting…';
  try {
    await window.arduino.connect(path);
    setConnected(true);
    addLog(`Connected to ${path}`, 'ok');
  } catch (err) {
    connStatus.textContent = 'Failed';
    addLog(`Error: ${err}`, 'trigger');
  } finally {
    connectBtn.disabled = false;
  }
});

function setConnected(state) {
  connected = state;
  connDot.className      = 'badge-dot' + (state ? ' on' : '');
  connStatus.textContent = state ? 'Connected' : 'Disconnected';
  connectBtn.textContent = state ? 'Disconnect' : 'Connect';
  if (!state) {
    settingsInitialized = false;
    drawRadar([], 2000, 1000);
    updateStatusUI(0, false, false);
    lockCountdown.textContent = '';
    emptyStartTime = null;
  }
}

window.arduino.onDisconnect(() => {
  setConnected(false);
  addLog('Arduino disconnected.', 'trigger');
});

// ── Status updates ────────────────────────────────────────────
window.arduino.onStatus((status) => {
  const targets    = status.targets || [];
  const count      = status.count ?? targets.length;
  const maxRange   = status.maxrange ?? 2000;
  const lockDelay  = status.lockdly ?? 30;
  const lockEn     = Boolean(status.locken);

  const maxX       = status.maxx ?? 1000;
  currentMaxRange  = maxRange;
  currentMaxX      = maxX;
  currentLockDelay = lockDelay;

  const displayTargets = smoothTargets(targets);
  if (targets.length === 0) smoothed = [{x:0,y:0},{x:0,y:0},{x:0,y:0}];
  drawRadar(displayTargets, maxRange, maxX);
  updateTargetList(displayTargets);
  updateStatusUI(count, lockEn, false);

  // Lock countdown display
  if (lockEn && count === 0) {
    if (!emptyStartTime) emptyStartTime = Date.now();
    const elapsed   = (Date.now() - emptyStartTime) / 1000;
    const remaining = Math.max(0, lockDelay - elapsed);
    lockCountdown.textContent = remaining > 0 ? `lock in ${remaining.toFixed(0)}s` : '';
  } else {
    emptyStartTime = null;
    lockCountdown.textContent = '';
  }

  // Sync sliders once on first status after connect, then leave them alone
  if (!settingsInitialized) {
    settingsInitialized      = true;
    coolSlider.value         = status.cool;
    coolVal.textContent      = status.cool + ' s';
    lockDlySlider.value      = lockDelay;
    lockDlyVal.textContent   = lockDelay + ' s';
    maxRangeSlider.value     = maxRange;
    maxRangeVal.textContent  = (maxRange / 1000).toFixed(1) + ' m';
    maxXSlider.value         = maxX;
    maxXVal.textContent      = '±' + (maxX / 1000).toFixed(1) + ' m';
  }

  // Toggles always reflect current state (can be changed from tray)
  if (lockEnToggle.checked !== lockEn) lockEnToggle.checked = lockEn;
  if (enableToggle.checked !== Boolean(status.enabled)) {
    enableToggle.checked    = Boolean(status.enabled);
    enableText.textContent  = status.enabled ? 'ON' : 'OFF';
  }
});

// ── Auto-update ───────────────────────────────────────────────
const updateBanner    = document.getElementById('updateBanner');
const updateMsg       = document.getElementById('updateMsg');
const installUpdateBtn = document.getElementById('installUpdateBtn');
const dismissUpdateBtn = document.getElementById('dismissUpdateBtn');

window.arduino.onUpdateStatus((data) => {
  switch (data.type) {
    case 'ready':
      updateMsg.textContent      = `Update v${data.version} ready`;
      installUpdateBtn.style.display = 'inline-block';
      updateBanner.style.display = 'flex';
      break;
    case 'downloading':
      updateMsg.textContent      = `Downloading update v${data.version}…`;
      installUpdateBtn.style.display = 'none';
      updateBanner.style.display = 'flex';
      break;
    case 'progress':
      updateMsg.textContent      = `Downloading update… ${data.percent}%`;
      installUpdateBtn.style.display = 'none';
      updateBanner.style.display = 'flex';
      break;
    case 'error':
      updateMsg.textContent      = `Update failed — check Activity log`;
      installUpdateBtn.style.display = 'none';
      updateBanner.style.display = 'flex';
      break;
    default:
      updateBanner.style.display = 'none';
  }
});

installUpdateBtn.addEventListener('click', () => window.arduino.installUpdate());
dismissUpdateBtn.addEventListener('click', () => { updateBanner.style.display = 'none'; });

// ── Triggered event ───────────────────────────────────────────
window.arduino.onTriggered((targets) => {
  radarCard.classList.remove('triggered');
  void radarCard.offsetWidth; // force reflow to restart animation
  radarCard.classList.add('triggered');
  setTimeout(() => radarCard.classList.remove('triggered'), 1800);
  updateStatusUI(targets.length, lockEnToggle.checked, false);
});

// ── Locked event ──────────────────────────────────────────────
window.arduino.onLocked(() => {
  lockCountdown.textContent = '';
  emptyStartTime = null;
  updateStatusUI(0, true, true);
  setTimeout(() => updateStatusUI(0, lockEnToggle.checked, false), 4000);
});

// ── Status badge helper ───────────────────────────────────────
function updateStatusUI(count, lockEn, isLocked) {
  countBadge.textContent = `${count} target${count !== 1 ? 's' : ''}`;
  if (isLocked) {
    statusBadge.textContent = 'LOCKED';
    statusBadge.className   = 'status-badge locked';
  } else if (count === 0) {
    statusBadge.textContent = lockEn ? 'WATCHING' : 'EMPTY';
    statusBadge.className   = 'status-badge empty';
  } else if (count === 1) {
    statusBadge.textContent = 'SAFE';
    statusBadge.className   = 'status-badge safe';
  } else {
    statusBadge.textContent = 'THREAT';
    statusBadge.className   = 'status-badge threat';
  }
}

// ── Target list ───────────────────────────────────────────────
const TARGET_COLORS = ['#3fb950', '#f85149', '#d29922'];

function updateTargetList(targets) {
  targetList.innerHTML = '';
  targets.forEach((t, i) => {
    const dist = (t.y / 1000).toFixed(2);
    const side = t.x === 0 ? 'center'
               : t.x < 0  ? `${(Math.abs(t.x) / 10).toFixed(0)}cm left`
               :             `${(t.x / 10).toFixed(0)}cm right`;
    const div = document.createElement('div');
    div.className = 'target-row';
    div.innerHTML = `<span class="target-dot t${i}"></span> T${i+1}: ${dist}m · ${side}`;
    targetList.appendChild(div);
  });
}

// ── Radar canvas ──────────────────────────────────────────────
function drawRadar(targets, maxRangeMm, maxXMm = 1000) {
  ctx.clearRect(0, 0, W, H);

  // Background
  ctx.fillStyle = '#010409';
  ctx.fillRect(0, 0, W, H);

  // Range arcs + labels
  const intervals = [0.25, 0.5, 0.75, 1.0];
  intervals.forEach(frac => {
    const r     = PLOT_R * frac;
    const label = ((maxRangeMm * frac) / 1000).toFixed(1) + 'm';

    // Arc (top semicircle from sensor position)
    ctx.beginPath();
    ctx.arc(SENSOR_X, SENSOR_Y, r, Math.PI, 2 * Math.PI);
    ctx.strokeStyle = frac === 1.0 ? '#2d333b' : '#21262d';
    ctx.lineWidth = 1;
    ctx.stroke();

    // Label at top of arc (center-top)
    ctx.fillStyle = '#484f58';
    ctx.font = '9px monospace';
    ctx.textAlign = 'left';
    ctx.fillText(label, SENSOR_X + 4, SENSOR_Y - r + 11);
  });

  // FOV guide lines (±60° from center)
  const fovAngle = Math.PI / 3; // 60° each side = 120° total
  ctx.strokeStyle = '#21262d';
  ctx.lineWidth = 1;
  ctx.setLineDash([3, 4]);
  [[Math.PI + fovAngle], [2 * Math.PI - fovAngle]].forEach(([a]) => {
    ctx.beginPath();
    ctx.moveTo(SENSOR_X, SENSOR_Y);
    ctx.lineTo(SENSOR_X + Math.cos(a) * PLOT_R, SENSOR_Y + Math.sin(a) * PLOT_R);
    ctx.stroke();
  });
  ctx.setLineDash([]);

  // Center line
  ctx.beginPath();
  ctx.moveTo(SENSOR_X, SENSOR_Y);
  ctx.lineTo(SENSOR_X, SENSOR_Y - PLOT_R);
  ctx.strokeStyle = '#2d333b';
  ctx.lineWidth = 1;
  ctx.stroke();

  // Detection width limit lines (±maxXMm)
  const xLimitPx = Math.min((maxXMm / maxRangeMm) * (W / 2 - 4), W / 2 - 4);
  ctx.setLineDash([4, 3]);
  ctx.strokeStyle = '#58a6ff44';
  ctx.lineWidth = 1;
  [-xLimitPx, xLimitPx].forEach(dx => {
    ctx.beginPath();
    ctx.moveTo(SENSOR_X + dx, SENSOR_Y);
    ctx.lineTo(SENSOR_X + dx, 0);
    ctx.stroke();
  });
  ctx.setLineDash([]);

  // Sensor dot
  ctx.beginPath();
  ctx.arc(SENSOR_X, SENSOR_Y, 4, 0, 2 * Math.PI);
  ctx.fillStyle = '#58a6ff';
  ctx.shadowBlur = 8;
  ctx.shadowColor = '#58a6ff';
  ctx.fill();
  ctx.shadowBlur = 0;

  // Targets
  targets.forEach((t, i) => {
    const px = SENSOR_X + (t.x / maxRangeMm) * (W / 2 - 4);
    const py = SENSOR_Y - (t.y / maxRangeMm) * PLOT_R;
    const color = TARGET_COLORS[i] || TARGET_COLORS[2];

    // Glow
    const grad = ctx.createRadialGradient(px, py, 0, px, py, 18);
    grad.addColorStop(0, color + 'aa');
    grad.addColorStop(1, color + '00');
    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.arc(px, py, 18, 0, 2 * Math.PI);
    ctx.fill();

    // Dot
    ctx.beginPath();
    ctx.arc(px, py, 6, 0, 2 * Math.PI);
    ctx.fillStyle = color;
    ctx.shadowBlur = 10;
    ctx.shadowColor = color;
    ctx.fill();
    ctx.shadowBlur = 0;
  });
}

// Draw empty radar on load
drawRadar([], 2000, 1000);

// ── Log ───────────────────────────────────────────────────────
window.arduino.onLog((msg) => {
  const cls = msg.includes('TRIGGERED') ? 'trigger'
            : msg.includes('locked')    ? 'warn'
            : msg.includes('OK')        ? 'ok'
            : 'info';
  addLog(msg, cls);
});

function addLog(msg, cls = 'muted') {
  const line = document.createElement('div');
  line.className   = 'log-' + cls;
  line.textContent = msg;
  logBox.appendChild(line);
  logBox.scrollTop = logBox.scrollHeight;
  while (logBox.children.length > 200) logBox.removeChild(logBox.firstChild);
}

clearLogBtn.addEventListener('click', () => { logBox.innerHTML = ''; });
openLogBtn.addEventListener('click',  () => { window.arduino.openLogFile(); });

// ── Sliders ───────────────────────────────────────────────────
function trackSlider(el, _key, valEl, fmt) {
  el.addEventListener('input', () => { valEl.textContent = fmt(el.value); });
}

trackSlider(coolSlider,    'cool',     coolVal,    v => v + ' s');
trackSlider(lockDlySlider, 'lockDly',  lockDlyVal, v => v + ' s');
trackSlider(maxRangeSlider,'maxRange', maxRangeVal,v => (v/1000).toFixed(1) + ' m');
trackSlider(maxXSlider,    'maxX',     maxXVal,    v => '±' + (v/1000).toFixed(1) + ' m');
trackSlider(pollingSlider, 'polling',  pollingVal, v => v + ' frames');
trackSlider(smoothSlider,  'smooth',   smoothVal,  v => v + '%');

// Polling changes app-side threat threshold immediately (no Apply needed)
pollingSlider.addEventListener('input', () => {
  const n = Number(pollingSlider.value);
  window.arduino.setThreatThreshold(n);
  addLog(`[${ts()}] Polling set to ${n} frame${n !== 1 ? 's' : ''}`, 'ok');
});

// Smoothing changes EMA alpha immediately (app-side only, no Apply needed)
smoothSlider.addEventListener('input', () => {
  SMOOTH_ALPHA = Number(smoothSlider.value) / 100;
  addLog(`[${ts()}] Smoothing set to ${smoothSlider.value}%`, 'ok');
});

// Live-update radar X lines while dragging
maxXSlider.addEventListener('input', () => drawRadar([], currentMaxRange, Number(maxXSlider.value)));

// ── Apply ─────────────────────────────────────────────────────
applyBtn.addEventListener('click', () => {
  if (!connected) { addLog('Not connected.', 'info'); return; }
  window.arduino.sendCommand(`SET COOL ${coolSlider.value}`);
  window.arduino.sendCommand(`SET LOCKDLY ${lockDlySlider.value}`);
  window.arduino.sendCommand(`SET MAXRANGE ${maxRangeSlider.value}`);
  window.arduino.sendCommand(`SET MAXX ${maxXSlider.value}`);
  window.arduino.sendCommand(`SET LOCKEN ${lockEnToggle.checked ? 1 : 0}`);
  addLog(`[${ts()}] Settings applied`, 'ok');
});

// ── Enable / Disable ──────────────────────────────────────────
enableToggle.addEventListener('change', () => {
  enableText.textContent = enableToggle.checked ? 'ON' : 'OFF';
  if (connected) window.arduino.sendCommand(enableToggle.checked ? 'ENABLE' : 'DISABLE');
  addLog(`[${ts()}] Protection ${enableToggle.checked ? 'enabled' : 'disabled'}`, 'info');
});

// Lock on empty toggle sends immediately (no need to click Apply)
lockEnToggle.addEventListener('change', () => {
  if (connected) window.arduino.sendCommand(`SET LOCKEN ${lockEnToggle.checked ? 1 : 0}`);
});

// Tray can toggle protection
window.arduino.onTrayToggle((val) => {
  enableToggle.checked   = val;
  enableText.textContent = val ? 'ON' : 'OFF';
});

// ── Helpers ───────────────────────────────────────────────────
function ts() { return new Date().toLocaleTimeString(); }
