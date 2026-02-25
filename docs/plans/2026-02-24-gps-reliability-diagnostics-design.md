# GPS Reliability + Diagnostics Design (#115)

**Date:** 2026-02-24
**Issue:** #115 — Bug: GPS Acquiring stays stuck on boot
**Target:** v0.44.0

## Problem Statement

GPS intermittently stays stuck in "Acquiring" after boot. A reboot resolves it in ~4 seconds (hot start), suggesting the GPS module has satellite data but the code's state tracking fails to recognize the transition from acquiring to valid.

## Root Cause Analysis

1. **strtok() misaligns empty NMEA fields** — skips consecutive commas, causing field index desync during acquisition when lat/lon/speed/course fields are empty
2. **Multi-constellation RMC conflict** — both $GPRMC and $GNRMC may arrive per cycle; if GPS-only has fix but combined GNSS doesn't, last processed overwrites valid state
3. **gpsData.receiving never resets** — set once on first NMEA byte, never cleared; GPS module hang shows "Acquiring" forever instead of "No GPS"
4. **Telemetry elapsed time bug** — `millis()/1000` instead of `(millis()-gpsFirstReceiveTime)/1000`

## Design

### 1. NMEA Parser Fix — Replace strtok with nmeaField()

Replace destructive `strtok()` tokenizer with a non-destructive field indexer that preserves empty fields.

**New helper:**
```cpp
// Parse NMEA sentence into field array, preserving empty fields
// Returns number of fields found
int nmeaParse(char* sentence, char* fields[], int maxFields);
```

Uses a single pass replacing commas with nulls (like strtok) but does NOT skip consecutive delimiters. Each `fields[i]` points to the start of field i, which may be an empty string `""`.

The existing RMC and GGA switch/case logic remains identical — only the tokenizer changes.

**Checksum truncation:** Before parsing, strip `*XX` checksum suffix so the last field isn't polluted.

### 2. Multi-Constellation RMC Handling

Track which talker IDs arrive per GPS update cycle:

```cpp
static bool gprmcValidThisCycle = false;  // $GPRMC said 'A'
static bool gnrmcValidThisCycle = false;  // $GNRMC said 'A'
static unsigned long lastRmcCycleTime = 0;
```

**Resolution logic** (at end of RMC parsing):
- Prefer $GNRMC (multi-GNSS, most comprehensive)
- If $GNRMC says 'V' but $GPRMC said 'A' in the same 1-second window, use $GPRMC's fix data
- Log conflicts at runtime debug level

**Cycle reset:** If `millis() - lastRmcCycleTime > 1200ms`, reset cycle flags (GPS typically sends at 1Hz).

### 3. GPS Data Staleness Timeout

Add `gpsLastByteTime` timestamp updated on every byte in `readGPS()`.

In `readGPS()` (or a dedicated `checkGpsStaleness()` called from loop):
- If `millis() - gpsLastByteTime > GPS_STALE_MS` (5000ms) AND `gpsData.receiving`:
  - Set `gpsData.receiving = false`
  - Set `gpsData.valid = false`
  - Log "[GPS] Data stale — no bytes for 5s"

### 4. Telemetry Elapsed Time Fix

Line 3405: `millis() / 1000` → `(millis() - gpsFirstReceiveTime) / 1000`

Also add reacquire elapsed when signal was lost after having fix (matching legacy screen logic).

### 5. Two-Tier GPS Debug Logging

| Tier | Control | Content | Output |
|------|---------|---------|--------|
| Compile-time (`DEBUG_GPS`) | `#define DEBUG_GPS 1` | Raw NMEA sentences | `Serial.println()` only (high-volume, bypasses log pipeline) |
| Runtime (`gpsDebugEnabled`) | `/gps/debug` web toggle | State transitions, talker conflicts, field parse issues, sat/HDOP changes | `logPrintf()` → SD + Serial + web ring |

Runtime debug messages:
- `[GPS:DBG] Fix gained — Sat:%d HDOP:%.1f Talker:%s`
- `[GPS:DBG] Fix lost — was valid for %lus`
- `[GPS:DBG] Talker conflict — GPRMC:A but GNRMC:V, using GP fix`
- `[GPS:DBG] Stale — no data for %lums`
- `[GPS:DBG] RMC fields: status=%c sats=%d hdop=%.1f`

**Future UI:** Runtime toggle will move to Settings → Configuration tab after LVGL Settings migration (#112).

### 6. GPS Soft Reset

**Web endpoint:** `GET /gps/reset`

**Actions:**
1. Clear GPS state: zero `gpsData`, reset `gpsHadFirstFix`, `gpsHadFirstReceive`, `gpsSignalLostTime`, `gpsFirstReceiveTime`, `gpsFirstFixTime`
2. Send hot-restart command: `Serial1.println("$PMTK101*32")` (MediaTek MTK chipset — ignored by non-MTK modules)
3. Flush Serial1 RX buffer
4. Log `[GPS] Soft reset triggered via web`
5. Return JSON: `{"status":"ok","message":"GPS reset"}`

**Web-only rationale:** Physical buttons are overloaded (A/B=navigate, C=enter, C-long=compass cal). Web reset is accessible from phone on same WiFi without risk of accidental trigger.

## Existing Infrastructure (No Changes Needed)

- **SD session logging** — Already implemented (#59): new file per boot at `/logs/serial_YYYYMMDD_HHMMSS.log`, all `logPrintf` output captured
- **Web log viewer** — `/logs` lists files, `/logs/download` serves them
- **Live serial monitor** — `/serial` with 100ms polling
- **Log rotation** — 48h retention with grace mode

## Build Impact

| Component | Estimate |
|-----------|----------|
| nmeaParse() helper | ~+200 bytes |
| RMC talker tracking | ~+150 bytes |
| Staleness timeout | ~+80 bytes |
| Runtime debug messages | ~+600 bytes |
| GPS reset endpoint | ~+350 bytes |
| Elapsed time fix | ~0 (replacement) |
| **Total** | ~+1,400 bytes flash |

No meaningful SRAM increase. No PSRAM impact.

## Testing Strategy

1. Compile and upload
2. Enable runtime GPS debug: `GET /gps/debug`
3. Monitor `/serial` or download session log from `/logs`
4. Observe GPS state transitions in log output
5. Test GPS reset: `GET /gps/reset`, verify reacquisition
6. Verify telemetry screen shows correct elapsed time
7. Check "No GPS" state appears after disconnecting GPS module for >5s
