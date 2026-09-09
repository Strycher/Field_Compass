#pragma once
// wifi_store.h -- saved WiFi networks in NVS, and the one-shot SD import (#295, epic #99).
//
// The store is the ESP32's NVS through Preferences, namespace "wifi": up to
// WIFI_STORE_MAX entries in priority order (index 0 is tried first). Nothing
// here is in git or on the removable card; it is plain text on the flash chip
// by decision (docs/plans/2026-09-09-wifi-configuration.md, "encrypt on NVS?").
//
// Pre-loading: /config/wifi.txt on the SD card, the #99 format:
//   [0]
//   ssid=MyNetwork
//   pass=secret
//   [1]
//   ...
// wifiStoreImportFromSD() reads it at boot and adds the entries (deduplicated
// by SSID). The file is never modified or removed here: the owner is asked
// Remove or Keep on the screen (#297) and the web page (#296), and
// wifiImportFileRemove() is what those call. A repeat import is harmless.
//
// SSIDs may be logged. Passwords are never logged, never sent to a page, and
// never leave this unit except into WiFi.begin().
#include <Arduino.h>

#define WIFI_STORE_MAX   5
#define WIFI_SSID_MAX    32     // 802.11 SSID limit
#define WIFI_PASS_MAX    63     // WPA2 passphrase limit
#define WIFI_IMPORT_PATH "/config/wifi.txt"

struct WifiCred {
  char ssid[WIFI_SSID_MAX + 1];
  char pass[WIFI_PASS_MAX + 1];
};

void wifiStoreInit();                                   // open NVS, load the list (call once, before initWiFi's first attempt)
int  wifiStoreCount();
bool wifiStoreGet(int idx, WifiCred& out);              // false if idx is out of range
bool wifiStoreAdd(const char* ssid, const char* pass);  // same SSID: password updated in place; full: last slot replaced
bool wifiStoreRemove(int idx);
bool wifiStoreMoveUp(int idx);                          // swap with idx-1 (higher priority)
void wifiStoreClear();                                  // the "forget all" -- callers confirm first

int  wifiStoreImportFromSD();                           // entries imported, or -1 when there is no file / no card
bool wifiImportFileRemove();                            // delete /config/wifi.txt (the owner's Remove)
extern int  wifiImportedCount;                          // result of the boot import, for the prompts
extern bool wifiImportFilePresent;                      // /config/wifi.txt still on the card
