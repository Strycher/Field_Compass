#pragma once
// touch.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Adafruit_FT6206.h>

extern Adafruit_FT6206 ctp;
extern bool touchAvailable;

void IRAM_ATTR touchISR();
void initTouch();
void serviceTouch();   // health probe; re-probe a dropped chip and re-init it (#289)

// Wake-from-sleep path (#290). While the TFT sleeps loop() skips
// lv_timer_handler(), so the LVGL indev callback -- the only reader of the
// chip -- never runs. touchPollForWake() reads the chip on a slow timer and
// returns true once per new touch; the caller wakes the displays. The touch
// that woke the panel is then swallowed until the finger lifts, so it does
// not also land as a click on whatever is under it (same as the buttons,
// which wake and return). touchWakeSwallow(touchedNow) returns true while
// that swallow is in force.
bool touchPollForWake();
bool touchWakeSwallow(bool touchedNow);
