# FRAM Write Buffer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace direct SD card writes with FRAM ring buffers for battery, weather, and BSEC data — reducing SD writes by 15x and surviving reboots.

**Architecture:** Three dedicated ring buffers in FRAM (battery 512 entries, weather 300 entries, BSEC single blob). Header at offset 0x00000 tracks ring state. Hybrid flush (5-min timer + button press + boot recovery) batches FRAM entries to SD. Falls back to direct SD writes if FRAM unavailable.

**Tech Stack:** Adafruit_FRAM_SPI library (already installed), Arduino ESP32-S3, single file `Field_Compass.ino`

**Design Doc:** `docs/plans/2025-02-13-fram-buffer-design.md`

---

### Task 1: FRAM Data Structures and Memory Map Constants

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — add structs and constants near existing FRAM definitions

**Step 1: Add memory map constants after the existing FRAM_CS define (~line 73)**

After `#define TFT_SPI_FREQ 32000000`, add:

```c
// FRAM Memory Map (256KB = 262,144 bytes, MB85RS2MTA)
#define FRAM_MAGIC          0x4652414D  // "FRAM" in ASCII
#define FRAM_VERSION        1
#define FRAM_HEADER_ADDR    0x00000
#define FRAM_HEADER_SIZE    64
#define FRAM_BSEC_ADDR     0x00040     // 512 bytes for BSEC state blob
#define FRAM_BSEC_SIZE      512
#define FRAM_BATT_ADDR     0x00240     // Battery ring buffer start
#define FRAM_BATT_ENTRY     20         // Bytes per battery entry
#define FRAM_BATT_COUNT     512        // Ring buffer capacity (~85 min at 10s)
#define FRAM_WX_ADDR       0x02A40     // Weather ring buffer start
#define FRAM_WX_ENTRY       24         // Bytes per weather entry
#define FRAM_WX_COUNT       300        // Ring buffer capacity (25 hrs at 5 min)
#define FRAM_FLUSH_INTERVAL 300000     // Flush to SD every 5 minutes (ms)
```

**Step 2: Add FRAM structs after the existing `shtData` struct (near line 230)**

```c
// FRAM ring buffer header (64 bytes, stored at FRAM_HEADER_ADDR)
struct FRAMHeader {
  uint32_t magic;            // FRAM_MAGIC validates initialized state
  uint8_t  version;          // Schema version
  uint8_t  flags;            // Bit 0: dirty, Bit 1: BSEC valid
  uint16_t reserved1;
  // Battery ring
  uint16_t battHead;         // Next write position (0 to FRAM_BATT_COUNT-1)
  uint16_t battTail;         // Next flush position
  uint16_t battCount;        // Entries pending flush
  uint16_t battCapacity;     // FRAM_BATT_COUNT
  // Weather ring
  uint16_t wxHead;
  uint16_t wxTail;
  uint16_t wxCount;
  uint16_t wxCapacity;       // FRAM_WX_COUNT
  // BSEC metadata
  uint32_t bsecTimestamp;    // millis() when last saved
  uint8_t  bsecAccuracy;    // IAQ accuracy at save time
  uint8_t  reserved2[27];   // Pad to 64 bytes total
};

struct FRAMBatteryEntry {
  uint32_t timestamp;        // millis()
  float    voltage;
  float    percent;
  float    rate;             // Charge rate (%/hr)
  uint16_t flags;            // Reserved
  uint16_t checksum;         // XOR checksum
};
// FRAMWeatherEntry is identical to WeatherReading (24 bytes) - reuse existing struct
```

**Step 3: Add FRAM state variables near other availability flags (~line 216)**

```c
// FRAM buffer state (in RAM — synced from FRAM header on boot)
FRAMHeader framHeader;
unsigned long lastFramFlush = 0;   // Last flush timestamp
```

**Step 4: Compile to verify structs are correct**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile, no errors.

**Step 5: Commit**

```
git add Field_Compass/Field_Compass.ino
git commit -m "Add FRAM memory map constants and ring buffer structs"
```

---

### Task 2: FRAM Header Validation and Formatting

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — expand `initFRAM()`, add `framFormat()` and `framReadHeader()`/`framWriteHeader()`

**Step 1: Add helper functions after the existing `initFRAM()` function (~line 872)**

```c
// Read the 64-byte FRAM header into RAM
void framReadHeader() {
  uint8_t buf[FRAM_HEADER_SIZE];
  for (int i = 0; i < FRAM_HEADER_SIZE; i++) {
    buf[i] = fram.read8(FRAM_HEADER_ADDR + i);
  }
  memcpy(&framHeader, buf, FRAM_HEADER_SIZE);
}

// Write the RAM header back to FRAM
void framWriteHeader() {
  uint8_t buf[FRAM_HEADER_SIZE];
  memcpy(buf, &framHeader, FRAM_HEADER_SIZE);
  for (int i = 0; i < FRAM_HEADER_SIZE; i++) {
    fram.write8(FRAM_HEADER_ADDR + i, buf[i]);
  }
}

// Format FRAM with clean header (zeroed ring buffers)
void framFormat() {
  logPrintln("[FRAM] Formatting...");
  memset(&framHeader, 0, sizeof(framHeader));
  framHeader.magic = FRAM_MAGIC;
  framHeader.version = FRAM_VERSION;
  framHeader.battCapacity = FRAM_BATT_COUNT;
  framHeader.wxCapacity = FRAM_WX_COUNT;
  framWriteHeader();
  logPrintln("[FRAM] Format complete");
}
```

**Step 2: Expand `initFRAM()` to validate header after detecting the chip**

Replace the existing `initFRAM()` body. After the existing `framAvailable = true` line, add:

```c
  // Read and validate FRAM header
  framReadHeader();
  if (framHeader.magic != FRAM_MAGIC || framHeader.version != FRAM_VERSION) {
    logPrintln("  Header invalid - formatting");
    framFormat();
  } else {
    logPrintf("  Batt:%d/%d Wx:%d/%d pending\n",
              framHeader.battCount, framHeader.battCapacity,
              framHeader.wxCount, framHeader.wxCapacity);
  }
```

**Step 3: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile.

**Step 4: Upload and verify via serial**

Run: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM6 Field_Compass/`
Expected serial output: `Initializing FRAM... OK (mfg:0x04 prod:0x0503, 256KB)` followed by
either `Header invalid - formatting` (first run) or `Batt:0/512 Wx:0/300 pending` (subsequent runs).

**Step 5: Commit**

```
git commit -m "Add FRAM header validation and formatting on boot"
```

---

### Task 3: Battery Ring Buffer — Write to FRAM

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — add `logBatteryToFRAM()`, modify loop() call site

**Step 1: Add `logBatteryToFRAM()` after existing `logBatteryToSD()` (~line 941)**

```c
// Write battery entry to FRAM ring buffer instead of SD
void logBatteryToFRAM() {
  if (!framAvailable || !batteryAvailable) return;

  float v = battery.cellVoltage();
  float p = battery.cellPercent();
  float r = battery.chargeRate();

  FRAMBatteryEntry entry;
  entry.timestamp = millis();
  entry.voltage = v;
  entry.percent = p;
  entry.rate = r;
  entry.flags = 0;
  // Simple XOR checksum
  uint16_t ck = 0;
  uint8_t* bytes = (uint8_t*)&entry;
  for (int i = 0; i < 18; i += 2) {  // First 18 bytes (before checksum field)
    ck ^= (bytes[i] | (bytes[i+1] << 8));
  }
  entry.checksum = ck;

  // Write entry to FRAM at head position
  uint32_t addr = FRAM_BATT_ADDR + (framHeader.battHead * FRAM_BATT_ENTRY);
  uint8_t* data = (uint8_t*)&entry;
  for (int i = 0; i < FRAM_BATT_ENTRY; i++) {
    fram.write8(addr + i, data[i]);
  }

  // Advance head (circular)
  framHeader.battHead = (framHeader.battHead + 1) % FRAM_BATT_COUNT;
  if (framHeader.battCount < FRAM_BATT_COUNT) {
    framHeader.battCount++;
  } else {
    // Ring full — tail advances too (oldest data lost)
    framHeader.battTail = (framHeader.battTail + 1) % FRAM_BATT_COUNT;
    logPrintln("[FRAM] WARN: Battery ring overflow");
  }
  framHeader.flags |= 0x01;  // Set dirty flag
  framWriteHeader();
}
```

**Step 2: Modify loop() to use FRAM instead of SD for battery logging (~line 582)**

Change:
```c
  if (sdAvailable && batteryAvailable && (millis() - lastBattLog > BATT_LOG_INTERVAL)) {
    logBatteryToSD();
    lastBattLog = millis();
  }
```
To:
```c
  if (batteryAvailable && (millis() - lastBattLog > BATT_LOG_INTERVAL)) {
    if (framAvailable) {
      logBatteryToFRAM();
    } else if (sdAvailable) {
      logBatteryToSD();  // Fallback: direct SD write
    }
    lastBattLog = millis();
  }
```

**Step 3: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile.

**Step 4: Commit**

```
git commit -m "Add battery ring buffer write to FRAM with SD fallback"
```

---

### Task 4: Weather Ring Buffer — Write to FRAM

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — add `logWeatherToFRAM()`, modify `logWeatherReading()`

**Step 1: Add `logWeatherToFRAM()` after `logBatteryToFRAM()`**

```c
// Write weather entry to FRAM ring buffer
void logWeatherToFRAM(WeatherReading& reading) {
  if (!framAvailable) return;

  // WeatherReading is already 24 bytes, same as our FRAM entry layout
  uint32_t addr = FRAM_WX_ADDR + (framHeader.wxHead * FRAM_WX_ENTRY);
  uint8_t* data = (uint8_t*)&reading;
  for (int i = 0; i < FRAM_WX_ENTRY; i++) {
    fram.write8(addr + i, data[i]);
  }

  framHeader.wxHead = (framHeader.wxHead + 1) % FRAM_WX_COUNT;
  if (framHeader.wxCount < FRAM_WX_COUNT) {
    framHeader.wxCount++;
  } else {
    framHeader.wxTail = (framHeader.wxTail + 1) % FRAM_WX_COUNT;
    logPrintln("[FRAM] WARN: Weather ring overflow");
  }
  framHeader.flags |= 0x01;  // dirty
  framWriteHeader();
}
```

**Step 2: Modify `logWeatherReading()` to use FRAM**

In existing `logWeatherReading()` (~line 1512), after `addToWeatherHistory(reading);`, change
the SD write block:

Before:
```c
  // Write to SD card (silent fail OK - weather logging is non-critical)
  char filename[32];
  getWeatherFilename(filename, 0);
  File file = sdOpenSafe(filename, "a", true);
  if (file) {
    file.printf(...)
    file.close();
    recordSDSuccess();
  }
```

After:
```c
  // Buffer to FRAM (SD flush happens on 5-min timer)
  if (framAvailable) {
    logWeatherToFRAM(reading);
  } else if (sdAvailable) {
    // Fallback: direct SD write
    char filename[32];
    getWeatherFilename(filename, 0);
    File file = sdOpenSafe(filename, "a", true);
    if (file) {
      file.printf("%lu,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                  reading.timestamp, reading.lat, reading.lon,
                  reading.pressure, reading.temp, reading.humidity);
      file.close();
      recordSDSuccess();
    }
  }
```

**Step 3: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile.

**Step 4: Commit**

```
git commit -m "Add weather ring buffer write to FRAM with SD fallback"
```

---

### Task 5: BSEC State — Save to FRAM

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — add `saveBsecToFRAM()` and `loadBsecFromFRAM()`

**Step 1: Add BSEC FRAM functions near the existing `saveBsecState()` (~line 1201)**

```c
// Save BSEC state to FRAM (fast, on every accuracy change)
bool saveBsecToFRAM() {
  if (!framAvailable || !bmeAvailable) return false;

  if (!envSensor.getState(bsecState)) {
    logPrintf("[FRAM] Failed to get BSEC state: %d\n", envSensor.status);
    return false;
  }

  // Write BSEC blob to FRAM
  for (int i = 0; i < BSEC_MAX_STATE_BLOB_SIZE && i < FRAM_BSEC_SIZE; i++) {
    fram.write8(FRAM_BSEC_ADDR + i, bsecState[i]);
  }

  // Update header metadata
  framHeader.bsecTimestamp = millis();
  framHeader.bsecAccuracy = envData.iaqAccuracy;
  framHeader.flags |= 0x02;  // Bit 1: BSEC valid
  framWriteHeader();

  logPrintf("[FRAM] BSEC state saved (acc:%d)\n", envData.iaqAccuracy);
  return true;
}

// Load BSEC state from FRAM (fast boot recovery)
bool loadBsecFromFRAM() {
  if (!framAvailable || !bmeAvailable) return false;
  if (!(framHeader.flags & 0x02)) return false;  // No valid BSEC in FRAM

  // Read BSEC blob from FRAM
  for (int i = 0; i < BSEC_MAX_STATE_BLOB_SIZE && i < FRAM_BSEC_SIZE; i++) {
    bsecState[i] = fram.read8(FRAM_BSEC_ADDR + i);
  }

  if (!envSensor.setState(bsecState)) {
    logPrintf("[FRAM] Failed to restore BSEC state: %d\n", envSensor.status);
    return false;
  }

  logPrintf("[FRAM] BSEC state restored (acc:%d, age:%lus)\n",
            framHeader.bsecAccuracy, (millis() - framHeader.bsecTimestamp) / 1000);
  bsecStateLoaded = true;
  return true;
}
```

**Step 2: Modify BSEC accuracy change handler to save to FRAM (~line 772)**

Change:
```c
  if (envData.iaqAccuracy == 3 && lastAccuracy < 3) {
    saveBsecState();
  }
```
To:
```c
  if (envData.iaqAccuracy != lastAccuracy) {
    if (framAvailable) {
      saveBsecToFRAM();  // Fast save to FRAM on every accuracy change
    }
    if (envData.iaqAccuracy == 3 && lastAccuracy < 3) {
      saveBsecState();   // Also save to SD when reaching accuracy 3
    }
  }
```

**Step 3: Modify boot sequence to try FRAM before SD for BSEC (~line 511)**

Change:
```c
  if (sdAvailable) {
    loadWeatherHistory();
    if (bmeAvailable) {
      loadBsecState();
    }
  }
```
To:
```c
  // Load BSEC state: try FRAM first (fast), fall back to SD
  if (bmeAvailable) {
    if (!loadBsecFromFRAM()) {
      if (sdAvailable) loadBsecState();
    }
  }
  if (sdAvailable) {
    loadWeatherHistory();
  }
```

**Step 4: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile.

**Step 5: Commit**

```
git commit -m "Add BSEC state save/restore to FRAM with SD fallback"
```

---

### Task 6: Flush Function — FRAM to SD

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — add `framFlushToSD()`, wire into loop and button handler

This is the core of the design: batch-write buffered FRAM entries to SD.

**Step 1: Add `framFlushToSD()` after the BSEC FRAM functions**

```c
// Flush all pending FRAM ring buffer entries to SD card
void framFlushToSD() {
  if (!framAvailable || !sdAvailable) return;
  if (framHeader.battCount == 0 && framHeader.wxCount == 0) return;  // Nothing to flush

  unsigned long startMs = millis();
  int battFlushed = 0, wxFlushed = 0;

  // --- Flush battery entries ---
  if (framHeader.battCount > 0) {
    File f = sdOpenSafe(BATT_LOG_FILE, "a", true);
    if (f) {
      if (f.size() == 0) {
        f.println("millis,voltage,percent,rate");
      }
      uint16_t idx = framHeader.battTail;
      for (int i = 0; i < framHeader.battCount; i++) {
        FRAMBatteryEntry entry;
        uint32_t addr = FRAM_BATT_ADDR + (idx * FRAM_BATT_ENTRY);
        uint8_t* data = (uint8_t*)&entry;
        for (int j = 0; j < FRAM_BATT_ENTRY; j++) {
          data[j] = fram.read8(addr + j);
        }
        f.printf("%lu,%.3f,%.2f,%.2f\n", entry.timestamp, entry.voltage, entry.percent, entry.rate);
        idx = (idx + 1) % FRAM_BATT_COUNT;
        battFlushed++;
      }
      f.close();
      recordSDSuccess();
    }
    framHeader.battTail = framHeader.battHead;
    framHeader.battCount = 0;
  }

  // --- Flush weather entries ---
  if (framHeader.wxCount > 0) {
    uint16_t idx = framHeader.wxTail;
    for (int i = 0; i < framHeader.wxCount; i++) {
      WeatherReading reading;
      uint32_t addr = FRAM_WX_ADDR + (idx * FRAM_WX_ENTRY);
      uint8_t* data = (uint8_t*)&reading;
      for (int j = 0; j < FRAM_WX_ENTRY; j++) {
        data[j] = fram.read8(addr + j);
      }
      // Write to date-specific weather file
      char filename[32];
      getWeatherFilename(filename, 0);  // Today's file
      File file = sdOpenSafe(filename, "a", true);
      if (file) {
        file.printf("%lu,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                    reading.timestamp, reading.lat, reading.lon,
                    reading.pressure, reading.temp, reading.humidity);
        file.close();
        recordSDSuccess();
      }
      idx = (idx + 1) % FRAM_WX_COUNT;
      wxFlushed++;
    }
    framHeader.wxTail = framHeader.wxHead;
    framHeader.wxCount = 0;
  }

  // Update header: clear dirty flag
  framHeader.flags &= ~0x01;
  framWriteHeader();

  unsigned long elapsed = millis() - startMs;
  logPrintf("[FRAM] Flushed: batt=%d wx=%d (%lums)\n", battFlushed, wxFlushed, elapsed);
  lastFramFlush = millis();
}
```

**Step 2: Add flush timer to loop() — after the weather/battery log section (~line 585)**

After the battery logging block, add:

```c
  // FRAM flush to SD (hybrid: every 5 minutes)
  if (framAvailable && sdAvailable && (millis() - lastFramFlush > FRAM_FLUSH_INTERVAL)) {
    framFlushToSD();
  }
```

**Step 3: Add flush on button press in `handleButtons()` (~line 2700)**

After `lastActivityTime = now;` and before the TFT reinit, add:

```c
  // Flush FRAM buffer on button press
  if (framAvailable && sdAvailable) framFlushToSD();
```

**Step 4: Add boot flush in `setup()` — after FRAM init but before weather/BSEC load**

In setup(), after `initFRAM()` and before the BSEC/weather load block, add:

```c
  // Flush any FRAM data from previous session to SD
  if (framAvailable && sdAvailable) {
    framFlushToSD();
  }
```

**Step 5: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile.

**Step 6: Commit**

```
git commit -m "Add FRAM-to-SD flush with hybrid trigger (timer + button + boot)"
```

---

### Task 7: Diagnostics Updates

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — update TFT diags and web diags to show FRAM buffer state

**Step 1: Update web diagnostics (`handleWebDiags()`, ~line 2009)**

Change the FRAM line from:
```c
  sprintf(buf, "FRAM:    %s\n", framAvailable ? "OK (256KB)" : "N/A");
```
To:
```c
  if (framAvailable) {
    sprintf(buf, "FRAM:    OK (256KB) Batt:%d/%d Wx:%d/%d %s\n",
            framHeader.battCount, FRAM_BATT_COUNT,
            framHeader.wxCount, FRAM_WX_COUNT,
            (framHeader.flags & 0x01) ? "DIRTY" : "Clean");
  } else {
    sprintf(buf, "FRAM:    N/A\n");
  }
  html += buf;
```

**Step 2: Update TFT diagnostics sensor row (`drawScreenDiags()`, ~line 3940)**

The existing `FRAM:Y` in the Sensors row is fine. Optionally, if space allows on the
240px-wide screen, the sensor row already has enough info. No changes needed here
unless we want a dedicated FRAM row — defer to user preference.

**Step 3: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile.

**Step 4: Commit**

```
git commit -m "Update web diagnostics to show FRAM buffer state and dirty flag"
```

---

### Task 8: Upload, Verify, Tag Release

**Step 1: Upload firmware**

Run: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM6 Field_Compass/`
Expected: Successful upload, device reboots.

**Step 2: Verify FRAM init via web diagnostics**

Wait 15 seconds for WiFi, then:
Run: `curl -s http://fieldcompass.local/diags`

Expected in output:
- `FRAM:    OK (256KB) Batt:0/512 Wx:0/300 Clean` (first boot after format)
- Or: `FRAM:    OK (256KB) Batt:N/512 Wx:M/300 DIRTY` (if entries buffered)

**Step 3: Verify battery data buffers in FRAM**

Wait 30 seconds (3 battery entries at 10s interval), then check diags:
Run: `curl -s http://fieldcompass.local/diags`
Expected: `Batt:3/512` (entries accumulating in FRAM, not yet flushed)

**Step 4: Verify flush happens (wait 5 minutes or press a button)**

Press a physical button on the device, then re-check diags:
Expected: `Batt:0/512 Wx:0/300 Clean` (flushed to SD, counters reset)

**Step 5: Verify SD files still receive data**

Run: `curl -s http://fieldcompass.local/diags` and check the battery log file
is growing after flushes occur.

**Step 6: Verify BSEC FRAM save**

Check serial log for `[FRAM] BSEC state saved (acc:X)` when accuracy changes.
Check diags for BSEC flag.

**Step 7: Bump version and tag**

Update `FW_VERSION` from `"0.21"` to `"0.22"`.

```
git add Field_Compass/Field_Compass.ino
git commit -m "v0.22 - FRAM write buffer for battery, weather, and BSEC data

Reduces SD writes from 373/hour to ~25/hour (15x reduction).
Battery and weather data survive reboots via FRAM ring buffers.
BSEC calibration saved to FRAM on every accuracy change.
Hybrid flush: 5-min timer + button press + boot recovery."

git tag -a v0.22.0 -m "v0.22.0 - FRAM write buffer system"
git push origin main --tags
```

**Step 8: Update MEMORY.md**

Update version to v0.22.0 and add FRAM buffer notes.
