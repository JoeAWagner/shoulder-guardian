/**
 * make-icon.js
 * Generates build/icon.png (512×512) for electron-builder.
 * electron-builder auto-converts it to .ico (Windows) and .icns (macOS).
 *
 * Style: dark radar screen with blue glowing sensor dot — matches the app UI.
 *
 * Run:  node scripts/make-icon.js
 * Deps: pngjs (devDependency)
 */

'use strict';

const fs   = require('fs');
const path = require('path');
const { PNG } = require('pngjs');

const SIZE = 512;
const png  = new PNG({ width: SIZE, height: SIZE, filterType: -1 });

const CX = SIZE / 2;
const CY = SIZE / 2;
const R  = SIZE / 2;          // outer radius of circular icon

// ── Colour helpers ────────────────────────────────────────────
function hex(h) {
  return [
    parseInt(h.slice(1,3),16),
    parseInt(h.slice(3,5),16),
    parseInt(h.slice(5,7),16),
  ];
}
const BG    = hex('#0d1117');  // app background
const ARC   = hex('#21262d');  // subtle arc lines
const BLUE  = hex('#58a6ff');  // sensor / accent

// Sensor sits near bottom of the radar arc area
const SENSOR_Y = CY + R * 0.30;
const PLOT_R   = R  * 0.80;

// ── Draw ──────────────────────────────────────────────────────
for (let y = 0; y < SIZE; y++) {
  for (let x = 0; x < SIZE; x++) {
    const idx = (y * SIZE + x) * 4;

    // Circular mask — transparent outside
    const d = Math.hypot(x - CX, y - CY);
    if (d > R - 0.5) {
      png.data[idx+3] = 0;
      continue;
    }

    let [r, g, b] = BG;
    let a = 255;

    // ── Radar arcs (semi-circles centred on sensor) ───────────
    const sd = Math.hypot(x - CX, y - SENSOR_Y);
    const onTop = y < SENSOR_Y;             // only draw upper half

    [0.30, 0.55, 0.80, 1.0].forEach(frac => {
      const arcR   = PLOT_R * frac;
      const thick  = frac === 1.0 ? 2.5 : 1.5;
      const bright = frac === 1.0 ? 0.25 : 0.12;
      if (onTop && Math.abs(sd - arcR) < thick) {
        r = Math.round(r + (ARC[0] - r) * bright + (BLUE[0] - r) * bright * 0.4);
        g = Math.round(g + (ARC[1] - g) * bright + (BLUE[1] - g) * bright * 0.4);
        b = Math.round(b + (ARC[2] - b) * bright + (BLUE[2] - b) * bright * 0.4);
      }
    });

    // ── Center vertical line ──────────────────────────────────
    if (onTop && Math.abs(x - CX) < 1.2 && sd < PLOT_R) {
      r = Math.round(r + (ARC[0] - r) * 0.20);
      g = Math.round(g + (ARC[1] - g) * 0.20);
      b = Math.round(b + (ARC[2] - b) * 0.20);
    }

    // ── FOV lines (±60° from top) ─────────────────────────────
    [Math.PI + Math.PI/3, 2*Math.PI - Math.PI/3].forEach(angle => {
      const lx = CX + Math.cos(angle) * PLOT_R;
      const ly = SENSOR_Y + Math.sin(angle) * PLOT_R;
      // Distance from pixel to the line segment
      const tx = lx - CX, ty = ly - SENSOR_Y;
      const len = Math.hypot(tx, ty);
      const t   = Math.max(0, Math.min(1, ((x-CX)*tx + (y-SENSOR_Y)*ty) / (len*len)));
      const px  = CX + t*tx, py = SENSOR_Y + t*ty;
      const ld  = Math.hypot(x - px, y - py);
      if (ld < 1.5) {
        r = Math.round(r + (ARC[0] - r) * 0.18);
        g = Math.round(g + (ARC[1] - g) * 0.18);
        b = Math.round(b + (ARC[2] - b) * 0.18);
      }
    });

    // ── Demo target dot (T1 — green) ─────────────────────────
    const t1x = CX + 0.15 * PLOT_R, t1y = SENSOR_Y - 0.50 * PLOT_R;
    const t1d  = Math.hypot(x - t1x, y - t1y);
    const T1   = hex('#3fb950');
    if (t1d < 22) {
      // outer glow
      const gf = Math.max(0, 1 - t1d/22);
      r = Math.round(r + (T1[0] - r) * gf * 0.30);
      g = Math.round(g + (T1[1] - g) * gf * 0.30);
      b = Math.round(b + (T1[2] - b) * gf * 0.30);
    }
    if (t1d < 8) {
      const df = Math.max(0, 1 - t1d/8);
      r = Math.round(r + (T1[0] - r) * df);
      g = Math.round(g + (T1[1] - g) * df);
      b = Math.round(b + (T1[2] - b) * df);
    }

    // ── Sensor dot (blue, centred, bottom of arcs) ────────────
    const dotR = R * 0.07;
    const sdd  = Math.hypot(x - CX, y - SENSOR_Y);
    if (sdd < dotR * 3) {
      // glow halo
      const gf = Math.max(0, 1 - sdd / (dotR*3));
      r = Math.round(r + (BLUE[0] - r) * gf * 0.35);
      g = Math.round(g + (BLUE[1] - g) * gf * 0.35);
      b = Math.round(b + (BLUE[2] - b) * gf * 0.35);
    }
    if (sdd < dotR) {
      const df = Math.max(0, 1 - sdd/dotR);
      r = Math.round(r + (BLUE[0] - r) * df);
      g = Math.round(g + (BLUE[1] - g) * df);
      b = Math.round(b + (BLUE[2] - b) * df);
    }

    // ── Thin blue outer ring ──────────────────────────────────
    if (Math.abs(d - R + 4) < 4) {
      const rf = 1 - Math.abs(d - R + 4) / 4;
      r = Math.round(r + (BLUE[0] - r) * rf * 0.5);
      g = Math.round(g + (BLUE[1] - g) * rf * 0.5);
      b = Math.round(b + (BLUE[2] - b) * rf * 0.5);
    }

    png.data[idx]   = r;
    png.data[idx+1] = g;
    png.data[idx+2] = b;
    png.data[idx+3] = a;
  }
}

// ── Write ─────────────────────────────────────────────────────
const outDir  = path.join(__dirname, '..', 'build');
const outPath = path.join(outDir, 'icon.png');
if (!fs.existsSync(outDir)) fs.mkdirSync(outDir, { recursive: true });
const buf = PNG.sync.write(png);
fs.writeFileSync(outPath, buf);
console.log(`✓ Icon written → ${outPath}  (${SIZE}×${SIZE})`);
