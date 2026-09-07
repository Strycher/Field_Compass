// oled_screens.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "oled_screens.h"
#include "oled.h"
#include "ui_state.h"
#include "gps.h"
#include "imu.h"
#include "env.h"
#include "weather.h"
#include "geocache.h"
#include "settings.h"
#include "geo.h"
#include "logging.h"

void updateOLED() {
  oled.clearDisplay();

  // Show different content based on current screen (mirroring TFT)
  switch (currentScreen) {
    case SCREEN_COMPASS:
      drawOLEDScreenCompass();
      break;
    case SCREEN_GEOCACHE:
      drawOLEDScreenGeocache();
      break;
    case SCREEN_ENV:
      drawOLEDScreenEnv();
      break;
    case SCREEN_TELEMETRY:
      drawOLEDScreenTelemetry();
      break;
    case SCREEN_SETTINGS:
      oled.setTextSize(1);
      oled.setCursor(0, 0);
      oled.print("SETTINGS");
      break;
  }

  // Draw screen indicator
  drawOLEDNavBar();

  oled.display();
}

// OLED Telemetry — combined GPS + IMU (#97)
void drawOLEDScreenTelemetry() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("TELEMETRY");

  // GPS section (rows 1-3)
  if (gpsData.valid) {
    oled.setCursor(0, 10);
    sprintf(buf, "%.5f %c", fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    oled.print(buf);

    oled.setCursor(0, 20);
    sprintf(buf, "%.5f %c", fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    oled.print(buf);

    float altVal = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
    char altUnit = useMetricUnits ? 'm' : 'f';
    oled.setCursor(0, 30);
    sprintf(buf, "%.0f%c S:%d H:%.1f", altVal, altUnit, gpsData.satellites, gpsData.hdop);
    oled.print(buf);
  } else if (gpsData.receiving) {
    oled.setCursor(0, 15);
    oled.print("Acquiring fix...");
    oled.setCursor(0, 25);
    sprintf(buf, "Sat: %d", gpsData.satellites);
    oled.print(buf);
  } else {
    oled.setCursor(0, 18);
    oled.print("No GPS data");
  }

  // IMU section (rows 4-5)
  if (imuAvailable && magAvailable) {
    oled.setCursor(0, 42);
    sprintf(buf, "%.0f%s R:%.0f P:%.0f", imuData.heading, getCardinal(imuData.heading), imuData.roll, imuData.pitch);
    oled.print(buf);

    oled.setCursor(0, 52);
    sprintf(buf, "Accel:%.2f m/s2", imuData.accelMag);
    oled.print(buf);
  } else {
    oled.setCursor(0, 46);
    oled.print("No IMU");
  }
}

void drawOLEDScreenEnv() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("ENVIRONMENT");

  if (bmeAvailable || shtAvailable) {
    // SHT41 temp/humidity preferred over BME688 (#48)
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    float humid = shtAvailable ? shtData.humidity : envData.humidity;

    oled.setCursor(0, 10);
    if (useFahrenheit)
      sprintf(buf, "%.1fF %.1f%% IAQ:%.0f", tempF, humid, envData.iaq);
    else
      sprintf(buf, "%.1fC %.1f%% IAQ:%.0f", tempC, humid, envData.iaq);
    oled.print(buf);

    oled.setCursor(0, 22);
    sprintf(buf, "%.0fhPa %.2f\"", envData.pressure, hPaToInHg(envData.pressure));
    oled.print(buf);

    oled.setCursor(0, 34);
    sprintf(buf, "%s %s", getTrendArrow(), weatherTrend.forecast);
    oled.print(buf);

    oled.setCursor(0, 46);
    sprintf(buf, "CO2:%.0f %s", envData.co2Equivalent, shtAvailable ? "SHT" : "BME");
    oled.print(buf);
  } else {
    oled.setCursor(0, 28);
    oled.print("No env sensors");
  }
}

void drawOLEDScreenCompass() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("COMPASS");

  if (imuAvailable && magAvailable) {
    oled.setTextSize(2);
    oled.setCursor(0, 12);
    sprintf(buf, "%.0f %s", imuData.heading, getCardinal(imuData.heading));
    oled.print(buf);

    oled.setTextSize(1);
    oled.setCursor(0, 36);
    if (gpsData.valid) {
      sprintf(buf, "%.1f mph", gpsData.speedKnots * 1.15078);
    } else {
      sprintf(buf, "-- mph");
    }
    oled.print(buf);
  } else {
    oled.setCursor(0, 16);
    oled.print("No IMU");
  }
}

void drawOLEDScreenGeocache() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("GEOCACHE");

  // Check if we have any caches and if selected cache is valid
  if (cacheListCount == 0 || selectedCacheIndex >= cacheListCount || !cacheList[selectedCacheIndex].valid) {
    oled.setCursor(0, 16);
    oled.print("No cache");
    return;
  }

  // Get reference to selected cache
  GeocacheEntry& cache = cacheList[selectedCacheIndex];

  // Distance and bearing
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);

  oled.setCursor(0, 12);
  if (useMetricUnits) {
    if (distKm >= 1.0) {
      sprintf(buf, "%.1fkm %d%c", distKm, (int)bearing, 247);
    } else {
      sprintf(buf, "%.0fm %d%c", distKm * 1000, (int)bearing, 247);
    }
  } else {
    float distMi = distKm * 0.621371;
    float distFt = distKm * 3280.84;
    if (distMi >= 1.0) {
      sprintf(buf, "%.1fmi %d%c", distMi, (int)bearing, 247);
    } else {
      sprintf(buf, "%.0fft %d%c", distFt, (int)bearing, 247);
    }
  }
  oled.print(buf);

  // Arrow direction
  float arrowAngle = bearing - imuData.heading;
  if (arrowAngle < 0) arrowAngle += 360;
  if (arrowAngle >= 360) arrowAngle -= 360;

  oled.setCursor(0, 24);
  sprintf(buf, "Arrow: %.0f%c", arrowAngle, 247);
  oled.print(buf);

  // D/T rating
  oled.setCursor(0, 36);
  sprintf(buf, "D:%.1f T:%.1f", cache.difficulty, cache.terrain);
  oled.print(buf);

  // Accuracy
  float accM = getGpsAccuracyMeters();
  oled.setCursor(0, 48);
  if (useMetricUnits) {
    sprintf(buf, "+/-%.0fm", accM);
  } else {
    sprintf(buf, "+/-%.0fft", accM * 3.28084);
  }
  oled.print(buf);
}

void drawOLEDNavBar() {
  // Draw screen number indicator at bottom right
  oled.setCursor(100, 56);
  oled.setTextSize(1);
  char buf[8];
  sprintf(buf, "[%d/%d]", currentScreen + 1, NUM_SCREENS);
  oled.print(buf);
}
