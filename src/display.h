#pragma once
// display.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <TFT_eSPI.h>
#include <lvgl.h>

#define TFT_REINIT_INTERVAL 1800000          // Preventive re-init every 30 minutes
#define DEBUG_TFT   1  // TFT display state logging (P1 blank bug debug)
#define TFT_SLEEP_TIMEOUT  0        // 0 = always on (LCD has no burn-in risk)
#define OLED_SLEEP_TIMEOUT 180000   // 3 minutes for OLED (high burn-in risk)
#define TFT_BL_PWM 255 // Default brightness (0=off, 255=full)

// Colors (RGB565) -- the TFT boot screen in setup() and initTFT use these
#define COLOR_BG        0x0000  // Black
#define COLOR_TEXT      0xFFFF  // White
#define COLOR_HEADER    0x07FF  // Cyan
#define COLOR_VALUE     0x07E0  // Green
#define COLOR_WARN      0xFD20  // Orange
#define COLOR_ERROR     0xF800  // Red
#define COLOR_DIM       0x7BEF  // Gray

extern TFT_eSPI tft;
extern bool tftSleeping;
extern unsigned long lastActivityTime;
extern unsigned long lastTFTReinit;
extern uint32_t tftUpdateCount;
extern unsigned long lastTFTUpdate;

void initTFT();
void checkTFTHealth();
void sleepTFT();
void wakeTFT();
void wakeAllDisplays();
void checkDisplaySleep();
