'use strict';
/**
 * weather.js — pushes current weather to the round-display board.
 *
 * Pipeline:
 *   1. Location  — ZIP code (Zippopotam) if set, else IP geolocation (ip-api)
 *   2. Weather   — NWS / weather.gov (real station observations, US, no key)
 *                  → falls back to Open-Meteo if NWS has no usable station
 *   3. Map condition → our 0-8 icon enum (matches firmware drawWeatherIcon)
 *   4. Write `SET WEATHER <code>,<temp>,F` over the serial port
 *
 * Refreshes every 15 minutes.
 */

const REFRESH_MS = 15 * 60 * 1000;

// NWS requires a User-Agent identifying the app + a contact.
const NWS_UA = 'ShoulderGuardian/1.0 (github.com/JoeAWagner/shoulder-guardian)';

let timer        = null;
let cachedLatLon = null;          // cached after first geo lookup
let cachedNWS    = null;          // cached NWS station URLs for the location
let currentZip   = '';            // user-set ZIP; empty = IP auto-detect

// ── ZIP code control ──────────────────────────────────────────
function setZip(zip) {
  const z = String(zip || '').trim();
  if (z !== currentZip) {
    currentZip   = z;
    cachedLatLon = null;          // force re-geocode
    cachedNWS    = null;          // and re-resolve NWS stations
  }
}

// ── Small fetch helper ────────────────────────────────────────
async function fetchJson(url, headers) {
  const r = await fetch(url, headers ? { headers } : undefined);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

// ── Location lookup — ZIP if set, otherwise IP geolocation ────
async function getLocation() {
  if (cachedLatLon) return cachedLatLon;

  if (currentZip) {
    const r = await fetch(`https://api.zippopotam.us/us/${encodeURIComponent(currentZip)}`);
    if (r.status === 404) throw new Error(`ZIP "${currentZip}" not found`);
    if (!r.ok) throw new Error(`ZIP lookup HTTP ${r.status}`);
    const data  = await r.json();
    const place = data.places && data.places[0];
    if (!place) throw new Error(`ZIP "${currentZip}" returned no location`);
    cachedLatLon = {
      lat:  parseFloat(place.latitude),
      lon:  parseFloat(place.longitude),
      city: place['place name'] || currentZip,
    };
    return cachedLatLon;
  }

  const r = await fetch('http://ip-api.com/json/?fields=status,message,lat,lon,city,country');
  if (!r.ok) throw new Error(`geo HTTP ${r.status}`);
  const data = await r.json();
  if (data.status !== 'success') throw new Error(`geo: ${data.message || 'unknown'}`);
  cachedLatLon = { lat: data.lat, lon: data.lon, city: data.city };
  return cachedLatLon;
}

// ── NWS condition icon → our 0-8 enum ────────────────────────
// NWS observation icon URLs look like:
//   https://api.weather.gov/icons/land/day/tsra,40?size=medium
// We pull the day/night flag and the condition code from the path.
function parseNWSIcon(iconUrl) {
  if (!iconUrl) return { isDay: true, cond: '' };
  const m = iconUrl.match(/\/icons\/land\/(day|night)\/([a-z_]+)/i);
  if (!m) return { isDay: true, cond: '' };
  return { isDay: m[1].toLowerCase() === 'day', cond: m[2].toLowerCase() };
}

function nwsCondToIcon(cond, isDay) {
  const c = (cond || '').replace(/^wind_/, '');   // strip "wind_" variants
  if (c.includes('tsra'))                                          return 5; // thunder
  if (c.includes('blizzard') || c.includes('snow') || c.includes('flurries')) return 4; // snow
  if (c.includes('rain') || c.includes('showers') ||
      c.includes('sleet') || c.includes('fzra') || c.includes('drizzle'))     return 3; // rain
  if (c.includes('fog') || c.includes('haze') ||
      c.includes('smoke') || c.includes('dust'))                   return 6; // fog
  if (c === 'skc' || c === 'few' || c === 'hot' || c === 'cold')   return isDay ? 0 : 7; // clear
  if (c === 'sct')                                                 return isDay ? 1 : 8; // partly
  if (c === 'bkn' || c === 'ovc')                                  return isDay ? 2 : 8; // cloudy
  return isDay ? 2 : 8;                                                       // default cloudy
}

// ── NWS current observation ───────────────────────────────────
async function getNWSWeather(lat, lon) {
  const H = { 'User-Agent': NWS_UA, 'Accept': 'application/geo+json' };

  // Resolve nearby observation stations (cached per location)
  if (!cachedNWS) {
    const pts = await fetchJson(
      `https://api.weather.gov/points/${lat.toFixed(4)},${lon.toFixed(4)}`, H);
    const stUrl = pts?.properties?.observationStations;
    if (!stUrl) throw new Error('NWS: no station list for this point');
    const stData = await fetchJson(stUrl, H);
    const urls = stData.observationStations || [];
    if (!urls.length) throw new Error('NWS: no stations nearby');
    cachedNWS = { stationUrls: urls.slice(0, 4) };   // keep the 4 closest
  }

  // Try the closest few stations until one reports a temperature
  for (const sUrl of cachedNWS.stationUrls) {
    let obs;
    try {
      obs = await fetchJson(`${sUrl}/observations/latest`, H);
    } catch (_) { continue; }
    const tC = obs?.properties?.temperature?.value;
    if (tC == null) continue;                        // station didn't report temp
    const { isDay, cond } = parseNWSIcon(obs.properties.icon);
    return {
      tempF: Math.round(tC * 9 / 5 + 32),
      icon:  nwsCondToIcon(cond, isDay),
      desc:  obs.properties.textDescription || cond || 'NWS',
    };
  }
  throw new Error('NWS: no station reported a temperature');
}

// ── Open-Meteo fallback ───────────────────────────────────────
function mapWmoToIcon(wmoCode, isDay) {
  if (wmoCode === 0 || wmoCode === 1)   return isDay ? 0 : 7;
  if (wmoCode === 2)                    return isDay ? 1 : 8;
  if (wmoCode === 3)                    return isDay ? 2 : 8;
  if (wmoCode === 45 || wmoCode === 48) return 6;
  if (wmoCode >= 51 && wmoCode <= 67)   return 3;
  if (wmoCode >= 71 && wmoCode <= 77)   return 4;
  if (wmoCode >= 80 && wmoCode <= 82)   return 3;
  if (wmoCode === 85 || wmoCode === 86) return 4;
  if (wmoCode >= 95)                    return 5;
  return 2;
}

async function getOpenMeteoWeather(lat, lon) {
  const url = `https://api.open-meteo.com/v1/forecast`
            + `?latitude=${lat}&longitude=${lon}`
            + `&current=temperature_2m,weather_code,is_day`
            + `&temperature_unit=fahrenheit`;
  const data = await fetchJson(url);
  if (!data?.current) throw new Error('open-meteo: missing current field');
  return {
    tempF: Math.round(data.current.temperature_2m),
    icon:  mapWmoToIcon(data.current.weather_code, data.current.is_day === 1),
    desc:  'open-meteo',
  };
}

// ── One-shot push ─────────────────────────────────────────────
async function pushWeatherOnce(sendCmd, log) {
  try {
    const loc = await getLocation();
    let w, src;
    try {
      w   = await getNWSWeather(loc.lat, loc.lon);
      src = 'NWS';
    } catch (nwsErr) {
      w   = await getOpenMeteoWeather(loc.lat, loc.lon);
      src = `Open-Meteo (NWS unavailable: ${nwsErr.message})`;
    }
    sendCmd(`SET WEATHER ${w.icon},${w.tempF},F`);
    log && log(`Weather pushed: ${w.tempF}F (${w.desc}), icon ${w.icon} via ${src} for ${loc.city}`);
  } catch (err) {
    log && log(`Weather fetch failed: ${err.message}`);
  }
}

// ── Public API ────────────────────────────────────────────────
function startWeatherSync(sendCmd, log) {
  stopWeatherSync();
  setTimeout(() => pushWeatherOnce(sendCmd, log), 3000);
  timer = setInterval(() => pushWeatherOnce(sendCmd, log), REFRESH_MS);
}

function stopWeatherSync() {
  if (timer) { clearInterval(timer); timer = null; }
}

function forceRefresh(sendCmd, log) {
  cachedLatLon = null;
  cachedNWS    = null;
  return pushWeatherOnce(sendCmd, log);
}

module.exports = { startWeatherSync, stopWeatherSync, forceRefresh, setZip };
