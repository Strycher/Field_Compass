#pragma once
// screen_settings.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <lvgl.h>

#define SETTINGS_MENU_COUNT 6  // Configuration, Display, Compass Cal, Diagnostics, About, Factory Reset (#104)
#define SETTINGS_SCROLL_STEP 50  // Pixels per A/B button press

extern lv_obj_t* settingsScr;
extern lv_obj_t* settingsScrollAreas[7];

void updateSettingsData();
void buildSettingsScreen();
