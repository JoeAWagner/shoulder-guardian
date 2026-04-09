#!/usr/bin/env node
/**
 * release.js — build + tag + publish a GitHub Release
 *
 * Usage:
 *   npm run release                  (uses version from package.json)
 *   npm run release -- --notes "What changed"
 *   npm run release -- --dry-run     (build only, skip git + GitHub steps)
 *
 * Requirements:
 *   - gh CLI installed and authenticated  (winget install GitHub.cli → gh auth login)
 *   - git remote "origin" pointing to GitHub repo
 */

'use strict';

const { execSync, spawnSync } = require('child_process');
const fs   = require('fs');
const path = require('path');

// ── Parse args ────────────────────────────────────────────────
const args     = process.argv.slice(2);
const dryRun   = args.includes('--dry-run');
const notesIdx = args.indexOf('--notes');
const cliNotes = notesIdx !== -1 ? args[notesIdx + 1] : null;

// ── Helpers ───────────────────────────────────────────────────
function run(cmd, opts = {}) {
  console.log(`  > ${cmd}`);
  try {
    return execSync(cmd, { stdio: 'inherit', cwd: ROOT, ...opts });
  } catch (e) {
    console.error(`\n✖ Command failed: ${cmd}`);
    process.exit(1);
  }
}

function runCapture(cmd) {
  try {
    return execSync(cmd, { cwd: ROOT, encoding: 'utf8', stdio: ['pipe','pipe','pipe'] }).trim();
  } catch {
    return '';
  }
}

function check(cmd, hint) {
  const result = spawnSync(cmd.split(' ')[0], cmd.split(' ').slice(1), { stdio: 'pipe' });
  if (result.error || result.status !== 0) {
    console.error(`\n✖ Required tool not found: ${cmd.split(' ')[0]}`);
    console.error(`  ${hint}`);
    process.exit(1);
  }
}

function step(msg) {
  console.log(`\n── ${msg}`);
}

// ── Root & package ────────────────────────────────────────────
const ROOT    = path.resolve(__dirname, '..');
const pkg     = JSON.parse(fs.readFileSync(path.join(ROOT, 'package.json'), 'utf8'));
const VERSION = pkg.version;
const TAG     = `v${VERSION}`;
const NAME    = pkg.build?.productName || pkg.name;

console.log(`\n╔══════════════════════════════════════╗`);
console.log(`║  ${NAME} — Release ${TAG}`.padEnd(38) + '║');
if (dryRun) console.log(`║  DRY RUN — skipping git & GitHub   ║`);
console.log(`╚══════════════════════════════════════╝`);

// ── Preflight checks ──────────────────────────────────────────
step('Checking tools');
check('git --version',  'Install git from https://git-scm.com');
if (!dryRun) check('gh --version', 'Install GitHub CLI: winget install GitHub.cli\nThen run: gh auth login');

// Check gh is authenticated
if (!dryRun) {
  const authStatus = runCapture('gh auth status');
  if (!authStatus.includes('Logged in')) {
    console.error('\n✖ GitHub CLI is not authenticated. Run: gh auth login');
    process.exit(1);
  }
}

// Check for uncommitted changes
step('Checking git status');
const dirty = runCapture('git status --porcelain');
if (dirty) {
  console.log('  Uncommitted changes detected — creating release commit...');
  if (!dryRun) {
    run('git add .');
    run(`git commit -m "Release ${TAG}"`);
  } else {
    console.log('  [dry-run] would commit all changes');
  }
} else {
  console.log('  Working tree clean.');
}

// Check tag doesn't already exist
const existingTag = runCapture(`git tag -l ${TAG}`);
if (existingTag === TAG) {
  console.error(`\n✖ Tag ${TAG} already exists. Bump version in package.json and try again.`);
  process.exit(1);
}

// ── Tag ───────────────────────────────────────────────────────
step(`Tagging ${TAG}`);
if (!dryRun) {
  run(`git tag ${TAG}`);
  run('git push origin main');
  run(`git push origin ${TAG}`);
} else {
  console.log(`  [dry-run] would tag ${TAG} and push`);
}

// ── Build ─────────────────────────────────────────────────────
step('Building');
const platform = process.platform;
const buildCmd = platform === 'darwin' ? 'npm run build-mac' : 'npm run build-win';
console.log(`  Platform: ${platform} → ${buildCmd}`);
run(buildCmd);

// ── Find build artifacts ──────────────────────────────────────
step('Finding build artifacts');
const distDir = path.join(ROOT, 'dist');
const exts    = platform === 'darwin' ? ['.dmg'] : ['.exe'];
const files   = fs.readdirSync(distDir)
  .filter(f => exts.some(e => f.endsWith(e)) && !f.startsWith('.'))
  .map(f => path.join(distDir, f));

if (!files.length) {
  console.error('✖ No build artifacts found in dist/');
  process.exit(1);
}

files.forEach(f => console.log(`  Found: ${path.basename(f)}`));

// ── Release notes ─────────────────────────────────────────────
const notes = cliNotes ||
  `## ${NAME} ${TAG}\n\n` +
  `Built for ${platform === 'darwin' ? 'macOS' : 'Windows'} on ${new Date().toISOString().slice(0,10)}.\n\n` +
  `### Files\n` +
  files.map(f => `- \`${path.basename(f)}\``).join('\n');

// ── Publish GitHub Release ────────────────────────────────────
step('Publishing GitHub Release');
if (!dryRun) {
  const fileArgs = files.map(f => `"${f}"`).join(' ');
  run(`gh release create ${TAG} ${fileArgs} --title "${NAME} ${TAG}" --notes "${notes.replace(/"/g, '\\"')}"`);
  console.log(`\n✔ Release published: https://github.com/${runCapture('gh repo view --json nameWithOwner -q .nameWithOwner')}/releases/tag/${TAG}`);
} else {
  console.log(`  [dry-run] would publish release ${TAG} with ${files.length} artifact(s)`);
  console.log('\n✔ Dry run complete — no changes made.');
}
