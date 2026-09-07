#pragma once
// web.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <WiFi.h>
#include <FS.h>
// WebServer.h names the unqualified `FS`. FS.h provides `using fs::FS;` unless
// FS_NO_GLOBALS is defined -- and TFT_eSPI defines it, so any unit that includes
// display.h before this header would lose the alias. src.ino included
// WebServer.h before TFT_eSPI.h, which hid this.
using fs::FS;
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
