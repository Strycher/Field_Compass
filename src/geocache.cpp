// geocache.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "geocache.h"
#include "gps.h"
#include "geo.h"
#include "settings.h"
#include "logging.h"

GeocacheEntry cacheList[MAX_CACHES];  // ~4KB RAM for 20 caches
int cacheListCount = 0;               // Number of loaded caches
int listHighlightIndex = 0;           // Currently highlighted item in list
int gcFilterFoundMode = 0;            // 0=all, 1=unfound only, 2=found only
float gcFilterDMin = 1.0f, gcFilterDMax = 5.0f;   // Difficulty range
float gcFilterTMin = 1.0f, gcFilterTMax = 5.0f;   // Terrain range
float gcFilterMaxDistKm = 0;          // 0 = disabled (show all)
int gcSortMode = 0;                   // 0=distance, 1=name
float gcCachedDist[MAX_CACHES];       // Cached distances for sort/filter
int gcFilteredIndices[MAX_CACHES];    // Indices into cacheList that pass filter
int gcFilteredCount = 0;              // Number of caches passing filter
unsigned long gcLastSortTime = 0;     // Throttle re-sorts

// Save found status for all caches to SD card
void saveCacheFoundStatus() {
  if (!sdAvailable) return;

  File file = sdOpenSafe(GEOCACHE_FOUND_FILE, "w", true);  // silent — don't red-flag SD
  if (!file) {
    logPrintln("[GEOCACHE] Failed to save found status");
    return;
  }

  // Write header
  file.println("gcCode,found,timestamp");

  // Write each cache's found status
  for (int i = 0; i < cacheListCount; i++) {
    if (cacheList[i].valid && cacheList[i].found) {
      file.printf("%s,1,%lu\n", cacheList[i].gcCode, cacheList[i].foundTime);
    }
  }

  file.close();
  recordSDSuccess();
  logPrintf("[GEOCACHE] Saved found status for %d caches\n", cacheListCount);
}

// Load found status from SD card and apply to loaded caches
void loadCacheFoundStatus() {
  if (!sdAvailable) return;

  File file = sdOpenSafe(GEOCACHE_FOUND_FILE, "r", true);  // silent fail
  if (!file) {
    logPrintln("[GEOCACHE] No saved found status file");
    return;
  }

  // Skip header line
  file.readStringUntil('\n');

  int loadedCount = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse CSV: gcCode,found,timestamp
    int comma1 = line.indexOf(',');
    int comma2 = line.indexOf(',', comma1 + 1);
    if (comma1 < 0 || comma2 < 0) continue;

    String gcCode = line.substring(0, comma1);
    String foundStr = line.substring(comma1 + 1, comma2);
    String timestampStr = line.substring(comma2 + 1);

    // Find matching cache and apply found status
    for (int i = 0; i < cacheListCount; i++) {
      if (strcmp(cacheList[i].gcCode, gcCode.c_str()) == 0) {
        cacheList[i].found = (foundStr == "1");
        cacheList[i].foundTime = timestampStr.toInt();
        loadedCount++;
        break;
      }
    }
  }

  file.close();
  recordSDSuccess();
  logPrintf("[GEOCACHE] Loaded found status: %d entries\n", loadedCount);
}

// Decode ROT13 hint in place (geocaching.com encodes hints this way)
void decodeROT13(char* str) {
  for (int i = 0; str[i]; i++) {
    char c = str[i];
    if ((c >= 'A' && c <= 'M') || (c >= 'a' && c <= 'm')) {
      str[i] = c + 13;
    } else if ((c >= 'N' && c <= 'Z') || (c >= 'n' && c <= 'z')) {
      str[i] = c - 13;
    }
  }
}

// Extract text between XML tags into destination buffer
bool extractXMLField(const String& xml, const char* startTag, const char* endTag,
                     char* dest, size_t destSize) {
  int start = xml.indexOf(startTag);
  if (start < 0) return false;
  start += strlen(startTag);

  int end = xml.indexOf(endTag, start);
  if (end < 0) return false;

  String value = xml.substring(start, end);
  value.trim();

  // Decode common HTML entities
  value.replace("&amp;", "&");
  value.replace("&lt;", "<");
  value.replace("&gt;", ">");
  value.replace("&quot;", "\"");
  value.replace("&#39;", "'");
  value.replace("&apos;", "'");

  strncpy(dest, value.c_str(), destSize - 1);
  dest[destSize - 1] = '\0';
  return true;
}

// Extract float value from XML tags with default
float extractXMLFloat(const String& xml, const char* startTag, const char* endTag, float defaultVal) {
  char buf[16];
  if (extractXMLField(xml, startTag, endTag, buf, sizeof(buf))) {
    return atof(buf);
  }
  return defaultVal;
}

// Parse GPX data and populate cacheList[]
// Returns: number of caches successfully parsed
int parseGPXFromString(const String& gpxData, bool append) {
  if (!append) {
    cacheListCount = 0;
    gcFilteredCount = 0;  // Reset filtered list (#122)
  }

  int searchPos = 0;
  int added = 0;
  while (cacheListCount < MAX_CACHES) {
    // Find next <wpt> element
    int wptStart = gpxData.indexOf("<wpt", searchPos);
    if (wptStart < 0) break;

    int wptEnd = gpxData.indexOf("</wpt>", wptStart);
    if (wptEnd < 0) break;

    String wptBlock = gpxData.substring(wptStart, wptEnd + 6);
    searchPos = wptEnd + 6;

    GeocacheEntry entry;
    memset(&entry, 0, sizeof(entry));

    // Extract lat/lon from <wpt lat="..." lon="...">
    int latPos = wptBlock.indexOf("lat=\"");
    int lonPos = wptBlock.indexOf("lon=\"");
    if (latPos < 0 || lonPos < 0) continue;

    entry.latitude = wptBlock.substring(latPos + 5, wptBlock.indexOf("\"", latPos + 5)).toFloat();
    entry.longitude = wptBlock.substring(lonPos + 5, wptBlock.indexOf("\"", lonPos + 5)).toFloat();

    // Validate coordinates
    if (entry.latitude < -90 || entry.latitude > 90 ||
        entry.longitude < -180 || entry.longitude > 180) continue;

    // Extract <name> (GC code)
    extractXMLField(wptBlock, "<name>", "</name>", entry.gcCode, sizeof(entry.gcCode));

    // Extract display name - try groundspeak:name first, then desc
    if (!extractXMLField(wptBlock, "<groundspeak:name>", "</groundspeak:name>",
                         entry.name, sizeof(entry.name))) {
      extractXMLField(wptBlock, "<desc>", "</desc>", entry.name, sizeof(entry.name));
    }

    // If still no name, use GC code
    if (strlen(entry.name) == 0) {
      strncpy(entry.name, entry.gcCode, sizeof(entry.name) - 1);
    }

    // Extract difficulty/terrain (default 2.5 if not found)
    entry.difficulty = extractXMLFloat(wptBlock, "<groundspeak:difficulty>", "</groundspeak:difficulty>", 2.5);
    entry.terrain = extractXMLFloat(wptBlock, "<groundspeak:terrain>", "</groundspeak:terrain>", 2.5);

    // Extract hint (decode ROT13 if present)
    extractXMLField(wptBlock, "<groundspeak:encoded_hints>", "</groundspeak:encoded_hints>",
                    entry.hint, sizeof(entry.hint));
    decodeROT13(entry.hint);  // Geocaching.com encodes hints in ROT13

    entry.valid = true;
    entry.found = false;
    entry.foundTime = 0;

    // Skip duplicate GC codes (same cache uploaded twice)
    bool dup = false;
    for (int d = 0; d < cacheListCount; d++) {
      if (strcmp(cacheList[d].gcCode, entry.gcCode) == 0) { dup = true; break; }
    }
    if (dup) continue;

    cacheList[cacheListCount++] = entry;
    added++;
  }

  logPrintf("[GEOCACHE] Parsed %d new waypoints (total %d)\n", added, cacheListCount);
  return added;
}

// Update cached distances for all caches (#122)
void gcUpdateDistances() {
  if (!gpsData.valid) return;
  for (int i = 0; i < cacheListCount; i++) {
    gcCachedDist[i] = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                       cacheList[i].latitude, cacheList[i].longitude);
  }
}

// Build filtered + sorted index list (#122)
// No physical reorder of cacheList — sort the filtered indices instead
void gcApplyFilters() {
  gcUpdateDistances();

  // Step 1: filter into gcFilteredIndices
  gcFilteredCount = 0;
  for (int i = 0; i < cacheListCount; i++) {
    GeocacheEntry& c = cacheList[i];
    if (gcFilterFoundMode == 1 && c.found) continue;
    if (gcFilterFoundMode == 2 && !c.found) continue;
    if (c.difficulty < gcFilterDMin || c.difficulty > gcFilterDMax) continue;
    if (c.terrain < gcFilterTMin || c.terrain > gcFilterTMax) continue;
    if (gcFilterMaxDistKm > 0 && gpsData.valid && gcCachedDist[i] > gcFilterMaxDistKm) continue;
    gcFilteredIndices[gcFilteredCount++] = i;
  }

  // Step 2: sort filtered indices (insertion sort, max 20)
  for (int i = 1; i < gcFilteredCount; i++) {
    int key = gcFilteredIndices[i];
    int j = i - 1;
    if (gcSortMode == 0 && gpsData.valid) {
      float kd = gcCachedDist[key];
      while (j >= 0 && gcCachedDist[gcFilteredIndices[j]] > kd) {
        gcFilteredIndices[j + 1] = gcFilteredIndices[j]; j--;
      }
    } else if (gcSortMode == 1) {
      while (j >= 0 && strcasecmp(cacheList[gcFilteredIndices[j]].name, cacheList[key].name) > 0) {
        gcFilteredIndices[j + 1] = gcFilteredIndices[j]; j--;
      }
    }
    gcFilteredIndices[j + 1] = key;
  }
  gcLastSortTime = millis();
  listHighlightIndex = 0;
}

// Load geocaches from all GPX files in /geocaches/ directory
void loadGeocachesFromSD() {
  if (!sdAvailable) {
    logPrintln("[GEOCACHE] SD not available, skipping GPX load");
    return;
  }

  // Support legacy single-file format
  if (SD.exists(GEOCACHE_GPX_FILE)) {
    File f = sdOpenSafe(GEOCACHE_GPX_FILE, "r", true);
    if (f) {
      size_t fileSize = f.size();
      if (fileSize <= GPX_MAX_FILE_SIZE) {
        String gpxData;
        gpxData.reserve(fileSize);
        while (f.available()) gpxData += (char)f.read();
        f.close();
        recordSDSuccess();
        parseGPXFromString(gpxData, false);  // Replace mode for legacy file
      } else {
        f.close();
      }
    }
  }

  // Scan directory for individual GPX files (multi-file upload support)
  if (!SD.exists(GEOCACHE_DIR)) return;
  File dir = SD.open(GEOCACHE_DIR);
  if (!dir || !dir.isDirectory()) return;

  File entry = dir.openNextFile();
  while (entry && cacheListCount < MAX_CACHES) {
    String name = entry.name();
    if (!entry.isDirectory() && name.endsWith(".gpx") && name != "caches.gpx") {
      size_t sz = entry.size();
      if (sz > 0 && sz <= GPX_MAX_FILE_SIZE) {
        String gpxData;
        gpxData.reserve(sz);
        while (entry.available()) gpxData += (char)entry.read();
        parseGPXFromString(gpxData, true);  // Append mode
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  if (cacheListCount > 0) {
    loadCacheFoundStatus();
    gcApplyFilters();
    logPrintf("[GEOCACHE] Loaded %d caches from SD\n", cacheListCount);
  } else {
    logPrintln("[GEOCACHE] No valid waypoints found on SD");
  }
}
