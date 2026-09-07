#pragma once
// navigation.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <lvgl.h>

lv_obj_t* fcHeaderCreate(lv_obj_t* parent, const char* title);
lv_obj_t* fcListPickerOpen(const char* title, const char** items, int count, int selectedIdx, lv_obj_t* caller);
void screenGestureCb(lv_event_t* e);   // swipe left/right on a main screen
void navigateScreen(int delta);
void navigateToSettings();
void navigateFromSettings();
void updateSDIndicators();
void updateDisplay();
