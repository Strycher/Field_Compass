#pragma once
// lvgl_port.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <lvgl.h>
#include <esp_timer.h>

#define LVGL_TEST_MODE 0

extern lv_display_t* lvglDisplay;
extern uint8_t* lvglBuf1;
extern uint8_t* lvglBuf2;
extern bool lvglAvailable;
extern lv_indev_t* lvglTouchIndev;
extern lv_indev_t* lvglEncoderIndev;
extern lv_group_t* lvglGroup;
extern int32_t  lastTouchX;
extern int32_t  lastTouchY;
extern uint32_t touchPressCount;
extern uint32_t touchReleaseCount;
extern bool     touchWasPressed;

uint32_t lvglTickCb(void);
void lvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
void lvglLogCb(lv_log_level_t level, const char* buf);
void lvglTouchReadCb(lv_indev_t* indev, lv_indev_data_t* data);
void lvglEncoderReadCb(lv_indev_t* indev, lv_indev_data_t* data);
void initFCTheme();
