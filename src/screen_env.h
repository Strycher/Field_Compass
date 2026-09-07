#pragma once
// screen_env.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <lvgl.h>

extern lv_obj_t* envScr;
extern lv_obj_t* envHeader;

void buildEnvScreen();
void updateEnvData();
