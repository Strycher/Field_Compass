# GPS Reliability + Diagnostics Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix GPS "stuck in Acquiring" bug by replacing the broken NMEA parser, adding multi-constellation awareness, staleness timeout, runtime diagnostics, and a GPS soft-reset endpoint.

**Architecture:** Replace strtok()-based NMEA parsing with a proper field parser that handles empty fields. Add GPS state transition logging at runtime. Add web endpoints for GPS debug toggle and soft reset. All diagnostic output flows through existing logPrintf → SD + Serial + web pipeline.

**Tech Stack:** Arduino/ESP32-S3, NMEA 0183, existing WebServer on port 80, existing SD session logging (#59)

---

### Task 1: Add GPS State Variables and Defines

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:150` (debug flags area)
- Modify: `Field_Compass/Field_Compass.ino:472-476` (GPS tracking variables)

**What:** Add the new state variables needed by subsequent tasks.

**Step 1:** After `#define DEBUG_GPS 0` (line 150), add staleness define:
```cpp
#define GPS_STALE_MS  5000   // No bytes for 5s → clear receiving/valid (#115)
```

**Step 2:** After `gpsHadFirstFix` (line 476), add new tracking variables:
```cpp
static unsigned long gpsLastByteTime = 0;    // Timestamp of last serial byte (#115)
static bool gpsDebugEnabled = false;          // Runtime GPS debug logging (#115)
// Multi-constellation RMC cycle tracking (#115)
static bool gprmcFixThisCycle = false;
static bool gnrmcFixThisCycle = false;
static unsigned long lastRmcCycleTime = 0;
```

**Step 3:** Compile.

**Step 4:** Commit: `feat(#115): Add GPS state variables and defines`

---

### Task 2: Replace strtok with nmeaParse()

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:6974-7098` (parseNMEA function)

**What:** Write a proper NMEA field parser and rewrite parseNMEA to use it.

**Step 1:** Add `nmeaParse()` helper immediately before `parseNMEA()` (before line 6974):
```cpp
// Parse NMEA sentence into field array, preserving empty fields (#115)
// Replaces strtok which skips consecutive commas (empty fields)
// Modifies sentence in-place (replaces commas and * with nulls)
// Returns number of fields found
#define NMEA_MAX_FIELDS 20
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
```

**Step 2:** Rewrite `parseNMEA()` to use `nmeaParse()` instead of strtok. Replace the entire function body (lines 6974-7098):

```cpp
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
```

**Step 3:** Compile.

**Step 4:** Commit: `fix(#115): Replace strtok NMEA parser with nmeaParse (empty field handling)`

---

### Task 3: Add GPS Staleness Timeout

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:6949-6972` (readGPS function)

**What:** Track last byte time and clear GPS state if no data for GPS_STALE_MS.

**Step 1:** In `readGPS()`, add timestamp update on every byte. Add staleness check after the while loop. Replace the entire function:

```cpp
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
```

**Step 2:** Compile.

**Step 3:** Commit: `fix(#115): Add GPS data staleness timeout (5s)`

---

### Task 4: Fix Telemetry Elapsed Time

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:3405` (updateTelemetryData)

**What:** Fix the elapsed time calculation and add reacquire awareness.

**Step 1:** Replace the elapsed time block (lines 3405-3407) with:

```cpp
    unsigned long elapsed;
    if (gpsHadFirstFix && gpsSignalLostTime > 0) {
      // Reacquiring after signal loss
      elapsed = (millis() - gpsSignalLostTime) / 1000;
      snprintf(buf, sizeof(buf), "Reacquire: %lum %lus", elapsed / 60, elapsed % 60);
    } else {
      // Never had fix — time since first NMEA data
      elapsed = (millis() - gpsFirstReceiveTime) / 1000;
      snprintf(buf, sizeof(buf), "Elapsed: %lum %lus", elapsed / 60, elapsed % 60);
    }
    lv_label_set_text(telLblGpsElapsed, buf);
```

**Step 2:** Compile.

**Step 3:** Commit: `fix(#115): Correct telemetry GPS elapsed time calculation`

---

### Task 5: Add GPS Debug Toggle and Reset Web Endpoints

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:6095-6098` (web server route registration)
- Add handler functions near `handleWebGPS()` (~line 5198)

**What:** Add `/gps/debug` toggle and `/gps/reset` soft-reset endpoints.

**Step 1:** Add two handler functions after `handleWebGPS()`:

```cpp
// GPS debug toggle (#115) — future: move to Settings LVGL screen (#112)
void handleWebGpsDebug() {
  gpsDebugEnabled = !gpsDebugEnabled;
  logPrintf("[GPS] Runtime debug logging %s\n", gpsDebugEnabled ? "ENABLED" : "DISABLED");
  String json = "{\"gpsDebug\":";
  json += gpsDebugEnabled ? "true" : "false";
  json += "}";
  webServer.send(200, "application/json", json);
}

// GPS soft reset (#115)
void handleWebGpsReset() {
  logPrintf("[GPS] Soft reset triggered via web\n");

  // Clear all GPS state
  gpsData.valid = false;
  gpsData.receiving = false;
  gpsData.latitude = 0;
  gpsData.longitude = 0;
  gpsData.altitude = 0;
  gpsData.hdop = 99.0;
  gpsData.satellites = 0;
  gpsData.speedKnots = 0;
  gpsData.timeValid = false;
  gpsData.dateValid = false;

  // Reset tracking flags
  gpsHadFirstReceive = false;
  gpsHadFirstFix = false;
  gpsFirstReceiveTime = 0;
  gpsFirstFixTime = 0;
  gpsSignalLostTime = 0;
  gpsLastByteTime = 0;
  gprmcFixThisCycle = false;
  gnrmcFixThisCycle = false;
  lastRmcCycleTime = 0;

  // Flush Serial1 RX buffer
  while (Serial1.available()) Serial1.read();

  // Send hot-restart command (MediaTek MTK chipsets — ignored by others)
  Serial1.println("$PMTK101*32");

  webServer.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"GPS reset\"}");
}
```

**Step 2:** Register the routes in `initWebServer()` after the serial log endpoints (line 6097):

```cpp
  // GPS debug/reset endpoints (#115)
  webServer.on("/gps/debug", HTTP_GET, handleWebGpsDebug);
  webServer.on("/gps/reset", HTTP_GET, handleWebGpsReset);
```

**Step 3:** Compile.

**Step 4:** Commit: `feat(#115): Add GPS debug toggle and soft-reset web endpoints`

---

### Task 6: Version Bump, Upload, Verify

**What:** Bump to v0.44.0, compile, upload, verify via serial log and web endpoints.

**Step 1:** Change FW_VERSION from "0.43.1" to "0.44.0" (line 28).

**Step 2:** Compile. Record flash/SRAM sizes.

**Step 3:** Upload to device (check COM port with `arduino-cli board list`).

**Step 4:** Verify via web:
- `GET http://fieldcompass.local/gps/debug` — toggle debug on, confirm JSON response
- `GET http://fieldcompass.local/gps/reset` — trigger reset, confirm JSON response
- `GET http://fieldcompass.local/serial` — observe GPS debug messages in live stream
- Navigate to Telemetry screen — confirm elapsed time shows reasonable value (not total uptime)

**Step 5:** Commit version bump: `v0.44.0 — GPS reliability + diagnostics (#115)`

**Step 6:** Tag and push: `git tag -a v0.44.0` + `git push origin main --tags`

**Step 7:** Close issue #115 with summary comment. Update MEMORY.md.
