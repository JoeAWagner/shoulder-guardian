// ═══════════════════════════════════════════════════════════════
//  Shoulder Guardian — ESP32-C3 Super Mini + HLK-LD2450 + GC9A01
//
//  ESP32-C3 variant.  For the ATmega32U4 (Pro Micro) version see
//  the shoulder_surfer folder.  Both sketches speak the same
//  STATUS / SET-command protocol, so the same Electron app works
//  with either board.
//
//  Key advantages of the ESP32-C3 build:
//    • 400 KB SRAM → full 240×240 framebuffer (no flicker)
//    • 160 MHz     → fast canvas flush over 80 MHz SPI
//    • 3.3 V GPIO  → no voltage divider needed for LD2450
//
//  ── Wiring — HLK-LD2450 radar ───────────────────────────────
//    LD2450 VCC  → 3.3V   (3.3 V — no voltage divider needed)
//    LD2450 GND  → GND
//    LD2450 TX   → GPIO 20 (Serial1 RX)
//    LD2450 RX   → GPIO 21 (Serial1 TX)
//
//  ── Wiring — GC9A01 1.28" round display ─────────────────────
//    GC9A01 VCC  → 3.3V
//    GC9A01 GND  → GND
//    GC9A01 SCL  → GPIO 4  (SPI SCK)
//    GC9A01 SDA  → GPIO 6  (SPI MOSI)
//    GC9A01 CS   → GPIO 7  (TFT_CS)
//    GC9A01 DC   → GPIO 3  (TFT_DC)
//    GC9A01 RST  → GPIO 10 (TFT_RST)
//    GC9A01 BLK  → GPIO 1  (TFT_BLK) or wire directly to 3.3V
//
//  ── Arduino IDE setup ────────────────────────────────────────
//    Board Manager : "esp32" by Espressif Systems
//    Board         : "ESP32C3 Dev Module"
//    USB CDC On Boot: "Enabled"    ← required for Serial over USB
//    Library       : "Arduino_GFX_Library" by Moon On Our Nation
// ═══════════════════════════════════════════════════════════════

#include <EEPROM.h>
#include <Arduino_GFX_Library.h>

// ── Pins ──────────────────────────────────────────────────────
#define LED_PIN       8    // onboard LED — active LOW on C3 Super Mini
#define LED_ACTIVE_LOW true

// LD2450 UART (Serial1)
#define LD_RX        20
#define LD_TX        21

// GC9A01 SPI — avoid strapping pins 2, 8, 9
#define TFT_SCK       4
#define TFT_MOSI      6
#define TFT_CS        7
#define TFT_DC        3
#define TFT_RST      10
#define TFT_BLK       1   // backlight; HIGH = on. Wire to 3.3V to skip GPIO control.

// ── EEPROM ────────────────────────────────────────────────────
#define EEPROM_SIZE  16
const int ADDR_COOL     = 0;   // uint16 — trigger cooldown (seconds)
const int ADDR_LOCKDLY  = 2;   // uint16 — lock delay (seconds)
const int ADDR_MAXRANGE = 4;   // uint16 — max detection depth (mm)
const int ADDR_LOCKEN   = 6;   // uint8  — lock-on-empty enabled
const int ADDR_ENABLED  = 7;   // uint8  — protection enabled
const int ADDR_MAXX     = 8;   // uint16 — max lateral offset (mm)

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

// ── Non-blocking serial command buffer ───────────────────────
char    cmdBuf[64];
uint8_t cmdIdx = 0;

// ── Timing ───────────────────────────────────────────────────
unsigned long statusTimer = 0;
const unsigned long STATUS_WATCHDOG_MS = 150;

// ── RGB565 colour palette ─────────────────────────────────────
#define C_BG     0x0882   // #0d1117 — matches app background
#define C_GRID   0x0200   // dark green — range arcs
#define C_LINE   0x0100   // very dark green — centre line
#define C_SENSOR 0x5D1F   // #58a6ff — sensor dot
#define C_DOT    0x07E0   // bright green — single target
#define C_THREAT 0xF800   // red — ≥2 targets (shoulder-surfer alert)

// ── Radar geometry (240 × 240 round display) ─────────────────
#define RADAR_CX   120    // sensor X centre
#define RADAR_SY   228    // sensor Y (near bottom of display)
#define RADAR_R    210    // pixel radius = maxRangeMm at full scale
#define RADAR_ARCS   4    // number of range rings
#define DOT_R        6    // target dot radius in pixels

// ── GFX objects ───────────────────────────────────────────────
// Canvas (off-screen framebuffer) → flicker-free full-frame rendering.
// 240×240 × 2 bytes = ~115 KB — comfortable on 400 KB ESP32 SRAM.
Arduino_DataBus *bus    = new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI);
Arduino_GFX    *panel   = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, false /* IPS — set true if display has washed-out/inverted colours */);
Arduino_Canvas *canvas  = new Arduino_Canvas(240, 240, panel, 0, 0);

// ─────────────────────────────────────────────────────────────

void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledWrite(false);

  Serial.begin(115200);
  delay(2000);  // give USB CDC time to enumerate before any prints
  Serial.println("[SG] Shoulder Guardian starting...");

  // Init display first — gives immediate visual feedback before the
  // 4-second LD2450 config sequence runs
  Serial.println("[SG] Init display...");
  initDisplay();

  Serial1.begin(250000, SERIAL_8N1, LD_RX, LD_TX); // LD2450 binary frames
  EEPROM.begin(EEPROM_SIZE);
  memset(targets, 0, sizeof(targets));
  loadSettings();

  Serial.println("[SG] Configuring LD2450...");
  configLD2450();
  Serial.println("[SG] Ready.");
  startupBlink();
}

void loop() {
  unsigned long now = millis();

  readLD2450();   // non-blocking — sends STATUS on every new frame
  handleSerial(); // non-blocking — processes commands on newline

  // LED: solid = targets present, slow blink = empty, fast blink = disabled
  if (enabled) {
    ledWrite(activeCount > 0 ? true : (now / 500) % 2 == 0);
  } else {
    ledWrite((now / 1000) % 2 == 0);
  }

  // Watchdog STATUS — keeps app responsive if radar goes quiet
  if (now - statusTimer >= STATUS_WATCHDOG_MS) {
    statusTimer = now;
    sendStatus();
  }
}

// ── LD2450 startup: enable multi-target mode ─────────────────
void configLD2450() {
  delay(2000); // let sensor fully boot

  for (int pass = 0; pass < 2; pass++) {
    uint8_t enterCfg[] = {0xFD,0xFC,0xFB,0xFA, 0x04,0x00, 0xFF,0x00,0x01,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(enterCfg, sizeof(enterCfg));
    delay(300);

    uint8_t multiTgt[] = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0x90,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(multiTgt, sizeof(multiTgt));
    delay(300);

    uint8_t exitCfg[] = {0xFD,0xFC,0xFB,0xFA, 0x02,0x00, 0xFE,0x00, 0x04,0x03,0x02,0x01};
    Serial1.write(exitCfg, sizeof(exitCfg));
    delay(500);

    while (Serial1.available()) Serial1.read(); // flush ACKs
  }
}

// ── LD2450 frame state machine (non-blocking) ────────────────
void readLD2450() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    if (frameIdx == 0 && b != 0xAA) continue;
    if (frameIdx == 1 && b != 0xFF) { frameIdx = 0; continue; }
    if (frameIdx == 2 && b != 0x03) { frameIdx = 0; continue; }
    if (frameIdx == 3 && b != 0x00) { frameIdx = 0; continue; }

    frameBuf[frameIdx++] = b;

    if (frameIdx == FRAME_LEN) {
      frameIdx = 0;
      if (frameBuf[28] == 0x55 && frameBuf[29] == 0xCC) {
        parseFrame();
        sendStatus();
        statusTimer = millis();
      }
    }
  }
}

// LD2450 sign-magnitude encoding: bit 15 = sign, bits 0-14 = magnitude
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
    targets[i].y     = (int16_t)(rawY & 0x7FFF); // Y bit-15 is presence flag, not sign
    targets[i].speed = decodeSM(rawS);
    targets[i].valid = (rawX != 0 || rawY != 0)
                       && targets[i].y <= (int16_t)maxRangeMm
                       && targets[i].y > 0
                       && abs(targets[i].x) <= (int16_t)maxXMm;
    if (targets[i].valid) activeCount++;
  }
  updateDisplay();
}

// ── STATUS line → Electron app ───────────────────────────────
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

// ── Non-blocking command handler ─────────────────────────────
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
  }
}

// ── EEPROM load ───────────────────────────────────────────────
void loadSettings() {
  uint16_t u16; uint8_t u8;
  EEPROM.get(ADDR_COOL,     u16); if (u16 != 0xFFFF && u16 >= 1   && u16 <= 300)  cooldownSec  = u16;
  EEPROM.get(ADDR_LOCKDLY,  u16); if (u16 != 0xFFFF && u16 >= 5   && u16 <= 120)  lockDelaySec = u16;
  EEPROM.get(ADDR_MAXRANGE, u16); if (u16 != 0xFFFF && u16 >= 500 && u16 <= 5000) maxRangeMm   = u16;
  EEPROM.get(ADDR_MAXX,     u16); if (u16 != 0xFFFF && u16 >= 200 && u16 <= 5000) maxXMm       = u16;
  EEPROM.get(ADDR_LOCKEN,   u8);  if (u8  != 0xFF) lockEnabled = (bool)u8;
  EEPROM.get(ADDR_ENABLED,  u8);  if (u8  != 0xFF) enabled     = (bool)u8;
}

// ── LED ───────────────────────────────────────────────────────
void ledWrite(bool on) {
  // C3 Super Mini LED is typically active LOW; flip LED_ACTIVE_LOW if yours differs
  digitalWrite(LED_PIN, (LED_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
}

void startupBlink() {
  for (int i = 0; i < 3; i++) { ledWrite(true); delay(100); ledWrite(false); delay(100); }
}

// ── Display ───────────────────────────────────────────────────
// mmToPx: radar millimetre coords → display pixel coords.
//   X: negative = left of sensor, positive = right.
//   Y: positive = distance from sensor (targets drawn above sensor dot).
void mmToPx(int16_t xMm, int16_t yMm, int16_t &px, int16_t &py) {
  px = (int16_t)(RADAR_CX + ((int32_t)xMm * RADAR_R) / (int32_t)maxRangeMm);
  py = (int16_t)(RADAR_SY - ((int32_t)yMm * RADAR_R) / (int32_t)maxRangeMm);
}

// drawRadar: renders the complete radar scene into the off-screen canvas,
// then flushes the full 240×240 frame to the display in one SPI burst.
// Because everything is drawn to RAM first, the screen never shows a
// partially-updated state — no flicker, no tearing.
void drawRadar() {
  // ── Background ──────────────────────────────────────────────
  canvas->fillScreen(C_BG);

  // ── Range arcs ──────────────────────────────────────────────
  // Full circles centred at the sensor position. The lower halves
  // extend off-screen (or are masked below); only the upper semicircles
  // appear as range rings in the radar view.
  for (int i = 1; i <= RADAR_ARCS; i++) {
    int16_t r = (int16_t)((uint32_t)RADAR_R * i / RADAR_ARCS);
    canvas->drawCircle(RADAR_CX, RADAR_SY, r, C_GRID);
  }

  // Mask the strip below the sensor row (hides any arc fragments)
  canvas->fillRect(0, RADAR_SY + 1, 240, 240 - RADAR_SY - 1, C_BG);

  // ── Centre line ─────────────────────────────────────────────
  canvas->drawFastVLine(RADAR_CX, 0, RADAR_SY, C_LINE);

  // ── Sensor dot ──────────────────────────────────────────────
  canvas->fillCircle(RADAR_CX, RADAR_SY, 4, C_SENSOR);

  // ── Target dots ─────────────────────────────────────────────
  // Green = lone presence, Red = shoulder-surfer alert (≥2 people)
  uint16_t dotCol = (activeCount >= 2) ? C_THREAT : C_DOT;
  for (int i = 0; i < 3; i++) {
    if (targets[i].valid) {
      int16_t px, py;
      mmToPx(targets[i].x, targets[i].y, px, py);
      px = constrain(px, DOT_R, 239 - DOT_R);
      py = constrain(py, DOT_R, RADAR_SY - DOT_R - 1);
      canvas->fillCircle(px, py, DOT_R, dotCol);
    }
  }

  // ── Flush canvas → display ───────────────────────────────────
  canvas->flush();
}

// useCanvas tracks whether the framebuffer allocation succeeded.
// If canvas->begin() fails the sketch falls back to direct panel drawing
// (same selective-redraw approach as the Pro Micro sketch).
static bool useCanvas = false;

// Previous dot positions for the selective-redraw fallback path
struct PrevDot { int16_t px, py; bool active; };
static PrevDot prevDots[3];

void initDisplay() {
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);  // backlight on

  useCanvas = canvas->begin(27000000UL);  // 27 MHz — conservative; safe on all modules
  if (useCanvas) {
    Serial.println("[SG] canvas ready (framebuffer mode)");
    drawRadar();
  } else {
    // canvas->begin() failed — fall back to direct panel drawing
    Serial.println("[SG] canvas FAILED — falling back to direct panel draw");
    if (panel->begin(27000000UL)) {
      Serial.println("[SG] panel ready (direct mode)");
      panel->fillScreen(C_BG);
      drawStaticDirect();
    } else {
      Serial.println("[SG] panel->begin() also failed — check wiring/pins");
    }
  }
  memset(prevDots, 0, sizeof(prevDots));
}

void updateDisplay() {
  if (useCanvas) {
    drawRadar();
  } else {
    updateDirect();
  }
}

// ── Direct-draw helpers (fallback, no framebuffer) ────────────
void drawStaticDirect() {
  for (int i = 1; i <= RADAR_ARCS; i++) {
    int16_t r = (int16_t)((uint32_t)RADAR_R * i / RADAR_ARCS);
    panel->drawCircle(RADAR_CX, RADAR_SY, r, C_GRID);
  }
  panel->fillRect(0, RADAR_SY + 1, 240, 240 - RADAR_SY - 1, C_BG);
  panel->drawFastVLine(RADAR_CX, 0, RADAR_SY, C_LINE);
  panel->fillCircle(RADAR_CX, RADAR_SY, 4, C_SENSOR);
}

void updateDirect() {
  for (int i = 0; i < 3; i++) {
    if (prevDots[i].active)
      panel->fillCircle(prevDots[i].px, prevDots[i].py, DOT_R + 1, C_BG);
  }
  drawStaticDirect();
  uint16_t dotCol = (activeCount >= 2) ? C_THREAT : C_DOT;
  for (int i = 0; i < 3; i++) {
    if (targets[i].valid) {
      int16_t px, py;
      mmToPx(targets[i].x, targets[i].y, px, py);
      px = constrain(px, DOT_R, 239 - DOT_R);
      py = constrain(py, DOT_R, RADAR_SY - DOT_R - 1);
      panel->fillCircle(px, py, DOT_R, dotCol);
      prevDots[i] = { px, py, true };
    } else {
      prevDots[i].active = false;
    }
  }
}
