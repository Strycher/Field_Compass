#pragma once
// settings.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Arduino.h>
#include <time.h>

#define TZ_PRESET_COUNT 14

struct TZPreset {
  const char* name;
  const char* posix;
  int8_t stdOffset;  // For display: "UTC-5"
};

extern bool useMetricUnits;
extern bool use12Hour;
extern bool useFahrenheit;
extern char posixTZ[48];
extern char tzDisplayName[24];
extern int  tzSelectedIndex;
extern uint8_t  tftBrightness;
extern uint32_t tftSleepMs;
extern uint32_t oledSleepMs;
extern const TZPreset tzPresets[];
extern bool settingsLoadedFromSD;
extern const uint32_t tftTimeoutPresets[];
extern const char*    tftTimeoutLabels[];
extern const int      TFT_TIMEOUT_COUNT;
extern const uint32_t oledTimeoutPresets[];
extern const char*    oledTimeoutLabels[];
extern const int      OLED_TIMEOUT_COUNT;

void saveSettingsToFRAM();
bool loadSettingsFromFRAM();
void applyTimezone();
time_t mktimeUTC(struct tm* tm);
int formatTimeStr(char* buf, int hour, int minute, int second, bool includeSeconds);
void loadSettings();
void saveSettings();
void factoryReset();
int findTimeoutIndex(const uint32_t presets[], int count, uint32_t value);
