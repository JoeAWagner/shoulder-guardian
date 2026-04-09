// ═══════════════════════════════════════════════════════════════
//  Shoulder Surfer — Pro Micro ATmega32U4 + HLK-LD2450
//
//  Wiring:
//    LD2450 VCC  → 5V
//    LD2450 GND  → GND
//    LD2450 TX   → Pro Micro pin 0  (Serial1 RX)
//    LD2450 RX   → Pro Micro pin 1  (Serial1 TX)
//    ⚠ Add a 1kΩ/2kΩ voltage divider on pin 0 if your module
//      outputs 5V on TX (most HLK modules are 3.3V tolerant).
//
//  Protocol:
//    LD2450 → Arduino : 250000 baud binary frames (30 bytes each)
//    Arduino → App    : 115200 baud text STATUS lines
//    App     → Arduino: 115200 baud text SET commands
// ═══════════════════════════════════════════════════════════════

#include <EEPROM.h>

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
