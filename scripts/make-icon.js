/**
 * make-icon.js
 * Generates build/icon.png (512×512) for electron-builder.
 * electron-builder auto-converts it to .ico (Windows) and .icns (macOS).
 *
 * Style: the "Argus radar-eye" — concentric radar rings + compass ticks
 * around a watchful eye (iris, pupil, glint), on the dark app background.
 * Matches the round-display boot splash so the brand is consistent across
 * device, app, installer, and tray.
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
const R  = SIZE / 2;

function hex(h) {
  return [parseInt(h.slice(1,3),16), parseInt(h.slice(3,5),16), parseInt(h.slice(5,7),16)];
}
const BG       = hex('#0d1117');  // app background
const CYAN     = hex('#4dd0ff');  // eye / iris (bright)
const CYAN_DIM = hex('#1c6e88');  // radar rings (dim)
const GLINT    = hex('#eaf6ff');  // eye highlight
const PUPIL    = hex('#05080c');  // pupil (near-black)

// clamp 0..1
const cl = v => v < 0 ? 0 : v > 1 ? 1 : v;
// blend col into [r,g,b] by coverage a
function mix(r, g, b, col, a) {
  return [r + (col[0]-r)*a, g + (col[1]-g)*a, b + (col[2]-b)*a];
}

// Emblem geometry
const RING_OUT = R * 0.82;   // outer radar ring (brighter)
const RING_MID = R * 0.64;   // mid radar ring (dim)
const EYE_RX   = R * 0.49;   // eye half-width
const EYE_RY   = R * 0.29;   // eye half-height
const EYE_W    = R * 0.022;  // eye outline thickness
const IRIS_R   = R * 0.20;
const PUPIL_R  = R * 0.10;
const GLINT_R  = R * 0.045;

for (let y = 0; y < SIZE; y++) {
  for (let x = 0; x < SIZE; x++) {
    const idx = (y * SIZE + x) * 4;

    const d = Math.hypot(x - CX, y - CY);
    if (d > R - 0.5) { png.data[idx+3] = 0; continue; }   // transparent outside

    let [r, g, b] = BG;

    // ── Radar rings ───────────────────────────────────────────
    const ringCov = (rr, half) => cl(half + 0.6 - Math.abs(d - rr));
    [[RING_OUT, R*0.012, 0.85], [RING_MID, R*0.009, 0.6]].forEach(([rr, half, str]) => {
      const c = ringCov(rr, half) * str;
      if (c > 0) [r,g,b] = mix(r,g,b, CYAN_DIM, c);
    });

    // ── Compass ticks at N / E / S / W (just outside outer ring) ─
    for (let a = 0; a < 360; a += 90) {
      const rad = a * Math.PI / 180;
      const ux = Math.cos(rad), uy = Math.sin(rad);
      const x1 = CX + ux * (RING_OUT + R*0.02), y1 = CY + uy * (RING_OUT + R*0.02);
      const x2 = CX + ux * (RING_OUT + R*0.08), y2 = CY + uy * (RING_OUT + R*0.08);
      const tx = x2 - x1, ty = y2 - y1, len = Math.hypot(tx, ty);
      const t  = cl(((x-x1)*tx + (y-y1)*ty) / (len*len));
      const px = x1 + t*tx, py = y1 + t*ty;
      const ld = Math.hypot(x - px, y - py);
      const c  = cl(R*0.013 - ld) * 0.85;
      if (c > 0) [r,g,b] = mix(r,g,b, CYAN_DIM, c);
    }

    // ── Eye outline (almond) ──────────────────────────────────
    const e   = Math.hypot((x - CX) / EYE_RX, (y - CY) / EYE_RY);
    const eMean = (EYE_RX + EYE_RY) / 2;
    const eDist = Math.abs(e - 1) * eMean;          // ~px distance to curve
    const eCov  = cl(EYE_W + 0.6 - eDist);
    if (eCov > 0) [r,g,b] = mix(r,g,b, CYAN, eCov);

    // ── Iris / pupil / glint ──────────────────────────────────
    const ir = Math.hypot(x - CX, y - CY);
    const irisCov = cl(IRIS_R + 0.5 - ir);
    if (irisCov > 0) [r,g,b] = mix(r,g,b, CYAN, irisCov);
    const pupilCov = cl(PUPIL_R + 0.5 - ir);
    if (pupilCov > 0) [r,g,b] = mix(r,g,b, PUPIL, pupilCov);
    const gd = Math.hypot(x - (CX - R*0.06), y - (CY - R*0.08));
    const glintCov = cl(GLINT_R + 0.5 - gd);
    if (glintCov > 0) [r,g,b] = mix(r,g,b, GLINT, glintCov);

    // ── Thin outer accent ring at the very edge ───────────────
    const edge = cl(R*0.012 - Math.abs(d - (R - R*0.02)));
    if (edge > 0) [r,g,b] = mix(r,g,b, CYAN_DIM, edge * 0.6);

    png.data[idx]   = Math.round(r);
    png.data[idx+1] = Math.round(g);
    png.data[idx+2] = Math.round(b);
    png.data[idx+3] = 255;
  }
}

const outDir  = path.join(__dirname, '..', 'build');
const outPath = path.join(outDir, 'icon.png');
if (!fs.existsSync(outDir)) fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(outPath, PNG.sync.write(png));
console.log(`✓ Icon written → ${outPath}  (${SIZE}×${SIZE})`);
