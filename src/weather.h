#pragma once
// weather.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Arduino.h>

#define WEATHER_LOG_INTERVAL  300000   // 5 minutes in ms
#define WEATHER_HISTORY_HOURS 24
#define WEATHER_SAMPLES_MAX   288      // 24hrs * 12 samples/hr

struct WeatherReading {
  uint32_t timestamp;
  float lat, lon;
  float pressure, temp, humidity;
};

struct WeatherTrend {
  float pressureChange3hr = 0;    // hPa change over 3 hours
  float tempChange3hr = 0;        // °C change over 3 hours
  float humidityChange3hr = 0;    // % change over 3 hours
  bool locationChanged = false;   // Moved >1km since last reading
  uint8_t trend = 0;              // 0=stable, 1=rising slow, 2=rising fast, 3=falling slow, 4=falling fast
  const char* forecast = "Init";  // "Clear", "Rain Likely", etc.
};

extern WeatherReading weatherHistory[WEATHER_SAMPLES_MAX];
extern int weatherHistoryCount;
extern int weatherHistoryHead;
extern unsigned long lastWeatherLog;
extern WeatherTrend weatherTrend;
extern int weatherLogFileCount;
extern int weatherLogEntryCount;
extern unsigned long lastFramFlush;

void framFlushToSD();
uint32_t getCurrentTimestamp();
void getWeatherFilename(char* buf, int daysAgo);
void addToWeatherHistory(WeatherReading reading);
WeatherReading* getWeatherReading(int samplesAgo);
void logWeatherToFRAM(WeatherReading& reading);
void logWeatherReading();
void loadWeatherHistory();
void calculateWeatherTrend();
const char* getTrendArrow();
const char* calculateForecast();
void updateWeatherLogStats();
