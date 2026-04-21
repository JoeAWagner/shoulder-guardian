'use strict';
/**
 * sign.js — Microsoft Trusted Signing hook for electron-builder
 *
 * Called automatically by electron-builder for each signable file.
 * Requires Azure CLI authenticated (run: az login) and these env vars:
 *
 *   AZURE_SIGNING_ENDPOINT   https://<region>.codesigning.azure.net/
 *   AZURE_SIGNING_ACCOUNT    your Trusted Signing account name
 *   AZURE_SIGNING_PROFILE    your certificate profile name
 *   AZURE_SIGNING_DLIB       full path to Azure.CodeSigning.Dlib.dll
 *   AZURE_TENANT_ID          your Azure tenant ID (GUID)
 */

const { spawnSync } = require('child_process');
const path          = require('path');
const fs            = require('fs');
const os            = require('os');

// Load .env from project root so env vars don't need to be set system-wide
const envPath = path.join(__dirname, '..', '.env');
if (fs.existsSync(envPath)) {
  fs.readFileSync(envPath, 'utf8').split(/\r?\n/).forEach(line => {
    const m = line.match(/^\s*([\w_]+)\s*=\s*(.+?)\s*$/);
    // Always override — .env is the source of truth; stale system env vars must not win
    if (m) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  });
}

// Ensure az CLI is in PATH so the dlib's DefaultAzureCredential can run
// `az account get-access-token` when signtool loads the dlib as a subprocess
const azCliPaths = [
  'C:\\Program Files (x86)\\Microsoft SDKs\\Azure\\CLI2\\wbin',
  'C:\\Program Files\\Microsoft SDKs\\Azure\\CLI2\\wbin',
];
for (const p of azCliPaths) {
  if (fs.existsSync(p) && !(process.env.PATH || '').includes(p)) {
    process.env.PATH = p + ';' + (process.env.PATH || '');
  }
}

// Find signtool.exe in the Windows SDK — it's not on PATH by default
function findSigntool() {
  const kitRoot = 'C:\\Program Files (x86)\\Windows Kits\\10\\bin';
  if (!fs.existsSync(kitRoot)) return null;
  const versions = fs.readdirSync(kitRoot)
    .filter(d => /^\d+\.\d+\.\d+\.\d+$/.test(d))
    .sort((a, b) => b.localeCompare(a, undefined, { numeric: true }));
  for (const ver of versions) {
    const candidate = path.join(kitRoot, ver, 'x64', 'signtool.exe');
    if (fs.existsSync(candidate)) return candidate;
  }
  const flat = path.join(kitRoot, 'x64', 'signtool.exe');
  return fs.existsSync(flat) ? flat : null;
}

// Run signtool and capture combined output — returns { ok, output, code }
function runSigntool(signtool, args) {
  const result = spawnSync(signtool, args, { encoding: 'utf8', maxBuffer: 10 * 1024 * 1024, env: process.env });
  const output = (result.stdout || '') + (result.stderr || '');
  return { ok: result.status === 0, output, code: result.status };
}

exports.default = async function sign(configuration) {
  const filePath = configuration.path;
  if (!filePath || !filePath.endsWith('.exe')) return;

  const endpoint = process.env.AZURE_SIGNING_ENDPOINT;
  const account  = process.env.AZURE_SIGNING_ACCOUNT;
  const profile  = process.env.AZURE_SIGNING_PROFILE;
  const dlib     = process.env.AZURE_SIGNING_DLIB;

  if (!endpoint || !account || !profile || !dlib) {
    console.warn('[sign] AZURE_SIGNING_* env vars not set — skipping code signing');
    return;
  }
  if (!fs.existsSync(dlib)) {
    throw new Error(`[sign] Dlib not found at: ${dlib}`);
  }
  if (!process.env.AZURE_TENANT_ID) {
    throw new Error('[sign] AZURE_TENANT_ID is not set. Find it in the Azure portal under Azure Active Directory → Overview.');
  }

  const signtool = findSigntool();
  if (!signtool) {
    throw new Error('[sign] signtool.exe not found. Install the Windows SDK from https://developer.microsoft.com/windows/downloads/windows-sdk/');
  }
  console.log(`[sign] Using signtool: ${signtool}`);

  // Write temp metadata JSON
  const metaPath = path.join(os.tmpdir(), `trusted-signing-${Date.now()}.json`);
  fs.writeFileSync(metaPath, JSON.stringify({
    Endpoint:               endpoint,
    CodeSigningAccountName: account,
    CertificateProfileName: profile,
  }, null, 2));

  // AuthenticodeDigestSignEx (exported by the fully-loaded dlib) handles cert
  // selection and signing entirely — no /sha1 or /a flags allowed or needed.
  const baseArgs = [
    'sign',
    '/dlib', dlib,
    '/dmdf', metaPath,
    '/fd',   'SHA256',
    '/tr',   'http://timestamp.acs.microsoft.com',
    '/td',   'SHA256',
    '/v',
    filePath,
  ];

  try {
    console.log(`[sign] Signing ${path.basename(filePath)}…`);
    let result = runSigntool(signtool, baseArgs);

    // Print output regardless
    if (result.output) process.stdout.write(result.output);

    if (!result.ok) {
      throw new Error(`signtool exited with code ${result.code}`);
    }
    console.log(`[sign] ✔ Signed: ${path.basename(filePath)}`);

  } finally {
    try { fs.unlinkSync(metaPath); } catch (_) {}
  }
};
