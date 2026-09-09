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

// Two banks, one atomic switch. Bank 0 uses keys "n0", "s0".."s4", "p0".."p4";
// bank 1 uses "n1", "t0".."t4", "q0".."q4"; "bank" says which is live. A change
// is written whole into the bank that is NOT live, then "bank" is flipped, one
// NVS key write, which NVS makes atomic. A power cut anywhere before the flip
// leaves the live bank untouched; after it, the new bank is complete. The
// earlier in-place scheme could pair an old SSID with a new password when a
// cut landed between the two keys of a shifted entry (review finding on #295).
static uint8_t liveBank = 0;

static void bankKey(char* key, size_t len, uint8_t bank, char kind, int i) {
  // kind 's' = SSID, 'p' = password; bank 1 uses 't' and 'q'
  char c = (bank == 0) ? kind : (kind == 's' ? 't' : 'q');
  snprintf(key, len, "%c%d", c, i);
}

static void persist() {
  if (!prefs.begin(kNamespace, false)) {
    logPrintln("[WIFI] store: NVS open for write failed; change not saved");
    return;
  }
  uint8_t target = liveBank ? 0 : 1;
  char key[4];
  for (int i = 0; i < credCount; i++) {
    bankKey(key, sizeof key, target, 's', i);
    prefs.putString(key, creds[i].ssid);
    bankKey(key, sizeof key, target, 'p', i);
    prefs.putString(key, creds[i].pass);
  }
  snprintf(key, sizeof key, "n%d", target);
  prefs.putUChar(key, (uint8_t)credCount);
  prefs.putUChar("bank", target);            // the switch
  liveBank = target;
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
  liveBank = prefs.getUChar("bank", 0) ? 1 : 0;
  char key[4];
  snprintf(key, sizeof key, "n%d", liveBank);
  int n = prefs.getUChar(key, 0);
  if (!prefs.isKey("bank") && prefs.isKey("n")) {
    // Written by the first #295 build (deac3d1, one bank, count under "n"):
    // read it as bank 0; the next change rewrites it in the two-bank form.
    n = prefs.getUChar("n", 0);
    liveBank = 0;
  }
  if (n > WIFI_STORE_MAX) n = WIFI_STORE_MAX;
  for (int i = 0; i < n; i++) {
    WifiCred c = {};
    bankKey(key, sizeof key, liveBank, 's', i);
    prefs.getString(key, c.ssid, sizeof c.ssid);
    bankKey(key, sizeof key, liveBank, 'p', i);
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
    char* p = line;
    // Notepad and friends write a UTF-8 BOM at the top of a file, and a block
    // pasted from such a file carries one mid-file; neither is part of "ssid=".
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    if (!p[0] || p[0] == '#' || p[0] == '[') continue;
    if (strncmp(p, "ssid=", 5) == 0) {
      if (open && cur.ssid[0] && wifiStoreAdd(cur.ssid, cur.pass)) imported++;
      memset(&cur, 0, sizeof cur);
      strlcpy(cur.ssid, p + 5, sizeof cur.ssid);
      for (int n = strlen(cur.ssid); n > 0 && (cur.ssid[n - 1] == ' ' || cur.ssid[n - 1] == '\t'); n--) {
        cur.ssid[n - 1] = '\0';   // an SSID never ends in whitespace on purpose; an editor's does
      }
      open = true;
    } else if (strncmp(p, "pass=", 5) == 0) {
      strlcpy(cur.pass, p + 5, sizeof cur.pass);
      // A passphrase may legitimately contain spaces, so it is kept exactly;
      // but say so, because a stray trailing space is the classic "rejected".
      size_t n = strlen(cur.pass);
      if (n && (cur.pass[0] == ' ' || cur.pass[n - 1] == ' ' || cur.pass[n - 1] == '\t')) {
        logPrintf("[WIFI] import: password for %s begins or ends with whitespace, kept as written\n", cur.ssid);
      }
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
