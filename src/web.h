#pragma once
// web.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>

#define WEB_SERVER_PORT 80
#define WIFI_RECONNECT_INTERVAL 30000

extern WebServer webServer;
extern bool wifiConnected;

void initWiFi();
void checkWiFi();
void handleWebRoot();
void handleWebOps();
void handleWebGPS();
void handleWebGpsDebug();
void handleWebGpsReset();
void handleWebEnv();
void handleWebIMU();
void handleWebDiags();
void handleWebSerial();
void handleWebSerialData();
void handleWebLogs();
void handleWebLogDownload();
void handleWebJSON();
void handleWebBattLog();
void handleWebBattLogClear();
void handleWebGeocaches();
void handleGeocacheUploadData();
void handleGeocacheUploadComplete();
void handleGeocacheDownload();
void handleGeocacheDelete();
void handleGeocacheClear();
void handleGeocacheToggleFound();
void initWebServer();
