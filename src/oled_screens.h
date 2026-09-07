#pragma once
// oled_screens.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Arduino.h>

void updateOLED();
void drawOLEDScreenTelemetry();
void drawOLEDScreenEnv();
void drawOLEDScreenCompass();
void drawOLEDScreenGeocache();
void drawOLEDNavBar();
