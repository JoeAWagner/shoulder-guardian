// ═══════════════════════════════════════════════════════════════
//  lgfx_test — minimal hardware-validation sketch
//
//  Runs at 1 MHz (very slow) and only does fillScreen.  If even this
//  produces garbled output, the display module is faulty.
// ═══════════════════════════════════════════════════════════════

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;

public:
  LGFX(void) {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 1000000;     // 1 MHz — as slow as it goes
      cfg.freq_read   = 1000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 0;
      cfg.pin_mosi    = 1;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = 3;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 10;
      cfg.pin_rst          = 8;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 240;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX tft;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[LGFX] init at 1 MHz");
  tft.init();
  tft.setRotation(0);
  Serial.println("[LGFX] init done — entering color loop");
}

void loop() {
  Serial.println("RED");
  tft.fillScreen(TFT_RED);
  delay(2000);

  Serial.println("GREEN");
  tft.fillScreen(TFT_GREEN);
  delay(2000);

  Serial.println("BLUE");
  tft.fillScreen(TFT_BLUE);
  delay(2000);

  Serial.println("WHITE");
  tft.fillScreen(TFT_WHITE);
  delay(2000);

  Serial.println("BLACK");
  tft.fillScreen(TFT_BLACK);
  delay(2000);
}
