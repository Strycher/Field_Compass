// battery.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "battery.h"
#include "fram.h"
#include "logging.h"

Adafruit_MAX17048 battery;
bool batteryAvailable = false;

void initBattery() {
  logPrint("Initializing MAX17048... ");

  if (!battery.begin()) {
    logPrintln("NOT FOUND");
    return;
  }

  batteryAvailable = true;
  logPrintln("OK");
}

// Log battery data to SD card for analysis
void logBatteryToSD() {
  if (!sdHealth.available || !batteryAvailable) return;

  File f = sdOpenSafe(BATT_LOG_FILE, "a", true);  // silent fail OK
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
  recordSDSuccess();
}

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
  // Simple XOR checksum over first 18 bytes (before checksum field)
  uint16_t ck = 0;
  uint8_t* bytes = (uint8_t*)&entry;
  for (int i = 0; i < 18; i += 2) {
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

// Check if a real LiPo battery is connected (not just USB power)
// NOTE: This is a simplified version for data collection.
// Will be enhanced after analyzing battery log data.
bool isBatteryConnected() {
  if (!batteryAvailable) return false;

  float pct = battery.cellPercent();

  // Check for invalid reading
  if (isnan(pct)) return false;

  // >100% is impossible for real LiPo - indicates USB-only power
  // MAX17048 reports 100-101% when connected to USB without battery
  if (pct > 100.0) return false;

  return true;
}
