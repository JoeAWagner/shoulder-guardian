'use strict';
/**
 * test-sign.js — standalone signing test
 * Run: node scripts/test-sign.js
 */

const { spawnSync } = require('child_process');
const path = require('path');
const fs   = require('fs');
const os   = require('os');

// Load .env
const envPath = path.join(__dirname, '..', '.env');
if (fs.existsSync(envPath)) {
  fs.readFileSync(envPath, 'utf8').split(/\r?\n/).forEach(line => {
    const m = line.match(/^\s*([\w_]+)\s*=\s*(.+?)\s*$/);
    // Always override — .env is the source of truth; stale system env vars must not win
    if (m) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  });
}

// Ensure az CLI is in PATH so the dlib's DefaultAzureCredential can find it
const azCliPaths = [
  'C:\\Program Files (x86)\\Microsoft SDKs\\Azure\\CLI2\\wbin',
  'C:\\Program Files\\Microsoft SDKs\\Azure\\CLI2\\wbin',
];
for (const p of azCliPaths) {
  if (fs.existsSync(p) && !(process.env.PATH || '').includes(p)) {
    process.env.PATH = p + ';' + (process.env.PATH || '');
  }
}

const endpoint = process.env.AZURE_SIGNING_ENDPOINT;
const account  = process.env.AZURE_SIGNING_ACCOUNT;
const profile  = process.env.AZURE_SIGNING_PROFILE;
const dlib     = process.env.AZURE_SIGNING_DLIB;
const tenant   = process.env.AZURE_TENANT_ID;

const azCliFound = azCliPaths.find(p => fs.existsSync(p));

console.log('\n── Env check ───────────────────────────────────────');
console.log(`  AZURE_SIGNING_ENDPOINT : ${endpoint}`);
console.log(`  AZURE_SIGNING_ACCOUNT  : ${account}`);
console.log(`  AZURE_SIGNING_PROFILE  : ${profile}`);
console.log(`  AZURE_SIGNING_DLIB     : ${dlib}`);
console.log(`  AZURE_TENANT_ID        : ${tenant}`);
console.log(`  Dlib exists            : ${fs.existsSync(dlib)}`);
console.log(`  az CLI path injected   : ${azCliFound || '(not found — auth may fail)'}`);

// Find signtool
const kitRoot = 'C:\\Program Files (x86)\\Windows Kits\\10\\bin';
let signtool = null;
if (fs.existsSync(kitRoot)) {
  const versions = fs.readdirSync(kitRoot)
    .filter(d => /^\d+\.\d+\.\d+\.\d+$/.test(d))
    .sort((a, b) => b.localeCompare(a, undefined, { numeric: true }));
  for (const ver of versions) {
    const c = path.join(kitRoot, ver, 'x64', 'signtool.exe');
    if (fs.existsSync(c)) { signtool = c; break; }
  }
}
console.log(`  signtool               : ${signtool}`);

if (!signtool || !fs.existsSync(dlib)) {
  console.error('\n✖ Missing signtool or dlib — cannot continue');
  process.exit(1);
}

// ── Azure auth check ─────────────────────────────────
console.log('\n── Azure auth check (az account get-access-token) ──');
// Use cmd /c az so Windows resolves az.cmd via the PATH we just injected
const tokenResult = spawnSync('cmd.exe', [
  '/c', 'az',
  'account', 'get-access-token',
  '--tenant', tenant,
  '--resource', 'https://codesigning.azure.net',
  '--query', 'accessToken',
  '-o', 'tsv',
], { encoding: 'utf8', env: process.env });

if (tokenResult.status === 0 && tokenResult.stdout.trim()) {
  const tok = tokenResult.stdout.trim();
  console.log(`  Token obtained         : YES (${tok.slice(0, 20)}…)`);
} else {
  console.error('  Token obtained         : NO — az auth is failing!');
  console.error('  stdout:', tokenResult.stdout || '(empty)');
  console.error('  stderr:', tokenResult.stderr || '(empty)');
  console.error('\n  ► Run: az login --tenant', tenant);
  console.error('  ► Then retry this script\n');
  process.exit(1);
}


// ── Direct API test ───────────────────────────────────
// Call the Trusted Signing data-plane API directly with our token so we can
// see exactly what Azure returns — 200 means RBAC+config are good, 403 means
// permission denied, 404 means wrong account/profile name.
console.log('\n── Direct API test ──────────────────────────────────');
const apiResult = spawnSync('cmd.exe', [
  '/c', 'az', 'rest',
  '--method', 'GET',
  '--url', `${endpoint}codesigningaccounts/${account}/certificateprofiles/${profile}`,
  '--url-parameters', 'api-version=2024-02-05-preview',
  '--resource', 'https://codesigning.azure.net',
], { encoding: 'utf8', env: process.env });
console.log('  HTTP status / output:');
if (apiResult.stdout) console.log(' ', apiResult.stdout.slice(0, 500));
if (apiResult.stderr) console.log('  STDERR:', apiResult.stderr.slice(0, 500));

// Create a tiny dummy exe to sign (copy cmd.exe)
const dummyExe = path.join(os.tmpdir(), 'sg-sign-test.exe');
fs.copyFileSync('C:\\Windows\\System32\\cmd.exe', dummyExe);
console.log(`\n── Signing test file: ${dummyExe}`);

// Write metadata
const metaPath = path.join(os.tmpdir(), 'sg-sign-test-meta.json');
fs.writeFileSync(metaPath, JSON.stringify({
  Endpoint:               endpoint,
  CodeSigningAccountName: account,
  CertificateProfileName: profile,
}, null, 2));

console.log('\n── Running signtool with /debug ────────────────────');
// AuthenticodeDigestSignEx (used by the dlib) handles cert selection internally —
// no /sha1, /a, or any other cert selection flags are allowed.
const result = spawnSync(signtool, [
  'sign',
  '/dlib', dlib,
  '/dmdf', metaPath,
  '/fd',   'SHA256',
  '/tr',   'http://timestamp.acs.microsoft.com',
  '/td',   'SHA256',
  '/v',
  dummyExe,
], { encoding: 'utf8', maxBuffer: 10 * 1024 * 1024, env: process.env });

console.log(result.stdout || '');
console.log(result.stderr || '');
console.log(`\n── Exit code: ${result.status}`);

// Cleanup
try { fs.unlinkSync(dummyExe); } catch (_) {}
try { fs.unlinkSync(metaPath); } catch (_) {}

if (result.status === 0) {
  console.log('✔ Signing test PASSED');
} else {
  console.log('✖ Signing test FAILED — see output above');
}
