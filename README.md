# Project Argus

**Radar-based shoulder-surfing protection for your screen.**

Project Argus watches the space behind you with a millimeter-wave radar — no
camera — and automatically hides your desktop or locks your screen the moment a
second person enters your space. An optional round display shows a live radar of
who's around you, doubling as a desk clock and weather widget when idle.

> Named for *Argus Panoptes*, the all-seeing, ever-watchful giant of Greek myth.
> (Formerly "Shoulder Guardian.")

---

## How it works

```
HLK-LD2450 radar  →  ESP32 (round display)  →  USB serial  →  Desktop app  →  hide / lock screen
   (mmWave)            counts people, draws radar         interprets, acts on the OS
```

- The **radar** tracks up to 3 people (position + speed) in front of it, ~10×/sec.
- The **device firmware** counts who's in a configurable zone and streams status.
- The **desktop app** decides when it's a threat (2+ people, debounced) and hides
  all windows or locks the workstation — with cooldowns, an approach-direction
  filter, and snooze so legitimate visitors don't constantly trip it.

Because detection is radar-based, it works in the dark, needs no webcam, and
raises none of the privacy concerns of camera/gaze approaches.

---

## Download

Grab the latest signed Windows build from
**[Releases](https://github.com/JoeAWagner/shoulder-guardian/releases/latest)**:

- `Project-Argus-Setup-x.y.z.exe` — installer (recommended; auto-updates)
- `Project-Argus-x.y.z.exe` — portable single-file

Builds are code-signed (Microsoft Trusted Signing) and update themselves via
GitHub Releases.

---

## Desktop app features

- **Threat protection** — hide desktop or lock screen when 2+ people are detected
- **Lock on empty** — lock after you walk away for a configurable delay
- **Snooze** — pause protection for a set duration (auto re-arms); from the app,
  tray, or by tapping the device
- **Approach filter** — ignore people walking *past* behind you; only those
  approaching or lingering count
- **Live radar** view, target list, and event history (persistent audit log)
- **Weather** pushed to the device from NWS (US), by ZIP or IP location
- **Auto-reconnect** to the device, **mini mode**, start-on-login, always-on-top
- System-tray control with at-a-glance status colour

---

## Hardware

**Recommended:** ESP32-2424S012C-I — an all-in-one ESP32-C3 + 1.28" round GC9A01
touch display — plus an **HLK-LD2450** 24 GHz radar on its 4-pin connector.

| LD2450 | Board (4-pin connector) |
|---|---|
| VCC | 5V |
| GND | GND |
| TX | RX pad (GPIO 20) |
| RX | TX pad (GPIO 21) |

Other sketches are included for the Waveshare ESP32-C6-LCD-1.47, ESP32-C3 Super
Mini, Pro Micro, and RP2040-Zero (see `arduino/`). They share the same serial
protocol, so the app works with any of them.

### Round-display features
Live radar with target trails and a sonar ripple; colour themes (green =
disarmed, blue = armed, amber = snoozed, red = threat); tap zones (top = snooze,
bottom = arm/disarm); an idle clock + weather face with auto-dim; and an Argus
boot splash. Font and size are selectable from the app.

---

## Building / flashing

### Desktop app
```bash
npm install
npm start            # run in dev
npm run build-win    # build installer + portable (unsigned without signing env)
npm run release      # tag, build, sign, publish a GitHub Release
```

### Firmware (Arduino IDE)
1. Board: **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled**
   (⚠ Arduino IDE updates sometimes reset this — if the device stops receiving
   commands, re-enable it and reflash.)
2. Library: **LovyanGFX**
3. Open `arduino/esp32_round_disp/esp32_round_disp/esp32_round_disp.ino` and upload.

See `docs/wiring.md` for the other board variants.

---

## Repository layout

| Path | What |
|---|---|
| `main.js`, `preload.js`, `renderer/` | Electron app (main process, bridge, UI) |
| `weather.js` | NWS / Open-Meteo weather + geolocation |
| `arduino/` | Firmware sketches (round display + other boards) |
| `scripts/` | Icon generator, release pipeline, signing |
| `docs/` | Wiring diagrams |

---

© 2026 Wagner Cybersecurity LLC. All rights reserved.
