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
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  });
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
  const result = spawnSync(signtool, args, { encoding: 'utf8', maxBuffer: 10 * 1024 * 1024 });
  const output = (result.stdout || '') + (result.stderr || '');
  return { ok: result.status === 0, output, code: result.status };
}

// Parse cert blocks from signtool verbose output into an array of objects
function parseCerts(output) {
  const certs = [];
  let current = null;
  for (const raw of output.split(/\r?\n/)) {
    const line = raw.trim();
    const m = (re) => line.match(re);
    if (m(/^Issued to:/))  { current = { issuedTo: line.replace(/^Issued to:\s*/, '') }; }
    else if (current && m(/^Issued by:/))  { current.issuedBy  = line.replace(/^Issued by:\s*/, ''); }
    else if (current && m(/^SHA1 hash:/))  {
      current.sha1 = line.replace(/^SHA1 hash:\s*/, '');
      certs.push(current);
      current = null;
    }
  }
  return certs;
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

    // If signtool found multiple certs (e.g. Adobe certs on the machine),
    // parse the output to find the Trusted Signing cert and retry with /sha1
    if (!result.ok && result.output.includes('Multiple certificates were found')) {
      console.warn('[sign] Multiple certs found — identifying Trusted Signing cert…');
      const certs = parseCerts(result.output);

      // Trusted Signing cert is self-signed (issuedTo === issuedBy) and not Adobe
      const trusted = certs.find(c =>
        !c.issuedTo?.toLowerCase().includes('adobe') &&
        !c.issuedBy?.toLowerCase().includes('adobe')
      );

      if (trusted) {
        console.log(`[sign] Retrying with Trusted Signing cert SHA1: ${trusted.sha1}`);
        const retryArgs = [
          'sign',
          '/dlib', dlib,
          '/dmdf', metaPath,
          '/fd',   'SHA256',
          '/tr',   'http://timestamp.acs.microsoft.com',
          '/td',   'SHA256',
          '/sha1', trusted.sha1,
          '/v',
          filePath,
        ];
        result = runSigntool(signtool, retryArgs);
      } else {
        console.error('[sign] Could not identify Trusted Signing cert in:\n', result.output);
      }
    }

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
