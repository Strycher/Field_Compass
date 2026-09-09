// web.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "web.h"
#include "fc_version.h"
#include "settings.h"
#include "rtc.h"
#include "logging.h"
#include "fc_config.h"
#include "fram.h"
#include "gps.h"
#include "imu.h"
#include "env.h"
#include "battery.h"
#include "weather.h"
#include "geocache.h"
#include "ui_state.h"
#include "oled.h"
#include "display.h"
#include "touch.h"
#include "geo.h"
#include "wifi_store.h"   // saved networks live in NVS, not here (#295, epic #99)

const char* NTP_SERVER = "pool.ntp.org";
WebServer webServer(WEB_SERVER_PORT);
bool wifiConnected = false;
bool ntpSynced = false;
bool webServerStarted = false;
bool rtcSyncedFromNTP = false;    // RTC was synced from NTP this session
static uint16_t webSerialReadPos = 0;
static String gpxUploadBuffer;
static bool gpxUploadSuccess = false;
static String gpxUploadError;

// ---- WiFi: one non-blocking state machine (#295, epic #99) -----------------
// The old initWiFi() blocked setup() for up to 22.5 s and checkWiFi() blocked
// loop() for 5 s every 30 s while the network was down (#292 measured it).
// Nothing here waits: initWiFi() starts the first attempt and returns, and
// checkWiFi(), called every loop pass, moves the machine along by looking at
// WiFi.status() and the clock. Saved networks are tried in store order; when
// none answers the machine rests WIFI_RECONNECT_INTERVAL and starts over. A
// network added at runtime (#296, #297) is picked up from IDLE or on the next
// round. NTP is started on connect and checked on later passes, never waited on.
enum WifiState { WIFI_ST_IDLE, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED, WIFI_ST_WAIT };
static WifiState wifiState = WIFI_ST_IDLE;
static int wifiTryIndex = 0;
static unsigned long wifiStateSince = 0;
static bool ntpPending = false;
static unsigned long ntpSince = 0;
#define WIFI_CONNECT_TIMEOUT_MS 8000    // per network, about what the old 15 x 500 ms gave
#define NTP_TIMEOUT_MS          15000
#define NTP_EPOCH_SANE          1700000000UL   // 2023-11-14: the SNTP client has answered

static void wifiStartAttempt(int idx) {
  WifiCred c;
  if (!wifiStoreGet(idx, c)) {
    wifiState = WIFI_ST_WAIT;
    wifiStateSince = millis();
    return;
  }
  logPrintf("[WIFI] Connecting to %s (%d of %d)\n", c.ssid, idx + 1, wifiStoreCount());
  WiFi.begin(c.ssid, c.pass);
  wifiTryIndex = idx;
  wifiState = WIFI_ST_CONNECTING;
  wifiStateSince = millis();
}

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);         // the core keeps no copy of the credentials in its own NVS area
  WiFi.setAutoReconnect(false);   // this machine is the only thing that reconnects, so the log is truthful
  wifiStoreInit();
  wifiStoreImportFromSD();
  if (wifiStoreCount() == 0) {
    logPrintf("[WIFI] No saved networks. Put %s on the SD card (see docs/plans/2026-09-09-wifi-configuration.md)\n",
              WIFI_IMPORT_PATH);
    wifiState = WIFI_ST_IDLE;
    return;
  }
  wifiStartAttempt(0);
}

static void wifiOnConnected() {
  wifiConnected = true;
  wifiState = WIFI_ST_CONNECTED;
  wifiStateSince = millis();
  logPrintf("[WIFI] Connected to %s, IP %s, RSSI %d dBm\n",
            WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (!ntpSynced) {
    configTime(0, 0, NTP_SERVER);   // NTP provides UTC; POSIX TZ handles offset (#98)
    applyTimezone();                 // Ensure TZ is set after configTime
    ntpPending = true;
    ntpSince = millis();
  }
  if (!webServerStarted) initWebServer();
}

static void wifiServiceNtp() {
  if (!ntpPending) return;
  if ((unsigned long)time(nullptr) > NTP_EPOCH_SANE) {
    ntpPending = false;
    ntpSynced = true;
    logPrintln("[NTP] Synced");
    // Sync RTC from NTP (if GPS hasn't already synced it)
    if (!rtcSyncedFromGPS && !rtcSyncedFromNTP) {
      syncRTCFromSystemTime("NTP");
      rtcSyncedFromNTP = true;
    }
  } else if (millis() - ntpSince > NTP_TIMEOUT_MS) {
    ntpPending = false;
    logPrintln("[NTP] No answer in 15 s; GPS/RTC time stands");
  }
}

void checkWiFi() {
  wl_status_t st = WiFi.status();
  unsigned long now = millis();
  switch (wifiState) {
    case WIFI_ST_IDLE:
      if (wifiStoreCount() > 0) wifiStartAttempt(0);   // a network was added since boot
      break;

    case WIFI_ST_CONNECTING:
      if (st == WL_CONNECTED) {
        wifiOnConnected();
        break;
      }
      if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
          now - wifiStateSince > WIFI_CONNECT_TIMEOUT_MS) {
        WifiCred c;
        wifiStoreGet(wifiTryIndex, c);
        logPrintf("[WIFI] %s: %s\n", c.ssid,
                  st == WL_NO_SSID_AVAIL ? "not in range" :
                  st == WL_CONNECT_FAILED ? "rejected (password?)" : "no answer in 8 s");
        int next = wifiTryIndex + 1;
        if (next < wifiStoreCount()) {
          wifiStartAttempt(next);
        } else {
          WiFi.disconnect();
          wifiState = WIFI_ST_WAIT;
          wifiStateSince = now;
          logPrintf("[WIFI] No saved network reachable; trying again in %d s\n", WIFI_RECONNECT_INTERVAL / 1000);
        }
      }
      break;

    case WIFI_ST_CONNECTED:
      if (st != WL_CONNECTED) {
        wifiConnected = false;
        logPrintln("[WIFI] Connection lost; reconnecting");
        wifiStartAttempt(0);
      }
      break;

    case WIFI_ST_WAIT:
      if (now - wifiStateSince > WIFI_RECONNECT_INTERVAL) wifiStartAttempt(0);
      break;
  }

  wifiServiceNtp();
  if (wifiConnected && !webServerStarted) initWebServer();
}

void handleWebRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Field Compass v";
  html += FW_VERSION;
  html += "</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta name='color-scheme' content='dark light'>";
  html += "<style>";
  html += "body{font-family:sans-serif;margin:20px;background:#1a1a1a;color:#e0e0e0;}";
  html += "h1{color:#00ffff;}";
  html += "a{color:#00ff00;display:block;padding:12px 15px;margin:8px 0;";
  html += "text-decoration:none;background:#2a2a2a;border-radius:5px;border:1px solid #444;}";
  html += "a:hover{background:#3a3a3a;border-color:#00ff00;}";
  html += "</style></head><body>";
  html += "<h1>Field Compass v";
  html += FW_VERSION;
  html += "</h1>";
  html += "<a href='/ops'>Operational Info</a>";
  html += "<a href='/gps'>GPS</a>";
  html += "<a href='/env'>Environment</a>";
  html += "<a href='/imu'>IMU / Compass</a>";
  html += "<a href='/diags'>Diagnostics</a>";
  html += "<a href='/geocaches'>Geocache Manager</a>";
  html += "<a href='/serial'>Serial Monitor</a>";
  html += "<a href='/logs'>Serial Logs (SD)</a>";
  html += "<a href='/json'>JSON API</a>";
  html += "<p style='color:#666;margin-top:20px;font-size:12px;'>http://fieldcompass.local/</p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebOps() {
  String html = "<!DOCTYPE html><html><head><title>OPS</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>OPERATIONAL</h2><pre>";

  // Time
  char buf[64];
  if (gpsData.timeValid) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char tbuf[16];
      formatTimeStr(tbuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, true);
      sprintf(buf, "Time:    %s GPS\n", tbuf);
    } else {
      strcpy(buf, "Time:    --:--:-- GPS\n");
    }
  } else if (ntpSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char tbuf[16];
      formatTimeStr(tbuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, true);
      sprintf(buf, "Time:    %s NTP\n", tbuf);
    }
  } else {
    sprintf(buf, "Time:    --:--:-- N/A\n");
  }
  html += buf;

  // Uptime
  unsigned long totalSec = millis() / 1000;
  int days = totalSec / 86400;
  int hours = (totalSec % 86400) / 3600;
  int mins = (totalSec % 3600) / 60;
  int secs = totalSec % 60;
  if (days > 0) {
    sprintf(buf, "Uptime:  %dd %02d:%02d:%02d\n", days, hours, mins, secs);
  } else {
    sprintf(buf, "Uptime:  %02d:%02d:%02d\n", hours, mins, secs);
  }
  html += buf;

  // WiFi
  if (wifiConnected) {
    html += "WiFi:    " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")\n";
  } else {
    html += "WiFi:    Disconnected\n";
  }

  // Battery
  if (batteryAvailable && isBatteryConnected()) {
    sprintf(buf, "Battery: %.0f%% (%.2fV)\n", battery.cellPercent(), battery.cellVoltage());
    html += buf;
  } else if (batteryAvailable) {
    html += "Battery: USB Only\n";
  } else {
    html += "Battery: N/A\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebGPS() {
  String html = "<!DOCTYPE html><html><head><title>GPS</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>GPS</h2><pre>";

  char buf[64];
  if (gpsData.valid) {
    sprintf(buf, "Latitude:  %.6f %c\n", fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    html += buf;
    sprintf(buf, "Longitude: %.6f %c\n", fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    html += buf;
    sprintf(buf, "Altitude:  %.1f m\n", gpsData.altitude);
    html += buf;
    html += "Status:    Fix OK\n";
  } else if (gpsData.receiving) {
    html += "Status: Acquiring fix...\n";
    if (gpsData.timeValid) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        char tbuf[16];
        formatTimeStr(tbuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, true);
        sprintf(buf, "Time:   %s\n", tbuf);
      } else {
        strcpy(buf, "Time:   --:--:--\n");
      }
      html += buf;
    }
  } else {
    html += "Status: No GPS data\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

// GPS debug toggle (#115) — future: move to Settings LVGL screen (#112)
void handleWebGpsDebug() {
  gpsDebugEnabled = !gpsDebugEnabled;
  logPrintf("[GPS] Runtime debug logging %s\n", gpsDebugEnabled ? "ENABLED" : "DISABLED");
  String json = "{\"gpsDebug\":";
  json += gpsDebugEnabled ? "true" : "false";
  json += "}";
  webServer.send(200, "application/json", json);
}

// GPS soft reset (#115)
void handleWebGpsReset() {
  logPrintf("[GPS] Soft reset triggered via web\n");

  // Clear all GPS state
  gpsData.valid = false;
  gpsData.receiving = false;
  gpsData.latitude = 0;
  gpsData.longitude = 0;
  gpsData.altitude = 0;
  gpsData.hdop = 99.0;
  gpsData.satellites = 0;
  gpsData.speedKnots = 0;
  gpsData.timeValid = false;
  gpsData.dateValid = false;

  // Reset tracking flags
  gpsHadFirstReceive = false;
  gpsHadFirstFix = false;
  gpsFirstReceiveTime = 0;
  gpsFirstFixTime = 0;
  gpsSignalLostTime = 0;
  gpsLastByteTime = 0;
  gprmcFixThisCycle = false;
  gnrmcFixThisCycle = false;
  lastRmcCycleTime = 0;

  // Flush Serial1 RX buffer
  while (Serial1.available()) Serial1.read();

  // Send hot-restart command (MediaTek MTK chipsets — ignored by others)
  Serial1.println("$PMTK101*32");

  webServer.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"GPS reset\"}");
}

void handleWebEnv() {
  String html = "<!DOCTYPE html><html><head><title>ENV</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>ENVIRONMENT</h2><pre>";

  char buf[80];
  if (bmeAvailable || shtAvailable) {
    // SHT41 preferred for temp/humidity (#48)
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    float humid = shtAvailable ? shtData.humidity : envData.humidity;
    const char* src = shtAvailable ? "SHT41" : "BME688";
    if (useFahrenheit)
      sprintf(buf, "Temp:     %.1fF (%.1fC) [%s]\n", tempF, tempC, src);
    else
      sprintf(buf, "Temp:     %.1fC (%.1fF) [%s]\n", tempC, tempF, src);
    html += buf;
    sprintf(buf, "Humidity: %.1f%% [%s]\n", humid, src);
    html += buf;
    if (bmeAvailable) {
      sprintf(buf, "IAQ:      %.0f [%s]\n", envData.iaq, getIaqAccuracyText(envData.iaqAccuracy));
      html += buf;
      sprintf(buf, "CO2:      %.0f ppm\n", envData.co2Equivalent);
      html += buf;
      sprintf(buf, "Pressure: %.1f hPa (%.2f\")\n", envData.pressure, hPaToInHg(envData.pressure));
      html += buf;
      sprintf(buf, "Forecast: %s %s\n", getTrendArrow(), weatherTrend.forecast);
      html += buf;
    }
  } else {
    html += "BME688 not available\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebIMU() {
  String html = "<!DOCTYPE html><html><head><title>IMU</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>IMU / COMPASS</h2><pre>";

  char buf[64];
  if (imuAvailable && magAvailable) {
    sprintf(buf, "Heading: %.0f %s\n", imuData.heading, getCardinal(imuData.heading));
    html += buf;
    sprintf(buf, "Roll:    %.0f deg\n", imuData.roll);
    html += buf;
    sprintf(buf, "Pitch:   %.0f deg\n", imuData.pitch);
    html += buf;
    sprintf(buf, "Accel:   %.2f m/s2\n", imuData.accelMag);
    html += buf;
  } else {
    html += "IMU not available\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebDiags() {
  String html = "<!DOCTYPE html><html><head><title>DIAGS</title>";
  html += "<meta http-equiv='refresh' content='10'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>DIAGNOSTICS</h2><pre>";

  char buf[80];

  // Firmware info
  html += "=== Firmware ===\n";
  sprintf(buf, "Version:  %s\n", FW_VERSION);
  html += buf;
  html += "Build:    Field Compass Dual\n";
  {
    unsigned long totalSec = millis() / 1000;
    int days = totalSec / 86400;
    int hours = (totalSec % 86400) / 3600;
    int mins = (totalSec % 3600) / 60;
    int secs = totalSec % 60;
    if (days > 0) {
      sprintf(buf, "Uptime:   %dd %02d:%02d:%02d\n", days, hours, mins, secs);
    } else {
      sprintf(buf, "Uptime:   %02d:%02d:%02d\n", hours, mins, secs);
    }
    html += buf;
  }
  if (wifiConnected) {
    html += "WiFi IP:  ";
    html += WiFi.localIP().toString();
    html += "\n";
  } else {
    html += "WiFi IP:  Disconnected\n";
  }
  html += "mDNS:     fieldcompass.local\n";

  // BSEC State
  html += "\n=== BSEC State ===\n";
  sprintf(buf, "Loaded:   %s\n", bsecStateLoaded ? "Yes" : "No");
  html += buf;
  sprintf(buf, "Saved:    %s\n", bsecStateSaved ? "Yes" : "No");
  html += buf;
  sprintf(buf, "Accuracy: %d (%s)\n", envData.iaqAccuracy, getIaqAccuracyText(envData.iaqAccuracy));
  html += buf;

  // Weather Log
  html += "\n=== Weather Log ===\n";
  sprintf(buf, "In Memory: %d readings\n", weatherHistoryCount);
  html += buf;
  sprintf(buf, "Files:     %d\n", weatherLogFileCount);
  html += buf;
  sprintf(buf, "Total:     %d entries\n", weatherLogEntryCount);
  html += buf;

  // System
  html += "\n=== System ===\n";
  sprintf(buf, "Free Heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  html += buf;
  sprintf(buf, "Heap Size: %lu bytes\n", (unsigned long)ESP.getHeapSize());
  html += buf;
  sprintf(buf, "CPU Freq:  %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
  html += buf;
  if (psramFound()) {
    sprintf(buf, "PSRAM:     %luK / %luK\n",
            (unsigned long)ESP.getFreePsram() / 1024,
            (unsigned long)ESP.getPsramSize() / 1024);
  } else {
    sprintf(buf, "PSRAM:     Not detected\n");
  }
  html += buf;
  sprintf(buf, "Render:    LVGL 9.5 (2x%dKB PSRAM buffers)\n", LVGL_BUF_SIZE / 1024);
  html += buf;

  // Sensors
  html += "\n=== Sensors ===\n";
  sprintf(buf, "BME688:  %s\n", bmeAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "SHT41:   %s\n", shtAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "IMU:     %s\n", imuAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "Mag:     %s\n", magAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "Battery: %s\n", batteryAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "SD Card: %s\n", sdAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "OLED:    %s\n", oledAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "Touch:   %s\n", touchAvailable ? "OK (FT6336U)" : "N/A");
  html += buf;
  if (framAvailable) {
    sprintf(buf, "FRAM:    OK (256KB) Batt:%d/%d Wx:%d/%d %s\n",
            framHeader.battCount, FRAM_BATT_COUNT,
            framHeader.wxCount, FRAM_WX_COUNT,
            (framHeader.flags & 0x01) ? "DIRTY" : "Clean");
  } else {
    sprintf(buf, "FRAM:    N/A\n");
  }
  html += buf;

  // Mag Calibration
  html += "\n=== Mag Calibration ===\n";
  if (magCalibrated) {
    html += "Status:   Calibrated\n";
    sprintf(buf, "Offsets:  X=%.2f Y=%.2f Z=%.2f\n", magOffsetX, magOffsetY, magOffsetZ);
    html += buf;
  } else if (magCalibrating) {
    unsigned long elapsed = (millis() - magCalStartTime) / 1000;
    sprintf(buf, "Status:   Calibrating (%lus / 15s)\n", elapsed);
    html += buf;
  } else {
    html += "Status:   Not Calibrated\n";
    html += "          (Settings > Compass Cal)\n";
  }

  // Temp comparison SHT41 vs BME688 (#48)
  html += "\n=== Temperature Comparison ===\n";
  if (shtAvailable) {
    float shtF = shtData.temperature * 9.0 / 5.0 + 32.0;
    if (useFahrenheit)
      sprintf(buf, "SHT41:   %.1fF (%.1fC)\n", shtF, shtData.temperature);
    else
      sprintf(buf, "SHT41:   %.1fC (%.1fF)\n", shtData.temperature, shtF);
    html += buf;
    sprintf(buf, "SHT41 H: %.1f%%\n", shtData.humidity);
    html += buf;
  } else {
    html += "SHT41:   N/A\n";
  }
  if (bmeAvailable) {
    float bmeF = envData.temperature * 9.0 / 5.0 + 32.0;
    if (useFahrenheit)
      sprintf(buf, "BME688:  %.1fF (%.1fC)\n", bmeF, envData.temperature);
    else
      sprintf(buf, "BME688:  %.1fC (%.1fF)\n", envData.temperature, bmeF);
    html += buf;
    sprintf(buf, "BME688 H: %.1f%%\n", envData.humidity);
    html += buf;
  } else {
    html += "BME688:  N/A\n";
  }
  if (shtAvailable && bmeAvailable) {
    float deltaF = (shtData.temperature - envData.temperature) * 9.0 / 5.0;
    sprintf(buf, "Delta:   %+.1fF (SHT - BME)\n", deltaF);
    html += buf;
  }

  // GPS (#68)
  html += "\n=== GPS ===\n";

  // Status: Fix OK / Acquiring / No Data
  const char* gpsStatus;
  if (gpsData.valid) {
    gpsStatus = "Fix OK";
  } else if (gpsData.receiving) {
    gpsStatus = "Acquiring";
  } else {
    gpsStatus = "No Data";
  }
  sprintf(buf, "Status:   %s\n", gpsStatus);
  html += buf;

  // TTFF - retained once acquired
  if (gpsHadFirstFix) {
    sprintf(buf, "TTFF:     %lu seconds\n", gpsFirstFixTime / 1000);
    html += buf;
  } else {
    html += "TTFF:     (not yet acquired)\n";
  }

  // Acquiring/Reacquire time - show if not currently valid
  if (!gpsData.valid && gpsHadFirstReceive) {
    if (gpsHadFirstFix && gpsSignalLostTime > 0) {
      // Lost signal after having fix - show time since signal lost
      unsigned long reacquiring = (millis() - gpsSignalLostTime) / 1000;
      sprintf(buf, "Reacquire: %lum %lus\n", reacquiring / 60, reacquiring % 60);
    } else {
      // Never had fix, show time since first NMEA data
      unsigned long elapsed = (millis() - gpsFirstReceiveTime) / 1000;
      sprintf(buf, "Elapsed:  %lum %lus\n", elapsed / 60, elapsed % 60);
    }
    html += buf;
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebSerial() {
  String html = "<!DOCTYPE html><html><head><title>Serial Log</title>";
  html += "<meta name='color-scheme' content='dark light'>";
  html += "<style>";
  // Default (dark theme) styles
  html += ":root{--bg:#1a1a1a;--fg:#e0e0e0;--log-bg:#000;--log-fg:#00ff00;--btn-bg:#2a2a2a;--btn-fg:#00ff00;--btn-border:#444;--link:#00ffff;--header:#00ffff;--warn:#ffcc00;--error:#ff4444;}";
  // Light theme override
  html += "@media(prefers-color-scheme:light){:root{--bg:#f5f5f5;--fg:#222;--log-bg:#222;--log-fg:#00cc00;--btn-bg:#ddd;--btn-fg:#006600;--btn-border:#999;--link:#006666;--header:#008888;--warn:#cc8800;--error:#cc0000;}}";
  html += "body{font-family:sans-serif;margin:0;padding:10px;background:var(--bg);color:var(--fg);}";
  html += ".log{background:var(--log-bg);color:var(--log-fg);font-family:monospace;padding:10px;font-size:12px;";
  html += "height:500px;overflow-y:auto;border-radius:8px;white-space:pre-wrap;}";
  html += ".log .w{color:var(--warn);}.log .e{color:var(--error);}";
  html += ".controls{margin:10px 0;display:flex;align-items:center;gap:8px;}";
  html += "button{padding:8px 16px;background:var(--btn-bg);color:var(--btn-fg);";
  html += "border:1px solid var(--btn-border);border-radius:4px;cursor:pointer;}";
  html += "button:hover{opacity:0.8;}";
  html += ".badge{display:inline-block;padding:4px 12px;border-radius:12px;font-size:12px;";
  html += "background:#333;color:#aaa;cursor:pointer;user-select:none;}";
  html += ".badge.live{background:#004400;color:#00ff00;}";
  html += "a{color:var(--link);}";
  html += "</style></head><body>";
  html += "<h2 style='color:var(--header);margin:0 0 10px 0;'>Serial Log</h2>";
  html += "<div class='log' id='log'></div>";
  html += "<div class='controls'>";
  html += "<button onclick='copyLog()'>Copy</button>";
  html += "<button onclick='clearLog()'>Clear</button>";
  html += "<span class='badge live' id='scBtn' onclick='toggleScroll()'>Live &#x25BC;</span>";
  html += "<a href='/'>Back</a>";
  html += "</div>";
  html += "<script>";
  // Core state
  html += "var log=document.getElementById('log');";
  html += "var scBtn=document.getElementById('scBtn');";
  html += "var autoScroll=true;";
  // Line classification for colorization
  html += "function cls(l){";
  html += "if(/ERROR|FAILED/i.test(l))return 'e';";
  html += "if(/WARN|NOT FOUND|Dropout/i.test(l))return 'w';";
  html += "return '';";
  html += "}";
  // Append text line-by-line with color spans
  html += "function appendLines(t){";
  html += "var lines=t.split('\\n');";
  html += "for(var i=0;i<lines.length;i++){";
  html += "if(i===lines.length-1&&lines[i]==='')break;";
  html += "var s=document.createElement('span');";
  html += "var c=cls(lines[i]);";
  html += "if(c)s.className=c;";
  html += "s.textContent=lines[i]+(i<lines.length-1?'\\n':'');";
  html += "log.appendChild(s);";
  html += "}";
  html += "}";
  // Check if scrolled near bottom
  html += "function nearBottom(){return log.scrollHeight-log.scrollTop-log.clientHeight<30;}";
  // Scroll event: auto-detect pause/resume
  html += "log.addEventListener('scroll',function(){";
  html += "if(nearBottom()){if(!autoScroll){autoScroll=true;updBtn();}}";
  html += "else{if(autoScroll){autoScroll=false;updBtn();}}";
  html += "});";
  // Update badge appearance
  html += "function updBtn(){";
  html += "if(autoScroll){scBtn.textContent='Live \\u25BC';scBtn.className='badge live';}";
  html += "else{scBtn.textContent='Paused \\u25B6';scBtn.className='badge';}";
  html += "}";
  // Manual toggle via badge click
  html += "function toggleScroll(){";
  html += "autoScroll=!autoScroll;";
  html += "if(autoScroll)log.scrollTop=log.scrollHeight;";
  html += "updBtn();";
  html += "}";
  // Poll for new data
  html += "async function poll(){";
  html += "try{";
  html += "var r=await fetch('/serial-data');";
  html += "var t=await r.text();";
  html += "if(t.length>0){appendLines(t);if(autoScroll)log.scrollTop=log.scrollHeight;}";
  html += "}catch(e){}";
  html += "}";
  // Clear log
  html += "function clearLog(){log.innerHTML='';autoScroll=true;updBtn();}";
  // Copy log (try Clipboard API first, textarea fallback for HTTP)
  html += "function copyLog(){";
  html += "try{";
  html += "navigator.clipboard.writeText(log.textContent)";
  html += ".then(function(){alert('Copied!');})";
  html += ".catch(function(){copyFallback();});";
  html += "}catch(e){copyFallback();}";
  html += "}";
  html += "function copyFallback(){";
  html += "var ta=document.createElement('textarea');";
  html += "ta.value=log.textContent;";
  html += "ta.style.position='fixed';ta.style.left='-9999px';";
  html += "document.body.appendChild(ta);ta.select();";
  html += "try{document.execCommand('copy');alert('Copied!');}catch(e){alert('Copy failed');}";
  html += "document.body.removeChild(ta);";
  html += "}";
  // Start polling
  html += "setInterval(poll,100);";
  html += "</script>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebSerialData() {
  // Return buffered serial data since last request (incremental)
  String out;
  out.reserve(1024);

  uint16_t head = serialRingHead;
  uint16_t tail = serialRingTail;

  // If our read pos is behind tail, we lost data - jump to tail
  uint16_t dist = (head >= webSerialReadPos) ?
                  (head - webSerialReadPos) :
                  (SERIAL_RING_SIZE - webSerialReadPos + head);
  if (dist > SERIAL_RING_SIZE - 100) {
    webSerialReadPos = tail;
  }

  // Read available data
  while (webSerialReadPos != head && out.length() < 1024) {
    char c = serialRing[webSerialReadPos];
    webSerialReadPos = (webSerialReadPos + 1) % SERIAL_RING_SIZE;
    if (c != '\r') {
      out += c;
    }
  }

  webServer.send(200, "text/plain", out);
}

// GET /logs — List serial log files with download links (#59)
void handleWebLogs() {
  String html = "<!DOCTYPE html><html><head><title>Serial Logs</title>";
  html += "<meta name='color-scheme' content='dark light'>";
  html += "<style>";
  html += ":root{--bg:#1a1a1a;--fg:#e0e0e0;--link:#00ffff;--header:#00ffff;--tbl-border:#444;--tbl-alt:#222;}";
  html += "@media(prefers-color-scheme:light){:root{--bg:#f5f5f5;--fg:#222;--link:#006666;--header:#008888;--tbl-border:#ccc;--tbl-alt:#eee;}}";
  html += "body{font-family:sans-serif;margin:0;padding:10px;background:var(--bg);color:var(--fg);}";
  html += "table{border-collapse:collapse;width:100%;margin:10px 0;}";
  html += "th,td{padding:6px 12px;text-align:left;border-bottom:1px solid var(--tbl-border);}";
  html += "tr:nth-child(even){background:var(--tbl-alt);}";
  html += "a{color:var(--link);}";
  html += "</style></head><body>";
  html += "<h2 style='color:var(--header);margin:0 0 10px 0;'>Serial Logs</h2>";

  if (!sdAvailable || !SD.exists(LOG_DIR)) {
    html += "<p>No log files available.</p>";
  } else {
    // Flush current buffer before listing
    serialLogFlush();

    html += "<table><tr><th>File</th><th>Size</th><th>Action</th></tr>";

    File dir = SD.open(LOG_DIR);
    if (dir && dir.isDirectory()) {
      File entry = dir.openNextFile();
      while (entry) {
        if (!entry.isDirectory()) {
          const char* name = entry.name();
          size_t sz = entry.size();
          html += "<tr><td>";
          html += name;
          html += "</td><td>";
          if (sz >= 1024) {
            html += String(sz / 1024.0, 1) + " KB";
          } else {
            html += String((unsigned long)sz) + " B";
          }
          html += "</td><td><a href='/logs/download?file=";
          html += name;
          html += "'>Download</a></td></tr>";
        }
        entry.close();
        entry = dir.openNextFile();
      }
      dir.close();
    }
    html += "</table>";
  }

  if (serialLogActive) {
    html += "<p>Currently logging to: ";
    html += serialLogFilename;
    html += "</p>";
  }
  html += "<p><a href='/'>Back</a></p></body></html>";
  webServer.send(200, "text/html", html);
}

// GET /logs/download?file=<filename> — Download a serial log file (#59)
void handleWebLogDownload() {
  if (!webServer.hasArg("file")) {
    webServer.send(400, "text/plain", "Missing file parameter");
    return;
  }

  String filename = webServer.arg("file");
  // Security: prevent path traversal — only allow alphanumeric, underscore, dot, hyphen
  for (unsigned int i = 0; i < filename.length(); i++) {
    char c = filename[i];
    if (!isalnum(c) && c != '_' && c != '.' && c != '-') {
      webServer.send(400, "text/plain", "Invalid filename");
      return;
    }
  }

  // Flush current buffer if this is the active log file
  if (serialLogActive) {
    String activeName = String(serialLogFilename).substring(strlen(LOG_DIR) + 1);
    if (filename.equals(activeName)) {
      serialLogFlush();
    }
  }

  String fullPath = String(LOG_DIR) + "/" + filename;
  if (!SD.exists(fullPath)) {
    webServer.send(404, "text/plain", "File not found");
    return;
  }

  File f = SD.open(fullPath, "r");
  if (!f) {
    webServer.send(500, "text/plain", "Failed to open file");
    return;
  }

  String disposition = "attachment; filename=" + filename;
  webServer.sendHeader("Content-Disposition", disposition);
  webServer.streamFile(f, "text/plain");
  f.close();
}

void handleWebJSON() {
  char buf[1024];
  bool battConnected = batteryAvailable && isBatteryConnected();
  // SHT41 preferred for temp/humidity in JSON API (#48)
  float jsonTempC = shtAvailable ? shtData.temperature : envData.temperature;
  float jsonHumid = shtAvailable ? shtData.humidity : envData.humidity;
  snprintf(buf, sizeof(buf),
    "{"
    "\"gps\":{\"valid\":%s,\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f},"
    "\"env\":{\"temp\":%.1f,\"humidity\":%.1f,\"pressure\":%.1f,\"iaq\":%.0f,\"iaqQuality\":\"%s\",\"co2\":%.0f,\"accuracy\":%d,\"tempSource\":\"%s\"},"
    "\"imu\":{\"heading\":%.1f,\"roll\":%.1f,\"pitch\":%.1f,\"accel\":%.2f},"
    "\"system\":{\"uptime\":%lu,\"wifi\":%s,\"battery\":%.1f,\"batteryConnected\":%s,\"heap\":%lu}"
    "}",
    gpsData.valid ? "true" : "false", gpsData.latitude, gpsData.longitude, gpsData.altitude,
    jsonTempC, jsonHumid, envData.pressure, envData.iaq, getIaqQualityText(envData.iaq), envData.co2Equivalent, envData.iaqAccuracy,
    shtAvailable ? "SHT41" : "BME688",
    imuData.heading, imuData.roll, imuData.pitch, imuData.accelMag,
    millis() / 1000, wifiConnected ? "true" : "false",
    battConnected ? battery.cellPercent() : -1.0,
    battConnected ? "true" : "false",
    (unsigned long)ESP.getFreeHeap()
  );
  webServer.send(200, "application/json", buf);
}

// Battery log download endpoint
void handleWebBattLog() {
  if (!sdHealth.available) {
    webServer.send(404, "text/plain", "SD card not available");
    return;
  }

  File f = sdOpenSafe(BATT_LOG_FILE, "r", true);
  if (!f) {
    webServer.send(404, "text/plain", "Battery log file not found. Wait for data collection.");
    return;
  }

  // Stream the file with CSV content type for easy download
  webServer.sendHeader("Content-Disposition", "attachment; filename=battlog.csv");
  webServer.streamFile(f, "text/csv");
  f.close();
}

// Battery log clear endpoint
void handleWebBattLogClear() {
  if (!sdHealth.available) {
    webServer.send(404, "text/plain", "SD card not available");
    return;
  }

  if (SD.exists(BATT_LOG_FILE)) {
    SD.remove(BATT_LOG_FILE);
    webServer.send(200, "text/plain", "Battery log cleared. New log will start on next cycle.");
  } else {
    webServer.send(200, "text/plain", "Battery log already empty.");
  }
}

// GET /geocaches - Main geocache manager page
void handleWebGeocaches() {
  String html = "<!DOCTYPE html><html><head><title>Geocaches</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:sans-serif;margin:20px;background:#1a1a1a;color:#e0e0e0;}";
  html += "h1,h2{color:#00ffff;}";
  html += "form{background:#2a2a2a;padding:20px;border-radius:8px;margin:20px 0;}";
  html += "input[type=file]{margin:10px 0;color:#e0e0e0;}";
  html += "button,.btn{background:#00aa00;color:white;padding:10px 20px;border:none;";
  html += "border-radius:5px;cursor:pointer;font-size:14px;text-decoration:none;display:inline-block;margin:2px;}";
  html += "button:hover,.btn:hover{background:#00cc00;}";
  html += ".btn-warn{background:#aa6600;}.btn-warn:hover{background:#cc8800;}";
  html += ".btn-danger{background:#aa0000;}.btn-danger:hover{background:#cc0000;}";
  html += ".btn-small{padding:5px 10px;font-size:12px;}";
  html += ".cache{background:#2a2a2a;padding:12px;margin:8px 0;border-radius:5px;display:flex;justify-content:space-between;align-items:center;}";
  html += ".cache-info{flex-grow:1;}";
  html += ".cache-actions{white-space:nowrap;}";
  html += ".found{border-left:4px solid #00ff00;}";
  html += ".notfound{border-left:4px solid #666;}";
  html += ".stats{color:#888;font-size:12px;}";
  html += "a{color:#00ff00;}";
  html += ".msg{padding:15px;border-radius:5px;margin:15px 0;}";
  html += ".msg-ok{background:#004400;border:1px solid #00aa00;}";
  html += ".msg-err{background:#440000;border:1px solid #aa0000;}";
  html += "</style></head><body>";

  html += "<h1>Geocache Manager</h1>";

  // Show status message if present
  if (webServer.hasArg("msg")) {
    String msg = webServer.arg("msg");
    bool isError = webServer.hasArg("err");
    html += "<div class='msg " + String(isError ? "msg-err" : "msg-ok") + "'>" + msg + "</div>";
  }

  // Upload form
  html += "<h2>Upload GPX File</h2>";
  html += "<form method='POST' action='/geocaches/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='gpxfile' accept='.gpx,.xml'><br>";
  html += "<button type='submit'>Upload GPX</button>";
  html += "</form>";
  html += "<p class='stats'>Max file size: 64KB | Max caches: " + String(MAX_CACHES) + "</p>";

  // Action buttons
  html += "<div style='margin:15px 0;'>";
  html += "<a href='/geocaches/download' class='btn btn-small'>Download Found Status</a> ";
  html += "<a href='/geocaches/clear?confirm=yes' class='btn btn-small btn-danger' onclick=\"return confirm('Clear all caches?')\">Clear All</a>";
  html += "</div>";

  // Current cache list
  html += "<h2>Loaded Caches (" + String(cacheListCount) + ")</h2>";

  if (cacheListCount == 0) {
    html += "<p>No caches loaded. Upload a GPX file to get started.</p>";
  } else {
    for (int i = 0; i < cacheListCount; i++) {
      GeocacheEntry& c = cacheList[i];
      if (!c.valid) continue;

      html += "<div class='cache " + String(c.found ? "found" : "notfound") + "'>";
      html += "<div class='cache-info'>";
      html += "<strong>" + String(c.gcCode) + "</strong>: " + String(c.name);
      html += "<br><span class='stats'>";
      html += "D" + String(c.difficulty, 1) + "/T" + String(c.terrain, 1);
      html += " | " + String(c.latitude, 5) + ", " + String(c.longitude, 5);
      if (c.found) html += " | <span style='color:#0f0'>FOUND</span>";
      html += "</span></div>";
      html += "<div class='cache-actions'>";
      html += "<a href='/geocaches/togglefound?idx=" + String(i) + "' class='btn btn-small " + String(c.found ? "btn-warn" : "") + "'>" + String(c.found ? "Unfound" : "Found") + "</a> ";
      html += "<a href='/geocaches/delete?idx=" + String(i) + "' class='btn btn-small btn-danger' onclick=\"return confirm('Delete this cache?')\">Del</a>";
      html += "</div></div>";
    }
  }

  html += "<br><a href='/'>Back to Home</a>";
  html += "</body></html>";

  webServer.send(200, "text/html", html);
}

// POST upload data handler - receives file chunks
void handleGeocacheUploadData() {
  HTTPUpload& upload = webServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    gpxUploadBuffer = "";
    gpxUploadSuccess = false;
    gpxUploadError = "";
    logPrintf("[GEOCACHE] Upload started: %s\n", upload.filename.c_str());

    // Validate file extension
    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".gpx") && !filename.endsWith(".xml")) {
      gpxUploadError = "Invalid file type. Please upload a .gpx file.";
      return;
    }
  }

  else if (upload.status == UPLOAD_FILE_WRITE) {
    // Check size limit
    if (gpxUploadBuffer.length() + upload.currentSize > GPX_MAX_FILE_SIZE) {
      gpxUploadError = "File too large. Maximum size is 64KB.";
      return;
    }

    // Append chunk to buffer
    for (size_t i = 0; i < upload.currentSize; i++) {
      gpxUploadBuffer += (char)upload.buf[i];
    }
  }

  else if (upload.status == UPLOAD_FILE_END) {
    logPrintf("[GEOCACHE] Upload complete: %d bytes\n", gpxUploadBuffer.length());

    if (gpxUploadError.length() > 0) {
      return;  // Already have an error
    }

    // Parse GPX data (append to existing caches)
    int parsed = parseGPXFromString(gpxUploadBuffer, true);

    if (parsed <= 0) {
      gpxUploadError = "No new waypoints found (duplicates skipped or invalid).";
      return;
    }

    // Save GPX to SD with original filename (allows multi-file accumulation)
    if (sdAvailable) {
      if (!SD.exists(GEOCACHE_DIR)) {
        SD.mkdir(GEOCACHE_DIR);
      }
      String savePath = String(GEOCACHE_DIR) + "/" + upload.filename;
      File f = sdOpenSafe(savePath.c_str(), "w", true);
      if (f) {
        f.print(gpxUploadBuffer);
        f.close();
        recordSDSuccess();
        logPrintf("[GEOCACHE] Saved GPX to SD: %s (%d bytes)\n", savePath.c_str(), gpxUploadBuffer.length());
      }
    }

    // Load any saved found status
    loadCacheFoundStatus();
    gcApplyFilters();        // Filter + sort (#122)

    gpxUploadSuccess = true;
    logPrintf("[GEOCACHE] Added %d caches (total %d)\n", parsed, cacheListCount);
  }

  else if (upload.status == UPLOAD_FILE_ABORTED) {
    gpxUploadError = "Upload aborted.";
    gpxUploadBuffer = "";
  }
}

// POST upload complete - send response
void handleGeocacheUploadComplete() {
  // Clear upload buffer to free RAM
  gpxUploadBuffer = "";

  String msg;
  if (gpxUploadSuccess) {
    msg = "Loaded " + String(cacheListCount) + " geocaches successfully!";
    webServer.sendHeader("Location", "/geocaches?msg=" + msg);
  } else {
    msg = gpxUploadError.length() > 0 ? gpxUploadError : "Upload failed";
    webServer.sendHeader("Location", "/geocaches?msg=" + msg + "&err=1");
  }
  webServer.send(302, "text/plain", "Redirecting...");
}

// GET /geocaches/download - Download found status CSV
void handleGeocacheDownload() {
  if (!sdAvailable || !SD.exists(GEOCACHE_FOUND_FILE)) {
    webServer.send(404, "text/plain", "No found status file available");
    return;
  }

  File f = sdOpenSafe(GEOCACHE_FOUND_FILE, "r");
  if (!f) {
    webServer.send(500, "text/plain", "Failed to open file");
    return;
  }

  webServer.sendHeader("Content-Disposition", "attachment; filename=geocache_found.csv");
  webServer.streamFile(f, "text/csv");
  f.close();
  recordSDSuccess();
}

// GET /geocaches/delete?idx=N - Delete single cache
void handleGeocacheDelete() {
  if (!webServer.hasArg("idx")) {
    webServer.sendHeader("Location", "/geocaches?msg=Missing+index&err=1");
    webServer.send(302);
    return;
  }

  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= cacheListCount) {
    webServer.sendHeader("Location", "/geocaches?msg=Invalid+index&err=1");
    webServer.send(302);
    return;
  }

  String deletedName = String(cacheList[idx].gcCode);

  // Shift remaining caches
  for (int i = idx; i < cacheListCount - 1; i++) {
    cacheList[i] = cacheList[i + 1];
  }
  cacheListCount--;

  // Update selected index if needed
  if (selectedCacheIndex >= cacheListCount) {
    selectedCacheIndex = max(0, cacheListCount - 1);
  }

  // Re-save GPX file (regenerate from memory)
  // For simplicity, we'll just mark the change happened - a full GPX rewrite would be complex
  logPrintf("[GEOCACHE] Deleted cache %s, %d remaining\n", deletedName.c_str(), cacheListCount);

  webServer.sendHeader("Location", "/geocaches?msg=Deleted+" + deletedName);
  webServer.send(302);
}

// GET /geocaches/clear?confirm=yes - Clear all caches
void handleGeocacheClear() {
  if (!webServer.hasArg("confirm") || webServer.arg("confirm") != "yes") {
    webServer.sendHeader("Location", "/geocaches?msg=Clear+requires+confirm=yes&err=1");
    webServer.send(302);
    return;
  }

  int oldCount = cacheListCount;

  // Clear memory
  cacheListCount = 0;
  selectedCacheIndex = 0;
  memset(cacheList, 0, sizeof(cacheList));

  // Delete all GPX files from SD (but keep found status)
  if (sdAvailable) {
    if (SD.exists(GEOCACHE_GPX_FILE)) SD.remove(GEOCACHE_GPX_FILE);
    if (SD.exists(GEOCACHE_DIR)) {
      File dir = SD.open(GEOCACHE_DIR);
      if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry) {
          String name = String(GEOCACHE_DIR) + "/" + entry.name();
          entry.close();
          if (name.endsWith(".gpx")) SD.remove(name.c_str());
          entry = dir.openNextFile();
        }
        dir.close();
      }
    }
    recordSDSuccess();
  }

  logPrintf("[GEOCACHE] Cleared all %d caches\n", oldCount);
  webServer.sendHeader("Location", "/geocaches?msg=Cleared+" + String(oldCount) + "+caches");
  webServer.send(302);
}

// GET /geocaches/togglefound?idx=N - Toggle found status
void handleGeocacheToggleFound() {
  if (!webServer.hasArg("idx")) {
    webServer.sendHeader("Location", "/geocaches?msg=Missing+index&err=1");
    webServer.send(302);
    return;
  }

  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= cacheListCount) {
    webServer.sendHeader("Location", "/geocaches?msg=Invalid+index&err=1");
    webServer.send(302);
    return;
  }

  cacheList[idx].found = !cacheList[idx].found;
  if (cacheList[idx].found) {
    cacheList[idx].foundTime = millis() / 1000;  // Simple timestamp
  } else {
    cacheList[idx].foundTime = 0;
  }

  // Save updated found status
  saveCacheFoundStatus();

  String status = cacheList[idx].found ? "found" : "not+found";
  webServer.sendHeader("Location", "/geocaches?msg=" + String(cacheList[idx].gcCode) + "+marked+" + status);
  webServer.send(302);
}

void initWebServer() {
  if (!wifiConnected) return;
  if (webServerStarted) return;  // Already started

  LOG_INFO("Starting web server...");

  // Setup mDNS
  if (MDNS.begin("fieldcompass")) {
    LOG_INFO("mDNS OK (fieldcompass.local)");
  }

  // Register handlers
  webServer.on("/", handleWebRoot);
  webServer.on("/ops", handleWebOps);
  webServer.on("/gps", handleWebGPS);
  webServer.on("/env", handleWebEnv);
  webServer.on("/imu", handleWebIMU);
  webServer.on("/diags", handleWebDiags);
  webServer.on("/serial", handleWebSerial);
  webServer.on("/serial-data", handleWebSerialData);
  // GPS debug/reset endpoints (#115)
  webServer.on("/gps/debug", HTTP_GET, handleWebGpsDebug);
  webServer.on("/gps/reset", HTTP_GET, handleWebGpsReset);
  webServer.on("/json", handleWebJSON);
  webServer.on("/battlog", handleWebBattLog);
  webServer.on("/battlog/clear", handleWebBattLogClear);

  // Geocache manager endpoints (#70)
  webServer.on("/geocaches", HTTP_GET, handleWebGeocaches);
  webServer.on("/geocaches/upload", HTTP_POST, handleGeocacheUploadComplete, handleGeocacheUploadData);
  webServer.on("/geocaches/download", HTTP_GET, handleGeocacheDownload);
  webServer.on("/geocaches/delete", HTTP_GET, handleGeocacheDelete);
  webServer.on("/geocaches/clear", HTTP_GET, handleGeocacheClear);
  webServer.on("/geocaches/togglefound", HTTP_GET, handleGeocacheToggleFound);

  // Serial log file endpoints (#59)
  webServer.on("/logs", HTTP_GET, handleWebLogs);
  webServer.on("/logs/download", HTTP_GET, handleWebLogDownload);

  webServer.begin();
  webServerStarted = true;
  LOG_INFO("Web server OK — http://%s/", WiFi.localIP().toString().c_str());
}
