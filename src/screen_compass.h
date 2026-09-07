#pragma once
// screen_compass.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <lvgl.h>

extern lv_obj_t* compassScr;
extern lv_obj_t* compassHeader;
extern lv_obj_t* compassNavBar;

void buildCompassScreen();
void updateCompassData();
