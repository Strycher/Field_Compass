// rtc.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "rtc.h"
#include "settings.h"
#include "logging.h"

RTC_PCF8523 rtc;
bool rtcAvailable = false;        // Adalogger RTC

void initRTC() {
  logPrint("Initializing RTC (PCF8523)... ");

  if (!rtc.begin()) {
    logPrintln("NOT FOUND");
    return;
  }

  rtcAvailable = true;

  // Check if RTC lost power and is running with invalid time
  if (!rtc.initialized() || rtc.lostPower()) {
    logPrintln("OK (needs time sync)");
    // Don't set a default time - wait for GPS or NTP to provide accurate time
    return;
  }

  // RTC has valid time - use it to set system time
  DateTime now = rtc.now();
  struct tm timeinfo;
  timeinfo.tm_year = now.year() - 1900;
  timeinfo.tm_mon = now.month() - 1;
  timeinfo.tm_mday = now.day();
  timeinfo.tm_hour = now.hour();
  timeinfo.tm_min = now.minute();
  timeinfo.tm_sec = now.second();

  time_t t = mktimeUTC(&timeinfo);  // RTC stores UTC — use UTC-aware mktime (#98 bugfix)
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, NULL);

  logPrintf("OK (%04d-%02d-%02d %02d:%02d:%02d)\n",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute(), now.second());
}

// Sync RTC from current system time (call after GPS or NTP sync)
void syncRTCFromSystemTime(const char* source) {
  if (!rtcAvailable) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  // Adjust for timezone - RTC stores UTC
  time_t now;
  time(&now);
  struct tm* utc = gmtime(&now);

  rtc.adjust(DateTime(utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                      utc->tm_hour, utc->tm_min, utc->tm_sec));

  logPrintf("[RTC] Synced from %s: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
            source,
            utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
            utc->tm_hour, utc->tm_min, utc->tm_sec);
}
