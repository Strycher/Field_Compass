#pragma once
// touch.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Adafruit_FT6206.h>

extern Adafruit_FT6206 ctp;
extern bool touchAvailable;

void IRAM_ATTR touchISR();
void initTouch();
