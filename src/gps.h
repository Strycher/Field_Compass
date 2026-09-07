#pragma once
// gps.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Arduino.h>

#define GPS_BAUD 9600
#define GPS_RX RX
#define GPS_TX TX
#define DEBUG_GPS   0  // GPS NMEA sentence logging
#define GPS_STALE_MS  5000   // No bytes for 5s → clear receiving/valid (#115)
#define NMEA_MAX_FIELDS 20

struct GpsData {
  bool valid = false;
  bool receiving = false;
  float latitude = 0;
  float longitude = 0;
  float altitude = 0;
  float hdop = 99.0;       // Horizontal dilution of precision (lower = better) (#70)
  int satellites = 0;      // Number of satellites in fix (#70)
  int hour = 0;
  int minute = 0;
  int second = 0;
  int day = 0;             // Date from RMC sentence
  int month = 0;
  int year = 0;
  bool timeValid = false;
  bool dateValid = false;  // True when date has been parsed from RMC
  float speedKnots = 0;   // Ground speed from RMC sentence
};

extern GpsData gpsData;
extern char gpsBuffer[128];
extern int gpsBufferIndex;
extern bool gpsDebugEnabled;
extern unsigned long gpsFirstFixTime;
extern unsigned long gpsFirstReceiveTime;
extern bool gpsHadFirstFix;
extern bool gpsHadFirstReceive;
extern unsigned long gpsLastByteTime;
extern unsigned long gpsSignalLostTime;
extern bool gprmcFixThisCycle;
extern bool gnrmcFixThisCycle;
extern unsigned long lastRmcCycleTime;
extern bool rtcSyncedFromGPS;

void initGPS();
void readGPS();
int nmeaParse(char* sentence, char* fields[], int maxFields);
void parseNMEA(char* sentence);
float getGpsAccuracyMeters();
