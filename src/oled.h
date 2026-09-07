#pragma once
// oled.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Adafruit_SH110X.h>

#define DEBUG_SLEEP 0  // Display sleep/wake logging

extern Adafruit_SH1107 oled;
extern bool oledAvailable;
extern bool oledSleeping;

void initOLED();
void sleepOLED();
void wakeOLED();
