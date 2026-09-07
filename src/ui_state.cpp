// ui_state.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "ui_state.h"

int  settingsSubScreen = 0;    // 0=menu, 1=compass cal, 2=diagnostics, ...
int  previousScreen    = 0;    // Screen to return to when exiting settings
int currentScreen = SCREEN_COMPASS;
int selectedCacheIndex = 0;           // Currently selected for navigation
int listScrollOffset = 0;             // For scrollable list display
int geocacheSubScreen = 0;            // 0=nav, 1=list, 2=details
