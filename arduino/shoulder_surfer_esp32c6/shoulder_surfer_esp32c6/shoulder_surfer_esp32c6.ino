// ═══════════════════════════════════════════════════════════════
//  Shoulder Guardian — Waveshare ESP32-C6-LCD-1.47 + HLK-LD2450
//
//  Same STATUS / SET-command protocol as the C3 build, so the
//  Electron app works unchanged.
//
//  ── Onboard wiring (already on PCB — no jumpers needed) ─────
//    LCD ST7789 172×320 IPS — MOSI=6 SCK=7 DC=15 CS=14 RST=21 BL=22
//    WS2812 RGB LED — GPIO 8
//
//  ── External wiring — HLK-LD2450 radar ──────────────────────
//    LD2450 VCC  → 5V (VBUS pin) — the radar's datasheet specifies
//                  5 V, NOT 3.3 V.  TX/RX logic is still 3.3 V tolerant.
//    LD2450 GND  → GND
//    LD2450 TX   → GPIO 16 (Serial1 RX)
//    LD2450 RX   → GPIO 17 (Serial1 TX)
//
//  ── Arduino IDE setup ───────────────────────────────────────
//    Board Manager : "esp32" by Espressif Systems v3.0 OR NEWER
//    Board         : "ESP32C6 Dev Module"
//    USB CDC On Boot: "Enabled"
//    Libraries     : LovyanGFX, Adafruit NeoPixel
// ═══════════════════════════════════════════════════════════════

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ── Pins ──────────────────────────────────────────────────────
#define RGB_PIN     8     // WS2812 NeoPixel
#define BL_PIN      22    // LCD backlight (PWM)
// Board silkscreen labels TX/RX from the BOARD's perspective:
//   board pad "TX" = GPIO 16 (U0TXD), board pad "RX" = GPIO 17 (U0RXD).
// The user wired LD2450 TX → board "RX" pad (GPIO 17), LD2450 RX → board
// "TX" pad (GPIO 16) — a correct crossover. So from our firmware's POV:
#define LD_RX       17    // Serial1 RX  ← LD2450 TX  (board pad "RX")
#define LD_TX       16    // Serial1 TX  → LD2450 RX  (board pad "TX")

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

// Per-slot smoothed positions for the on-screen radar dots.
// Raw radar data jitters ±30-50 mm even when a person is still; the EMA
// makes the dots glide instead of strobing.  STATUS protocol keeps
// sending raw values so the Electron app's own smoothing is unaffected.
float smoothX[3] = {0, 0, 0};
float smoothY[3] = {0, 0, 0};
bool  smoothInit[3] = {false, false, false};
const float SMOOTH_ALPHA = 0.20f;   // lower = smoother but laggier; 0.20 is ~5-frame settle

// ── Non-blocking serial command buffer ───────────────────────
char    cmdBuf[64];
uint8_t cmdIdx = 0;

// ── Timing ───────────────────────────────────────────────────
unsigned long statusTimer = 0;
const unsigned long STATUS_WATCHDOG_MS = 150;

// ── Diagnostics (visible on screen) ──────────────────────────
uint32_t ld_bytes_total = 0;   // total bytes received from LD2450
uint32_t ld_frames_ok   = 0;   // valid frames parsed
unsigned long lastByteMs = 0;  // last time we got any radar byte

// ── LovyanGFX panel ──────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
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
      cfg.pin_sclk    = 7;
      cfg.pin_mosi    = 6;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = 15;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = 14;
      cfg.pin_rst       = 21;
      cfg.pin_busy      = -1;
      cfg.panel_width   = 172;
      cfg.panel_height  = 320;
      cfg.offset_x      = 34;
      cfg.offset_y      = 0;
      cfg.invert        = true;
      cfg.rgb_order     = false;
      cfg.bus_shared    = true;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX                 tft;
// Waveshare's onboard WS2812 on this board is RGB-ordered, not the
// usual GRB.  Using NEO_GRB causes red/green to swap.
Adafruit_NeoPixel    led(1, RGB_PIN, NEO_RGB + NEO_KHZ800);

// Use a sprite (off-screen framebuffer) for flicker-free radar updates
LGFX_Sprite          fb(&tft);

// ── Colours (RGB565) ─────────────────────────────────────────
#define C_BG     0x0000   // black
#define C_GRID   0x03E0   // green arcs
#define C_LINE   0x01E0   // mid-green centre line
#define C_SENSOR 0x5D1F   // blue sensor dot
#define C_DOT    0x07E0   // bright green target
#define C_THREAT 0xF800   // red threat
#define C_TEXT   0xC618   // light grey text

// ── Radar geometry — landscape 320×172 with sensor centre-bottom ─
#define SCREEN_W   320
#define SCREEN_H   172
#define RADAR_CX   160     // sensor X
#define RADAR_SY   165     // sensor Y near bottom
#define RADAR_R    150     // pixel radius at maxRangeMm
#define RADAR_ARCS   4
#define DOT_R        6

// ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[SG] Shoulder Guardian C6 starting...");

  // RGB LED
  led.begin();
  led.setBrightness(40);
  led.setPixelColor(0, led.Color(0, 0, 64)); // dim blue = booting
  led.show();

  // Backlight
  pinMode(BL_PIN, OUTPUT);
  analogWrite(BL_PIN, 200); // ~80% brightness

  // Display
  Serial.println("[SG] init display...");
  tft.init();
  tft.setRotation(3); // 320×172 landscape, USB at the bottom
  tft.fillScreen(C_BG);

  // Sprite framebuffer for flicker-free radar
  fb.setColorDepth(16);
  if (!fb.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("[SG] sprite alloc failed — falling back to direct draw");
  }
  drawRadar();

  // LD2450 UART — datasheet spec is 256000 baud
  Serial1.begin(256000, SERIAL_8N1, LD_RX, LD_TX);
  EEPROM.begin(EEPROM_SIZE);
  memset(targets, 0, sizeof(targets));
  loadSettings();

  Serial.println("[SG] configuring LD2450...");
  configLD2450();
  Serial.println("[SG] ready");

  led.setPixelColor(0, led.Color(0, 64, 0)); // green = ready
  led.show();
}

void loop() {
  unsigned long now = millis();

  readLD2450();
  handleSerial();

  // RGB LED status
  if (!enabled) {
    led.setPixelColor(0, ((now / 500) % 2) ? led.Color(20, 20, 0) : 0); // dim yellow blink = disabled
  } else if (activeCount >= 2) {
    led.setPixelColor(0, led.Color(120, 0, 0)); // red = threat
  } else if (activeCount == 1) {
    led.setPixelColor(0, led.Color(0, 80, 0)); // green = single target
  } else {
    led.setPixelColor(0, led.Color(0, 0, 30)); // dim blue = idle
  }
  led.show();

  if (now - statusTimer >= STATUS_WATCHDOG_MS) {
    statusTimer = now;
    sendStatus();
  }

  // Refresh radar display every 250 ms even with no new frames,
  // so the diagnostic counters stay live.
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

// ── LD2450 frame state machine ───────────────────────────────
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

    // EMA on the smoothed position used for drawing.  First sample
    // initialises directly so the dot doesn't slide in from (0,0).
    if (targets[i].valid) {
      if (smoothInit[i]) {
        smoothX[i] = SMOOTH_ALPHA * targets[i].x + (1.0f - SMOOTH_ALPHA) * smoothX[i];
        smoothY[i] = SMOOTH_ALPHA * targets[i].y + (1.0f - SMOOTH_ALPHA) * smoothY[i];
      } else {
        smoothX[i]   = targets[i].x;
        smoothY[i]   = targets[i].y;
        smoothInit[i] = true;
      }
    } else {
      smoothInit[i] = false;
    }
  }
  drawRadar();
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

void loadSettings() {
  uint16_t u16; uint8_t u8;
  EEPROM.get(ADDR_COOL,     u16); if (u16 != 0xFFFF && u16 >= 1   && u16 <= 300)  cooldownSec  = u16;
  EEPROM.get(ADDR_LOCKDLY,  u16); if (u16 != 0xFFFF && u16 >= 5   && u16 <= 120)  lockDelaySec = u16;
  EEPROM.get(ADDR_MAXRANGE, u16); if (u16 != 0xFFFF && u16 >= 500 && u16 <= 5000) maxRangeMm   = u16;
  EEPROM.get(ADDR_MAXX,     u16); if (u16 != 0xFFFF && u16 >= 200 && u16 <= 5000) maxXMm       = u16;
  EEPROM.get(ADDR_LOCKEN,   u8);  if (u8  != 0xFF) lockEnabled = (bool)u8;
  EEPROM.get(ADDR_ENABLED,  u8);  if (u8  != 0xFF) enabled     = (bool)u8;
}

// ── Radar drawing — full-frame to sprite then push to LCD ─────
// X mm: negative = left of sensor, positive = right
// Y mm: positive = depth from sensor (drawn upward on screen)
void mmToPx(int16_t xMm, int16_t yMm, int16_t &px, int16_t &py) {
  px = (int16_t)(RADAR_CX + ((int32_t)xMm * RADAR_R) / (int32_t)maxRangeMm);
  py = (int16_t)(RADAR_SY - ((int32_t)yMm * RADAR_R) / (int32_t)maxRangeMm);
}

void drawRadar() {
  LovyanGFX *g = fb.getBuffer() ? (LovyanGFX *)&fb : (LovyanGFX *)&tft;
  g->fillScreen(C_BG);

  // Range arcs — half circles (top half only, since sensor is at bottom)
  for (int i = 1; i <= RADAR_ARCS; i++) {
    int16_t r = (int16_t)((uint32_t)RADAR_R * i / RADAR_ARCS);
    g->drawCircle(RADAR_CX, RADAR_SY, r, C_GRID);
  }

  // Erase the half below the sensor so arcs only show as half-circles
  g->fillRect(0, RADAR_SY + 1, SCREEN_W, SCREEN_H - RADAR_SY - 1, C_BG);

  // Centre line
  g->drawFastVLine(RADAR_CX, 0, RADAR_SY, C_LINE);

  // Sensor dot
  g->fillCircle(RADAR_CX, RADAR_SY, 4, C_SENSOR);

  // Status text — top corners (size 2 = 12×16 px)
  g->setTextColor(C_TEXT);
  g->setTextSize(2);
  g->setCursor(6, 6);
  g->printf("T:%d", activeCount);
  g->setCursor(SCREEN_W - 70, 6);
  g->printf("%s", enabled ? "ARMED" : "OFF ");

  // Diagnostic line — bottom corner (size 1 = small, fits more info).
  // ld_bytes should grow constantly when LD2450 is wired correctly.
  // ld_frames should grow at ~10 Hz.
  // age = ms since last byte arrived (smaller = healthier).
  unsigned long age = millis() - lastByteMs;
  g->setTextSize(1);
  g->setCursor(4, SCREEN_H - 10);
  g->setTextColor(ld_bytes_total > 0 ? C_GRID : C_THREAT);
  g->printf("LD2450 b:%lu f:%lu age:%lums",
            (unsigned long)ld_bytes_total,
            (unsigned long)ld_frames_ok,
            age);

  // Target dots — draw using smoothed positions so they glide
  uint16_t dotCol = (activeCount >= 2) ? C_THREAT : C_DOT;
  for (int i = 0; i < 3; i++) {
    if (!targets[i].valid) continue;
    int16_t px, py;
    mmToPx((int16_t)smoothX[i], (int16_t)smoothY[i], px, py);
    px = constrain(px, DOT_R, SCREEN_W - 1 - DOT_R);
    py = constrain(py, DOT_R, RADAR_SY - DOT_R - 1);
    g->fillCircle(px, py, DOT_R, dotCol);
  }

  // Push sprite to LCD in one DMA burst (no flicker)
  if (fb.getBuffer()) fb.pushSprite(0, 0);
}
