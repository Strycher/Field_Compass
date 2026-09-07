#pragma once
// battery.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Adafruit_MAX1704X.h>

#define BATT_LOG_INTERVAL 10000  // Log every 10 seconds
#define BATT_LOG_FILE "/battlog.csv"

extern Adafruit_MAX17048 battery;
extern bool batteryAvailable;

void initBattery();
void logBatteryToSD();
void logBatteryToFRAM();
bool isBatteryConnected();
