// settings.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "settings.h"
#include "fram.h"
#include "logging.h"
#include "fc_config.h"

bool useMetricUnits = false;  // false = imperial (ft/mi), true = metric (m/km)
bool use12Hour     = true;                                // 12-hour format default (#98)
bool useFahrenheit = true;                                // Fahrenheit default (#98)
char posixTZ[48]   = "EST5EDT,M3.2.0,M11.1.0";           // POSIX TZ string (#98)
char tzDisplayName[24] = "US Eastern";                    // Friendly TZ name (#98)
int  tzSelectedIndex   = 0;                               // Index in tzPresets[] (#98)
uint8_t  tftBrightness   = 255;                            // PWM 25-255, step 25
uint32_t tftSleepMs      = 0;                              // 0 = never (default)
uint32_t oledSleepMs     = 300000;                         // 5 minutes default
const TZPreset tzPresets[] = {
  {"US Eastern",     "EST5EDT,M3.2.0,M11.1.0",        -5},
  {"US Central",     "CST6CDT,M3.2.0,M11.1.0",        -6},
  {"US Mountain",    "MST7MDT,M3.2.0,M11.1.0",        -7},
  {"US Pacific",     "PST8PDT,M3.2.0,M11.1.0",        -8},
  {"US Alaska",      "AKST9AKDT,M3.2.0,M11.1.0",      -9},
  {"US Hawaii",      "HST10",                          -10},
  {"US Arizona",     "MST7",                            -7},
  {"UTC",            "UTC0",                              0},
  {"UK / Ireland",   "GMT0BST,M3.5.0/1,M10.5.0",        0},
  {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3",      1},
  {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4",    2},
  {"Japan / Korea",  "JST-9",                             9},
  {"Australia East", "AEST-10AEDT,M10.1.0,M4.1.0/3",    10},
  {"New Zealand",    "NZST-12NZDT,M9.5.0,M4.1.0/3",    12},
};
bool settingsLoadedFromSD = false;  // Deferred load flag (#118)
const uint32_t tftTimeoutPresets[]  = {0, 60000, 120000, 300000, 600000, 900000, 1800000};
const char*    tftTimeoutLabels[]   = {"Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
const int      TFT_TIMEOUT_COUNT    = 7;
const uint32_t oledTimeoutPresets[] = {60000, 120000, 300000, 600000, 900000, 1800000};
const char*    oledTimeoutLabels[]  = {"1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
const int      OLED_TIMEOUT_COUNT   = 6;

// XOR-32 checksum over raw bytes (excludes the checksum field itself)
static uint32_t framSettingsChecksum(const FRAMSettings& s) {
  const uint8_t* p = (const uint8_t*)&s;
  size_t len = offsetof(FRAMSettings, checksum);  // everything before checksum
  uint32_t ck = 0;
  for (size_t i = 0; i < len; i++) ck ^= ((uint32_t)p[i]) << ((i & 3) * 8);
  return ck;
}

void saveSettingsToFRAM() {
  if (!framAvailable) return;

  FRAMSettings s;
  memset(&s, 0, sizeof(s));
  s.magic          = FRAM_SETTINGS_MAGIC;
  s.version        = FRAM_SETTINGS_VER;
  s.use12Hour      = use12Hour ? 1 : 0;
  s.useFahrenheit  = useFahrenheit ? 1 : 0;
  s.useMetricUnits = useMetricUnits ? 1 : 0;
  strncpy(s.posixTZ, posixTZ, sizeof(s.posixTZ) - 1);
  strncpy(s.tzDisplayName, tzDisplayName, sizeof(s.tzDisplayName) - 1);
  s.tzSelectedIndex = (int8_t)tzSelectedIndex;
  s.tftBrightness   = tftBrightness;
  s.tftSleepMs      = tftSleepMs;
  s.oledSleepMs     = oledSleepMs;
  s.checksum        = framSettingsChecksum(s);

  const uint8_t* data = (const uint8_t*)&s;
  for (size_t i = 0; i < sizeof(s); i++) {
    fram.write8(FRAM_SETTINGS_ADDR + i, data[i]);
  }
  logPrintln("[SETTINGS] Saved to FRAM");
}

bool loadSettingsFromFRAM() {
  if (!framAvailable) return false;

  FRAMSettings s;
  uint8_t* data = (uint8_t*)&s;
  for (size_t i = 0; i < sizeof(s); i++) {
    data[i] = fram.read8(FRAM_SETTINGS_ADDR + i);
  }

  if (s.magic != FRAM_SETTINGS_MAGIC || s.version != FRAM_SETTINGS_VER) return false;
  if (s.checksum != framSettingsChecksum(s)) {
    logPrintln("[SETTINGS] FRAM checksum mismatch, ignoring");
    return false;
  }

  use12Hour       = s.use12Hour;
  useFahrenheit   = s.useFahrenheit;
  useMetricUnits  = s.useMetricUnits;
  strncpy(posixTZ, s.posixTZ, sizeof(posixTZ) - 1);
  strncpy(tzDisplayName, s.tzDisplayName, sizeof(tzDisplayName) - 1);
  tzSelectedIndex = s.tzSelectedIndex;
  tftBrightness   = constrain(s.tftBrightness, 25, 255);
  tftSleepMs      = s.tftSleepMs;
  oledSleepMs     = s.oledSleepMs;

  logPrintf("[SETTINGS] Loaded from FRAM: 12h=%d F=%d metric=%d tz=%s bright=%d\n",
            use12Hour, useFahrenheit, useMetricUnits, tzDisplayName, tftBrightness);
  return true;
}

// Apply POSIX timezone to system (#98)
void applyTimezone() {
  setenv("TZ", posixTZ, 1);
  tzset();
  logPrintf("[SETTINGS] TZ applied: %s (%s)\n", tzDisplayName, posixTZ);
}

// Convert struct tm (interpreted as UTC) to time_t, ignoring active POSIX TZ (#98 bugfix)
// mktime() always treats its argument as local time; this temporarily sets UTC
// so GPS/RTC UTC values are stored correctly as time_t.
time_t mktimeUTC(struct tm* tm) {
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(tm);
  setenv("TZ", posixTZ, 1);  // Restore user's TZ
  tzset();
  return t;
}

// Format current time respecting 12/24h preference (#98)
// Returns chars written. buf must be >= 16 bytes.
int formatTimeStr(char* buf, int hour, int minute, int second, bool includeSeconds) {
  if (use12Hour) {
    const char* ampm = (hour >= 12) ? "PM" : "AM";
    int h12 = hour % 12;
    if (h12 == 0) h12 = 12;
    if (includeSeconds)
      return sprintf(buf, "%d:%02d:%02d %s", h12, minute, second, ampm);
    else
      return sprintf(buf, "%d:%02d %s", h12, minute, ampm);
  } else {
    if (includeSeconds)
      return sprintf(buf, "%02d:%02d:%02d", hour, minute, second);
    else
      return sprintf(buf, "%02d:%02d", hour, minute);
  }
}

// Load user settings — try SD first, fall back to FRAM (#98, #118)
void loadSettings() {
  if (!sdHealth.available) {
    // SD unavailable — try FRAM backup (#118)
    if (loadSettingsFromFRAM()) {
      settingsLoadedFromSD = true;  // Treat FRAM load as success for deferred-load flag
      applyTimezone();
      return;
    }
    logPrintln("[SETTINGS] SD + FRAM unavailable, using defaults");
    applyTimezone();
    return;
  }

  File f = sdOpenSafe("/config/settings.txt", "r", true);  // silent — normal on first boot
  if (!f) {
    // No SD file — try FRAM backup (#118)
    if (loadSettingsFromFRAM()) {
      settingsLoadedFromSD = true;
      applyTimezone();
      return;
    }
    logPrintln("[SETTINGS] No settings file or FRAM backup, using defaults");
    applyTimezone();
    return;
  }

  char line[80];
  while (f.available()) {
    int idx = 0;
    while (f.available() && idx < 79) {
      char c = f.read();
      if (c == '\n' || c == '\r') break;
      line[idx++] = c;
    }
    line[idx] = '\0';
    if (idx == 0) continue;

    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    const char* key = line;
    const char* val = eq + 1;

    if (strcmp(key, "use12Hour") == 0)          use12Hour = atoi(val);
    else if (strcmp(key, "useFahrenheit") == 0)  useFahrenheit = atoi(val);
    else if (strcmp(key, "useMetricUnits") == 0) useMetricUnits = atoi(val);
    else if (strcmp(key, "posixTZ") == 0)        strncpy(posixTZ, val, sizeof(posixTZ) - 1);
    else if (strcmp(key, "tzName") == 0)         strncpy(tzDisplayName, val, sizeof(tzDisplayName) - 1);
    else if (strcmp(key, "tzIndex") == 0)        tzSelectedIndex = atoi(val);
    else if (strcmp(key, "tftBrightness") == 0)  tftBrightness = constrain(atoi(val), 25, 255);
    else if (strcmp(key, "tftSleepMs") == 0)     tftSleepMs = strtoul(val, NULL, 10);
    else if (strcmp(key, "oledSleepMs") == 0)    oledSleepMs = strtoul(val, NULL, 10);
  }
  f.close();
  settingsLoadedFromSD = true;
  applyTimezone();
  logPrintf("[SETTINGS] Loaded: 12h=%d F=%d metric=%d tz=%s bright=%d\n",
            use12Hour, useFahrenheit, useMetricUnits, tzDisplayName, tftBrightness);
}

// Save user settings to SD + FRAM (#98, #118)
// SD: atomic write via temp file (.tmp → rename) to prevent truncation loss.
// FRAM: instant backup — survives SD mount failures on next boot.
void saveSettings() {
  // Always save to FRAM first — instant, no SD dependency (#118)
  saveSettingsToFRAM();

  if (!sdHealth.available) {
    logPrintln("[SETTINGS] SD unavailable, attempting re-init before save...");
    trySDReInit();  // Try to recover SD before giving up (#118)
    if (!sdHealth.available) {
      logPrintln("[SETTINGS] SD still unavailable — saved to FRAM only");
      return;
    }
  }
  if (!SD.exists("/config")) SD.mkdir("/config");

  const char* tmpPath = "/config/settings.tmp";
  const char* finalPath = "/config/settings.txt";

  File f = sdOpenSafe(tmpPath, "w", true);  // silent — FRAM already has settings (#31 SD RED fix)
  if (!f) {
    logPrintln("[SETTINGS] Failed to open temp file — saved to FRAM only");
    return;
  }

  f.printf("use12Hour=%d\n", use12Hour ? 1 : 0);
  f.printf("useFahrenheit=%d\n", useFahrenheit ? 1 : 0);
  f.printf("useMetricUnits=%d\n", useMetricUnits ? 1 : 0);
  f.printf("posixTZ=%s\n", posixTZ);
  f.printf("tzName=%s\n", tzDisplayName);
  f.printf("tzIndex=%d\n", tzSelectedIndex);
  f.printf("tftBrightness=%d\n", tftBrightness);
  f.printf("tftSleepMs=%lu\n", tftSleepMs);
  f.printf("oledSleepMs=%lu\n", oledSleepMs);
  f.flush();
  f.close();

  // Atomic swap: remove old, rename temp to final
  if (SD.exists(finalPath)) SD.remove(finalPath);
  SD.rename(tmpPath, finalPath);
  logPrintln("[SETTINGS] Settings saved to SD + FRAM");
}

// Factory reset — delete settings file and restore compiled defaults (#104)
void factoryReset() {
  // Delete stored settings from SD
  if (sdHealth.available && SD.exists("/config/settings.txt")) {
    SD.remove("/config/settings.txt");
  }

  // Clear FRAM settings backup (#118) — zero the magic so it won't be loaded
  if (framAvailable) {
    for (size_t i = 0; i < FRAM_SETTINGS_SIZE; i++) {
      fram.write8(FRAM_SETTINGS_ADDR + i, 0);
    }
    logPrintln("[SETTINGS] FRAM settings cleared");
  }

  // Restore compiled defaults
  useFahrenheit    = true;
  use12Hour        = true;
  useMetricUnits   = false;
  strncpy(posixTZ, "EST5EDT,M3.2.0,M11.1.0", sizeof(posixTZ) - 1);
  strncpy(tzDisplayName, "US Eastern", sizeof(tzDisplayName) - 1);
  tzSelectedIndex  = 0;
  tftBrightness    = 255;
  tftSleepMs       = 0;
  oledSleepMs      = 300000;

  // Apply
  applyTimezone();
  analogWrite(TFT_BL, tftBrightness);

  logPrintln("[SETTINGS] Factory reset — all settings restored to defaults");
}

// Helper: find index in timeout preset array matching a value
int findTimeoutIndex(const uint32_t presets[], int count, uint32_t value) {
  for (int i = 0; i < count; i++)
    if (presets[i] == value) return i;
  return 0;  // Default to first if not found
}
