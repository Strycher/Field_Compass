#pragma once
// rtc.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <RTClib.h>

extern RTC_PCF8523 rtc;
extern bool rtcAvailable;

void initRTC();
void syncRTCFromSystemTime(const char* source);
