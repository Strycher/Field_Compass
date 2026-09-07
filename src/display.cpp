// display.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "display.h"
#include "oled.h"
#include "settings.h"
#include "fc_config.h"
#include "logging.h"

TFT_eSPI tft = TFT_eSPI();
bool tftSleeping = false;
unsigned long lastActivityTime = 0;
unsigned long lastTFTUpdate = 0;      // millis() of last successful TFT draw
unsigned long lastTFTReinit = 0;      // millis() of last preventive re-init
uint32_t tftUpdateCount = 0;          // Total TFT update cycles

void initTFT() {
  logPrint("Initializing ST7796U TFT... ");

  tft.init();

  // Turn on backlight via PWM
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, TFT_BL_PWM);

  // Landscape mode (480x320). Rotation 3, NOT 1 — the Hosyond MSP3526 panel is
  // mounted 180 degrees from TFT_eSPI's assumption, so the orientation we want is
  // the complement of the nominal landscape rotation. Rotation 3 writes MADCTL
  // MX|MY|MV|COLOR_ORDER, which is what this panel needs.
  //
  // This previously required patching TFT_eSPI's TFT_Drivers/ST7796_Rotation.h to
  // swap the complementary rotation pairs, so that setRotation(1) emitted case 3's
  // MADCTL. That patch lived only in the local Arduino libraries folder, outside
  // git, and any library upgrade silently reverted it. Stock case 3 is byte-identical
  // to what the patch made case 1, so this one-line change replaces the fork. (#157)
  tft.setRotation(3);
  tft.fillScreen(TFT_RED);  // Flash red to confirm TFT is working
  delay(100);
  tft.fillScreen(COLOR_BG);

  lastTFTReinit = millis();  // Track init time

  // TFT_eSprite removed — PSRAM now used for LVGL draw buffers only (#114)
  logPrintf("OK (480x320, PSRAM: %dKB free)\n", ESP.getFreePsram() / 1024);
}

// Preventive TFT re-initialization (P1 blank bug workaround)
// LVGL continuously repaints, so blank-screen is self-healing.
// Kept as a safety net — re-init every 30 minutes + invalidate LVGL.
void checkTFTHealth() {
  unsigned long now = millis();
  if (tftSleeping) return;

  if (now - lastTFTReinit > TFT_REINIT_INTERVAL) {
    #if DEBUG_TFT
    logPrintf("[TFT] Soft repaint at %lus (updates:%lu)\n",
              now / 1000, tftUpdateCount);
    #endif
    // tft.init() + setRotation() removed — caused visible flash every 30 min.
    // LVGL continuously repaints, so a full invalidation is sufficient as a
    // safety net against stale display state without reinitializing hardware.
    lv_obj_invalidate(lv_screen_active());
    lastTFTReinit = now;
  }
}

void sleepTFT() {
  if (tftSleeping) return;

  tftSleeping = true;
  analogWrite(TFT_BL, 0);            // Backlight off
  tft.writecommand(0x10);  // MIPI DCS Sleep In
  #if DEBUG_SLEEP
  Serial.println("TFT sleeping");
  #endif
}

void wakeTFT() {
  if (!tftSleeping) return;

  tftSleeping = false;
  tft.writecommand(0x11);  // MIPI DCS Sleep Out
  delay(120);  // ST7796U datasheet: 120ms delay after sleep out
  analogWrite(TFT_BL, tftBrightness); // Backlight on (user brightness, #91)
  lv_obj_invalidate(lv_screen_active());  // Force LVGL full repaint on wake (#113)
  #if DEBUG_SLEEP
  Serial.println("TFT woke up");
  #endif
}

void wakeAllDisplays() {
  lastActivityTime = millis();
  wakeTFT();
  wakeOLED();
}

void checkDisplaySleep() {
  unsigned long elapsed = millis() - lastActivityTime;

  // Check OLED sleep (0 = disabled) — uses runtime variable (#91)
  if (oledSleepMs > 0 && !oledSleeping && oledAvailable && elapsed > oledSleepMs) {
    sleepOLED();
  }

  // Check TFT sleep (0 = disabled, LCD has no burn-in risk) — uses runtime variable (#91)
  if (tftSleepMs > 0 && !tftSleeping && elapsed > tftSleepMs) {
    sleepTFT();
  }
}
