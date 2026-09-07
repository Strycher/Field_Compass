#pragma once
// geo.h -- extracted from src.ino by scripts/extract_unit.py (#262, E4).

#define LOCATION_THRESHOLD    0.01     // ~1km in degrees

bool sameLocation(float lat1, float lon1, float lat2, float lon2);
const char* getCardinal(float heading);
float calcDistanceKm(float lat1, float lon1, float lat2, float lon2);
float calcBearing(float lat1, float lon1, float lat2, float lon2);
