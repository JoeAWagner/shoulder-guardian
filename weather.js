'use strict';
/**
 * weather.js — pushes current location's weather to the round-display board
 * over the existing serial protocol.
 *
 * Pipeline:
 *   1. IP geolocation (ip-api.com, free, no key) → lat/lon, city
 *   2. Current weather (open-meteo.com, free, no key) → temp °F + WMO code
 *   3. Map WMO code → our 0-8 icon enum (matches firmware's drawWeatherIcon)
 *   4. Write `SET WEATHER <code>,<tempF>°F` over the serial port
 *
 * Refreshes every 15 minutes after the first push.
 */

const REFRESH_MS = 15 * 60 * 1000;

let timer        = null;
let cachedLatLon = null;          // cached after first successful geo lookup
let currentZip   = '';            // user-set ZIP; empty = IP-based auto-detect

// Set / change the ZIP code used for weather.  Empty string reverts to
// IP-based auto-detect.  Clears the location cache so the next lookup
// re-geocodes.
function setZip(zip) {
  const z = String(zip || '').trim();
  if (z !== currentZip) {
    currentZip   = z;
    cachedLatLon = null;
  }
}

// ── Location lookup — ZIP if set, otherwise IP geolocation ────
async function getLocation() {
  if (cachedLatLon) return cachedLatLon;

  // Preferred: user-configured US ZIP code via Zippopotam (free, HTTPS, no key)
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

  // Fallback: IP geolocation (ip-api.com — free, no key, HTTP-only on free tier)
  const r = await fetch('http://ip-api.com/json/?fields=status,message,lat,lon,city,country');
  if (!r.ok) throw new Error(`geo HTTP ${r.status}`);
  const data = await r.json();
  if (data.status !== 'success') throw new Error(`geo: ${data.message || 'unknown'}`);
  cachedLatLon = { lat: data.lat, lon: data.lon, city: data.city };
  return cachedLatLon;
}

// ── Open-Meteo current weather ────────────────────────────────
async function getWeather(lat, lon) {
  const url = `https://api.open-meteo.com/v1/forecast`
            + `?latitude=${lat}&longitude=${lon}`
            + `&current=temperature_2m,weather_code,is_day`
            + `&temperature_unit=fahrenheit`;
  const r = await fetch(url);
  if (!r.ok) throw new Error(`weather HTTP ${r.status}`);
  const data = await r.json();
  if (!data?.current) throw new Error('weather: missing current field');
  return {
    tempF: Math.round(data.current.temperature_2m),
    code:  data.current.weather_code,
    isDay: data.current.is_day === 1,
  };
}

// ── WMO weather code → our 0-8 icon enum ──────────────────────
//   0 sunny | 1 partly cloudy | 2 cloudy | 3 rain | 4 snow
//   5 thunder | 6 fog | 7 clear night | 8 cloudy night
function mapWmoToIcon(wmoCode, isDay) {
  if (wmoCode === 0 || wmoCode === 1)            return isDay ? 0 : 7;  // clear
  if (wmoCode === 2)                              return isDay ? 1 : 8;  // partly cloudy
  if (wmoCode === 3)                              return isDay ? 2 : 8;  // overcast
  if (wmoCode === 45 || wmoCode === 48)           return 6;              // fog
  if (wmoCode >= 51 && wmoCode <= 67)             return 3;              // drizzle / rain
  if (wmoCode >= 71 && wmoCode <= 77)             return 4;              // snow
  if (wmoCode >= 80 && wmoCode <= 82)             return 3;              // rain showers
  if (wmoCode === 85 || wmoCode === 86)           return 4;              // snow showers
  if (wmoCode >= 95)                              return 5;              // thunder
  return 2;                                                              // fallback
}

// ── One-shot push ─────────────────────────────────────────────
async function pushWeatherOnce(sendCmd, log) {
  try {
    const loc  = await getLocation();
    const w    = await getWeather(loc.lat, loc.lon);
    const icon = mapWmoToIcon(w.code, w.isDay);
    // Send plain ASCII — temp digits and unit letter separately.
    // The firmware draws the ° degree symbol itself (the LCD font has none).
    sendCmd(`SET WEATHER ${icon},${w.tempF},F`);
    log && log(`Weather pushed: ${w.tempF}F, icon ${icon} (WMO ${w.code}) for ${loc.city}`);
  } catch (err) {
    log && log(`Weather fetch failed: ${err.message}`);
  }
}

// ── Public API ────────────────────────────────────────────────
function startWeatherSync(sendCmd, log) {
  stopWeatherSync();
  // First push deferred 3 s so the radar/board has time to settle after connect
  setTimeout(() => pushWeatherOnce(sendCmd, log), 3000);
  timer = setInterval(() => pushWeatherOnce(sendCmd, log), REFRESH_MS);
}

function stopWeatherSync() {
  if (timer) { clearInterval(timer); timer = null; }
}

function forceRefresh(sendCmd, log) {
  cachedLatLon = null;   // also re-fetch location
  return pushWeatherOnce(sendCmd, log);
}

module.exports = { startWeatherSync, stopWeatherSync, forceRefresh, setZip };
