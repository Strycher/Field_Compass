# FRAM Write Buffer Design

**Date:** 2025-02-13
**Status:** Approved
**Related Issues:** #72 (FRAM future), battery logging, weather logging, BSEC state

## Problem

The firmware makes synchronous SD card writes at high frequency:
- Battery logger: every 10 seconds (360 writes/hour)
- Weather logger: every 5 minutes (12 writes/hour)
- BSEC state: every 1 hour (1 write/hour)

Each SD write blocks the main loop for 5-50ms. Battery data and weather history
are lost on reboot. BSEC calibration is only saved hourly — a reboot at minute 59
loses an hour of IAQ learning.

## Solution

Use the 256KB SPI FRAM (MB85RS2MTA on GPIO 15) as a write buffer with dedicated
ring buffers for each data stream. Writes hit FRAM at SPI speed (microseconds),
then flush to SD in batches on a hybrid trigger.

## Architecture: Smart Ring Buffers (Approach A)

Each data type gets a fixed-size ring buffer in FRAM. A header region tracks
buffer state (head, tail, count, version). Writes go directly to FRAM. A shared
`framFlushToSD()` function writes pending entries to their respective SD files.

### FRAM Memory Map (256 KB = 262,144 bytes)

| Region         | Offset     | Size      | Purpose                                  |
|----------------|------------|-----------|------------------------------------------|
| Header         | `0x00000`  | 64 B      | Magic, version, flags, ring pointers     |
| BSEC State     | `0x00040`  | 512 B     | BME688 calibration blob (452B + padding) |
| Battery Ring   | `0x00240`  | 10,240 B  | 512 entries x 20 B (~85 min at 10s)      |
| Weather Ring   | `0x02A40`  | 7,200 B   | 300 entries x 24 B (25 hrs at 5 min)     |
| Reserved       | `0x04600`  | 244,128 B | GPS ephemeris, user settings, tracks     |

**Total used: ~18 KB (7%). 93% reserved for future features.**

### Data Structures

#### FRAM Header (64 bytes at 0x00000)

```c
struct FRAMHeader {
  uint32_t magic;            // 0x4652414D ("FRAM") - validates initialized state
  uint8_t  version;          // Schema version (start at 1)
  uint8_t  flags;            // Bit 0: dirty (unwritten data), Bit 1: BSEC valid
  uint16_t reserved1;

  // Battery ring state
  uint16_t battHead;         // Next write position (0-511)
  uint16_t battTail;         // Next flush position (0-511)
  uint16_t battCount;        // Entries waiting to flush
  uint16_t battCapacity;     // 512

  // Weather ring state
  uint16_t wxHead;           // Next write position (0-299)
  uint16_t wxTail;           // Next flush position (0-299)
  uint16_t wxCount;          // Entries waiting to flush
  uint16_t wxCapacity;       // 300

  // BSEC state metadata
  uint32_t bsecTimestamp;    // millis() when last saved
  uint8_t  bsecAccuracy;    // IAQ accuracy at save time
  uint8_t  reserved2[27];   // Pad to 64 bytes
};
```

#### Battery Entry (20 bytes)

```c
struct FRAMBatteryEntry {
  uint32_t timestamp;   // millis()
  float    voltage;     // Battery voltage
  float    percent;     // Battery percentage
  float    rate;        // Charge rate (%/hr)
  uint16_t flags;       // Reserved
  uint16_t checksum;    // Simple XOR checksum
};
```

#### Weather Entry (24 bytes)

Identical to existing `WeatherReading` struct:
```c
struct FRAMWeatherEntry {
  uint32_t timestamp;   // Unix epoch
  float    lat, lon;    // GPS position
  float    pressure;    // hPa
  float    temp;        // Celsius
  float    humidity;    // %RH
};
```

### Flush Strategy (Hybrid)

Flush triggers — whichever fires first:

1. **Timer**: Every 5 minutes (aligns with weather log interval)
2. **Button press**: Any user interaction triggers flush
3. **Boot**: On startup, flush any buffered FRAM data to SD before normal operation

The flush function:
1. Reads the FRAM header to get tail/count for each ring
2. Opens each SD file, appends all pending entries, closes
3. Updates FRAM header (tail = head, count = 0, dirty = false)
4. Logs flush stats: `[FRAM] Flushed: batt=30 wx=1 (42ms)`

### BSEC State Handling

BSEC is different — it's not a ring buffer, it's a single blob overwritten in place.

- **Write to FRAM**: On every IAQ accuracy level change (0→1, 1→2, 2→3, or regression)
- **Write to SD**: On the existing hourly schedule (unchanged)
- **Read on boot**: Try FRAM first (fast). If FRAM magic invalid, fall back to SD file.
- **Benefit**: Even with hourly SD saves, BSEC calibration survives any reboot with
  at-most minutes of lost progress instead of up to an hour.

### Boot Sequence

```
1. initFRAM()          — detect FRAM, read header
2. framValidate()      — check magic bytes, version
   - If invalid: format FRAM (write clean header, zero rings)
   - If valid: log "FRAM: 30 batt + 1 wx entries pending"
3. framFlushToSD()     — write any pending entries to SD (before normal logging starts)
4. framLoadBsecState() — restore BSEC state from FRAM (faster than SD)
5. Normal operation begins with empty/clean ring buffers
```

### Write Path Changes

**Battery (every 10s):**
- Before: `logBatteryToSD()` → open file → fprintf → close
- After: `logBatteryToFRAM()` → write 20 bytes to FRAM at head offset → bump head

**Weather (every 5 min):**
- Before: `logWeatherReading()` → open file → fprintf → close
- After: `logWeatherToFRAM()` → write 24 bytes to FRAM → bump head
  (SD flush happens immediately after since it's the 5-min timer trigger anyway)

**BSEC (on accuracy change):**
- Before: `saveBsecState()` → write 452B to SD → hourly
- After: `saveBsecToFRAM()` → write 452B to FRAM → immediate
  (SD save still happens hourly as a secondary backup)

### Error Handling

- **FRAM write failure**: Fall back to direct SD write (existing behavior)
- **FRAM read failure on boot**: Skip FRAM recovery, use SD files as before
- **SD flush failure**: Keep data in FRAM, retry on next trigger. Set dirty flag.
- **FRAM full (ring wrap without flush)**: Oldest entries overwritten. Log warning.
  This should never happen with 5-min flush intervals (battery ring holds 85 min).

### Diagnostics

Add to TFT and web diagnostics:
- FRAM status: `OK (256KB) | Batt:30/512 Wx:1/300`
- Last flush: `42ms ago, 31 entries`
- Dirty flag: `Clean` or `DIRTY (pending flush)`

### SD Write Reduction Summary

| Stream  | Before       | After        | Reduction |
|---------|-------------|-------------|-----------|
| Battery | 360/hour    | 12/hour     | **30x**   |
| Weather | 12/hour     | 12/hour     | Same frequency, but buffered |
| BSEC    | 1/hour      | 1/hour      | Same to SD, but FRAM backup on every change |
| **Total SD writes** | **373/hour** | **25/hour** | **~15x reduction** |

### Future FRAM Uses (Reserved Space: 244 KB)

- GPS ephemeris cache (~32 KB) — warm start in 10-30s instead of 2-5 min
- User settings (~1 KB) — WiFi, timezone, log intervals without recompile
- Weather history mirror (~7 KB) — survive reboots without re-reading SD
- Track breadcrumbs (~10 KB) — last 500 GPS positions for trail display
- Geocache proximity alerts (~4 KB) — precomputed distances
