// gps.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "gps.h"
#include "settings.h"
#include "rtc.h"
#include "logging.h"

bool rtcSyncedFromGPS = false;    // RTC was synced from GPS this session
GpsData gpsData;
char gpsBuffer[128];
int gpsBufferIndex = 0;
unsigned long gpsFirstReceiveTime = 0;  // When first NMEA data received
unsigned long gpsFirstFixTime = 0;       // When first valid fix acquired
unsigned long gpsSignalLostTime = 0;    // When signal was lost (for reacquire timing)
bool gpsHadFirstReceive = false;         // Tracks if we ever received data
bool gpsHadFirstFix = false;             // Tracks if we ever had a fix
unsigned long gpsLastByteTime = 0;    // Timestamp of last serial byte (#115)
bool gpsDebugEnabled = false;          // Runtime GPS debug logging (#115)
bool gprmcFixThisCycle = false;
bool gnrmcFixThisCycle = false;   // published: handleWebGpsReset clears it (#271)
unsigned long lastRmcCycleTime = 0;

void initGPS() {
  logPrintf("Initializing GPS on RX=%d, TX=%d... ", GPS_RX, GPS_TX);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(100);  // Let UART settle before sending command
  Serial1.println("$PMTK101*32");  // Hot restart — ensures search is active (#115)
  logPrintln("OK (9600 baud, PMTK101 sent)");
}

void readGPS() {
  while (Serial1.available()) {
    char c = Serial1.read();
    gpsLastByteTime = millis();  // Track for staleness (#115)
    gpsData.receiving = true;

    // Track when GPS first starts receiving NMEA data (#68)
    if (!gpsHadFirstReceive) {
      gpsFirstReceiveTime = millis();
      gpsHadFirstReceive = true;
      logPrintf("[GPS] First NMEA data at %lus\n", gpsFirstReceiveTime / 1000);
    }

    if (c == '\n') {
      gpsBuffer[gpsBufferIndex] = '\0';
      #if DEBUG_GPS
      Serial.println(gpsBuffer);
      #endif
      // Runtime NMEA logging: RMC+GGA only to avoid I2C starvation (#115)
      if (gpsDebugEnabled && (strstr(gpsBuffer, "RMC,") || strstr(gpsBuffer, "GGA,"))) {
        logPrintf("[GPS:RAW] %s\n", gpsBuffer);
      }
      parseNMEA(gpsBuffer);
      gpsBufferIndex = 0;
    } else if (c != '\r' && gpsBufferIndex < sizeof(gpsBuffer) - 1) {
      gpsBuffer[gpsBufferIndex++] = c;
    }
  }

  // Staleness check: no bytes for GPS_STALE_MS → clear state (#115)
  if (gpsData.receiving && gpsLastByteTime > 0 &&
      (millis() - gpsLastByteTime > GPS_STALE_MS)) {
    if (gpsDebugEnabled) {
      logPrintf("[GPS:DBG] Stale — no data for %lums\n", millis() - gpsLastByteTime);
    }
    gpsData.receiving = false;
    gpsData.valid = false;
  }
}

int nmeaParse(char* sentence, char* fields[], int maxFields) {
  // Strip checksum (*XX) if present
  char* star = strchr(sentence, '*');
  if (star) *star = '\0';

  int count = 0;
  fields[count++] = sentence;  // Field 0 starts at beginning

  while (*sentence && count < maxFields) {
    if (*sentence == ',') {
      *sentence = '\0';         // Terminate previous field
      fields[count++] = sentence + 1;  // Next field starts after comma
    }
    sentence++;
  }
  return count;
}

void parseNMEA(char* sentence) {
  char* fields[NMEA_MAX_FIELDS];
  int nFields = nmeaParse(sentence, fields, NMEA_MAX_FIELDS);

  // Identify sentence type from field 0 (e.g., "$GNRMC", "$GPGGA")
  const char* talker = fields[0];  // e.g., "$GNRMC"
  bool isRMC = (nFields >= 3 && strstr(talker, "RMC") != NULL);
  bool isGGA = (nFields >= 10 && strstr(talker, "GGA") != NULL);

  // ---- RMC: Time, position, speed, date ----
  if (isRMC) {
    // Field 1: Time HHMMSS.sss
    if (strlen(fields[1]) >= 6) {
      gpsData.hour   = (fields[1][0] - '0') * 10 + (fields[1][1] - '0');
      gpsData.minute = (fields[1][2] - '0') * 10 + (fields[1][3] - '0');
      gpsData.second = (fields[1][4] - '0') * 10 + (fields[1][5] - '0');
      gpsData.timeValid = true;
    }

    // Field 2: Status A=valid, V=void
    char status = (strlen(fields[2]) > 0) ? fields[2][0] : 'V';

    // Determine talker: GP (GPS-only) or GN (multi-GNSS) (#115)
    bool isGN = (talker[2] == 'N');  // $GN... vs $GP...

    // Reset cycle tracking if >1.2s since last RMC
    if (millis() - lastRmcCycleTime > 1200) {
      gprmcFixThisCycle = false;
      gnrmcFixThisCycle = false;
    }
    lastRmcCycleTime = millis();

    if (status == 'A') {
      // Parse position
      float lat = (strlen(fields[3]) > 0) ? atof(fields[3]) : 0;
      char latDir = (strlen(fields[4]) > 0) ? fields[4][0] : 'N';
      float lon = (strlen(fields[5]) > 0) ? atof(fields[5]) : 0;
      char lonDir = (strlen(fields[6]) > 0) ? fields[6][0] : 'W';

      int latDeg = (int)(lat / 100);
      float latMin = lat - (latDeg * 100);
      gpsData.latitude = latDeg + (latMin / 60.0f);
      if (latDir == 'S') gpsData.latitude = -gpsData.latitude;

      int lonDeg = (int)(lon / 100);
      float lonMin = lon - (lonDeg * 100);
      gpsData.longitude = lonDeg + (lonMin / 60.0f);
      if (lonDir == 'W') gpsData.longitude = -gpsData.longitude;

      // Speed (field 7)
      if (strlen(fields[7]) > 0) gpsData.speedKnots = atof(fields[7]);

      // Date (field 9)
      if (nFields > 9 && strlen(fields[9]) >= 6) {
        gpsData.day   = (fields[9][0] - '0') * 10 + (fields[9][1] - '0');
        gpsData.month = (fields[9][2] - '0') * 10 + (fields[9][3] - '0');
        gpsData.year  = 2000 + (fields[9][4] - '0') * 10 + (fields[9][5] - '0');
        gpsData.dateValid = true;
      }

      // Track talker fix state
      if (isGN) gnrmcFixThisCycle = true;
      else      gprmcFixThisCycle = true;

      // Set valid — runtime debug log on transition
      if (!gpsData.valid && gpsDebugEnabled) {
        logPrintf("[GPS:DBG] Fix gained — Talker:%s Sat:%d HDOP:%.1f\n",
                  isGN ? "GN" : "GP", gpsData.satellites, gpsData.hdop);
      }
      gpsData.valid = true;

      // Track time to first fix (#68)
      if (!gpsHadFirstFix) {
        gpsFirstFixTime = millis();
        gpsHadFirstFix = true;
        logPrintf("[GPS] First fix acquired in %lus (TTFF)\n", gpsFirstFixTime / 1000);
      }

      // Sync RTC from GPS time (once per session)
      if (gpsData.timeValid && gpsData.dateValid && !rtcSyncedFromGPS) {
        struct tm gpsTime;
        gpsTime.tm_year = gpsData.year - 1900;
        gpsTime.tm_mon  = gpsData.month - 1;
        gpsTime.tm_mday = gpsData.day;
        gpsTime.tm_hour = gpsData.hour;
        gpsTime.tm_min  = gpsData.minute;
        gpsTime.tm_sec  = gpsData.second;

        time_t t = mktimeUTC(&gpsTime);
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        syncRTCFromSystemTime("GPS");
        rtcSyncedFromGPS = true;

        logPrintf("[GPS] System time set: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  gpsData.year, gpsData.month, gpsData.day,
                  gpsData.hour, gpsData.minute, gpsData.second);
      }

    } else {
      // Status V — but check multi-constellation override (#115)
      if (isGN) {
        gnrmcFixThisCycle = false;
        // If GPRMC said 'A' this cycle, keep GPS-only fix
        if (gprmcFixThisCycle) {
          if (gpsDebugEnabled) {
            logPrintf("[GPS:DBG] Talker conflict — GPRMC:A but GNRMC:V, keeping GP fix\n");
          }
          // Don't invalidate — GPRMC fix is still good
          return;
        }
      } else {
        gprmcFixThisCycle = false;
      }

      // Track when signal is lost (#68)
      if (gpsData.valid && gpsHadFirstFix) {
        gpsSignalLostTime = millis();
        if (gpsDebugEnabled) {
          unsigned long validFor = (gpsSignalLostTime - gpsFirstFixTime) / 1000;
          logPrintf("[GPS:DBG] Fix lost — was valid for %lus\n", validFor);
        } else {
          logPrintf("[GPS] Signal lost at %lus\n", gpsSignalLostTime / 1000);
        }
      }
      gpsData.valid = false;
    }
  }

  // ---- GGA: Altitude, satellites, HDOP ----
  if (isGGA) {
    // Field 7: Number of satellites
    if (strlen(fields[7]) > 0) {
      int newSats = atoi(fields[7]);
      if (gpsDebugEnabled && newSats != gpsData.satellites) {
        logPrintf("[GPS:DBG] Sats: %d -> %d\n", gpsData.satellites, newSats);
      }
      gpsData.satellites = newSats;
    }
    // Field 8: HDOP
    if (strlen(fields[8]) > 0) gpsData.hdop = atof(fields[8]);
    // Field 9: Altitude (meters)
    if (strlen(fields[9]) > 0) gpsData.altitude = atof(fields[9]);
  }
}

// Get GPS accuracy estimate based on HDOP
float getGpsAccuracyMeters() {
  return 3.0 * gpsData.hdop;  // Typical GPS accuracy ≈ 3m × HDOP
}
