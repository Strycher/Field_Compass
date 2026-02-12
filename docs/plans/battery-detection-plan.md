# Plan: Battery/USB/Charging Detection (#27)

## Context

**Goal:** Infer power state from MAX17048 readings to show meaningful status:
- Is USB connected? (charging source)
- Is battery present? (energy storage)
- Is battery charging? (both present, actively charging)

**Hardware Limitation:** No GPIO for VBUS or charge status - must infer from `cellVoltage()`, `cellPercent()`, and `chargeRate()`.

## Expected Signatures (Hypothesis)

| State | Voltage | Percent | Charge Rate | Display |
|-------|---------|---------|-------------|---------|
| USB only, no battery | ~4.19V | >100% | near 0 or phantom | "USB Power" |
| USB + battery charging | 3.7-4.2V | <100% | **positive** | "Charging XX%" |
| USB + battery full | ~4.2V | ~100% | ~0 | "Full 100%" |
| Battery only (discharging) | 3.2-4.2V | varies | **negative** | "XX%" |

**Key insight:** Positive charge rate = USB + battery (actively charging). This is the clearest signal.

## Phase 1: Data Collection

Deploy diagnostic build that logs all three MAX17048 values to SD card for extended capture.

### SD Card Logging

Create `/battlog.csv` on SD card with header and periodic entries:

```cpp
// Log format: timestamp_ms, voltage, percent, rate
// battlog.csv example:
// millis,voltage,percent,rate
// 10000,4.187,101.05,-0.12
// 20000,4.185,101.02,-0.15
```

**Logging interval:** Every 10 seconds (matches existing status log interval)

**Implementation:**
```cpp
void logBatteryToSD() {
  if (!sdAvailable || !batteryAvailable) return;

  File f = SD.open("/battlog.csv", FILE_APPEND);
  if (!f) return;

  // Write header if new file
  if (f.size() == 0) {
    f.println("millis,voltage,percent,rate");
  }

  float v = battery.cellVoltage();
  float p = battery.cellPercent();
  float r = battery.chargeRate();

  f.printf("%lu,%.3f,%.2f,%.2f\n", millis(), v, p, r);
  f.close();
}
```

### Web Endpoint for Download

Add `/battlog` endpoint to serve the CSV file for easy download:

```cpp
webServer.on("/battlog", HTTP_GET, []() {
  if (!sdAvailable) {
    webServer.send(404, "text/plain", "SD card not available");
    return;
  }
  File f = SD.open("/battlog.csv", FILE_READ);
  if (!f) {
    webServer.send(404, "text/plain", "Log file not found");
    return;
  }
  webServer.streamFile(f, "text/csv");
  f.close();
});
```

**Access:** `http://<device-ip>/battlog` to download CSV

### Serial logging (unchanged)
```cpp
snprintf(buf, sizeof(buf), "[%lus] ... Batt:%.3fV/%.1f%%/%+.1f%%hr\n",
         millis() / 1000, ..., battV, battP, battR);
```

### Test matrix (record actual values)

Run device for 5-10 minutes in each scenario, then download `/battlog` CSV:

| Scenario | Duration | Expected Patterns |
|----------|----------|-------------------|
| USB only, no battery | 5 min | pct >100%, rate near 0 or erratic |
| USB + depleted battery (~20%) | 10 min | pct rising, rate positive |
| USB + mid battery (~50%) | 5 min | pct rising, rate positive |
| USB + full battery | 5 min | pct ~100%, rate near 0 |
| Battery only, discharging | 5 min | pct falling, rate negative |

### Clear log between tests
Add `/battlog/clear` endpoint or manually delete file from SD card between scenarios.

## Phase 2: Detection Logic

Based on test data, implement state detection:

```cpp
enum PowerState {
  POWER_USB_ONLY,      // USB connected, no battery
  POWER_CHARGING,      // USB + battery, actively charging
  POWER_FULL,          // USB + battery, fully charged
  POWER_BATTERY,       // Battery only, discharging
  POWER_UNKNOWN        // Can't determine
};

PowerState getPowerState() {
  float volt = battery.cellVoltage();
  float pct = battery.cellPercent();
  float rate = battery.chargeRate();

  // USB only: >100% is impossible for real LiPo
  if (pct > 100.0) return POWER_USB_ONLY;

  // Actively charging: positive rate indicates USB + battery
  if (rate > CHARGING_THRESHOLD) return POWER_CHARGING;

  // Discharging: negative rate indicates battery only
  if (rate < -DISCHARGE_THRESHOLD) return POWER_BATTERY;

  // Near-zero rate at high charge = full battery on USB
  if (pct >= 99.0 && fabs(rate) < IDLE_THRESHOLD) return POWER_FULL;

  // Default to battery if we have valid readings
  return POWER_BATTERY;
}
```

### Display strings
- `POWER_USB_ONLY` → "USB Power"
- `POWER_CHARGING` → "Charging XX%" or "⚡ XX%"
- `POWER_FULL` → "Full" or "100% ✓"
- `POWER_BATTERY` → "XX%"

## Phase 3: Implementation

### Files to modify
- `Field_Compass/Field_Compass.ino`
  - Add `logBatteryToSD()` function (Phase 1)
  - Add `/battlog` and `/battlog/clear` web endpoints (Phase 1)
  - Add `PowerState` enum (Phase 3)
  - Replace `isBatteryConnected()` with `getPowerState()` (Phase 3)
  - Update TFT display to show appropriate status (Phase 3)
  - Update OLED display (Phase 3)
  - Update Web OPS page (Phase 3)
  - Update JSON API (Phase 3)

### Cleanup (remove failed approaches)
- Remove variance tracking globals (`battVoltSamples[]`, etc.)
- Remove `updateBatteryVoltSamples()` and `getBatteryVoltageVariance()` functions
- Remove call to `updateBatteryVoltSamples()` in loop()

## Verification

Test each scenario and verify correct display:
1. USB only → shows "USB Power"
2. Plug in depleted battery → shows "Charging XX%" with rising percent
3. Wait for full charge → shows "Full"
4. Unplug USB → shows "XX%" (just percent, discharging)
5. Unplug battery (USB only) → shows "USB Power"

## Sources

- [Adafruit MAX17048 Battery Monitor](https://learn.adafruit.com/adafruit-esp32-s3-feather/battery-monitor-max17048)
