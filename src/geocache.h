#pragma once
// geocache.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Arduino.h>

#define MAX_CACHES 20
#define GEOCACHE_GPX_FILE "/geocaches/caches.gpx"
#define GEOCACHE_DIR "/geocaches"
#define GPX_MAX_FILE_SIZE (64 * 1024)  // 64KB max upload
#define GEOCACHE_FOUND_FILE "/geocache_found.csv"

struct GeocacheEntry {
  bool valid;
  float latitude;
  float longitude;
  float difficulty;
  float terrain;
  char name[40];
  char hint[80];
  char gcCode[12];      // GC code (e.g., "GC12345")
  bool found;           // Found status
  uint32_t foundTime;   // When found (unix timestamp)
};

extern GeocacheEntry cacheList[MAX_CACHES];
extern int cacheListCount;
extern int gcFilterFoundMode;
extern float gcFilterDMin, gcFilterDMax;
extern float gcFilterTMin, gcFilterTMax;
extern float gcFilterMaxDistKm;
extern int gcSortMode;
extern float gcCachedDist[MAX_CACHES];
extern int gcFilteredIndices[MAX_CACHES];
extern int gcFilteredCount;
extern unsigned long gcLastSortTime;
extern int listHighlightIndex;

void saveCacheFoundStatus();
void loadCacheFoundStatus();
void decodeROT13(char* str);
bool extractXMLField(const String& xml, const char* startTag, const char* endTag, char* dest, size_t destSize);
float extractXMLFloat(const String& xml, const char* startTag, const char* endTag, float defaultVal);
int parseGPXFromString(const String& gpxData, bool append = false);
void gcUpdateDistances();
void gcApplyFilters();
void loadGeocachesFromSD();
