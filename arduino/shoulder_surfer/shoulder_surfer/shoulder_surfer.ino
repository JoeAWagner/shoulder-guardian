// ═══════════════════════════════════════════════════════════════
//  Shoulder Surfer — Pro Micro ATmega32U4 + HLK-LD2450
//
//  Wiring — LD2450 radar:
//    LD2450 VCC  → 5V
//    LD2450 GND  → GND
//    LD2450 TX   → Pro Micro pin 0  (Serial1 RX)
//    LD2450 RX   → Pro Micro pin 1  (Serial1 TX)
//    ⚠ Add a 1kΩ/2kΩ voltage divider on pin 0 if your module
//      outputs 5V on TX (most HLK modules are 3.3V tolerant).
//
//  Wiring — GC9A01 1.28" round display (optional, see #define HAS_DISPLAY):
//    GC9A01 VCC  → 3.3V
//    GC9A01 GND  → GND
//    GC9A01 SCL  → Pro Micro pin 15 (SPI SCK)
//    GC9A01 SDA  → Pro Micro pin 16 (SPI MOSI)
//    GC9A01 CS   → Pro Micro pin 10 (TFT_CS)
//    GC9A01 DC   → Pro Micro pin  9 (TFT_DC)
//    GC9A01 RST  → Pro Micro pin  8 (TFT_RST)
//    GC9A01 BLK  → Pro Micro pin  7 (TFT_BLK) or directly to 3.3V
//
//  Protocol:
//    LD2450 → Arduino : 250000 baud binary frames (30 bytes each)
//    Arduino → App    : 115200 baud text STATUS lines
//    App     → Arduino: 115200 baud text SET commands
// ═══════════════════════════════════════════════════════════════

#include <EEPROM.h>

// ═══════════════════════════════════════════════════════════════
//  OPTIONAL: GC9A01 1.28" round TFT display
//
//  To enable the radar display:
//    1. Install "Arduino_GFX_Library" by Moon On Our Nation
//       via Sketch → Include Library → Manage Libraries
//    2. Uncomment the #define HAS_DISPLAY line below
//    3. Wire the display to the pins defined in TFT_* below
//
//  Without a display connected, leave HAS_DISPLAY commented out.
//  The sketch runs identically — no display code is compiled in.
// ═══════════════════════════════════════════════════════════════
// #define HAS_DISPLAY

#ifdef HAS_DISPLAY
#include <Arduino_GFX_Library.h>

// ── Display SPI pins (adjust to match your wiring) ───────────
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8
#define TFT_BLK    7   // backlight PWM pin; wire to 3.3V directly if unused

// ── RGB565 colour palette ─────────────────────────────────────
#define C_BG      0x0882   // #0d1117 — matches app background
#define C_GRID    0x0200   // dark green — range arcs
#define C_LINE    0x0100   // very dark green — centre line
#define C_SENSOR  0x5D1F   // #58a6ff — sensor dot (matches app blue)
#define C_DOT     0x07E0   // bright green — single target
#define C_THREAT  0xF800   // red — ≥2 targets (shoulder-surfer alert)

// ── Radar geometry (240 × 240 round display) ─────────────────
#define RADAR_CX   120     // sensor centre X (middle of screen)
#define RADAR_SY   228     // sensor Y (near bottom edge)
#define RADAR_R    210     // pixel radius that represents maxRangeMm
#define RADAR_ARCS   4     // number of range rings to draw
#define DOT_R        5     // target dot radius in pixels

// ── GFX object ───────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_HWSPI(TFT_DC, TFT_CS);
Arduino_GFX    *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, false /* IPS — set true if display has washed-out/inverted colours */);

struct PrevDot { int16_t px, py; bool active; };
static PrevDot prevDots[3];

// Forward declarations
void initDisplay();
void updateDisplay();
void drawStaticElements();
void mmToPx(int16_t xMm, int16_t yMm, int16_t &px, int16_t &py);

#endif // HAS_DISPLAY

// ── Pin ──────────────────────────────────────────────────────
const int  LED_PIN        = 17;    // Pro Micro RX LED
const bool LED_ACTIVE_LOW = true;  // RX LED lights on LOW

// ── EEPROM layout ────────────────────────────────────────────
const int ADDR_COOL     = 0;  // uint16 — trigger cooldown (seconds)
const int ADDR_LOCKDLY  = 2;  // uint16 — lock delay (seconds)
const int ADDR_MAXRANGE = 4;  // uint16 — max detection range (mm)
const int ADDR_LOCKEN   = 6;  // uint8  — lock on empty enabled
const int ADDR_ENABLED  = 7;  // uint8  — protection enabled
const int ADDR_MAXX     = 8;  // uint16 — max X offset from center (mm)

// ── Settings ─────────────────────────────────────────────────
uint16_t cooldownSec  = 5;     // seconds between minimize triggers
uint16_t lockDelaySec = 30;    // seconds of no presence before locking
uint16_t maxRangeMm   = 2000;  // ignore targets beyond this depth (mm)
uint16_t maxXMm       = 2000;  // ignore targets outside ±this from center (mm)
bool     lockEnabled  = false;
bool     enabled      = true;

// ── LD2450 frame ─────────────────────────────────────────────
const int     FRAME_LEN = 30;
uint8_t       frameBuf[FRAME_LEN];
int           frameIdx  = 0;

struct Target {
  int16_t  x;      // mm — negative=left, positive=right of sensor
  int16_t  y;      // mm — positive = distance from sensor
  int16_t  speed;  // cm/s — positive=approaching, negative=receding
  bool     valid;
};

Target  targets[3];
int     activeCount = 0;

// ── Non-blocking serial command buffer ───────────────────────
char    cmdBuf[64];
uint8_t cmdIdx = 0;

// ── Timing ───────────────────────────────────────────────────
unsigned long statusTimer   = 0;
const unsigned long STATUS_WATCHDOG_MS = 150; // fallback if no frames arrive

// ════════════════════════════════════════════════════════════

void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledWrite(false);

  Serial.begin(115200);   // USB → Electron app
  Serial1.begin(250000);  // LD2450 binary frames

  memset(targets, 0, sizeof(targets));
  loadSettings();
  configLD2450();
#ifdef HAS_DISPLAY
  initDisplay();
#endif
  startupBlink();
}

void loop() {
  unsigned long now = millis();

  readLD2450();   // non-blocking — sends STATUS immediately on each new frame
  handleSerial(); // non-blocking — accumulates chars, processes on '\n'

  // LED: solid = protected & targets present, slow blink = empty, off = disabled
  if (enabled) {
    ledWrite(activeCount > 0 ? true : (now / 500) % 2 == 0);
  } else {
    ledWrite((now / 1000) % 2 == 0);
  }

  // Watchdog: send STATUS if no LD2450 frame has arrived recently
  // (keeps the app responsive even if radar is disconnected)
  if (now - statusTimer >= STATUS_WATCHDOG_MS) {
    statusTimer = now;
    sendStatus();
  }
}

// ── LD2450 startup config: enable multi-target mode ──────────
void configLD2450() {
  delay(2000);  // wait for sensor to fully boot

  // Send sequence twice for reliability
  for (int pass = 0; pass < 2; pass++) {
    // 1. Enter config mode
    uint8_t enterCfg[] = {0xFD,0xFC,0xFB,0xFA, 0x04,0x00, 0xFF,0x00,0x01,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(enterCfg, sizeof(enterCfg));
    delay(300);

    // 2. Set multi-target tracking mode (command 0x0090)
    //    Single target would be 0x0080 — these are separate commands, no value byte
    uint8_t multiTgt[] = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0x90,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(multiTgt, sizeof(multiTgt));
    delay(300);

    // 3. Exit config mode
    uint8_t exitCfg[] = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0xFE,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(exitCfg, sizeof(exitCfg));
    delay(500);

    // Flush ACK frames between passes
    while (Serial1.available()) Serial1.read();
  }
}

// ── LD2450 frame state machine (non-blocking) ─────────────────
void readLD2450() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    // Re-sync to header if needed
    if (frameIdx == 0 && b != 0xAA) continue;
    if (frameIdx == 1 && b != 0xFF) { frameIdx = 0; continue; }
    if (frameIdx == 2 && b != 0x03) { frameIdx = 0; continue; }
    if (frameIdx == 3 && b != 0x00) { frameIdx = 0; continue; }

    frameBuf[frameIdx++] = b;

    if (frameIdx == FRAME_LEN) {
      frameIdx = 0;
      if (frameBuf[28] == 0x55 && frameBuf[29] == 0xCC) {
        parseFrame();
        // Send STATUS immediately after parsing — reduces latency from
        // up to 100ms (timer-based) down to ~1ms (right after frame arrives)
        sendStatus();
        statusTimer = millis(); // reset watchdog
      }
    }
  }
}

// LD2450 uses sign-magnitude encoding, NOT two's complement.
// Bit 15 = sign flag (1 = negative for X/Speed; 1 = "present" for Y).
// Bits 0-14 = magnitude in mm (X/Y) or cm/s (Speed).
static int16_t decodeSM(uint16_t raw) {
  int16_t mag = (int16_t)(raw & 0x7FFF);
  return (raw & 0x8000) ? -mag : mag;
}

void parseFrame() {
  activeCount = 0;
  for (int i = 0; i < 3; i++) {
    int base = 4 + i * 8;
    uint16_t rawX = (uint16_t)(frameBuf[base]   | (frameBuf[base+1] << 8));
    uint16_t rawY = (uint16_t)(frameBuf[base+2] | (frameBuf[base+3] << 8));
    uint16_t rawS = (uint16_t)(frameBuf[base+4] | (frameBuf[base+5] << 8));

    // Y bit-15 is a presence flag, not a negative sign — Y is always >= 0
    targets[i].x     = decodeSM(rawX);
    targets[i].y     = (int16_t)(rawY & 0x7FFF);
    targets[i].speed = decodeSM(rawS);

    // Valid = non-zero data, within depth range, within X width
    targets[i].valid = (rawX != 0 || rawY != 0)
                       && (targets[i].y <= (int16_t)maxRangeMm)
                       && (targets[i].y > 0)
                       && (abs(targets[i].x) <= (int16_t)maxXMm);
    if (targets[i].valid) activeCount++;
  }
#ifdef HAS_DISPLAY
  updateDisplay();
#endif
}

// ── STATUS to app ─────────────────────────────────────────────
void sendStatus() {
  Serial.print("STATUS:");
  Serial.print("count=");    Serial.print(activeCount);
  Serial.print(",enabled="); Serial.print(enabled ? 1 : 0);
  Serial.print(",locken=");  Serial.print(lockEnabled ? 1 : 0);
  Serial.print(",cool=");    Serial.print(cooldownSec);
  Serial.print(",lockdly="); Serial.print(lockDelaySec);
  Serial.print(",maxrange=");Serial.print(maxRangeMm);
  Serial.print(",maxx=");    Serial.print(maxXMm);

  for (int i = 0; i < 3; i++) {
    Serial.print(",t"); Serial.print(i); Serial.print("x=");
    Serial.print(targets[i].valid ? targets[i].x : 0);
    Serial.print(",t"); Serial.print(i); Serial.print("y=");
    Serial.print(targets[i].valid ? targets[i].y : 0);
    Serial.print(",t"); Serial.print(i); Serial.print("s=");
    Serial.print(targets[i].valid ? targets[i].speed : 0);
  }
  Serial.println();
}

// ── Non-blocking serial command handler ───────────────────────
// Accumulates characters into cmdBuf and processes on newline.
// Unlike readStringUntil(), this never blocks the main loop —
// so readLD2450() keeps running even while commands arrive.
void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdIdx > 0) {
        cmdBuf[cmdIdx] = '\0';
        processCommand(cmdBuf);
        cmdIdx = 0;
      }
    } else if (cmdIdx < (uint8_t)(sizeof(cmdBuf) - 1)) {
      cmdBuf[cmdIdx++] = c;
    }
  }
}

void processCommand(const char* cmd) {
  if (strncmp(cmd, "SET COOL ", 9) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 9);
    if (v >= 1 && v <= 300) {
      cooldownSec = v;
      EEPROM.put(ADDR_COOL, cooldownSec);
      Serial.println("OK:COOL=" + String(v));
    }
  } else if (strncmp(cmd, "SET LOCKDLY ", 12) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 12);
    if (v >= 5 && v <= 120) {
      lockDelaySec = v;
      EEPROM.put(ADDR_LOCKDLY, lockDelaySec);
      Serial.println("OK:LOCKDLY=" + String(v));
    }
  } else if (strncmp(cmd, "SET MAXRANGE ", 13) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 13);
    if (v >= 500 && v <= 5000) {
      maxRangeMm = v;
      EEPROM.put(ADDR_MAXRANGE, maxRangeMm);
      Serial.println("OK:MAXRANGE=" + String(v));
    }
  } else if (strncmp(cmd, "SET MAXX ", 9) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 9);
    if (v >= 200 && v <= 5000) {
      maxXMm = v;
      EEPROM.put(ADDR_MAXX, maxXMm);
      Serial.println("OK:MAXX=" + String(v));
    }
  } else if (strncmp(cmd, "SET LOCKEN ", 11) == 0) {
    lockEnabled = atoi(cmd + 11) != 0;
    EEPROM.put(ADDR_LOCKEN, (uint8_t)lockEnabled);
    Serial.println("OK:LOCKEN=" + String(lockEnabled ? 1 : 0));
  } else if (strcmp(cmd, "DEBUG") == 0) {
    for (int i = 0; i < 3; i++) {
      Serial.print("RAW t"); Serial.print(i);
      Serial.print(": x="); Serial.print(targets[i].x);
      Serial.print(" y=");  Serial.print(targets[i].y);
      Serial.print(" s=");  Serial.print(targets[i].speed);
      Serial.print(" valid="); Serial.println(targets[i].valid ? 1 : 0);
    }
    Serial.print("maxRange="); Serial.print(maxRangeMm);
    Serial.print(" maxX=");    Serial.println(maxXMm);
  } else if (strcmp(cmd, "ENABLE") == 0) {
    enabled = true;
    EEPROM.put(ADDR_ENABLED, (uint8_t)1);
    Serial.println("OK:ENABLED");
  } else if (strcmp(cmd, "DISABLE") == 0) {
    enabled = false;
    EEPROM.put(ADDR_ENABLED, (uint8_t)0);
    Serial.println("OK:DISABLED");
  }
}

// ── EEPROM load ───────────────────────────────────────────────
void loadSettings() {
  uint16_t u16; uint8_t u8;

  EEPROM.get(ADDR_COOL,     u16); if (u16 != 0xFFFF && u16 >= 1   && u16 <= 300)  cooldownSec  = u16;
  EEPROM.get(ADDR_LOCKDLY,  u16); if (u16 != 0xFFFF && u16 >= 5   && u16 <= 120)  lockDelaySec = u16;
  EEPROM.get(ADDR_MAXRANGE, u16); if (u16 != 0xFFFF && u16 >= 500  && u16 <= 5000) maxRangeMm = u16;
  EEPROM.get(ADDR_MAXX,     u16); if (u16 != 0xFFFF && u16 >= 200  && u16 <= 5000) maxXMm     = u16;
  EEPROM.get(ADDR_LOCKEN,   u8);  if (u8  != 0xFF)   lockEnabled = u8;
  EEPROM.get(ADDR_ENABLED,  u8);  if (u8  != 0xFF)   enabled     = u8;
}

// ── LED ───────────────────────────────────────────────────────
void ledWrite(bool on) {
  digitalWrite(LED_PIN, (LED_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
}

void startupBlink() {
  for (int i = 0; i < 3; i++) { ledWrite(true); delay(100); ledWrite(false); delay(100); }
}

// ── Display functions (compiled only when HAS_DISPLAY is set) ─
#ifdef HAS_DISPLAY

// Convert mm radar coords → screen pixel coords.
// X: negative = left of sensor, positive = right.
// Y: always positive (distance from sensor).
// Sensor sits at (RADAR_CX, RADAR_SY); targets are drawn above it.
void mmToPx(int16_t xMm, int16_t yMm, int16_t &px, int16_t &py) {
  px = (int16_t)(RADAR_CX + ((int32_t)xMm * RADAR_R) / (int32_t)maxRangeMm);
  py = (int16_t)(RADAR_SY - ((int32_t)yMm * RADAR_R) / (int32_t)maxRangeMm);
}

// Draw range arcs, centre line and sensor dot.
// Called once during init and again each frame after erasing old dots
// (to restore any arc/line pixels the erase fillCircle may have overwritten).
void drawStaticElements() {
  // Range rings — full circles centred at sensor position.
  // The lower halves extend below the screen edge and are naturally clipped.
  for (int i = 1; i <= RADAR_ARCS; i++) {
    int16_t r = (int16_t)((uint32_t)RADAR_R * i / RADAR_ARCS);
    gfx->drawCircle(RADAR_CX, RADAR_SY, r, C_GRID);
  }
  // Mask the small strip below the sensor row to hide any arc fragments
  // that land inside the visible area (RADAR_SY to the display bottom).
  gfx->fillRect(0, RADAR_SY + 1, 240, 240 - RADAR_SY - 1, C_BG);
  // Vertical centre line from top of display down to sensor
  gfx->drawFastVLine(RADAR_CX, 0, RADAR_SY, C_LINE);
  // Sensor dot on top of everything else
  gfx->fillCircle(RADAR_CX, RADAR_SY, 4, C_SENSOR);
}

// One-time display initialisation called from setup().
// Safe to call even if no display is physically connected:
// SPI writes go to void, nothing crashes.
void initDisplay() {
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);   // turn on backlight
  gfx->begin();                  // init SPI + send GC9A01 init sequence
  gfx->fillScreen(C_BG);
  drawStaticElements();
  memset(prevDots, 0, sizeof(prevDots));  // mark all slots inactive
}

// Called from parseFrame() after targets[] and activeCount are updated.
// Strategy: erase old dots → redraw static elements → draw new dots.
// No framebuffer needed — only the changed pixels are touched.
void updateDisplay() {
  // 1. Erase previous dot positions
  for (int i = 0; i < 3; i++) {
    if (prevDots[i].active) {
      gfx->fillCircle(prevDots[i].px, prevDots[i].py, DOT_R + 1, C_BG);
    }
  }

  // 2. Restore any static elements the erase may have clipped
  drawStaticElements();

  // 3. Colour: green for a lone presence, red when ≥2 (shoulder-surfer alert)
  uint16_t dotCol = (activeCount >= 2) ? C_THREAT : C_DOT;

  // 4. Draw new dots and record their positions for the next erase pass
  for (int i = 0; i < 3; i++) {
    if (targets[i].valid) {
      int16_t px, py;
      mmToPx(targets[i].x, targets[i].y, px, py);
      // Clamp to the radar field (above sensor row, inside screen width)
      px = constrain(px, 0, 239);
      py = constrain(py, 0, RADAR_SY - DOT_R - 1);
      gfx->fillCircle(px, py, DOT_R, dotCol);
      prevDots[i].px     = px;
      prevDots[i].py     = py;
      prevDots[i].active = true;
    } else {
      prevDots[i].active = false;
    }
  }
}

#endif // HAS_DISPLAY
