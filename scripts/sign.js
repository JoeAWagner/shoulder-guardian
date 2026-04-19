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
 */

const { execSync } = require('child_process');
const path         = require('path');
const fs           = require('fs');
const os           = require('os');

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
  // Find the highest-versioned SDK subfolder
  const versions = fs.readdirSync(kitRoot)
    .filter(d => /^\d+\.\d+\.\d+\.\d+$/.test(d))
    .sort((a, b) => b.localeCompare(a, undefined, { numeric: true }));
  for (const ver of versions) {
    const candidate = path.join(kitRoot, ver, 'x64', 'signtool.exe');
    if (fs.existsSync(candidate)) return candidate;
  }
  // Fallback: flat x64 folder
  const flat = path.join(kitRoot, 'x64', 'signtool.exe');
  if (fs.existsSync(flat)) return flat;
  return null;
}

exports.default = async function sign(configuration) {
  const filePath = configuration.path;

  // Only sign .exe files
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
    console.error(`[sign] Dlib not found at: ${dlib}`);
    throw new Error(`Azure.CodeSigning.Dlib.dll not found at: ${dlib}`);
  }

  if (!process.env.AZURE_TENANT_ID) {
    throw new Error('AZURE_TENANT_ID is not set. Add it to your .env file. Find it in the Azure portal under Azure Active Directory → Overview.');
  }

  const signtool = findSigntool();
  if (!signtool) {
    throw new Error('signtool.exe not found. Install the Windows 10/11 SDK from https://developer.microsoft.com/windows/downloads/windows-sdk/');
  }
  console.log(`[sign] Using signtool: ${signtool}`);

  // Write a temp metadata JSON that the dlib reads
  const metadata  = {
    Endpoint:                 endpoint,
    CodeSigningAccountName:   account,
    CertificateProfileName:   profile,
  };
  const metaPath = path.join(os.tmpdir(), `trusted-signing-${Date.now()}.json`);
  fs.writeFileSync(metaPath, JSON.stringify(metadata, null, 2));

  try {
    console.log(`[sign] Signing ${path.basename(filePath)}…`);
    execSync(
      [
        `"${signtool}" sign`,
        `/dlib "${dlib}"`,
        `/dmdf "${metaPath}"`,
        '/fd SHA256',
        '/tr http://timestamp.acs.microsoft.com',
        '/td SHA256',
        '/v',
        `"${filePath}"`,
      ].join(' '),
      { stdio: 'inherit' }
    );
    console.log(`[sign] ✔ Signed: ${path.basename(filePath)}`);
  } catch (err) {
    console.error(`[sign] ✖ Signing failed: ${err.message}`);
    throw err;
  } finally {
    try { fs.unlinkSync(metaPath); } catch (_) {}
  }
};
