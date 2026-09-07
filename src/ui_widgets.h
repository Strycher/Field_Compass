#pragma once
// ui_widgets.h -- extracted from src.ino by scripts/extract_unit.py (#262, E4).
#include <lvgl.h>

enum SDIndicatorState { SD_IND_OK, SD_IND_ERROR, SD_IND_MISSING };  // (#120)

void fcHeaderSetTitle(lv_obj_t* header, const char* title);
void fcHeaderSetSDStatus(lv_obj_t* header, SDIndicatorState state);
lv_obj_t* fcNavBarCreate(lv_obj_t* parent, uint8_t screenCount, uint8_t activeIdx);
void fcNavBarSetActive(lv_obj_t* navBar, uint8_t activeIdx);
lv_obj_t* fcActionBarCreate(lv_obj_t* parent, bool showBack, bool showOK);
lv_obj_t* fcToggleCreate(lv_obj_t* parent, int16_t y, const char* label, const char* optA, const char* optB, bool value);
bool fcToggleGetValue(lv_obj_t* toggle);
void fcToggleSetValue(lv_obj_t* toggle, bool value);
lv_obj_t* fcDropdownCreate(lv_obj_t* parent, int16_t y, const char* label, const char* initialValue);
int fcDropdownGetIndex(lv_obj_t* dropdown);
void fcDropdownSetValue(lv_obj_t* dropdown, int idx, const char* text);
