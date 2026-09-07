#pragma once
// screen_telemetry.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <lvgl.h>

extern lv_obj_t* telemetryScr;
extern lv_obj_t* telHeader;

void buildTelemetryScreen();
void updateTelemetryData();
