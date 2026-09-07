#pragma once
// screen_geocache.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <lvgl.h>

extern lv_obj_t* geocacheScr;
extern lv_obj_t* gcNavHeader;

void buildGeocacheScreen();
void updateGeocacheData();
void gcUpdateFilterLabels();
void handleGeocacheButtons(bool buttonA, bool buttonB);
