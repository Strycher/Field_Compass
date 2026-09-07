#pragma once
// fram.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Adafruit_FRAM_SPI.h>

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
#define FRAM_SETTINGS_ADDR  0x046C0    // User settings backup (after weather ring)
#define FRAM_SETTINGS_SIZE  128        // Allocated block size
#define FRAM_SETTINGS_MAGIC 0x53544E47 // "STNG" in ASCII
#define FRAM_SETTINGS_VER   1

// FRAM ring buffer header (64 bytes, stored at FRAM_HEADER_ADDR)
struct FRAMHeader {
  uint32_t magic;            // FRAM_MAGIC validates initialized state
  uint8_t  version;          // Schema version
  uint8_t  flags;            // Bit 0: dirty (unwritten data), Bit 1: BSEC valid
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
// FRAMWeatherEntry: reuse existing WeatherReading struct (24 bytes, same layout)

// FRAM settings backup struct (#118) — written as raw bytes to FRAM_SETTINGS_ADDR
struct FRAMSettings {
  uint32_t magic;              // FRAM_SETTINGS_MAGIC
  uint8_t  version;            // FRAM_SETTINGS_VER
  uint8_t  use12Hour;
  uint8_t  useFahrenheit;
  uint8_t  useMetricUnits;
  char     posixTZ[48];
  char     tzDisplayName[24];
  int8_t   tzSelectedIndex;
  uint8_t  tftBrightness;
  uint8_t  _pad[2];            // Align to 4 bytes
  uint32_t tftSleepMs;
  uint32_t oledSleepMs;
  uint32_t checksum;           // XOR-32 of all preceding bytes
};
static_assert(sizeof(FRAMSettings) <= FRAM_SETTINGS_SIZE, "FRAMSettings exceeds allocated block");

extern Adafruit_FRAM_SPI fram;
extern FRAMHeader framHeader;
extern bool framAvailable;

void framReadHeader();
void framWriteHeader();
void framFormat();
void initFRAM();
