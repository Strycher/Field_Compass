// wifi_store.cpp -- saved WiFi networks in NVS, and the one-shot SD import (#295, epic #99).
#include "wifi_store.h"
#include "logging.h"
#include <Preferences.h>
#include <SD.h>

static const char* kNamespace = "wifi";

static Preferences prefs;
static WifiCred creds[WIFI_STORE_MAX];
static int credCount = 0;

int  wifiImportedCount = 0;
bool wifiImportFilePresent = false;

// Keys: "n" = count, "s0".."s4" = SSIDs, "p0".."p4" = passwords. The whole
// list is rewritten on every change; five entries is not worth a delta.
static void persist() {
  if (!prefs.begin(kNamespace, false)) {
    logPrintln("[WIFI] store: NVS open for write failed; change not saved");
    return;
  }
  prefs.clear();
  prefs.putUChar("n", (uint8_t)credCount);
  char key[4];
  for (int i = 0; i < credCount; i++) {
    snprintf(key, sizeof key, "s%d", i);
    prefs.putString(key, creds[i].ssid);
    snprintf(key, sizeof key, "p%d", i);
    prefs.putString(key, creds[i].pass);
  }
  prefs.end();
}

void wifiStoreInit() {
  credCount = 0;
  memset(creds, 0, sizeof creds);
  // Read-only open fails when the namespace has never been written: a fresh
  // chip. That is "no saved networks", not an error.
  if (!prefs.begin(kNamespace, true)) {
    logPrintln("[WIFI] store: empty (nothing saved yet)");
    return;
  }
  int n = prefs.getUChar("n", 0);
  if (n > WIFI_STORE_MAX) n = WIFI_STORE_MAX;
  char key[4];
  for (int i = 0; i < n; i++) {
    WifiCred c = {};
    snprintf(key, sizeof key, "s%d", i);
    prefs.getString(key, c.ssid, sizeof c.ssid);
    snprintf(key, sizeof key, "p%d", i);
    prefs.getString(key, c.pass, sizeof c.pass);
    if (c.ssid[0]) creds[credCount++] = c;   // compacts any hole left by a bad write
  }
  prefs.end();
  logPrintf("[WIFI] store: %d saved network(s)\n", credCount);
  for (int i = 0; i < credCount; i++) logPrintf("  %d: %s\n", i, creds[i].ssid);
}

int wifiStoreCount() {
  return credCount;
}

bool wifiStoreGet(int idx, WifiCred& out) {
  if (idx < 0 || idx >= credCount) return false;
  out = creds[idx];
  return true;
}

static int findSsid(const char* ssid) {
  for (int i = 0; i < credCount; i++) {
    if (strcmp(creds[i].ssid, ssid) == 0) return i;
  }
  return -1;
}

bool wifiStoreAdd(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0] || strlen(ssid) > WIFI_SSID_MAX) return false;
  if (!pass) pass = "";
  if (strlen(pass) > WIFI_PASS_MAX) return false;
  int idx = findSsid(ssid);
  if (idx >= 0) {
    strlcpy(creds[idx].pass, pass, sizeof creds[idx].pass);   // same network, new password; priority unchanged
  } else {
    if (credCount < WIFI_STORE_MAX) {
      idx = credCount++;
    } else {
      idx = WIFI_STORE_MAX - 1;                                  // full: the lowest-priority slot goes
      logPrintf("[WIFI] store full; replacing %s\n", creds[idx].ssid);
    }
    memset(&creds[idx], 0, sizeof creds[idx]);
    strlcpy(creds[idx].ssid, ssid, sizeof creds[idx].ssid);
    strlcpy(creds[idx].pass, pass, sizeof creds[idx].pass);
  }
  persist();
  logPrintf("[WIFI] store: saved %s at priority %d\n", ssid, idx);
  return true;
}

bool wifiStoreRemove(int idx) {
  if (idx < 0 || idx >= credCount) return false;
  logPrintf("[WIFI] store: removed %s\n", creds[idx].ssid);
  for (int i = idx; i < credCount - 1; i++) creds[i] = creds[i + 1];
  credCount--;
  memset(&creds[credCount], 0, sizeof creds[credCount]);
  persist();
  return true;
}

bool wifiStoreMoveUp(int idx) {
  if (idx <= 0 || idx >= credCount) return false;
  WifiCred t = creds[idx - 1];
  creds[idx - 1] = creds[idx];
  creds[idx] = t;
  persist();
  return true;
}

void wifiStoreClear() {
  credCount = 0;
  memset(creds, 0, sizeof creds);
  persist();
  logPrintln("[WIFI] store: all saved networks forgotten");
}

// ---- SD import -------------------------------------------------------------

// One line, CR/LF stripped, truncated to the buffer. Returns false at EOF
// with nothing read.
static bool readLine(File& f, char* buf, size_t len) {
  size_t n = 0;
  bool any = false;
  while (f.available()) {
    int c = f.read();
    if (c < 0) break;
    any = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (n < len - 1) buf[n++] = (char)c;
  }
  buf[n] = '\0';
  return any;
}

int wifiStoreImportFromSD() {
  wifiImportedCount = 0;
  wifiImportFilePresent = false;
  if (!sdHealth.available) return -1;
  if (!SD.exists(WIFI_IMPORT_PATH)) return -1;
  wifiImportFilePresent = true;

  File f = sdOpenSafe(WIFI_IMPORT_PATH, "r", true);
  if (!f) {
    logPrintf("[WIFI] import: %s present but would not open\n", WIFI_IMPORT_PATH);
    return -1;
  }

  // "[n]" headers are ignored; every "ssid=" starts an entry and the next
  // "ssid=" or the end of the file closes it. "#" lines are comments.
  char line[WIFI_SSID_MAX + WIFI_PASS_MAX + 8];
  WifiCred cur = {};
  bool open = false;
  int imported = 0;
  while (readLine(f, line, sizeof line)) {
    if (!line[0] || line[0] == '#' || line[0] == '[') continue;
    if (strncmp(line, "ssid=", 5) == 0) {
      if (open && cur.ssid[0] && wifiStoreAdd(cur.ssid, cur.pass)) imported++;
      memset(&cur, 0, sizeof cur);
      strlcpy(cur.ssid, line + 5, sizeof cur.ssid);
      open = true;
    } else if (strncmp(line, "pass=", 5) == 0) {
      strlcpy(cur.pass, line + 5, sizeof cur.pass);
    }
  }
  if (open && cur.ssid[0] && wifiStoreAdd(cur.ssid, cur.pass)) imported++;
  f.close();

  wifiImportedCount = imported;
  logPrintf("[WIFI] import: %d network(s) from %s; file left in place until you say Remove\n",
            imported, WIFI_IMPORT_PATH);
  return imported;
}

bool wifiImportFileRemove() {
  if (!sdHealth.available) return false;
  bool ok = SD.remove(WIFI_IMPORT_PATH);
  if (ok) wifiImportFilePresent = false;
  logPrintf("[WIFI] import: %s %s\n", WIFI_IMPORT_PATH, ok ? "removed" : "NOT removed");
  return ok;
}
