#pragma once
// ui_state.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Arduino.h>

#define NUM_SCREENS 4
#define SCREEN_COMPASS   0
#define SCREEN_GEOCACHE  1  // Geocaching navigation (#70)
#define SCREEN_ENV       2
#define SCREEN_TELEMETRY 3  // Combined GPS + IMU (#97)
#define SCREEN_SETTINGS  4  // Modal overlay — outside NUM_SCREENS, not in swipe/button cycling

extern int currentScreen;
extern int  previousScreen;
extern int  settingsSubScreen;
extern int geocacheSubScreen;
extern int selectedCacheIndex;
extern int listScrollOffset;
