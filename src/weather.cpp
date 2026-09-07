// weather.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "weather.h"
#include "fram.h"
#include "env.h"
#include "gps.h"
#include "battery.h"
#include "geo.h"
#include "logging.h"

unsigned long lastFramFlush = 0;
int weatherLogFileCount = 0;
int weatherLogEntryCount = 0;
static unsigned long lastWeatherLogCheck = 0;
WeatherReading weatherHistory[WEATHER_SAMPLES_MAX];
int weatherHistoryCount = 0;
int weatherHistoryHead = 0;
unsigned long lastWeatherLog = 0;
WeatherTrend weatherTrend;

// Flush all pending FRAM ring buffer entries to SD card
void framFlushToSD() {
  if (!framAvailable || !sdAvailable) return;
  if (framHeader.battCount == 0 && framHeader.wxCount == 0) return;  // Nothing to flush

  unsigned long startMs = millis();
  int battFlushed = 0, wxFlushed = 0;

  // --- Flush battery entries ---
  if (framHeader.battCount > 0) {
    File f = sdOpenSafe(BATT_LOG_FILE, "a", true);
    if (f) {
      if (f.size() == 0) {
        f.println("millis,voltage,percent,rate");
      }
      uint16_t idx = framHeader.battTail;
      for (int i = 0; i < framHeader.battCount; i++) {
        FRAMBatteryEntry entry;
        uint32_t addr = FRAM_BATT_ADDR + (idx * FRAM_BATT_ENTRY);
        uint8_t* data = (uint8_t*)&entry;
        for (int j = 0; j < FRAM_BATT_ENTRY; j++) {
          data[j] = fram.read8(addr + j);
        }
        f.printf("%lu,%.3f,%.2f,%.2f\n", entry.timestamp, entry.voltage, entry.percent, entry.rate);
        idx = (idx + 1) % FRAM_BATT_COUNT;
        battFlushed++;
      }
      f.close();
      recordSDSuccess();
      framHeader.battTail = framHeader.battHead;
      framHeader.battCount = 0;
    }
  }

  // --- Flush weather entries ---
  if (framHeader.wxCount > 0) {
    char filename[32];
    getWeatherFilename(filename, 0);  // Today's file
    File file = sdOpenSafe(filename, "a", true);
    if (file) {
      uint16_t idx = framHeader.wxTail;
      for (int i = 0; i < framHeader.wxCount; i++) {
        WeatherReading reading;
        uint32_t addr = FRAM_WX_ADDR + (idx * FRAM_WX_ENTRY);
        uint8_t* data = (uint8_t*)&reading;
        for (int j = 0; j < FRAM_WX_ENTRY; j++) {
          data[j] = fram.read8(addr + j);
        }
        file.printf("%lu,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                    reading.timestamp, reading.lat, reading.lon,
                    reading.pressure, reading.temp, reading.humidity);
        idx = (idx + 1) % FRAM_WX_COUNT;
        wxFlushed++;
      }
      file.close();
      recordSDSuccess();
      framHeader.wxTail = framHeader.wxHead;
      framHeader.wxCount = 0;
    }
  }

  // Update header: clear dirty flag
  framHeader.flags &= ~0x01;
  framWriteHeader();

  unsigned long elapsed = millis() - startMs;
  logPrintf("[FRAM] Flushed: batt=%d wx=%d (%lums)\n", battFlushed, wxFlushed, elapsed);
  lastFramFlush = millis();
}

// Get current timestamp from NTP or GPS
uint32_t getCurrentTimestamp() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t now;
    time(&now);
    return (uint32_t)now;
  }
  return 0;
}

// Get today's weather log filename
void getWeatherFilename(char* buf, int daysAgo) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t now;
    time(&now);
    now -= daysAgo * 86400;  // Subtract days
    struct tm* t = localtime(&now);
    sprintf(buf, "/weather/%04d-%02d-%02d.csv",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  } else {
    sprintf(buf, "/weather/unknown.csv");
  }
}

// Add reading to circular buffer
void addToWeatherHistory(WeatherReading reading) {
  weatherHistory[weatherHistoryHead] = reading;
  weatherHistoryHead = (weatherHistoryHead + 1) % WEATHER_SAMPLES_MAX;
  if (weatherHistoryCount < WEATHER_SAMPLES_MAX) {
    weatherHistoryCount++;
  }
}

// Get reading from N samples ago (0 = most recent)
WeatherReading* getWeatherReading(int samplesAgo) {
  if (samplesAgo >= weatherHistoryCount) return NULL;
  int index = (weatherHistoryHead - 1 - samplesAgo + WEATHER_SAMPLES_MAX) % WEATHER_SAMPLES_MAX;
  return &weatherHistory[index];
}

// Log current weather reading to SD card
// Write weather entry to FRAM ring buffer
void logWeatherToFRAM(WeatherReading& reading) {
  if (!framAvailable) return;

  // WeatherReading is 24 bytes, matches our FRAM entry layout
  uint32_t addr = FRAM_WX_ADDR + (framHeader.wxHead * FRAM_WX_ENTRY);
  uint8_t* data = (uint8_t*)&reading;
  for (int i = 0; i < FRAM_WX_ENTRY; i++) {
    fram.write8(addr + i, data[i]);
  }

  framHeader.wxHead = (framHeader.wxHead + 1) % FRAM_WX_COUNT;
  if (framHeader.wxCount < FRAM_WX_COUNT) {
    framHeader.wxCount++;
  } else {
    framHeader.wxTail = (framHeader.wxTail + 1) % FRAM_WX_COUNT;
    logPrintln("[FRAM] WARN: Weather ring overflow");
  }
  framHeader.flags |= 0x01;  // dirty
  framWriteHeader();
}

void logWeatherReading() {
  if ((!sdHealth.available && !framAvailable) || (!bmeAvailable && !shtAvailable)) return;

  uint32_t timestamp = getCurrentTimestamp();
  if (timestamp == 0) return;  // No valid time

  // Get current GPS position (use 0,0 if no fix)
  float lat = gpsData.valid ? gpsData.latitude : 0;
  float lon = gpsData.valid ? gpsData.longitude : 0;

  // Create reading
  WeatherReading reading;
  reading.timestamp = timestamp;
  reading.lat = lat;
  reading.lon = lon;
  reading.pressure = envData.pressure;
  // SHT41 temp/humidity preferred for weather log accuracy (#48)
  reading.temp = shtAvailable ? shtData.temperature : envData.temperature;
  reading.humidity = shtAvailable ? shtData.humidity : envData.humidity;

  // Add to in-memory buffer
  addToWeatherHistory(reading);

  // Buffer to FRAM (SD flush happens on 5-min timer)
  if (framAvailable) {
    logWeatherToFRAM(reading);
  } else if (sdAvailable) {
    // Fallback: direct SD write
    char filename[32];
    getWeatherFilename(filename, 0);
    File file = sdOpenSafe(filename, "a", true);
    if (file) {
      file.printf("%lu,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                  timestamp, lat, lon,
                  reading.pressure, reading.temp, reading.humidity);
      file.close();
      recordSDSuccess();
    }
  }
}

// Load weather history from SD card on boot
void loadWeatherHistory() {
  if (!sdHealth.available) return;

  logPrint("Loading weather history... ");

  int loaded = 0;
  uint32_t now = getCurrentTimestamp();
  uint32_t cutoff = now - (WEATHER_HISTORY_HOURS * 3600);

  // Load today's and yesterday's files
  for (int daysAgo = 1; daysAgo >= 0; daysAgo--) {
    char filename[32];
    getWeatherFilename(filename, daysAgo);

    if (!SD.exists(filename)) continue;

    File file = sdOpenSafe(filename, "r", true);  // silent fail
    if (!file) continue;

    char line[80];
    while (file.available()) {
      int len = file.readBytesUntil('\n', line, sizeof(line) - 1);
      line[len] = '\0';

      WeatherReading reading;
      if (sscanf(line, "%lu,%f,%f,%f,%f,%f",
                 &reading.timestamp, &reading.lat, &reading.lon,
                 &reading.pressure, &reading.temp, &reading.humidity) == 6) {
        // Only load readings within history window
        if (reading.timestamp >= cutoff) {
          addToWeatherHistory(reading);
          loaded++;
        }
      }
    }
    file.close();
    recordSDSuccess();
  }

  logPrintf("%d readings\n", loaded);
}

// Calculate weather trend from history
void calculateWeatherTrend() {
  // Need at least some history
  if (weatherHistoryCount < 2) {
    weatherTrend.forecast = "Init";
    return;
  }

  WeatherReading* current = getWeatherReading(0);
  if (!current) return;

  // Check for location change
  WeatherReading* prev = getWeatherReading(1);
  if (prev && (prev->lat != 0 || prev->lon != 0) && (current->lat != 0 || current->lon != 0)) {
    weatherTrend.locationChanged = !sameLocation(current->lat, current->lon, prev->lat, prev->lon);
  } else {
    weatherTrend.locationChanged = false;
  }

  // Find reading from ~3 hours ago (36 samples at 5-min intervals)
  int samples3hr = 36;
  if (samples3hr > weatherHistoryCount - 1) {
    samples3hr = weatherHistoryCount - 1;
  }

  WeatherReading* reading3hr = getWeatherReading(samples3hr);
  if (!reading3hr) {
    weatherTrend.forecast = "Learning";
    return;
  }

  // Calculate changes
  weatherTrend.pressureChange3hr = current->pressure - reading3hr->pressure;
  weatherTrend.tempChange3hr = current->temp - reading3hr->temp;
  weatherTrend.humidityChange3hr = current->humidity - reading3hr->humidity;

  // Determine trend direction
  float pChange = weatherTrend.pressureChange3hr;
  if (pChange > 3.0) {
    weatherTrend.trend = 2;  // Rising fast
  } else if (pChange > 1.0) {
    weatherTrend.trend = 1;  // Rising slow
  } else if (pChange < -3.0) {
    weatherTrend.trend = 4;  // Falling fast
  } else if (pChange < -1.0) {
    weatherTrend.trend = 3;  // Falling slow
  } else {
    weatherTrend.trend = 0;  // Stable
  }

  // Calculate forecast
  weatherTrend.forecast = calculateForecast();
}

// Get trend arrow character
const char* getTrendArrow() {
  switch (weatherTrend.trend) {
    case 1: return "^";    // Rising slow
    case 2: return "^^";   // Rising fast
    case 3: return "v";    // Falling slow
    case 4: return "vv";   // Falling fast
    default: return "-";   // Stable
  }
}

// Calculate weather forecast based on conditions
const char* calculateForecast() {
  float p = envData.pressure;
  // SHT41 preferred for forecast accuracy (#48)
  float t = shtAvailable ? shtData.temperature : envData.temperature;
  float h = shtAvailable ? shtData.humidity : envData.humidity;
  float pChange = weatherTrend.pressureChange3hr;

  // Location changed - can't predict
  if (weatherTrend.locationChanged) {
    return "Traveled";
  }

  // Not enough history (need 3 hours)
  if (weatherHistoryCount < 36) {
    return "Learning";
  }

  bool lowPressure = (p < 1000);
  bool highPressure = (p > 1015);
  bool highHumidity = (h > 70);
  bool fallingFast = (pChange < -3.0);
  bool fallingSlow = (pChange < -1.0 && pChange >= -3.0);
  bool risingFast = (pChange > 3.0);
  bool risingSlow = (pChange > 1.0 && pChange <= 3.0);
  bool cold = (t < 5.0);  // Below 5°C

  // Storm conditions
  if (lowPressure && fallingFast && highHumidity) {
    return "Storm Likely";
  }

  // Precipitation
  if (lowPressure && (fallingFast || fallingSlow) && highHumidity) {
    if (cold) return "Snow Likely";
    return "Rain Likely";
  }

  // Clearing
  if (highPressure && (risingFast || risingSlow)) {
    return "Clearing";
  }

  // Fair weather
  if (highPressure && fabs(pChange) < 1.0) {
    return "Fair";
  }

  // Unsettled
  if (lowPressure && fabs(pChange) < 1.0 && highHumidity) {
    return "Unsettled";
  }

  // Possible change
  if (fallingSlow && h > 60) {
    return "Precip Poss";
  }

  return "Stable";
}

void updateWeatherLogStats() {
  if (!sdHealth.available) return;
  if (millis() - lastWeatherLogCheck < 60000) return;  // Check every minute
  lastWeatherLogCheck = millis();

  weatherLogFileCount = 0;
  weatherLogEntryCount = 0;

  File dir = SD.open("/weather");
  if (!dir) return;

  // Limit iterations to prevent hang on corrupted filesystem
  int maxFiles = 100;
  int filesChecked = 0;

  while (File entry = dir.openNextFile()) {
    if (filesChecked++ >= maxFiles) {
      entry.close();
      break;  // Safety limit
    }
    if (!entry.isDirectory()) {
      weatherLogFileCount++;
      // Count lines with safety limit
      int maxLines = 10000;
      int linesRead = 0;
      while (entry.available() && linesRead < maxLines) {
        if (entry.read() == '\n') weatherLogEntryCount++;
        linesRead++;
      }
    }
    entry.close();
  }
  dir.close();
  recordSDSuccess();
}
