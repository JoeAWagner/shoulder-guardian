// ═══════════════════════════════════════════════════════════════
//  Shoulder Guardian — ESP32 Round Disp (ESP32-2424S012C-I)
//
//  All-in-one round display module: ESP32-C3-MINI + 1.28" GC9A01
//  240×240 IPS LCD on the same PCB (no breadboard SPI).
//
//  Same STATUS / SET-command protocol as the other variants — the
//  Electron app works unchanged.
//
//  ── Onboard wiring (already on PCB) ──────────────────────────
//    GC9A01 LCD — MOSI=7 SCK=6 DC=2 CS=10 RST=tied BL=3
//    CST816S touch (unused here) — SDA=4 SCL=5 INT=0 RST=1
//
//  ── External wiring — HLK-LD2450 radar (4-pin connector) ─────
//    LD2450 VCC  → 3.3V or 5V depending on your wiring/source
//                  (datasheet specifies 5V; works at 3.3V on some units)
//    LD2450 GND  → GND
//    LD2450 TX   → board "RX" pad on the 4-pin connector  (GPIO 20)
//    LD2450 RX   → board "TX" pad on the 4-pin connector  (GPIO 21)
//
//  ── Arduino IDE setup ────────────────────────────────────────
//    Board Manager : "esp32" by Espressif Systems v3.0 OR NEWER
//    Board         : "ESP32C3 Dev Module"
//    USB CDC On Boot: "Enabled"
//    Libraries     : LovyanGFX
// ═══════════════════════════════════════════════════════════════

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <EEPROM.h>
#include <Wire.h>

// CST816S touch — talked to directly via Wire; LovyanGFX's driver
// doesn't always init this chip on these boards.
#define TOUCH_SDA   4
#define TOUCH_SCL   5
#define TOUCH_INT   0
#define TOUCH_RST   1
#define CST816S_ADDR 0x15

// ── Pins ──────────────────────────────────────────────────────
#define BL_PIN      3      // LCD backlight (PWM)

// LD2450 UART — see board-label note in the C6 sketch.  Board pad "TX"
// (GPIO 21, U0TXD) is the board's transmit; pad "RX" (GPIO 20, U0RXD)
// is the board's receive.  After crossing TX↔RX between board ↔ radar,
// our firmware reads on GPIO 20 and writes on GPIO 21.
#define LD_RX       20     // Serial1 RX  ← LD2450 TX  (board pad "RX")
#define LD_TX       21     // Serial1 TX  → LD2450 RX  (board pad "TX")

// ── EEPROM ────────────────────────────────────────────────────
#define EEPROM_SIZE  16
const int ADDR_COOL     = 0;
const int ADDR_LOCKDLY  = 2;
const int ADDR_MAXRANGE = 4;
const int ADDR_LOCKEN   = 6;
const int ADDR_ENABLED  = 7;
const int ADDR_MAXX     = 8;

// ── Settings ─────────────────────────────────────────────────
uint16_t cooldownSec  = 5;
uint16_t lockDelaySec = 30;
uint16_t maxRangeMm   = 2000;
uint16_t maxXMm       = 2000;
bool     lockEnabled  = false;
bool     enabled      = true;

// ── LD2450 frame ─────────────────────────────────────────────
const int FRAME_LEN = 30;
uint8_t   frameBuf[FRAME_LEN];
int       frameIdx  = 0;

struct Target {
  int16_t x, y, speed;
  bool    valid;
};
Target targets[3];
int    activeCount = 0;

// Per-slot smoothed positions for on-screen dots (raw goes out via STATUS).
float smoothX[3] = {0, 0, 0};
float smoothY[3] = {0, 0, 0};
bool  smoothInit[3] = {false, false, false};
const float SMOOTH_ALPHA = 0.20f;

// ── Non-blocking serial command buffer ───────────────────────
char    cmdBuf[64];
uint8_t cmdIdx = 0;

// ── Timing ───────────────────────────────────────────────────
unsigned long statusTimer = 0;
const unsigned long STATUS_WATCHDOG_MS = 150;

// ── Diagnostics (visible on screen) ──────────────────────────
uint32_t      ld_bytes_total = 0;
uint32_t      ld_frames_ok   = 0;
unsigned long lastByteMs     = 0;
uint32_t      touch_count    = 0;     // how many times getTouch() returned true
int16_t       touch_last_x   = 0;
int16_t       touch_last_y   = 0;

// ── Weather widget — pushed by the Electron app ──────────────
//   Command: SET WEATHER <code>,<temp>,<unit>
//     code: 0 sunny, 1 partly cloudy, 2 cloudy, 3 rain, 4 snow,
//           5 thunder, 6 fog, 7 clear night, 8 cloudy night
//     temp: digits only (no degree symbol — that's drawn by the firmware)
//     unit: single char, 'F' or 'C'
//   e.g. SET WEATHER 0,72,F
uint8_t  weatherCode        = 0;
char     weatherTempStr[8]  = "";   // digits only, e.g. "72"
char     weatherUnit        = 'F';
bool     weatherSet         = false;

// ── LovyanGFX panel — GC9A01 240×240 IPS, no touch (manual via Wire) ─
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01  _panel;
  lgfx::Bus_SPI       _bus;
public:
  LGFX(void) {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.spi_3wire   = true;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 6;
      cfg.pin_mosi    = 7;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = 10;
      cfg.pin_rst       = -1;       // tied to MCU reset on this board
      cfg.pin_busy      = -1;
      cfg.panel_width   = 240;
      cfg.panel_height  = 240;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.invert        = true;     // IPS panel
      cfg.rgb_order     = false;    // this board's GC9A01 is BGR-ordered
      cfg.bus_shared    = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX        tft;
LGFX_Sprite fb(&tft);

// ── Colours (standard RGB565) ────────────────────────────────
#define C_BG          0x0000
#define C_TEXT        0xC618

// Three themes — the whole UI flips colour based on state:
//   GREEN  = disarmed (touch toggled off, no app traffic going out)
//   BLUE   = armed, area clear or 1 target
//   RED    = armed, 2+ targets (threat)

// Green theme — disarmed (calm/safe)
#define C_GREEN_ARC    0x05E0   // medium green arcs
#define C_GREEN_LINE   0x0240   // dim green centre line
#define C_GREEN_SENSOR 0x07E0   // bright green sensor dot
#define C_GREEN_DOT    0x07E0   // target dots also green (not actively armed)

// Blue theme — armed, no threat
#define C_BLUE_ARC    0x04BF    // medium blue, slight cyan tint
#define C_BLUE_LINE   0x000F    // dim blue
#define C_BLUE_SENSOR 0x07FF    // bright cyan sensor dot
#define C_TARGET_DOT  0x07E0    // single-target green

// Red theme — threat alert
#define C_RED_ARC     0xF800    // full red
#define C_RED_LINE    0x7800    // dim red
#define C_RED_SENSOR  0xFFE0    // yellow sensor dot
#define C_RED_DOT     0xF800    // multi-target red

// ── Radar geometry (240×240 round) ───────────────────────────
#define SCREEN_W   240
#define SCREEN_H   240
#define RADAR_CX   120
#define RADAR_SY   228
#define RADAR_R    210
#define RADAR_ARCS   4
#define DOT_R        6

// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[SG] ESP32 Round Disp starting...");

  pinMode(BL_PIN, OUTPUT);
  analogWrite(BL_PIN, 200); // ~80% brightness

  Serial.println("[SG] init display...");
  tft.init();
  //tft.setRotation(2);   // USB connector on the right side of the device
  tft.fillScreen(C_BG);

  fb.setColorDepth(16);
  if (!fb.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("[SG] sprite alloc failed — falling back to direct draw");
  }
  drawRadar();

  // ── CST816S touch — manual init ────────────────────────────
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(20);
  digitalWrite(TOUCH_RST, HIGH);
  delay(100);                         // chip needs ~50 ms to come out of reset
  pinMode(TOUCH_INT, INPUT_PULLUP);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);

  // I²C bus scan — prints any addresses that ACK
  Serial.println("[i2c] scanning bus...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c]   device at 0x%02X\n", addr);
    }
  }
  Serial.println("[i2c] scan done");

  Serial1.begin(256000, SERIAL_8N1, LD_RX, LD_TX);
  EEPROM.begin(EEPROM_SIZE);
  memset(targets, 0, sizeof(targets));
  loadSettings();

  // Note: configLD2450() is intentionally NOT called.  The newer LD2450
  // firmware revisions default to multi-target tracking already, and the
  // legacy enter-config / multi-target / exit-config sequence appears to
  // put the new firmware into a state where it stops emitting frames.
  // Leaving the radar at its power-on defaults works perfectly.
  Serial.println("[SG] ready");
}

void loop() {
  unsigned long now = millis();

  readLD2450();
  handleSerial();
  handleTouch(now);

  if (now - statusTimer >= STATUS_WATCHDOG_MS) {
    statusTimer = now;
    sendStatus();
  }

  // Refresh radar 4× per second so diagnostics stay live.
  static unsigned long lastDraw = 0;
  if (now - lastDraw > 250) {
    lastDraw = now;
    drawRadar();
  }
}

// ── LD2450 startup: enable multi-target mode ─────────────────
void configLD2450() {
  delay(2000);
  for (int pass = 0; pass < 2; pass++) {
    uint8_t enterCfg[] = {0xFD,0xFC,0xFB,0xFA, 0x04,0x00, 0xFF,0x00,0x01,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(enterCfg, sizeof(enterCfg));
    delay(300);
    uint8_t multiTgt[] = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0x90,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(multiTgt, sizeof(multiTgt));
    delay(300);
    uint8_t exitCfg[]  = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0xFE,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(exitCfg, sizeof(exitCfg));
    delay(500);
    while (Serial1.available()) Serial1.read();
  }
}

void readLD2450() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    ld_bytes_total++;
    lastByteMs = millis();
    if (frameIdx == 0 && b != 0xAA) continue;
    if (frameIdx == 1 && b != 0xFF) { frameIdx = 0; continue; }
    if (frameIdx == 2 && b != 0x03) { frameIdx = 0; continue; }
    if (frameIdx == 3 && b != 0x00) { frameIdx = 0; continue; }
    frameBuf[frameIdx++] = b;
    if (frameIdx == FRAME_LEN) {
      frameIdx = 0;
      if (frameBuf[28] == 0x55 && frameBuf[29] == 0xCC) {
        ld_frames_ok++;
        parseFrame();
        sendStatus();
        statusTimer = millis();
      }
    }
  }
}

static int16_t decodeSM(uint16_t raw) {
  int16_t mag = (int16_t)(raw & 0x7FFF);
  return (raw & 0x8000) ? -mag : mag;
}

void parseFrame() {
  activeCount = 0;
  for (int i = 0; i < 3; i++) {
    int      base = 4 + i * 8;
    uint16_t rawX = (uint16_t)(frameBuf[base]   | (frameBuf[base+1] << 8));
    uint16_t rawY = (uint16_t)(frameBuf[base+2] | (frameBuf[base+3] << 8));
    uint16_t rawS = (uint16_t)(frameBuf[base+4] | (frameBuf[base+5] << 8));
    targets[i].x     = decodeSM(rawX);
    targets[i].y     = (int16_t)(rawY & 0x7FFF);
    targets[i].speed = decodeSM(rawS);
    targets[i].valid = (rawX != 0 || rawY != 0)
                       && targets[i].y <= (int16_t)maxRangeMm
                       && targets[i].y > 0
                       && abs(targets[i].x) <= (int16_t)maxXMm;
    if (targets[i].valid) activeCount++;

    if (targets[i].valid) {
      if (smoothInit[i]) {
        smoothX[i] = SMOOTH_ALPHA * targets[i].x + (1.0f - SMOOTH_ALPHA) * smoothX[i];
        smoothY[i] = SMOOTH_ALPHA * targets[i].y + (1.0f - SMOOTH_ALPHA) * smoothY[i];
      } else {
        smoothX[i]    = targets[i].x;
        smoothY[i]    = targets[i].y;
        smoothInit[i] = true;
      }
    } else {
      smoothInit[i] = false;
    }
  }
  drawRadar();
}

void sendStatus() {
  // When disarmed, stay silent — no STATUS frames go to the app, so
  // the desktop never gets a count > 0 and won't lock the screen.
  // The OK:DISABLED / OK:ENABLED acks still fire on toggle so the app
  // knows the state change happened.
  if (!enabled) return;
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

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdIdx > 0) { cmdBuf[cmdIdx] = '\0'; processCommand(cmdBuf); cmdIdx = 0; }
    } else if (cmdIdx < (uint8_t)(sizeof(cmdBuf) - 1)) {
      cmdBuf[cmdIdx++] = c;
    }
  }
}

void processCommand(const char* cmd) {
  if (strncmp(cmd, "SET COOL ", 9) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 9);
    if (v >= 1 && v <= 300) {
      cooldownSec = v; EEPROM.put(ADDR_COOL, v); EEPROM.commit();
      Serial.println("OK:COOL=" + String(v));
    }
  } else if (strncmp(cmd, "SET LOCKDLY ", 12) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 12);
    if (v >= 5 && v <= 120) {
      lockDelaySec = v; EEPROM.put(ADDR_LOCKDLY, v); EEPROM.commit();
      Serial.println("OK:LOCKDLY=" + String(v));
    }
  } else if (strncmp(cmd, "SET MAXRANGE ", 13) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 13);
    if (v >= 500 && v <= 5000) {
      maxRangeMm = v; EEPROM.put(ADDR_MAXRANGE, v); EEPROM.commit();
      Serial.println("OK:MAXRANGE=" + String(v));
    }
  } else if (strncmp(cmd, "SET MAXX ", 9) == 0) {
    uint16_t v = (uint16_t)atoi(cmd + 9);
    if (v >= 200 && v <= 5000) {
      maxXMm = v; EEPROM.put(ADDR_MAXX, v); EEPROM.commit();
      Serial.println("OK:MAXX=" + String(v));
    }
  } else if (strncmp(cmd, "SET LOCKEN ", 11) == 0) {
    lockEnabled = atoi(cmd + 11) != 0;
    EEPROM.put(ADDR_LOCKEN, (uint8_t)lockEnabled); EEPROM.commit();
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
    enabled = true; EEPROM.put(ADDR_ENABLED, (uint8_t)1); EEPROM.commit();
    Serial.println("OK:ENABLED");
  } else if (strcmp(cmd, "DISABLE") == 0) {
    enabled = false; EEPROM.put(ADDR_ENABLED, (uint8_t)0); EEPROM.commit();
    Serial.println("OK:DISABLED");
  } else if (strncmp(cmd, "SET WEATHER ", 12) == 0) {
    // Format: SET WEATHER <code>,<temp>,<unit>    e.g. SET WEATHER 0,72,F
    const char *p  = cmd + 12;
    int code       = atoi(p);
    const char *c1 = strchr(p, ',');               // after code
    if (c1 && code >= 0 && code <= 8) {
      const char *tempPart = c1 + 1;
      const char *c2 = strchr(tempPart, ',');       // after temp
      int n = c2 ? (int)(c2 - tempPart) : (int)strlen(tempPart);
      if (n > (int)sizeof(weatherTempStr) - 1) n = sizeof(weatherTempStr) - 1;
      strncpy(weatherTempStr, tempPart, n);
      weatherTempStr[n] = '\0';
      weatherCode = (uint8_t)code;
      weatherUnit = (c2 && c2[1]) ? c2[1] : 'F';    // default F if unit omitted
      weatherSet  = true;
      Serial.print("OK:WEATHER ");
      Serial.print(weatherTempStr); Serial.println(weatherUnit);
    }
  }
}

// ── Touch — manual CST816S read via Wire ────────────────────
// Register layout (CST816S):
//   0x01: gesture id        (0x01 single tap, 0x02 long press, etc.)
//   0x02: number of points  (0 = no touch, 1+ = touched)
//   0x03: x high (top 4 bits in lower nibble)
//   0x04: x low
//   0x05: y high (top 4 bits in lower nibble)
//   0x06: y low
bool readCST816S(uint16_t &x, uint16_t &y) {
  // The CST816S pulls INT LOW only when there's a fresh touch event.
  // Reading I²C anytime else returns stale or noisy data → ghost taps.
  if (digitalRead(TOUCH_INT) != LOW) return false;

  Wire.beginTransmission(CST816S_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST816S_ADDR, (uint8_t)6) != 6) return false;
  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();
  uint8_t points = buf[1];
  if (points == 0) return false;
  x = ((buf[2] & 0x0F) << 8) | buf[3];
  y = ((buf[4] & 0x0F) << 8) | buf[5];
  // Coordinate sanity: legit taps land in 0-239
  if (x > 239 || y > 239) return false;
  return true;
}

void handleTouch(unsigned long now) {
  static unsigned long lastPoll = 0;
  static unsigned long lastTap  = 0;
  static bool          wasDown  = false;

  if (now - lastPoll < 16) return;
  lastPoll = now;

  uint16_t tx = 0, ty = 0;
  bool down = readCST816S(tx, ty);   // INT-pin gated → no ghost reads

  // Rising edge with 300 ms debounce.  INT-pin gate alone is enough to
  // kill ghost touches; no need for a sustained-poll requirement.
  if (down && !wasDown && (now - lastTap) > 300) {
    lastTap = now;
    enabled = !enabled;
    EEPROM.put(ADDR_ENABLED, (uint8_t)(enabled ? 1 : 0));
    EEPROM.commit();
    Serial.printf("[touch] tap @%d,%d  → %s\n",
                  tx, ty, enabled ? "ENABLED" : "DISABLED");
    Serial.println(enabled ? "OK:ENABLED" : "OK:DISABLED");
    drawRadar();
  }
  wasDown = down;
}

void loadSettings() {
  uint16_t u16; uint8_t u8;
  EEPROM.get(ADDR_COOL,     u16); if (u16 != 0xFFFF && u16 >= 1   && u16 <= 300)  cooldownSec  = u16;
  EEPROM.get(ADDR_LOCKDLY,  u16); if (u16 != 0xFFFF && u16 >= 5   && u16 <= 120)  lockDelaySec = u16;
  EEPROM.get(ADDR_MAXRANGE, u16); if (u16 != 0xFFFF && u16 >= 500 && u16 <= 5000) maxRangeMm   = u16;
  EEPROM.get(ADDR_MAXX,     u16); if (u16 != 0xFFFF && u16 >= 200 && u16 <= 5000) maxXMm       = u16;
  EEPROM.get(ADDR_LOCKEN,   u8);  if (u8  != 0xFF) lockEnabled = (bool)u8;
  EEPROM.get(ADDR_ENABLED,  u8);  if (u8  != 0xFF) enabled     = (bool)u8;
}

// ── Weather icon drawing — simple 24×24 vector icons ─────────
// Colours for icon parts
#define C_SUN     0xFFE0   // yellow
#define C_CLOUD   0x8410   // grey
#define C_RAIN    0x04BF   // blue
#define C_SNOW    0xFFFF   // white
#define C_BOLT    0xFFE0   // yellow
#define C_MOON    0xC618   // light grey
#define C_FOG     0xBDF7   // pale grey

static void iconSun(LovyanGFX *g, int cx, int cy) {
  g->fillCircle(cx, cy, 5, C_SUN);
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159f / 4.0f;
    int x1 = cx + (int)(8 * cosf(a));
    int y1 = cy + (int)(8 * sinf(a));
    int x2 = cx + (int)(11 * cosf(a));
    int y2 = cy + (int)(11 * sinf(a));
    g->drawLine(x1, y1, x2, y2, C_SUN);
  }
}

static void iconCloud(LovyanGFX *g, int cx, int cy, uint16_t col) {
  // 3 overlapping fillCircles forming a cloud, plus base rect
  g->fillCircle(cx - 5, cy + 2, 5, col);
  g->fillCircle(cx + 5, cy + 2, 5, col);
  g->fillCircle(cx,     cy - 2, 6, col);
  g->fillRect(cx - 9, cy + 2, 18, 5, col);
}

static void iconMoon(LovyanGFX *g, int cx, int cy) {
  g->fillCircle(cx, cy, 8, C_MOON);
  g->fillCircle(cx + 4, cy - 2, 7, C_BG);  // bite to make crescent
}

static void drawWeatherIcon(LovyanGFX *g, int cx, int cy, uint8_t code) {
  switch (code) {
    case 0: iconSun(g, cx, cy); break;
    case 1: // partly cloudy: small sun + cloud
      iconSun(g, cx - 4, cy - 4);
      iconCloud(g, cx + 3, cy + 3, C_CLOUD);
      break;
    case 2: iconCloud(g, cx, cy, C_CLOUD); break;
    case 3: // rain
      iconCloud(g, cx, cy - 2, C_CLOUD);
      for (int i = 0; i < 3; i++)
        g->drawLine(cx - 6 + i * 6, cy + 6, cx - 8 + i * 6, cy + 11, C_RAIN);
      break;
    case 4: // snow
      iconCloud(g, cx, cy - 2, C_CLOUD);
      for (int i = 0; i < 3; i++)
        g->fillCircle(cx - 6 + i * 6, cy + 9, 1, C_SNOW);
      break;
    case 5: // thunder
      iconCloud(g, cx, cy - 2, C_CLOUD);
      g->fillTriangle(cx - 1, cy + 5, cx + 3, cy + 5, cx - 2, cy + 10, C_BOLT);
      g->fillTriangle(cx - 2, cy + 10, cx + 4, cy + 10, cx + 1, cy + 13, C_BOLT);
      break;
    case 6: // fog
      for (int i = 0; i < 4; i++)
        g->drawFastHLine(cx - 9 + (i & 1) * 2, cy - 6 + i * 4, 16, C_FOG);
      break;
    case 7: iconMoon(g, cx, cy); break;
    case 8: // cloudy night
      iconMoon(g, cx - 4, cy - 4);
      iconCloud(g, cx + 3, cy + 3, C_CLOUD);
      break;
  }
}

// ── Radar drawing (full-frame to sprite, then push) ──────────
void mmToPx(int16_t xMm, int16_t yMm, int16_t &px, int16_t &py) {
  px = (int16_t)(RADAR_CX + ((int32_t)xMm * RADAR_R) / (int32_t)maxRangeMm);
  py = (int16_t)(RADAR_SY - ((int32_t)yMm * RADAR_R) / (int32_t)maxRangeMm);
}

void drawRadar() {
  LovyanGFX *g = fb.getBuffer() ? (LovyanGFX *)&fb : (LovyanGFX *)&tft;
  g->fillScreen(C_BG);

  // Three themes — green (disarmed), blue (armed clear), red (threat).
  uint16_t arcCol, lineCol, sensorCol, dotCol;
  if (!enabled) {
    arcCol = C_GREEN_ARC; lineCol = C_GREEN_LINE;
    sensorCol = C_GREEN_SENSOR; dotCol = C_GREEN_DOT;
  } else if (activeCount >= 2) {
    arcCol = C_RED_ARC; lineCol = C_RED_LINE;
    sensorCol = C_RED_SENSOR; dotCol = C_RED_DOT;
  } else {
    arcCol = C_BLUE_ARC; lineCol = C_BLUE_LINE;
    sensorCol = C_BLUE_SENSOR; dotCol = C_TARGET_DOT;
  }

  // Range arcs centred at the bottom — only the upper half is visible
  // on the round 240×240 panel, naturally creating a half-circle radar.
  for (int i = 1; i <= RADAR_ARCS; i++) {
    int16_t r = (int16_t)((uint32_t)RADAR_R * i / RADAR_ARCS);
    g->drawCircle(RADAR_CX, RADAR_SY, r, arcCol);
  }
  g->fillRect(0, RADAR_SY + 1, SCREEN_W, SCREEN_H - RADAR_SY - 1, C_BG);
  g->drawFastVLine(RADAR_CX, 0, RADAR_SY, lineCol);
  g->fillCircle(RADAR_CX, RADAR_SY, 4, sensorCol);

  // "Targets = N" near the top, then ARMED/DISARMED line below in the
  // current theme colour (so the label and the radar reinforce each other).
  g->setTextColor(C_TEXT);
  g->setTextSize(2);
  char buf[24];
  snprintf(buf, sizeof(buf), "Targets = %d", activeCount);
  g->setCursor(RADAR_CX - (int)strlen(buf) * 6, 24);
  g->print(buf);

  const char *modeLabel = enabled ? "ARMED" : "DISARMED";
  g->setTextColor(arcCol);
  g->setCursor(RADAR_CX - (int)strlen(modeLabel) * 6, 46);
  g->print(modeLabel);

  // Weather widget — icon on the left, temperature on the right.
  // Only drawn once the Electron app pushes a `SET WEATHER` command.
  // The degree symbol is drawn as a small circle (the font has no °).
  if (weatherSet) {
    drawWeatherIcon(g, 30, 82, weatherCode);
    g->setTextColor(C_TEXT);
    g->setTextSize(2);
    int numW   = (int)strlen(weatherTempStr) * 12;  // size-2 chars are 12 px
    int totalW = numW + 7 /*degree*/ + 12 /*unit char*/;
    int startX = 224 - totalW;                       // right-aligned
    g->setCursor(startX, 74);
    g->print(weatherTempStr);                        // e.g. "72"
    g->drawCircle(startX + numW + 3, 77, 2, C_TEXT); // ° degree symbol
    g->setCursor(startX + numW + 8, 74);
    g->print(weatherUnit);                           // 'F' or 'C'
  }

  // Target dots — smoothed positions, colour from the palette above.
  for (int i = 0; i < 3; i++) {
    if (!targets[i].valid) continue;
    int16_t px, py;
    mmToPx((int16_t)smoothX[i], (int16_t)smoothY[i], px, py);
    px = constrain(px, DOT_R, SCREEN_W - 1 - DOT_R);
    py = constrain(py, DOT_R, RADAR_SY - DOT_R - 1);
    g->fillCircle(px, py, DOT_R, dotCol);
  }

  if (fb.getBuffer()) fb.pushSprite(0, 0);
}
