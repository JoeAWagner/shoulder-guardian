#!/usr/bin/env node
/**
 * auto-release.js
 * Called by Claude Code's Stop hook at the end of each turn.
 * If there are uncommitted changes, bumps the patch version,
 * commits everything, and pushes to GitHub.
 *
 * Skips silently if the working tree is clean.
 */

'use strict';

const { execSync } = require('child_process');
const fs   = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

function run(cmd) {
  return execSync(cmd, { cwd: ROOT, encoding: 'utf8', stdio: ['pipe','pipe','pipe'] }).trim();
}

function runSafe(cmd) {
  try { return run(cmd); } catch { return ''; }
}

// ── Check for uncommitted changes ─────────────────────────────
const dirty = runSafe('git status --porcelain');
if (!dirty) process.exit(0);

// ── Bump patch version ────────────────────────────────────────
const pkgPath = path.join(ROOT, 'package.json');
const pkg     = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
const [major, minor, patch] = pkg.version.split('.').map(Number);
pkg.version = `${major}.${minor}.${patch + 1}`;
fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n', 'utf8');

// ── Commit and push ───────────────────────────────────────────
try {
  run('git add .');
  run(`git commit -m "v${pkg.version}"`);
  run('git push origin main');
  console.log(`[auto-release] Pushed v${pkg.version}`);
} catch (e) {
  console.error(`[auto-release] Push failed: ${e.message}`);
  process.exit(0); // don't block Claude on push failures
}
