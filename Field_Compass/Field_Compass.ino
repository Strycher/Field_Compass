/*
 * Field Compass - Dual Display Firmware
 *
 * Hardware:
 * - Adafruit ESP32-S3 Feather 8MB w.FL
 * - Adafruit SH1107 OLED FeatherWing 128x64 (I2C)
 * - Adafruit ILI9341 3.2" TFT 320x240 (SPI direct wiring)
 * - Adafruit Ultimate GPS FeatherWing PA1616D (Serial)
 * - Adafruit BME688 (I2C - STEMMA QT) with BSEC2
 * - Adafruit SHT41 (I2C 0x44 - STEMMA QT) (#48)
 * - Adafruit LSM6DSOX + LIS3MDL 9-DoF IMU (I2C - STEMMA QT)
 * - Adafruit SPI FRAM 256KB MB85RS2MTA (SPI, CS=GPIO15)
 *
 * Screens:
 * 1. Operational Info (time, uptime, WiFi, battery)
 * 2. GPS Info (coordinates, altitude, address)
 * 3. Environmental (SHT41 temp/humidity, BME688 pressure/IAQ/CO2)
 * 4. IMU/Compass (heading, orientation, acceleration)
 * 5. Diagnostics (BSEC state, weather log, system info)
 *
 * Navigation: Button A = prev screen, Button B = next screen
 * Display Sleep: OLED 3 min, TFT 15 min (button press wakes)
 *
 * Issues: #44, #46, #47
 */

// Firmware version
#define FW_VERSION "0.23"

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <stdarg.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_SH110X.h>
#include <bsec2.h>

// BSEC2 IAQ config for BME680/688 at 3.3V, 3-second sample rate, 4-day calibration
const uint8_t bsec2_config[] = {
  #include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};

#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_LIS3MDL.h>
#include <Adafruit_MAX1704X.h>
#include <Adafruit_SHT4x.h>   // SHT41 temp/humidity (#48)
#include <Adafruit_FRAM_SPI.h> // SPI FRAM 256KB (#72 future, init now)
#include <RTClib.h>           // Adalogger PCF8523 RTC
#include <SD.h>
#include <esp_task_wdt.h>

// ============== Configuration ==============

// WiFi credentials
const char* WIFI_SSID_1 = "tsunami";
const char* WIFI_PASS_1 = "Ch33t0s!";
const char* WIFI_SSID_2 = "tsunami_5G";
const char* WIFI_PASS_2 = "Ch33t0s!";
const char* WIFI_SSID_3 = "SM-N950UD60";
const char* WIFI_PASS_3 = "5132547071";

// NTP configuration
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = -18000;  // EST = UTC-5
const int DAYLIGHT_OFFSET_SEC = 3600; // DST +1 hour

// TFT Display pins (ILI9341 3.2" breakout, direct SPI wiring)
#define TFT_CS    18  // A0 -> CS
#define TFT_DC    17  // A1 -> D/C
#define TFT_RST   16  // A2 -> RST
#define FRAM_CS   15  // A3 -> FRAM CS (was TOUCH_CS, repurposed for FRAM)
#define SD_CS     10  // Adalogger FeatherWing SD slot (was 14 for TFT breakout)

// SPI clock frequency for TFT (ILI9341 supports up to 40MHz; 32MHz safe for breadboard)
#define TFT_SPI_FREQ  32000000

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

// SPI pins (explicit definition for PSRAM variant compatibility)
// Adafruit ESP32-S3 Feather default SPI pins
#define SPI_SCK   36  // Default Feather SPI clock
#define SPI_MOSI  35  // Default Feather SPI MOSI
#define SPI_MISO  37  // Default Feather SPI MISO (unused for TFT)

// Button pins (directly wired, active LOW)
#define BUTTON_A 9
#define BUTTON_B 6
#define BUTTON_C 5

// GPS Serial configuration
#define GPS_BAUD 9600
#define GPS_RX RX
#define GPS_TX TX

// Debug flags (set to 1 to enable)
#define DEBUG_GPS   0  // GPS NMEA sentence logging
#define DEBUG_BSEC  0  // BSEC2 readings logging
#define DEBUG_SLEEP 0  // Display sleep/wake logging
#define DEBUG_TFT   1  // TFT display state logging (P1 blank bug debug)

// Screen settings
#define NUM_SCREENS 7
#define SCREEN_OPS 0
#define SCREEN_COMPASS 1
#define SCREEN_GPS 2
#define SCREEN_ENV 3
#define SCREEN_IMU 4
#define SCREEN_DIAGS 5
#define SCREEN_GEOCACHE 6  // Geocaching navigation (#70)

// TFT dimensions
#define TFT_WIDTH 320
#define TFT_HEIGHT 240

// Debounce time in ms
#define DEBOUNCE_MS 200

// WiFi reconnect interval (ms)
#define WIFI_RECONNECT_INTERVAL 30000

// Display sleep timeouts (ms, 0 = disabled)
#define TFT_SLEEP_TIMEOUT  0        // 0 = always on (LCD has no burn-in risk)
#define OLED_SLEEP_TIMEOUT 180000   // 3 minutes for OLED (high burn-in risk)

// Weather logging configuration
#define WEATHER_LOG_INTERVAL  300000   // 5 minutes in ms
#define WEATHER_HISTORY_HOURS 24
#define WEATHER_SAMPLES_MAX   288      // 24hrs * 12 samples/hr
#define LOCATION_THRESHOLD    0.01     // ~1km in degrees

// BSEC sample rate: BSEC_SAMPLE_RATE_LP = 3 sec, BSEC_SAMPLE_RATE_ULP = 5 min

// BSEC state persistence to SD card
#define BSEC_STATE_FILE "/bsec_state.bin"
#define BSEC_STATE_SAVE_INTERVAL 3600000  // 1 hour in ms

// Web server configuration
#define WEB_SERVER_PORT 80
#define SERIAL_RING_SIZE 4096  // 4KB ring buffer for serial capture

// Serial log to SD (#59)
#define LOG_DIR "/logs"
#define LOG_RETENTION_HOURS   48       // Normal retention window
#define LOG_GRACE_HOURS       24       // Grace period after extended off
#define LOG_FLUSH_INTERVAL    5000     // Flush SD buffer every 5 seconds (ms)
#define LOG_SD_BUF_SIZE       512      // RAM buffer before SD write
#define LOG_ROTATION_INTERVAL 3600000  // Check rotation hourly (ms)

// Geocache file configuration (#70)
#define GEOCACHE_GPX_FILE "/geocaches/caches.gpx"
#define GEOCACHE_DIR "/geocaches"
#define GPX_MAX_FILE_SIZE (64 * 1024)  // 64KB max upload

// Watchdog configuration (auto-reset on hang)
#define WDT_TIMEOUT_SEC 30  // Reset if loop hangs for 30 seconds

// Colors (RGB565)
#define COLOR_BG        0x0000  // Black
#define COLOR_TEXT      0xFFFF  // White
#define COLOR_HEADER    0x07FF  // Cyan
#define COLOR_VALUE     0x07E0  // Green
#define COLOR_WARN      0xFD20  // Orange
#define COLOR_ERROR     0xF800  // Red
#define COLOR_DIM       0x7BEF  // Gray

// ============== Global Objects ==============

// TFT Display (hardware SPI - 3-argument constructor)
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// OLED Display
Adafruit_SH1107 oled = Adafruit_SH1107(64, 128, &Wire);

// Sensors
Bsec2 envSensor;
Adafruit_SHT4x sht4 = Adafruit_SHT4x();  // SHT41 temp/humidity (#48)
Adafruit_LSM6DSOX lsm;
Adafruit_LIS3MDL lis;
Adafruit_MAX17048 battery;
Adafruit_FRAM_SPI fram = Adafruit_FRAM_SPI(FRAM_CS);  // SPI FRAM 256KB

// RTC (Adalogger FeatherWing PCF8523)
RTC_PCF8523 rtc;

// Web Server
WebServer webServer(WEB_SERVER_PORT);

// Serial ring buffer for web streaming
static char serialRing[SERIAL_RING_SIZE];
static volatile uint16_t serialRingHead = 0;
static volatile uint16_t serialRingTail = 0;

// Serial log to SD (#59)
static File serialLogFile;
static bool serialLogActive = false;
static char serialLogBuf[LOG_SD_BUF_SIZE];
static uint16_t serialLogBufPos = 0;
static unsigned long lastLogFlush = 0;
static unsigned long lastLogRotation = 0;
static char serialLogFilename[40];  // "/logs/serial_YYYYMMDD_HHMMSS.log"

// ============== Global State ==============

// Current screen (0-3)
int currentScreen = SCREEN_OPS;

// Button debounce
unsigned long lastButtonPress = 0;

// WiFi reconnect tracking
unsigned long lastWiFiAttempt = 0;

// Display sleep state
bool tftSleeping = false;
bool oledSleeping = false;
unsigned long lastActivityTime = 0;

// TFT health monitoring (P1 blank bug debug)
static unsigned long lastTFTUpdate = 0;      // millis() of last successful TFT draw
static unsigned long lastTFTReinit = 0;      // millis() of last preventive re-init
static uint32_t tftUpdateCount = 0;          // Total TFT update cycles
#define TFT_REINIT_INTERVAL 1800000          // Preventive re-init every 30 minutes

// OLED availability
bool oledAvailable = false;

// Sensor availability flags
bool bmeAvailable = false;
bool shtAvailable = false;          // SHT41 temp/humidity (#48)
bool imuAvailable = false;
bool magAvailable = false;
bool batteryAvailable = false;
bool framAvailable = false;           // SPI FRAM 256KB

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

// FRAM state (in RAM — synced from FRAM header on boot)
FRAMHeader framHeader;
unsigned long lastFramFlush = 0;

bool sdAvailable = false;
bool rtcAvailable = false;        // Adalogger RTC
bool wifiConnected = false;
bool ntpSynced = false;
bool webServerStarted = false;

// RTC sync tracking - avoid repeated syncs
bool rtcSyncedFromGPS = false;    // RTC was synced from GPS this session
bool rtcSyncedFromNTP = false;    // RTC was synced from NTP this session

// Periodic status logging
static unsigned long lastStatusLog = 0;
#define STATUS_LOG_INTERVAL 10000  // Log status every 10 seconds

// BSEC state persistence
static uint8_t bsecState[BSEC_MAX_STATE_BLOB_SIZE];
static unsigned long lastBsecStateSave = 0;

// Diagnostics state
static bool bsecStateLoaded = false;
static bool bsecStateSaved = false;
static int weatherLogFileCount = 0;
static int weatherLogEntryCount = 0;
static unsigned long lastWeatherLogCheck = 0;

// Battery logging to SD card
static unsigned long lastBattLog = 0;
#define BATT_LOG_INTERVAL 10000  // Log every 10 seconds
#define BATT_LOG_FILE "/battlog.csv"

// GPS data
struct {
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
} gpsData;

char gpsBuffer[128];
int gpsBufferIndex = 0;

// GPS time-to-first-fix tracking (#68)
static unsigned long gpsFirstReceiveTime = 0;  // When first NMEA data received
static unsigned long gpsFirstFixTime = 0;       // When first valid fix acquired
static unsigned long gpsSignalLostTime = 0;    // When signal was lost (for reacquire timing)
static bool gpsHadFirstReceive = false;         // Tracks if we ever received data
static bool gpsHadFirstFix = false;             // Tracks if we ever had a fix

// IMU data
struct {
  float heading = 0;
  float roll = 0;
  float pitch = 0;
  float accelX = 0;
  float accelY = 0;
  float accelZ = 0;
  float accelMag = 0;
} imuData;

// Geocache data (#70)
#define MAX_CACHES 20

struct GeocacheEntry {
  bool valid;
  float latitude;
  float longitude;
  float difficulty;
  float terrain;
  char name[40];
  char hint[80];
  char gcCode[12];      // GC code (e.g., "GC12345")
  bool found;           // Found status
  uint32_t foundTime;   // When found (unix timestamp)
};

GeocacheEntry cacheList[MAX_CACHES];  // ~4KB RAM for 20 caches
int cacheListCount = 0;               // Number of loaded caches
int selectedCacheIndex = 0;           // Currently selected for navigation
int listScrollOffset = 0;             // For scrollable list display
int geocacheSubScreen = 0;            // 0=nav, 1=list, 2=details
int listHighlightIndex = 0;           // Currently highlighted item in list

// Button C long-press tracking
unsigned long buttonCPressStart = 0;
bool buttonCLongPressHandled = false;
#define LONG_PRESS_MS 800             // 800ms for long press

// User settings (#70)
bool useMetricUnits = false;  // false = imperial (ft/mi), true = metric (m/km)

// SHT41 data (#48) — primary source for temp/humidity
struct {
  float temperature = 0;      // Temperature (C) — ±0.2°C accuracy
  float humidity = 0;         // Relative humidity (%) — ±1.8% accuracy
} shtData;

// BME688 data (via BSEC2)
struct {
  float temperature = 0;      // Compensated temperature (C)
  float humidity = 0;         // Compensated humidity (%)
  float pressure = 0;         // Pressure (hPa)
  float iaq = 0;              // Indoor Air Quality (0-500)
  float co2Equivalent = 0;    // CO2 equivalent (ppm)
  float bvocEquivalent = 0;   // Breath VOC equivalent (ppm)
  float gasResistance = 0;    // Raw gas resistance (kOhm)
  uint8_t iaqAccuracy = 0;    // 0=INIT, 1=LEARN, 2=CAL, 3=OK
} envData;

// Weather history for trend tracking
struct WeatherReading {
  uint32_t timestamp;
  float lat, lon;
  float pressure, temp, humidity;
};

WeatherReading weatherHistory[WEATHER_SAMPLES_MAX];
int weatherHistoryCount = 0;
int weatherHistoryHead = 0;
unsigned long lastWeatherLog = 0;

// Weather trend data
struct {
  float pressureChange3hr = 0;    // hPa change over 3 hours
  float tempChange3hr = 0;        // °C change over 3 hours
  float humidityChange3hr = 0;    // % change over 3 hours
  bool locationChanged = false;   // Moved >1km since last reading
  uint8_t trend = 0;              // 0=stable, 1=rising slow, 2=rising fast, 3=falling slow, 4=falling fast
  const char* forecast = "Init";  // "Clear", "Rain Likely", etc.
} weatherTrend;

// ============== SD Card Health Monitoring ==============
// Tracks SD card errors and enables graceful degradation

enum SDErrorType {
  SD_ERR_NONE = 0,
  SD_ERR_OPEN_FAIL,
  SD_ERR_READ_FAIL,
  SD_ERR_WRITE_FAIL,
  SD_ERR_REINIT_FAIL
};

struct SDHealth {
  bool available;              // Current availability status
  unsigned long lastSuccess;   // millis() of last successful operation
  unsigned long lastAttempt;   // millis() of last attempted operation
  uint16_t errorCount;         // Total errors since boot
  uint8_t consecutiveFailures; // Consecutive failures (resets on success)
  uint8_t reInitCount;         // Re-initialization attempts
  uint8_t lastError;           // Last error type (SDErrorType)
  unsigned long lastReInit;    // millis() of last re-init attempt
};

static SDHealth sdHealth = {false, 0, 0, 0, 0, 0, SD_ERR_NONE, 0};

#define SD_MAX_CONSECUTIVE_FAILURES 3
#define SD_MAX_REINIT_ATTEMPTS 5
#define SD_REINIT_COOLDOWN 60000  // Wait 60s between re-init attempts

// Forward declarations for SD health functions (defined after initSD)
void recordSDSuccess();
void recordSDError(SDErrorType err);
bool shouldAttemptReInit();
bool trySDReInit();
File sdOpenSafe(const char* path, const char* mode, bool silent = false);

// Forward declarations for serial log to SD (#59)
void initSerialLog();
void serialLogAppend(const char* str);
void serialLogFlush();
void serialLogRotate();

// ============== Serial Ring Buffer (moved before setup for use in init) ==============

void serialRingAppend(const char* str) {
  while (*str) {
    serialRing[serialRingHead] = *str++;
    serialRingHead = (serialRingHead + 1) % SERIAL_RING_SIZE;
    // If we catch up to tail, advance tail (lose oldest data)
    if (serialRingHead == serialRingTail) {
      serialRingTail = (serialRingTail + 1) % SERIAL_RING_SIZE;
    }
  }
}

// Read and clear the ring buffer
String serialRingRead() {
  String result;
  result.reserve(SERIAL_RING_SIZE);
  while (serialRingTail != serialRingHead) {
    result += serialRing[serialRingTail];
    serialRingTail = (serialRingTail + 1) % SERIAL_RING_SIZE;
  }
  return result;
}

// Peek at ring buffer without clearing
String serialRingPeek() {
  String result;
  result.reserve(SERIAL_RING_SIZE);
  uint16_t pos = serialRingTail;
  while (pos != serialRingHead) {
    result += serialRing[pos];
    pos = (pos + 1) % SERIAL_RING_SIZE;
  }
  return result;
}

// Web serial streaming position tracker
static uint16_t webSerialReadPos = 0;

// Custom print that captures to ring buffer and SD log (#59)
void logPrint(const char* msg) {
  serialRingAppend(msg);
  serialLogAppend(msg);
  Serial.print(msg);
}

void logPrintln(const char* msg) {
  serialRingAppend(msg);
  serialRingAppend("\n");
  serialLogAppend(msg);
  serialLogAppend("\n");
  Serial.println(msg);
}

void logPrintf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  serialRingAppend(buf);
  serialLogAppend(buf);
  Serial.print(buf);
}

// ============== Serial Log to SD (#59) ==============

void serialLogAppend(const char* str) {
  if (!serialLogActive) return;
  while (*str) {
    serialLogBuf[serialLogBufPos++] = *str++;
    if (serialLogBufPos >= LOG_SD_BUF_SIZE) {
      serialLogFlush();
    }
  }
}

void serialLogFlush() {
  if (!serialLogActive || serialLogBufPos == 0) return;
  if (serialLogFile) {
    size_t written = serialLogFile.write((uint8_t*)serialLogBuf, serialLogBufPos);
    if (written != (size_t)serialLogBufPos) {
      // Write failure — close and reopen, retry once
      serialLogFile.close();
      serialLogFile = SD.open(serialLogFilename, FILE_APPEND);
      if (serialLogFile) {
        serialLogFile.write((uint8_t*)serialLogBuf, serialLogBufPos);
      } else {
        serialLogActive = false;
        Serial.println("[LOG] SD write failed, logging disabled");
      }
    }
    serialLogFile.flush();
  }
  serialLogBufPos = 0;
  lastLogFlush = millis();
}

void serialLogRotate() {
  if (!sdAvailable) return;

  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return;

  // First pass: find the most recent existing log timestamp
  time_t now;
  time(&now);
  time_t newestLog = 0;

  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char* name = entry.name();
      int yr, mo, dy, hr, mn, sc;
      if (sscanf(name, "serial_%4d%2d%2d_%2d%2d%2d.log", &yr, &mo, &dy, &hr, &mn, &sc) == 6) {
        struct tm t = {};
        t.tm_year = yr - 1900;
        t.tm_mon = mo - 1;
        t.tm_mday = dy;
        t.tm_hour = hr;
        t.tm_min = mn;
        t.tm_sec = sc;
        time_t logTime = mktime(&t);
        if (logTime > newestLog) newestLog = logTime;
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  // Decide retention: normal (48h) or grace (keep all)
  double hoursSinceNewest = (newestLog > 0) ? difftime(now, newestLog) / 3600.0 : 0;
  bool graceMode = (newestLog > 0 && hoursSinceNewest >= LOG_RETENTION_HOURS);

  if (graceMode) {
    logPrintf("[LOG] Grace mode: last log %.0fh old, keeping all files\n", hoursSinceNewest);
    return;  // Keep everything — rotation resumes after LOG_GRACE_HOURS of uptime
  }

  // Normal mode: delete files older than LOG_RETENTION_HOURS
  time_t cutoff = now - ((time_t)LOG_RETENTION_HOURS * 3600);
  int deleted = 0;

  dir = SD.open(LOG_DIR);
  entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char* name = entry.name();
      int yr, mo, dy, hr, mn, sc;
      if (sscanf(name, "serial_%4d%2d%2d_%2d%2d%2d.log", &yr, &mo, &dy, &hr, &mn, &sc) == 6) {
        struct tm t = {};
        t.tm_year = yr - 1900;
        t.tm_mon = mo - 1;
        t.tm_mday = dy;
        t.tm_hour = hr;
        t.tm_min = mn;
        t.tm_sec = sc;
        time_t logTime = mktime(&t);
        if (logTime < cutoff) {
          char fullPath[60];
          snprintf(fullPath, sizeof(fullPath), LOG_DIR "/%s", name);
          entry.close();
          SD.remove(fullPath);
          deleted++;
          entry = dir.openNextFile();
          continue;
        }
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  if (deleted > 0) {
    logPrintf("[LOG] Rotation: deleted %d files older than %dh\n", deleted, LOG_RETENTION_HOURS);
  }
}

void initSerialLog() {
  if (!sdAvailable || !rtcAvailable) {
    logPrintln("[LOG] Serial log disabled (no SD or RTC)");
    return;
  }

  // Create /logs/ directory if missing
  if (!SD.exists(LOG_DIR)) {
    SD.mkdir(LOG_DIR);
  }

  // Build filename from RTC time
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    logPrintln("[LOG] Serial log disabled (no time source)");
    return;
  }

  snprintf(serialLogFilename, sizeof(serialLogFilename),
           LOG_DIR "/serial_%04d%02d%02d_%02d%02d%02d.log",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  // Run smart rotation before opening new file
  serialLogRotate();

  // Open log file for append
  serialLogFile = SD.open(serialLogFilename, FILE_APPEND);
  if (!serialLogFile) {
    logPrintf("[LOG] Failed to open %s\n", serialLogFilename);
    return;
  }

  serialLogActive = true;
  serialLogBufPos = 0;
  lastLogFlush = millis();
  lastLogRotation = millis();
  logPrintf("[LOG] Logging to %s\n", serialLogFilename);
}

// ============== Setup ==============

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Boot banner - capture to ring buffer for web serial
  char banner[128];
  snprintf(banner, sizeof(banner), "=================================\nField Compass Dual %s\n=================================\n\n", FW_VERSION);
  serialRingAppend(banner);
  Serial.print(banner);

  // Initialize SPI for TFT with explicit pins
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS);

  // Initialize TFT first for visual feedback
  initTFT();

  // Initialize I2C
  Wire.begin();

  // Show init screen
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(COLOR_HEADER);
  tft.setTextSize(3);
  tft.setCursor(40, 40);
  tft.println("Field Compass");
  tft.setTextSize(2);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(90, 90);
  char verStr[24];
  snprintf(verStr, sizeof(verStr), "%s BSEC2", FW_VERSION);
  tft.println(verStr);
  tft.setTextSize(1);
  tft.setCursor(40, 140);
  tft.setTextColor(COLOR_DIM);
  tft.println("Initializing hardware...");

  // Scan I2C bus
  scanI2C();

  // Initialize all hardware
  initOLED();
  initGPS();
  initBME688();
  initSHT41();   // SHT41 temp/humidity (#48)
  initIMU();
  initBattery();

  // Initialize hardware SPI for SD card (Adalogger uses different CS pin)
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  initSD();
  initFRAM();   // SPI FRAM 256KB (shared bus with TFT/SD)
  initRTC();    // Adalogger RTC - sets system time if RTC has valid time
  initSerialLog();  // Serial log to SD (#59) - needs SD + RTC
  initWiFi();   // Will sync NTP if connected, then sync RTC

  // Flush any FRAM data from previous session to SD
  if (framAvailable && sdAvailable) {
    framFlushToSD();
  }

  // Load BSEC state: try FRAM first (fast), fall back to SD
  if (bmeAvailable) {
    if (!loadBsecFromFRAM()) {
      if (sdAvailable) loadBsecState();
    }
  }

  // Load weather history from SD
  if (sdAvailable) {
    loadWeatherHistory();
  }

  // Initialize web server
  initWebServer();

  // Setup buttons
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

  logPrintln("\nSetup complete!\n");

  // Initialize software watchdog (auto-reset on hang)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << 0) | (1 << 1),  // Watch both cores
    .trigger_panic = true  // Reset on timeout
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);  // Add current task to watchdog
  logPrintf("Watchdog enabled: %ds timeout\n", WDT_TIMEOUT_SEC);

  // Initialize activity timer for display sleep
  lastActivityTime = millis();

  // Load geocaches from SD card (#70)
  loadGeocachesFromSD();

  // Clear screen for main display
  tft.fillScreen(COLOR_BG);
}

// ============== Main Loop ==============

void loop() {
  // Handle button navigation
  handleButtons();

  // Check display sleep timeout
  checkDisplaySleep();

  // Check WiFi and attempt reconnect if needed
  checkWiFi();

  // Handle web server requests
  if (wifiConnected) {
    webServer.handleClient();
  }

  // Update sensor data (even when display sleeping)
  readGPS();
  if (bmeAvailable) readBME688();
  if (shtAvailable) readSHT41();   // SHT41 temp/humidity (#48)
  if (imuAvailable && magAvailable) readIMU();

  // Weather logging (every 5 minutes)
  if (sdAvailable && bmeAvailable && (millis() - lastWeatherLog > WEATHER_LOG_INTERVAL)) {
    logWeatherReading();
    calculateWeatherTrend();
    lastWeatherLog = millis();
  }

  // Update weather log statistics periodically
  updateWeatherLogStats();

  // Battery logging (every 10 seconds) — FRAM buffered, SD fallback
  if (batteryAvailable && (millis() - lastBattLog > BATT_LOG_INTERVAL)) {
    if (framAvailable) {
      logBatteryToFRAM();
    } else if (sdAvailable) {
      logBatteryToSD();  // Fallback: direct SD write
    }
    lastBattLog = millis();
  }

  // FRAM flush to SD (hybrid: every 5 minutes)
  if (framAvailable && sdAvailable && (millis() - lastFramFlush > FRAM_FLUSH_INTERVAL)) {
    framFlushToSD();
  }

  // Serial log flush to SD (every 5 seconds) (#59)
  if (serialLogActive && (millis() - lastLogFlush > LOG_FLUSH_INTERVAL)) {
    serialLogFlush();
  }

  // Serial log rotation check (hourly) (#59)
  // After grace period (LOG_GRACE_HOURS of uptime), switch to normal rotation
  if (serialLogActive && (millis() - lastLogRotation > LOG_ROTATION_INTERVAL)) {
    serialLogRotate();
    lastLogRotation = millis();
  }

  // Periodic status logging for web serial monitor
  if (millis() - lastStatusLog > STATUS_LOG_INTERVAL) {
    char buf[256];
    float battV = batteryAvailable ? battery.cellVoltage() : 0;
    float battP = batteryAvailable ? battery.cellPercent() : 0;
    float battR = batteryAvailable ? battery.chargeRate() : 0;
    // Include GPS TTFF in status (#68)
    const char* gpsStatus = gpsHadFirstFix ? "GPS:OK" : (gpsHadFirstReceive ? "GPS:--" : "GPS:NO");
    // SHT41 temp/humidity preferred in status log (#48)
    float logTempC = shtAvailable ? shtData.temperature : envData.temperature;
    float logHumid = shtAvailable ? shtData.humidity : envData.humidity;
    snprintf(buf, sizeof(buf), "[%lus] T:%.1fF H:%.0f%% IAQ:%.0f Hdg:%.0f %s Sat:%d HDOP:%.1f TTFF:%lus Batt:%.3fV/%.2f%%/%+.1f%%hr\n",
             millis() / 1000,
             logTempC * 9.0 / 5.0 + 32.0,
             logHumid,
             envData.iaq,
             imuData.heading,
             gpsStatus,
             gpsData.satellites,
             gpsData.hdop,
             gpsHadFirstFix ? gpsFirstFixTime / 1000 : 0,
             battV, battP, battR);
    serialRingAppend(buf);
    serialLogAppend(buf);  // SD log (#59)
    Serial.print(buf);

    // TFT debug logging (P1 blank bug investigation)
    #if DEBUG_TFT
    unsigned long tftAge = (millis() - lastTFTUpdate) / 1000;
    unsigned long reinitAge = (millis() - lastTFTReinit) / 1000;
    snprintf(buf, sizeof(buf), "[TFT] sleep:%d scr:%d upd:%lus ago reinit:%lus ago cnt:%lu\n",
             tftSleeping, currentScreen, tftAge, reinitAge, tftUpdateCount);
    serialRingAppend(buf);
    serialLogAppend(buf);  // SD log (#59)
    Serial.print(buf);
    #endif

    lastStatusLog = millis();
  }

  // Check TFT health and perform preventive re-init if needed
  checkTFTHealth();

  // Update display based on current screen
  updateDisplay();

  // Reset watchdog timer (proves loop is not hung)
  esp_task_wdt_reset();

  delay(50);
}

// ============== I2C Scanner ==============

void scanI2C() {
  logPrintln("Scanning I2C bus...");

  int deviceCount = 0;
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0) {
      const char* desc = "";
      if (address == 0x3C || address == 0x3D) {
        desc = " (OLED Display)";
      } else if (address == 0x76 || address == 0x77) {
        desc = " (BME688)";
      } else if (address == 0x6A || address == 0x6B) {
        desc = " (LSM6DSOX - Accel/Gyro)";
      } else if (address == 0x1C || address == 0x1E) {
        desc = " (LIS3MDL - Magnetometer)";
      } else if (address == 0x36) {
        desc = " (MAX17048 - Battery Gauge)";
      } else if (address == 0x44) {
        desc = " (SHT41 - Temp/Humidity)";
      } else if (address == 0x29) {
        desc = " (VEML7700 - Light Sensor)";
      } else if (address == 0x68) {
        desc = " (PCF8523 - RTC)";
      } else if (address == 0x7E) {
        desc = " (MAX17048 - Alt Addr)";
      }
      logPrintf("  Found device at 0x%02X%s\n", address, desc);
      deviceCount++;
    }
  }
  logPrintf("  Total devices: %d\n\n", deviceCount);
}

// ============== Initialization Functions ==============

void initTFT() {
  logPrint("Initializing ILI9341 TFT... ");

  tft.begin(TFT_SPI_FREQ);

  tft.setRotation(3);  // Landscape mode, flipped 180 for ILI9341 board orientation
  tft.fillScreen(ILI9341_RED);  // Flash red to confirm TFT is working
  delay(100);
  tft.fillScreen(COLOR_BG);

  lastTFTReinit = millis();  // Track init time
  logPrintln("OK (320x240)");
}

// Preventive TFT re-initialization (P1 blank bug workaround)
// TFT goes blank after ~40 minutes with no errors - re-init as workaround
void checkTFTHealth() {
  unsigned long now = millis();

  // Skip if TFT is sleeping (intentional blank)
  if (tftSleeping) return;

  // Preventive re-init every 30 minutes
  if (now - lastTFTReinit > TFT_REINIT_INTERVAL) {
    #if DEBUG_TFT
    logPrintf("[TFT] Preventive re-init at %lus (updates:%lu)\n",
              now / 1000, tftUpdateCount);
    #endif

    // Re-initialize TFT
    tft.begin(TFT_SPI_FREQ);
    tft.setRotation(3);
    tft.fillScreen(COLOR_BG);

    lastTFTReinit = now;
    logPrintln("[TFT] Preventive re-init complete");
  }
}

void initOLED() {
  logPrint("Initializing OLED... ");

  if (!oled.begin(0x3C, true)) {
    if (!oled.begin(0x3D, true)) {
      logPrintln("NOT FOUND");
      return;
    }
  }

  oled.setRotation(1);
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.display();

  oledAvailable = true;
  logPrintln("OK (128x64)");
}

void initGPS() {
  logPrintf("Initializing GPS on RX=%d, TX=%d... ", GPS_RX, GPS_TX);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  logPrintln("OK (9600 baud)");
}

// BSEC2 callback - called when new sensor data is available
void bsecDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec) {
  if (!outputs.nOutputs) return;

  for (uint8_t i = 0; i < outputs.nOutputs; i++) {
    const bsecData output = outputs.output[i];
    switch (output.sensor_id) {
      case BSEC_OUTPUT_IAQ:
        envData.iaq = output.signal;
        envData.iaqAccuracy = output.accuracy;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
        envData.temperature = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
        envData.humidity = output.signal;
        break;
      case BSEC_OUTPUT_RAW_PRESSURE:
        #if DEBUG_BSEC
        Serial.print("Raw pressure: ");
        Serial.println(output.signal);
        #endif
        envData.pressure = output.signal;  // Already in hPa with BME680 IAQ config
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:
        envData.co2Equivalent = output.signal;
        break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
        envData.bvocEquivalent = output.signal;
        break;
      case BSEC_OUTPUT_RAW_GAS:
        envData.gasResistance = output.signal / 1000.0;  // Ohm to kOhm
        break;
    }
  }

  // BSEC state persistence: save to FRAM on every accuracy change, SD on level 3
  static uint8_t lastAccuracy = 0;
  if (envData.iaqAccuracy != lastAccuracy) {
    if (framAvailable) {
      saveBsecToFRAM();  // Fast save to FRAM on every accuracy change
    }
    if (envData.iaqAccuracy == 3 && lastAccuracy < 3) {
      saveBsecState();   // Also save to SD when reaching accuracy 3
    }
  }
  lastAccuracy = envData.iaqAccuracy;

  // Periodic BSEC state save (hourly)
  if (millis() - lastBsecStateSave > BSEC_STATE_SAVE_INTERVAL) {
    saveBsecState();
  }
}

void initBME688() {
  logPrint("Initializing BME688 (BSEC2)... ");

  // BSEC2 sensor outputs to subscribe to
  bsecSensor sensorList[] = {
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT
  };

  // Try primary address (0x77), then secondary (0x76)
  if (!envSensor.begin(0x77, Wire)) {
    if (!envSensor.begin(0x76, Wire)) {
      logPrintln("NOT FOUND");
      logPrintf("  BSEC status: %d\n", envSensor.status);
      logPrintf("  Sensor status: %d\n", envSensor.sensor.status);
      return;
    }
  }

  // Load BSEC2 IAQ config
  if (!envSensor.setConfig(bsec2_config)) {
    logPrintln("CONFIG FAILED");
    logPrintf("  BSEC status: %d\n", envSensor.status);
    return;
  }

  // Set temperature offset for self-heating compensation
  envSensor.setTemperatureOffset(3.0);  // Adjust based on testing

  // Subscribe to desired outputs (LP = 3 second sample rate)
  if (!envSensor.updateSubscription(sensorList, sizeof(sensorList) / sizeof(sensorList[0]), BSEC_SAMPLE_RATE_LP)) {
    logPrintln("SUBSCRIPTION FAILED");
    logPrintf("  BSEC status: %d\n", envSensor.status);
    return;
  }

  // Attach callback for new data
  envSensor.attachCallback(bsecDataCallback);

  bmeAvailable = true;
  logPrintln("OK");
  logPrintf("  BSEC version: %d.%d.%d.%d\n",
            envSensor.version.major, envSensor.version.minor,
            envSensor.version.major_bugfix, envSensor.version.minor_bugfix);
}

void initSHT41() {
  logPrint("Initializing SHT41... ");

  if (!sht4.begin()) {
    logPrintln("NOT FOUND");
    return;
  }

  // Use high precision, no heater (best accuracy, ~8.2ms measurement)
  sht4.setPrecision(SHT4X_HIGH_PRECISION);
  sht4.setHeater(SHT4X_NO_HEATER);

  shtAvailable = true;
  logPrintln("OK (0x44)");

  // Read initial values immediately
  sensors_event_t humEv, tempEv;
  if (sht4.getEvent(&humEv, &tempEv)) {
    shtData.temperature = tempEv.temperature;
    shtData.humidity = humEv.relative_humidity;
    logPrintf("  Initial: %.1fC / %.1f%%\n", shtData.temperature, shtData.humidity);
  }
}

// Read FRAM header into RAM (use sizeof to avoid overflowing the struct)
void framReadHeader() {
  uint8_t buf[FRAM_HEADER_SIZE];
  for (int i = 0; i < FRAM_HEADER_SIZE; i++) {
    buf[i] = fram.read8(FRAM_HEADER_ADDR + i);
  }
  memcpy(&framHeader, buf, sizeof(framHeader));  // only copy struct-sized bytes
}

// Write the RAM header back to FRAM (zero-pad to fill full 64-byte region)
void framWriteHeader() {
  uint8_t buf[FRAM_HEADER_SIZE];
  memset(buf, 0, FRAM_HEADER_SIZE);              // zero-fill padding bytes
  memcpy(buf, &framHeader, sizeof(framHeader));   // copy struct into buffer
  for (int i = 0; i < FRAM_HEADER_SIZE; i++) {
    fram.write8(FRAM_HEADER_ADDR + i, buf[i]);
  }
}

// Format FRAM with clean header (zeroed ring buffers)
void framFormat() {
  logPrintln("  Formatting...");
  memset(&framHeader, 0, sizeof(framHeader));
  framHeader.magic = FRAM_MAGIC;
  framHeader.version = FRAM_VERSION;
  framHeader.battCapacity = FRAM_BATT_COUNT;
  framHeader.wxCapacity = FRAM_WX_COUNT;
  framWriteHeader();
  logPrintln("  Format complete");
}

void initFRAM() {
  logPrint("Initializing FRAM... ");
  // Safety check: struct must fit within FRAM header region
  static_assert(sizeof(FRAMHeader) <= FRAM_HEADER_SIZE, "FRAMHeader exceeds FRAM_HEADER_SIZE");

  if (!fram.begin()) {
    logPrintln("NOT FOUND (check wiring)");
    return;
  }

  // Verify FRAM by reading manufacturer/product IDs
  uint8_t mfgId;
  uint16_t prodId;
  fram.getDeviceID(&mfgId, &prodId);
  framAvailable = true;
  logPrintf("OK (mfg:0x%02X prod:0x%04X, 256KB, hdr=%d/%d bytes)\n",
            mfgId, prodId, sizeof(FRAMHeader), FRAM_HEADER_SIZE);

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
}

void initIMU() {
  logPrint("Initializing LSM6DSOX... ");

  if (!lsm.begin_I2C(0x6A)) {
    if (!lsm.begin_I2C(0x6B)) {
      logPrintln("NOT FOUND");
      return;
    }
  }

  lsm.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
  lsm.setGyroRange(LSM6DS_GYRO_RANGE_500_DPS);
  lsm.setAccelDataRate(LSM6DS_RATE_104_HZ);
  lsm.setGyroDataRate(LSM6DS_RATE_104_HZ);

  imuAvailable = true;
  logPrintln("OK");

  logPrint("Initializing LIS3MDL... ");

  if (!lis.begin_I2C(0x1C)) {
    if (!lis.begin_I2C(0x1E)) {
      logPrintln("NOT FOUND");
      return;
    }
  }

  lis.setPerformanceMode(LIS3MDL_MEDIUMMODE);
  lis.setOperationMode(LIS3MDL_CONTINUOUSMODE);
  lis.setDataRate(LIS3MDL_DATARATE_155_HZ);
  lis.setRange(LIS3MDL_RANGE_4_GAUSS);

  magAvailable = true;
  logPrintln("OK");
}

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

void initSD() {
  logPrint("Initializing SD card... ");

  if (!SD.begin(SD_CS)) {
    logPrintln("NOT FOUND");
    sdHealth.available = false;
    return;
  }

  sdAvailable = true;
  sdHealth.available = true;
  sdHealth.lastSuccess = millis();
  sdHealth.lastAttempt = millis();

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  logPrintf("OK (%llu MB)\n", cardSize);

  // Create weather directory if needed
  if (!SD.exists("/weather")) {
    SD.mkdir("/weather");
  }
}

void initRTC() {
  logPrint("Initializing RTC (PCF8523)... ");

  if (!rtc.begin()) {
    logPrintln("NOT FOUND");
    return;
  }

  rtcAvailable = true;

  // Check if RTC lost power and is running with invalid time
  if (!rtc.initialized() || rtc.lostPower()) {
    logPrintln("OK (needs time sync)");
    // Don't set a default time - wait for GPS or NTP to provide accurate time
    return;
  }

  // RTC has valid time - use it to set system time
  DateTime now = rtc.now();
  struct tm timeinfo;
  timeinfo.tm_year = now.year() - 1900;
  timeinfo.tm_mon = now.month() - 1;
  timeinfo.tm_mday = now.day();
  timeinfo.tm_hour = now.hour();
  timeinfo.tm_min = now.minute();
  timeinfo.tm_sec = now.second();

  time_t t = mktime(&timeinfo);
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, NULL);

  logPrintf("OK (%04d-%02d-%02d %02d:%02d:%02d)\n",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute(), now.second());
}

// Sync RTC from current system time (call after GPS or NTP sync)
void syncRTCFromSystemTime(const char* source) {
  if (!rtcAvailable) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  // Adjust for timezone - RTC stores UTC
  time_t now;
  time(&now);
  struct tm* utc = gmtime(&now);

  rtc.adjust(DateTime(utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                      utc->tm_hour, utc->tm_min, utc->tm_sec));

  logPrintf("[RTC] Synced from %s: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
            source,
            utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
            utc->tm_hour, utc->tm_min, utc->tm_sec);
}

// ============== SD Card Health Functions ==============

// Record successful SD operation
void recordSDSuccess() {
  sdHealth.lastSuccess = millis();
  sdHealth.consecutiveFailures = 0;
}

// Record SD error and potentially trigger re-init
void recordSDError(SDErrorType err) {
  sdHealth.lastError = err;
  sdHealth.errorCount++;
  sdHealth.consecutiveFailures++;
  sdHealth.lastAttempt = millis();

  logPrintf("[SD] Error %d (total:%d consec:%d)\n",
            err, sdHealth.errorCount, sdHealth.consecutiveFailures);

  // If too many consecutive failures, try re-init
  if (sdHealth.consecutiveFailures >= SD_MAX_CONSECUTIVE_FAILURES) {
    trySDReInit();
  }
}

// Check if we should attempt SD re-initialization
bool shouldAttemptReInit() {
  // Don't exceed max attempts
  if (sdHealth.reInitCount >= SD_MAX_REINIT_ATTEMPTS) return false;

  // Enforce cooldown period
  if (millis() - sdHealth.lastReInit < SD_REINIT_COOLDOWN) return false;

  return true;
}

// Attempt SD re-initialization with backoff
bool trySDReInit() {
  if (!shouldAttemptReInit()) {
    logPrintln("[SD] Re-init skipped (cooldown or max attempts)");
    return false;
  }

  sdHealth.reInitCount++;
  sdHealth.lastReInit = millis();

  logPrintf("[SD] Attempting re-init #%d...\n", sdHealth.reInitCount);

  // End current SD session
  SD.end();
  delay(100);

  // Try to re-initialize
  if (SD.begin(SD_CS)) {
    sdAvailable = true;
    sdHealth.available = true;
    sdHealth.consecutiveFailures = 0;
    sdHealth.lastSuccess = millis();
    logPrintln("[SD] Re-init SUCCESS");
    return true;
  } else {
    sdAvailable = false;
    sdHealth.available = false;
    sdHealth.lastError = SD_ERR_REINIT_FAIL;
    logPrintln("[SD] Re-init FAILED");
    return false;
  }
}

// Safe file open with error tracking
File sdOpenSafe(const char* path, const char* mode, bool silent) {
  if (!sdHealth.available) {
    return File();  // Return invalid file
  }

  sdHealth.lastAttempt = millis();

  File f;
  if (strcmp(mode, "r") == 0 || strcmp(mode, FILE_READ) == 0) {
    f = SD.open(path, FILE_READ);
  } else if (strcmp(mode, "w") == 0 || strcmp(mode, FILE_WRITE) == 0) {
    f = SD.open(path, FILE_WRITE);
  } else if (strcmp(mode, "a") == 0) {
    f = SD.open(path, FILE_APPEND);
  } else {
    f = SD.open(path);  // Default mode
  }

  if (!f) {
    if (!silent) {
      recordSDError(SD_ERR_OPEN_FAIL);
    }
    return File();
  }

  recordSDSuccess();
  return f;
}

// ============== BSEC State Persistence ==============

bool loadBsecState() {
  if (!sdHealth.available) return false;

  if (!SD.exists(BSEC_STATE_FILE)) {
    logPrintln("No BSEC state file found");
    return false;
  }

  File file = sdOpenSafe(BSEC_STATE_FILE, "r");
  if (!file) {
    logPrintln("Failed to open BSEC state file");
    return false;
  }

  size_t bytesRead = file.read(bsecState, BSEC_MAX_STATE_BLOB_SIZE);
  file.close();

  if (bytesRead != BSEC_MAX_STATE_BLOB_SIZE) {
    logPrintln("Invalid BSEC state file size");
    recordSDError(SD_ERR_READ_FAIL);
    return false;
  }

  if (!envSensor.setState(bsecState)) {
    logPrintf("Failed to restore BSEC state: %d\n", envSensor.status);
    return false;
  }

  logPrintln("BSEC state restored from SD card");
  bsecStateLoaded = true;
  return true;
}

bool saveBsecState() {
  if (!sdHealth.available) return false;

  if (!envSensor.getState(bsecState)) {
    logPrintf("Failed to get BSEC state: %d\n", envSensor.status);
    return false;
  }

  File file = sdOpenSafe(BSEC_STATE_FILE, "w");
  if (!file) {
    logPrintln("Failed to create BSEC state file");
    return false;
  }

  size_t bytesWritten = file.write(bsecState, BSEC_MAX_STATE_BLOB_SIZE);
  file.close();

  if (bytesWritten != BSEC_MAX_STATE_BLOB_SIZE) {
    logPrintln("Failed to write BSEC state");
    recordSDError(SD_ERR_WRITE_FAIL);
    return false;
  }

  logPrintln("BSEC state saved to SD card");
  lastBsecStateSave = millis();
  bsecStateSaved = true;
  return true;
}

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
            framHeader.bsecAccuracy, framHeader.bsecTimestamp / 1000);
  bsecStateLoaded = true;
  return true;
}

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
      framHeader.battTail = framHeader.battHead;
      framHeader.battCount = 0;
    }
  }

  // --- Flush weather entries ---
  if (framHeader.wxCount > 0) {
    char filename[32];
    getWeatherFilename(filename, 0);  // Today's file
    File file = sdOpenSafe(filename, "a", true);
    if (file) {
      uint16_t idx = framHeader.wxTail;
      for (int i = 0; i < framHeader.wxCount; i++) {
        WeatherReading reading;
        uint32_t addr = FRAM_WX_ADDR + (idx * FRAM_WX_ENTRY);
        uint8_t* data = (uint8_t*)&reading;
        for (int j = 0; j < FRAM_WX_ENTRY; j++) {
          data[j] = fram.read8(addr + j);
        }
        file.printf("%lu,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                    reading.timestamp, reading.lat, reading.lon,
                    reading.pressure, reading.temp, reading.humidity);
        idx = (idx + 1) % FRAM_WX_COUNT;
        wxFlushed++;
      }
      file.close();
      recordSDSuccess();
      framHeader.wxTail = framHeader.wxHead;
      framHeader.wxCount = 0;
    }
  }

  // Update header: clear dirty flag
  framHeader.flags &= ~0x01;
  framWriteHeader();

  unsigned long elapsed = millis() - startMs;
  logPrintf("[FRAM] Flushed: batt=%d wx=%d (%lums)\n", battFlushed, wxFlushed, elapsed);
  lastFramFlush = millis();
}

// ============== Geocache Found Status Persistence (#70) ==============

#define GEOCACHE_FOUND_FILE "/geocache_found.csv"

// Save found status for all caches to SD card
void saveCacheFoundStatus() {
  if (!sdAvailable) return;

  File file = sdOpenSafe(GEOCACHE_FOUND_FILE, "w");
  if (!file) {
    logPrintln("[GEOCACHE] Failed to save found status");
    return;
  }

  // Write header
  file.println("gcCode,found,timestamp");

  // Write each cache's found status
  for (int i = 0; i < cacheListCount; i++) {
    if (cacheList[i].valid && cacheList[i].found) {
      file.printf("%s,1,%lu\n", cacheList[i].gcCode, cacheList[i].foundTime);
    }
  }

  file.close();
  recordSDSuccess();
  logPrintf("[GEOCACHE] Saved found status for %d caches\n", cacheListCount);
}

// Load found status from SD card and apply to loaded caches
void loadCacheFoundStatus() {
  if (!sdAvailable) return;

  File file = sdOpenSafe(GEOCACHE_FOUND_FILE, "r", true);  // silent fail
  if (!file) {
    logPrintln("[GEOCACHE] No saved found status file");
    return;
  }

  // Skip header line
  file.readStringUntil('\n');

  int loadedCount = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse CSV: gcCode,found,timestamp
    int comma1 = line.indexOf(',');
    int comma2 = line.indexOf(',', comma1 + 1);
    if (comma1 < 0 || comma2 < 0) continue;

    String gcCode = line.substring(0, comma1);
    String foundStr = line.substring(comma1 + 1, comma2);
    String timestampStr = line.substring(comma2 + 1);

    // Find matching cache and apply found status
    for (int i = 0; i < cacheListCount; i++) {
      if (strcmp(cacheList[i].gcCode, gcCode.c_str()) == 0) {
        cacheList[i].found = (foundStr == "1");
        cacheList[i].foundTime = timestampStr.toInt();
        loadedCount++;
        break;
      }
    }
  }

  file.close();
  recordSDSuccess();
  logPrintf("[GEOCACHE] Loaded found status: %d entries\n", loadedCount);
}

// ============== GPX File Parser (#70) ==============

// GPX upload state (for multipart handler)
static String gpxUploadBuffer;
static bool gpxUploadSuccess = false;
static String gpxUploadError;

// Decode ROT13 hint in place (geocaching.com encodes hints this way)
void decodeROT13(char* str) {
  for (int i = 0; str[i]; i++) {
    char c = str[i];
    if ((c >= 'A' && c <= 'M') || (c >= 'a' && c <= 'm')) {
      str[i] = c + 13;
    } else if ((c >= 'N' && c <= 'Z') || (c >= 'n' && c <= 'z')) {
      str[i] = c - 13;
    }
  }
}

// Extract text between XML tags into destination buffer
bool extractXMLField(const String& xml, const char* startTag, const char* endTag,
                     char* dest, size_t destSize) {
  int start = xml.indexOf(startTag);
  if (start < 0) return false;
  start += strlen(startTag);

  int end = xml.indexOf(endTag, start);
  if (end < 0) return false;

  String value = xml.substring(start, end);
  value.trim();

  // Decode common HTML entities
  value.replace("&amp;", "&");
  value.replace("&lt;", "<");
  value.replace("&gt;", ">");
  value.replace("&quot;", "\"");
  value.replace("&#39;", "'");
  value.replace("&apos;", "'");

  strncpy(dest, value.c_str(), destSize - 1);
  dest[destSize - 1] = '\0';
  return true;
}

// Extract float value from XML tags with default
float extractXMLFloat(const String& xml, const char* startTag, const char* endTag, float defaultVal) {
  char buf[16];
  if (extractXMLField(xml, startTag, endTag, buf, sizeof(buf))) {
    return atof(buf);
  }
  return defaultVal;
}

// Parse GPX data and populate cacheList[]
// Returns: number of caches successfully parsed
int parseGPXFromString(const String& gpxData) {
  cacheListCount = 0;

  int searchPos = 0;
  while (cacheListCount < MAX_CACHES) {
    // Find next <wpt> element
    int wptStart = gpxData.indexOf("<wpt", searchPos);
    if (wptStart < 0) break;

    int wptEnd = gpxData.indexOf("</wpt>", wptStart);
    if (wptEnd < 0) break;

    String wptBlock = gpxData.substring(wptStart, wptEnd + 6);
    searchPos = wptEnd + 6;

    GeocacheEntry entry;
    memset(&entry, 0, sizeof(entry));

    // Extract lat/lon from <wpt lat="..." lon="...">
    int latPos = wptBlock.indexOf("lat=\"");
    int lonPos = wptBlock.indexOf("lon=\"");
    if (latPos < 0 || lonPos < 0) continue;

    entry.latitude = wptBlock.substring(latPos + 5, wptBlock.indexOf("\"", latPos + 5)).toFloat();
    entry.longitude = wptBlock.substring(lonPos + 5, wptBlock.indexOf("\"", lonPos + 5)).toFloat();

    // Validate coordinates
    if (entry.latitude < -90 || entry.latitude > 90 ||
        entry.longitude < -180 || entry.longitude > 180) continue;

    // Extract <name> (GC code)
    extractXMLField(wptBlock, "<name>", "</name>", entry.gcCode, sizeof(entry.gcCode));

    // Extract display name - try groundspeak:name first, then desc
    if (!extractXMLField(wptBlock, "<groundspeak:name>", "</groundspeak:name>",
                         entry.name, sizeof(entry.name))) {
      extractXMLField(wptBlock, "<desc>", "</desc>", entry.name, sizeof(entry.name));
    }

    // If still no name, use GC code
    if (strlen(entry.name) == 0) {
      strncpy(entry.name, entry.gcCode, sizeof(entry.name) - 1);
    }

    // Extract difficulty/terrain (default 2.5 if not found)
    entry.difficulty = extractXMLFloat(wptBlock, "<groundspeak:difficulty>", "</groundspeak:difficulty>", 2.5);
    entry.terrain = extractXMLFloat(wptBlock, "<groundspeak:terrain>", "</groundspeak:terrain>", 2.5);

    // Extract hint (decode ROT13 if present)
    extractXMLField(wptBlock, "<groundspeak:encoded_hints>", "</groundspeak:encoded_hints>",
                    entry.hint, sizeof(entry.hint));
    decodeROT13(entry.hint);  // Geocaching.com encodes hints in ROT13

    entry.valid = true;
    entry.found = false;
    entry.foundTime = 0;

    cacheList[cacheListCount++] = entry;
  }

  logPrintf("[GEOCACHE] Parsed %d waypoints from GPX\n", cacheListCount);
  return cacheListCount;
}

// Load geocaches from saved GPX file on SD card
void loadGeocachesFromSD() {
  if (!sdAvailable) {
    logPrintln("[GEOCACHE] SD not available, skipping GPX load");
    return;
  }

  if (!SD.exists(GEOCACHE_GPX_FILE)) {
    logPrintln("[GEOCACHE] No saved GPX file found on SD");
    return;
  }

  File f = sdOpenSafe(GEOCACHE_GPX_FILE, "r", true);
  if (!f) {
    logPrintln("[GEOCACHE] Failed to open GPX file");
    return;
  }

  // Read file into string
  String gpxData;
  size_t fileSize = f.size();
  if (fileSize > GPX_MAX_FILE_SIZE) {
    logPrintf("[GEOCACHE] GPX file too large: %d bytes (max %d)\n", fileSize, GPX_MAX_FILE_SIZE);
    f.close();
    return;
  }

  gpxData.reserve(fileSize);
  while (f.available()) {
    gpxData += (char)f.read();
  }
  f.close();
  recordSDSuccess();

  // Parse the GPX
  int parsed = parseGPXFromString(gpxData);

  if (parsed > 0) {
    loadCacheFoundStatus();  // Apply saved found status
    logPrintf("[GEOCACHE] Loaded %d caches from SD\n", parsed);
  } else {
    logPrintln("[GEOCACHE] No valid waypoints in saved GPX");
  }
}

// ============== Weather Logging Functions ==============

// Get current timestamp from NTP or GPS
uint32_t getCurrentTimestamp() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t now;
    time(&now);
    return (uint32_t)now;
  }
  return 0;
}

// Get today's weather log filename
void getWeatherFilename(char* buf, int daysAgo) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t now;
    time(&now);
    now -= daysAgo * 86400;  // Subtract days
    struct tm* t = localtime(&now);
    sprintf(buf, "/weather/%04d-%02d-%02d.csv",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  } else {
    sprintf(buf, "/weather/unknown.csv");
  }
}

// Add reading to circular buffer
void addToWeatherHistory(WeatherReading reading) {
  weatherHistory[weatherHistoryHead] = reading;
  weatherHistoryHead = (weatherHistoryHead + 1) % WEATHER_SAMPLES_MAX;
  if (weatherHistoryCount < WEATHER_SAMPLES_MAX) {
    weatherHistoryCount++;
  }
}

// Get reading from N samples ago (0 = most recent)
WeatherReading* getWeatherReading(int samplesAgo) {
  if (samplesAgo >= weatherHistoryCount) return NULL;
  int index = (weatherHistoryHead - 1 - samplesAgo + WEATHER_SAMPLES_MAX) % WEATHER_SAMPLES_MAX;
  return &weatherHistory[index];
}

// Check if two locations are within threshold
bool sameLocation(float lat1, float lon1, float lat2, float lon2) {
  return (abs(lat1 - lat2) < LOCATION_THRESHOLD &&
          abs(lon1 - lon2) < LOCATION_THRESHOLD);
}

// Log current weather reading to SD card
// Write weather entry to FRAM ring buffer
void logWeatherToFRAM(WeatherReading& reading) {
  if (!framAvailable) return;

  // WeatherReading is 24 bytes, matches our FRAM entry layout
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

void logWeatherReading() {
  if ((!sdHealth.available && !framAvailable) || (!bmeAvailable && !shtAvailable)) return;

  uint32_t timestamp = getCurrentTimestamp();
  if (timestamp == 0) return;  // No valid time

  // Get current GPS position (use 0,0 if no fix)
  float lat = gpsData.valid ? gpsData.latitude : 0;
  float lon = gpsData.valid ? gpsData.longitude : 0;

  // Create reading
  WeatherReading reading;
  reading.timestamp = timestamp;
  reading.lat = lat;
  reading.lon = lon;
  reading.pressure = envData.pressure;
  // SHT41 temp/humidity preferred for weather log accuracy (#48)
  reading.temp = shtAvailable ? shtData.temperature : envData.temperature;
  reading.humidity = shtAvailable ? shtData.humidity : envData.humidity;

  // Add to in-memory buffer
  addToWeatherHistory(reading);

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
                  timestamp, lat, lon,
                  reading.pressure, reading.temp, reading.humidity);
      file.close();
      recordSDSuccess();
    }
  }
}

// Load weather history from SD card on boot
void loadWeatherHistory() {
  if (!sdHealth.available) return;

  logPrint("Loading weather history... ");

  int loaded = 0;
  uint32_t now = getCurrentTimestamp();
  uint32_t cutoff = now - (WEATHER_HISTORY_HOURS * 3600);

  // Load today's and yesterday's files
  for (int daysAgo = 1; daysAgo >= 0; daysAgo--) {
    char filename[32];
    getWeatherFilename(filename, daysAgo);

    if (!SD.exists(filename)) continue;

    File file = sdOpenSafe(filename, "r", true);  // silent fail
    if (!file) continue;

    char line[80];
    while (file.available()) {
      int len = file.readBytesUntil('\n', line, sizeof(line) - 1);
      line[len] = '\0';

      WeatherReading reading;
      if (sscanf(line, "%lu,%f,%f,%f,%f,%f",
                 &reading.timestamp, &reading.lat, &reading.lon,
                 &reading.pressure, &reading.temp, &reading.humidity) == 6) {
        // Only load readings within history window
        if (reading.timestamp >= cutoff) {
          addToWeatherHistory(reading);
          loaded++;
        }
      }
    }
    file.close();
    recordSDSuccess();
  }

  logPrintf("%d readings\n", loaded);
}

// Calculate weather trend from history
void calculateWeatherTrend() {
  // Need at least some history
  if (weatherHistoryCount < 2) {
    weatherTrend.forecast = "Init";
    return;
  }

  WeatherReading* current = getWeatherReading(0);
  if (!current) return;

  // Check for location change
  WeatherReading* prev = getWeatherReading(1);
  if (prev && (prev->lat != 0 || prev->lon != 0) && (current->lat != 0 || current->lon != 0)) {
    weatherTrend.locationChanged = !sameLocation(current->lat, current->lon, prev->lat, prev->lon);
  } else {
    weatherTrend.locationChanged = false;
  }

  // Find reading from ~3 hours ago (36 samples at 5-min intervals)
  int samples3hr = 36;
  if (samples3hr > weatherHistoryCount - 1) {
    samples3hr = weatherHistoryCount - 1;
  }

  WeatherReading* reading3hr = getWeatherReading(samples3hr);
  if (!reading3hr) {
    weatherTrend.forecast = "Learning";
    return;
  }

  // Calculate changes
  weatherTrend.pressureChange3hr = current->pressure - reading3hr->pressure;
  weatherTrend.tempChange3hr = current->temp - reading3hr->temp;
  weatherTrend.humidityChange3hr = current->humidity - reading3hr->humidity;

  // Determine trend direction
  float pChange = weatherTrend.pressureChange3hr;
  if (pChange > 3.0) {
    weatherTrend.trend = 2;  // Rising fast
  } else if (pChange > 1.0) {
    weatherTrend.trend = 1;  // Rising slow
  } else if (pChange < -3.0) {
    weatherTrend.trend = 4;  // Falling fast
  } else if (pChange < -1.0) {
    weatherTrend.trend = 3;  // Falling slow
  } else {
    weatherTrend.trend = 0;  // Stable
  }

  // Calculate forecast
  weatherTrend.forecast = calculateForecast();
}

// Get trend arrow character
const char* getTrendArrow() {
  switch (weatherTrend.trend) {
    case 1: return "^";    // Rising slow
    case 2: return "^^";   // Rising fast
    case 3: return "v";    // Falling slow
    case 4: return "vv";   // Falling fast
    default: return "-";   // Stable
  }
}

// Calculate weather forecast based on conditions
const char* calculateForecast() {
  float p = envData.pressure;
  // SHT41 preferred for forecast accuracy (#48)
  float t = shtAvailable ? shtData.temperature : envData.temperature;
  float h = shtAvailable ? shtData.humidity : envData.humidity;
  float pChange = weatherTrend.pressureChange3hr;

  // Location changed - can't predict
  if (weatherTrend.locationChanged) {
    return "Traveled";
  }

  // Not enough history (need 3 hours)
  if (weatherHistoryCount < 36) {
    return "Learning";
  }

  bool lowPressure = (p < 1000);
  bool highPressure = (p > 1015);
  bool highHumidity = (h > 70);
  bool fallingFast = (pChange < -3.0);
  bool fallingSlow = (pChange < -1.0 && pChange >= -3.0);
  bool risingFast = (pChange > 3.0);
  bool risingSlow = (pChange > 1.0 && pChange <= 3.0);
  bool cold = (t < 5.0);  // Below 5°C

  // Storm conditions
  if (lowPressure && fallingFast && highHumidity) {
    return "Storm Likely";
  }

  // Precipitation
  if (lowPressure && (fallingFast || fallingSlow) && highHumidity) {
    if (cold) return "Snow Likely";
    return "Rain Likely";
  }

  // Clearing
  if (highPressure && (risingFast || risingSlow)) {
    return "Clearing";
  }

  // Fair weather
  if (highPressure && fabs(pChange) < 1.0) {
    return "Fair";
  }

  // Unsettled
  if (lowPressure && fabs(pChange) < 1.0 && highHumidity) {
    return "Unsettled";
  }

  // Possible change
  if (fallingSlow && h > 60) {
    return "Precip Poss";
  }

  return "Stable";
}

void initWiFi() {
  logPrint("Connecting to WiFi");

  // Try networks in order
  const char* ssids[] = {WIFI_SSID_1, WIFI_SSID_2, WIFI_SSID_3};
  const char* passwords[] = {WIFI_PASS_1, WIFI_PASS_2, WIFI_PASS_3};

  for (int net = 0; net < 3; net++) {
    logPrintf(" [%s]", ssids[net]);

    WiFi.begin(ssids[net], passwords[net]);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 15) {
      delay(500);
      logPrint(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) break;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    logPrintln(" OK");
    logPrintf("  IP: %s\n", WiFi.localIP().toString().c_str());

    // Sync NTP time
    logPrint("Syncing NTP time... ");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      ntpSynced = true;
      logPrintln("OK");

      // Sync RTC from NTP (if GPS hasn't already synced it)
      if (!rtcSyncedFromGPS && !rtcSyncedFromNTP) {
        syncRTCFromSystemTime("NTP");
        rtcSyncedFromNTP = true;
      }
    } else {
      logPrintln("FAILED");
    }
  } else {
    logPrintln(" FAILED");
  }

  lastWiFiAttempt = millis();
}

void checkWiFi() {
  // Update connection status
  wifiConnected = (WiFi.status() == WL_CONNECTED);

  // Attempt reconnect if disconnected
  if (!wifiConnected && (millis() - lastWiFiAttempt > WIFI_RECONNECT_INTERVAL)) {
    logPrintln("WiFi disconnected, attempting reconnect...");
    WiFi.reconnect();
    lastWiFiAttempt = millis();

    // Wait briefly for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      attempts++;
    }

    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
      logPrintln("WiFi reconnected!");
      // Start web server if not already running
      if (!webServerStarted) {
        initWebServer();
      }
    }
  }

  // Ensure web server is started if WiFi is connected
  if (wifiConnected && !webServerStarted) {
    initWebServer();
  }
}

// ============== Web Server Handlers ==============

void handleWebRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Field Compass</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta name='color-scheme' content='dark light'>";
  html += "<style>";
  html += "body{font-family:sans-serif;margin:20px;background:#1a1a1a;color:#e0e0e0;}";
  html += "h1{color:#00ffff;}";
  html += "a{color:#00ff00;display:block;padding:12px 15px;margin:8px 0;";
  html += "text-decoration:none;background:#2a2a2a;border-radius:5px;border:1px solid #444;}";
  html += "a:hover{background:#3a3a3a;border-color:#00ff00;}";
  html += "</style></head><body>";
  html += "<h1>Field Compass</h1>";
  html += "<a href='/ops'>Operational Info</a>";
  html += "<a href='/gps'>GPS</a>";
  html += "<a href='/env'>Environment</a>";
  html += "<a href='/imu'>IMU / Compass</a>";
  html += "<a href='/diags'>Diagnostics</a>";
  html += "<a href='/geocaches'>Geocache Manager</a>";
  html += "<a href='/serial'>Serial Monitor</a>";
  html += "<a href='/logs'>Serial Logs (SD)</a>";
  html += "<a href='/json'>JSON API</a>";
  html += "<p style='color:#666;margin-top:20px;font-size:12px;'>http://fieldcompass.local/</p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebOps() {
  String html = "<!DOCTYPE html><html><head><title>OPS</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>OPERATIONAL</h2><pre>";

  // Time
  char buf[64];
  if (gpsData.timeValid) {
    int hour = gpsData.hour + (GMT_OFFSET_SEC / 3600);
    if (hour < 0) hour += 24;
    if (hour >= 24) hour -= 24;
    sprintf(buf, "Time:    %02d:%02d:%02d GPS\n", hour, gpsData.minute, gpsData.second);
  } else if (ntpSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      strftime(buf, sizeof(buf), "Time:    %H:%M:%S NTP\n", &timeinfo);
    }
  } else {
    sprintf(buf, "Time:    --:--:-- N/A\n");
  }
  html += buf;

  // Uptime
  unsigned long totalSec = millis() / 1000;
  int days = totalSec / 86400;
  int hours = (totalSec % 86400) / 3600;
  int mins = (totalSec % 3600) / 60;
  int secs = totalSec % 60;
  if (days > 0) {
    sprintf(buf, "Uptime:  %dd %02d:%02d:%02d\n", days, hours, mins, secs);
  } else {
    sprintf(buf, "Uptime:  %02d:%02d:%02d\n", hours, mins, secs);
  }
  html += buf;

  // WiFi
  if (wifiConnected) {
    html += "WiFi:    " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")\n";
  } else {
    html += "WiFi:    Disconnected\n";
  }

  // Battery
  if (batteryAvailable && isBatteryConnected()) {
    sprintf(buf, "Battery: %.0f%% (%.2fV)\n", battery.cellPercent(), battery.cellVoltage());
    html += buf;
  } else if (batteryAvailable) {
    html += "Battery: USB Only\n";
  } else {
    html += "Battery: N/A\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebGPS() {
  String html = "<!DOCTYPE html><html><head><title>GPS</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>GPS</h2><pre>";

  char buf[64];
  if (gpsData.valid) {
    sprintf(buf, "Latitude:  %.6f %c\n", fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    html += buf;
    sprintf(buf, "Longitude: %.6f %c\n", fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    html += buf;
    sprintf(buf, "Altitude:  %.1f m\n", gpsData.altitude);
    html += buf;
    html += "Status:    Fix OK\n";
  } else if (gpsData.receiving) {
    html += "Status: Acquiring fix...\n";
    if (gpsData.timeValid) {
      int hour = gpsData.hour + (GMT_OFFSET_SEC / 3600);
      if (hour < 0) hour += 24;
      if (hour >= 24) hour -= 24;
      sprintf(buf, "Time:   %02d:%02d:%02d\n", hour, gpsData.minute, gpsData.second);
      html += buf;
    }
  } else {
    html += "Status: No GPS data\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebEnv() {
  String html = "<!DOCTYPE html><html><head><title>ENV</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>ENVIRONMENT</h2><pre>";

  char buf[80];
  if (bmeAvailable || shtAvailable) {
    // SHT41 preferred for temp/humidity (#48)
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    float humid = shtAvailable ? shtData.humidity : envData.humidity;
    const char* src = shtAvailable ? "SHT41" : "BME688";
    sprintf(buf, "Temp:     %.1fF (%.1fC) [%s]\n", tempF, tempC, src);
    html += buf;
    sprintf(buf, "Humidity: %.1f%% [%s]\n", humid, src);
    html += buf;
    if (bmeAvailable) {
      sprintf(buf, "IAQ:      %.0f [%s]\n", envData.iaq, getIaqAccuracyText(envData.iaqAccuracy));
      html += buf;
      sprintf(buf, "CO2:      %.0f ppm\n", envData.co2Equivalent);
      html += buf;
      sprintf(buf, "Pressure: %.1f hPa (%.2f\")\n", envData.pressure, hPaToInHg(envData.pressure));
      html += buf;
      sprintf(buf, "Forecast: %s %s\n", getTrendArrow(), weatherTrend.forecast);
      html += buf;
    }
  } else {
    html += "BME688 not available\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebIMU() {
  String html = "<!DOCTYPE html><html><head><title>IMU</title>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>IMU / COMPASS</h2><pre>";

  char buf[64];
  if (imuAvailable && magAvailable) {
    sprintf(buf, "Heading: %.0f %s\n", imuData.heading, getCardinal(imuData.heading));
    html += buf;
    sprintf(buf, "Roll:    %.0f deg\n", imuData.roll);
    html += buf;
    sprintf(buf, "Pitch:   %.0f deg\n", imuData.pitch);
    html += buf;
    sprintf(buf, "Accel:   %.2f m/s2\n", imuData.accelMag);
    html += buf;
  } else {
    html += "IMU not available\n";
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebDiags() {
  String html = "<!DOCTYPE html><html><head><title>DIAGS</title>";
  html += "<meta http-equiv='refresh' content='10'>";
  html += "<style>body{font-family:monospace;background:#1a1a1a;color:#0f0;padding:20px;}</style></head><body>";
  html += "<h2>DIAGNOSTICS</h2><pre>";

  char buf[80];

  // BSEC State
  html += "=== BSEC State ===\n";
  sprintf(buf, "Loaded:   %s\n", bsecStateLoaded ? "Yes" : "No");
  html += buf;
  sprintf(buf, "Saved:    %s\n", bsecStateSaved ? "Yes" : "No");
  html += buf;
  sprintf(buf, "Accuracy: %d (%s)\n", envData.iaqAccuracy, getIaqAccuracyText(envData.iaqAccuracy));
  html += buf;

  // Weather Log
  html += "\n=== Weather Log ===\n";
  sprintf(buf, "In Memory: %d readings\n", weatherHistoryCount);
  html += buf;
  sprintf(buf, "Files:     %d\n", weatherLogFileCount);
  html += buf;
  sprintf(buf, "Total:     %d entries\n", weatherLogEntryCount);
  html += buf;

  // System
  html += "\n=== System ===\n";
  sprintf(buf, "Free Heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  html += buf;
  sprintf(buf, "Heap Size: %lu bytes\n", (unsigned long)ESP.getHeapSize());
  html += buf;
  sprintf(buf, "CPU Freq:  %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
  html += buf;

  // Sensors
  html += "\n=== Sensors ===\n";
  sprintf(buf, "BME688:  %s\n", bmeAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "SHT41:   %s\n", shtAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "IMU:     %s\n", imuAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "Mag:     %s\n", magAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "Battery: %s\n", batteryAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "SD Card: %s\n", sdAvailable ? "OK" : "N/A");
  html += buf;
  sprintf(buf, "OLED:    %s\n", oledAvailable ? "OK" : "N/A");
  html += buf;
  if (framAvailable) {
    sprintf(buf, "FRAM:    OK (256KB) Batt:%d/%d Wx:%d/%d %s\n",
            framHeader.battCount, FRAM_BATT_COUNT,
            framHeader.wxCount, FRAM_WX_COUNT,
            (framHeader.flags & 0x01) ? "DIRTY" : "Clean");
  } else {
    sprintf(buf, "FRAM:    N/A\n");
  }
  html += buf;

  // Temp comparison SHT41 vs BME688 (#48)
  html += "\n=== Temperature Comparison ===\n";
  if (shtAvailable) {
    float shtF = shtData.temperature * 9.0 / 5.0 + 32.0;
    sprintf(buf, "SHT41:   %.1fF (%.1fC)\n", shtF, shtData.temperature);
    html += buf;
    sprintf(buf, "SHT41 H: %.1f%%\n", shtData.humidity);
    html += buf;
  } else {
    html += "SHT41:   N/A\n";
  }
  if (bmeAvailable) {
    float bmeF = envData.temperature * 9.0 / 5.0 + 32.0;
    sprintf(buf, "BME688:  %.1fF (%.1fC)\n", bmeF, envData.temperature);
    html += buf;
    sprintf(buf, "BME688 H: %.1f%%\n", envData.humidity);
    html += buf;
  } else {
    html += "BME688:  N/A\n";
  }
  if (shtAvailable && bmeAvailable) {
    float deltaF = (shtData.temperature - envData.temperature) * 9.0 / 5.0;
    sprintf(buf, "Delta:   %+.1fF (SHT - BME)\n", deltaF);
    html += buf;
  }

  // GPS (#68)
  html += "\n=== GPS ===\n";

  // Status: Fix OK / Acquiring / No Data
  const char* gpsStatus;
  if (gpsData.valid) {
    gpsStatus = "Fix OK";
  } else if (gpsData.receiving) {
    gpsStatus = "Acquiring";
  } else {
    gpsStatus = "No Data";
  }
  sprintf(buf, "Status:   %s\n", gpsStatus);
  html += buf;

  // TTFF - retained once acquired
  if (gpsHadFirstFix) {
    sprintf(buf, "TTFF:     %lu seconds\n", gpsFirstFixTime / 1000);
    html += buf;
  } else {
    html += "TTFF:     (not yet acquired)\n";
  }

  // Acquiring/Reacquire time - show if not currently valid
  if (!gpsData.valid && gpsHadFirstReceive) {
    if (gpsHadFirstFix && gpsSignalLostTime > 0) {
      // Lost signal after having fix - show time since signal lost
      unsigned long reacquiring = (millis() - gpsSignalLostTime) / 1000;
      sprintf(buf, "Reacquire: %lum %lus\n", reacquiring / 60, reacquiring % 60);
    } else {
      // Never had fix, show time since first NMEA data
      unsigned long elapsed = (millis() - gpsFirstReceiveTime) / 1000;
      sprintf(buf, "Elapsed:  %lum %lus\n", elapsed / 60, elapsed % 60);
    }
    html += buf;
  }

  html += "</pre><a href='/'>Back</a></body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebSerial() {
  String html = "<!DOCTYPE html><html><head><title>Serial Log</title>";
  html += "<meta name='color-scheme' content='dark light'>";
  html += "<style>";
  // Default (dark theme) styles
  html += ":root{--bg:#1a1a1a;--fg:#e0e0e0;--log-bg:#000;--log-fg:#00ff00;--btn-bg:#2a2a2a;--btn-fg:#00ff00;--btn-border:#444;--link:#00ffff;--header:#00ffff;}";
  // Light theme override
  html += "@media(prefers-color-scheme:light){:root{--bg:#f5f5f5;--fg:#222;--log-bg:#222;--log-fg:#00cc00;--btn-bg:#ddd;--btn-fg:#006600;--btn-border:#999;--link:#006666;--header:#008888;}}";
  html += "body{font-family:sans-serif;margin:0;padding:10px;background:var(--bg);color:var(--fg);}";
  html += ".log{background:var(--log-bg);color:var(--log-fg);font-family:monospace;padding:10px;font-size:12px;";
  html += "height:500px;overflow-y:auto;border-radius:8px;white-space:pre-wrap;}";
  html += ".controls{margin:10px 0;}";
  html += "button{padding:8px 16px;margin-right:8px;background:var(--btn-bg);color:var(--btn-fg);";
  html += "border:1px solid var(--btn-border);border-radius:4px;cursor:pointer;}";
  html += "button:hover{opacity:0.8;}";
  html += "a{color:var(--link);}";
  html += "</style></head><body>";
  html += "<h2 style='color:var(--header);margin:0 0 10px 0;'>Serial Log</h2>";
  html += "<div class='log' id='log'></div>";
  html += "<div class='controls'>";
  html += "<button onclick='copyLog()'>Copy</button>";
  html += "<button onclick='clearLog()'>Clear</button>";
  html += "<a href='/'>Back</a>";
  html += "</div>";
  html += "<script>";
  html += "const log=document.getElementById('log');";
  html += "async function poll(){";
  html += "try{";
  html += "const r=await fetch('/serial-data');";
  html += "const t=await r.text();";
  html += "if(t.length>0){";
  html += "log.textContent+=t;";
  html += "log.scrollTop=log.scrollHeight;";
  html += "}";
  html += "}catch(e){}";
  html += "}";
  html += "function clearLog(){log.textContent='';}";
  html += "function copyLog(){";
  html += "var ta=document.createElement('textarea');";
  html += "ta.value=log.textContent;";
  html += "ta.style.position='fixed';";
  html += "ta.style.left='-9999px';";
  html += "document.body.appendChild(ta);";
  html += "ta.select();";
  html += "try{document.execCommand('copy');alert('Copied!');}catch(e){alert('Copy failed');}";
  html += "document.body.removeChild(ta);";
  html += "}";
  html += "setInterval(poll,100);";
  html += "</script>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebSerialData() {
  // Return buffered serial data since last request (incremental)
  String out;
  out.reserve(1024);

  uint16_t head = serialRingHead;
  uint16_t tail = serialRingTail;

  // If our read pos is behind tail, we lost data - jump to tail
  uint16_t dist = (head >= webSerialReadPos) ?
                  (head - webSerialReadPos) :
                  (SERIAL_RING_SIZE - webSerialReadPos + head);
  if (dist > SERIAL_RING_SIZE - 100) {
    webSerialReadPos = tail;
  }

  // Read available data
  while (webSerialReadPos != head && out.length() < 1024) {
    char c = serialRing[webSerialReadPos];
    webSerialReadPos = (webSerialReadPos + 1) % SERIAL_RING_SIZE;
    if (c != '\r') {
      out += c;
    }
  }

  webServer.send(200, "text/plain", out);
}

// GET /logs — List serial log files with download links (#59)
void handleWebLogs() {
  String html = "<!DOCTYPE html><html><head><title>Serial Logs</title>";
  html += "<meta name='color-scheme' content='dark light'>";
  html += "<style>";
  html += ":root{--bg:#1a1a1a;--fg:#e0e0e0;--link:#00ffff;--header:#00ffff;--tbl-border:#444;--tbl-alt:#222;}";
  html += "@media(prefers-color-scheme:light){:root{--bg:#f5f5f5;--fg:#222;--link:#006666;--header:#008888;--tbl-border:#ccc;--tbl-alt:#eee;}}";
  html += "body{font-family:sans-serif;margin:0;padding:10px;background:var(--bg);color:var(--fg);}";
  html += "table{border-collapse:collapse;width:100%;margin:10px 0;}";
  html += "th,td{padding:6px 12px;text-align:left;border-bottom:1px solid var(--tbl-border);}";
  html += "tr:nth-child(even){background:var(--tbl-alt);}";
  html += "a{color:var(--link);}";
  html += "</style></head><body>";
  html += "<h2 style='color:var(--header);margin:0 0 10px 0;'>Serial Logs</h2>";

  if (!sdAvailable || !SD.exists(LOG_DIR)) {
    html += "<p>No log files available.</p>";
  } else {
    // Flush current buffer before listing
    serialLogFlush();

    html += "<table><tr><th>File</th><th>Size</th><th>Action</th></tr>";

    File dir = SD.open(LOG_DIR);
    if (dir && dir.isDirectory()) {
      File entry = dir.openNextFile();
      while (entry) {
        if (!entry.isDirectory()) {
          const char* name = entry.name();
          size_t sz = entry.size();
          html += "<tr><td>";
          html += name;
          html += "</td><td>";
          if (sz >= 1024) {
            html += String(sz / 1024.0, 1) + " KB";
          } else {
            html += String((unsigned long)sz) + " B";
          }
          html += "</td><td><a href='/logs/download?file=";
          html += name;
          html += "'>Download</a></td></tr>";
        }
        entry.close();
        entry = dir.openNextFile();
      }
      dir.close();
    }
    html += "</table>";
  }

  if (serialLogActive) {
    html += "<p>Currently logging to: ";
    html += serialLogFilename;
    html += "</p>";
  }
  html += "<p><a href='/'>Back</a></p></body></html>";
  webServer.send(200, "text/html", html);
}

// GET /logs/download?file=<filename> — Download a serial log file (#59)
void handleWebLogDownload() {
  if (!webServer.hasArg("file")) {
    webServer.send(400, "text/plain", "Missing file parameter");
    return;
  }

  String filename = webServer.arg("file");
  // Security: prevent path traversal — only allow alphanumeric, underscore, dot, hyphen
  for (unsigned int i = 0; i < filename.length(); i++) {
    char c = filename[i];
    if (!isalnum(c) && c != '_' && c != '.' && c != '-') {
      webServer.send(400, "text/plain", "Invalid filename");
      return;
    }
  }

  // Flush current buffer if this is the active log file
  if (serialLogActive) {
    String activeName = String(serialLogFilename).substring(strlen(LOG_DIR) + 1);
    if (filename.equals(activeName)) {
      serialLogFlush();
    }
  }

  String fullPath = String(LOG_DIR) + "/" + filename;
  if (!SD.exists(fullPath)) {
    webServer.send(404, "text/plain", "File not found");
    return;
  }

  File f = SD.open(fullPath, "r");
  if (!f) {
    webServer.send(500, "text/plain", "Failed to open file");
    return;
  }

  String disposition = "attachment; filename=" + filename;
  webServer.sendHeader("Content-Disposition", disposition);
  webServer.streamFile(f, "text/plain");
  f.close();
}

void handleWebJSON() {
  char buf[1024];
  bool battConnected = batteryAvailable && isBatteryConnected();
  // SHT41 preferred for temp/humidity in JSON API (#48)
  float jsonTempC = shtAvailable ? shtData.temperature : envData.temperature;
  float jsonHumid = shtAvailable ? shtData.humidity : envData.humidity;
  snprintf(buf, sizeof(buf),
    "{"
    "\"gps\":{\"valid\":%s,\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f},"
    "\"env\":{\"temp\":%.1f,\"humidity\":%.1f,\"pressure\":%.1f,\"iaq\":%.0f,\"co2\":%.0f,\"accuracy\":%d,\"tempSource\":\"%s\"},"
    "\"imu\":{\"heading\":%.1f,\"roll\":%.1f,\"pitch\":%.1f,\"accel\":%.2f},"
    "\"system\":{\"uptime\":%lu,\"wifi\":%s,\"battery\":%.1f,\"batteryConnected\":%s,\"heap\":%lu}"
    "}",
    gpsData.valid ? "true" : "false", gpsData.latitude, gpsData.longitude, gpsData.altitude,
    jsonTempC, jsonHumid, envData.pressure, envData.iaq, envData.co2Equivalent, envData.iaqAccuracy,
    shtAvailable ? "SHT41" : "BME688",
    imuData.heading, imuData.roll, imuData.pitch, imuData.accelMag,
    millis() / 1000, wifiConnected ? "true" : "false",
    battConnected ? battery.cellPercent() : -1.0,
    battConnected ? "true" : "false",
    (unsigned long)ESP.getFreeHeap()
  );
  webServer.send(200, "application/json", buf);
}

// Battery log download endpoint
void handleWebBattLog() {
  if (!sdHealth.available) {
    webServer.send(404, "text/plain", "SD card not available");
    return;
  }

  File f = sdOpenSafe(BATT_LOG_FILE, "r", true);
  if (!f) {
    webServer.send(404, "text/plain", "Battery log file not found. Wait for data collection.");
    return;
  }

  // Stream the file with CSV content type for easy download
  webServer.sendHeader("Content-Disposition", "attachment; filename=battlog.csv");
  webServer.streamFile(f, "text/csv");
  f.close();
}

// Battery log clear endpoint
void handleWebBattLogClear() {
  if (!sdHealth.available) {
    webServer.send(404, "text/plain", "SD card not available");
    return;
  }

  if (SD.exists(BATT_LOG_FILE)) {
    SD.remove(BATT_LOG_FILE);
    webServer.send(200, "text/plain", "Battery log cleared. New log will start on next cycle.");
  } else {
    webServer.send(200, "text/plain", "Battery log already empty.");
  }
}

// ============== Geocache Web Handlers (#70) ==============

// GET /geocaches - Main geocache manager page
void handleWebGeocaches() {
  String html = "<!DOCTYPE html><html><head><title>Geocaches</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:sans-serif;margin:20px;background:#1a1a1a;color:#e0e0e0;}";
  html += "h1,h2{color:#00ffff;}";
  html += "form{background:#2a2a2a;padding:20px;border-radius:8px;margin:20px 0;}";
  html += "input[type=file]{margin:10px 0;color:#e0e0e0;}";
  html += "button,.btn{background:#00aa00;color:white;padding:10px 20px;border:none;";
  html += "border-radius:5px;cursor:pointer;font-size:14px;text-decoration:none;display:inline-block;margin:2px;}";
  html += "button:hover,.btn:hover{background:#00cc00;}";
  html += ".btn-warn{background:#aa6600;}.btn-warn:hover{background:#cc8800;}";
  html += ".btn-danger{background:#aa0000;}.btn-danger:hover{background:#cc0000;}";
  html += ".btn-small{padding:5px 10px;font-size:12px;}";
  html += ".cache{background:#2a2a2a;padding:12px;margin:8px 0;border-radius:5px;display:flex;justify-content:space-between;align-items:center;}";
  html += ".cache-info{flex-grow:1;}";
  html += ".cache-actions{white-space:nowrap;}";
  html += ".found{border-left:4px solid #00ff00;}";
  html += ".notfound{border-left:4px solid #666;}";
  html += ".stats{color:#888;font-size:12px;}";
  html += "a{color:#00ff00;}";
  html += ".msg{padding:15px;border-radius:5px;margin:15px 0;}";
  html += ".msg-ok{background:#004400;border:1px solid #00aa00;}";
  html += ".msg-err{background:#440000;border:1px solid #aa0000;}";
  html += "</style></head><body>";

  html += "<h1>Geocache Manager</h1>";

  // Show status message if present
  if (webServer.hasArg("msg")) {
    String msg = webServer.arg("msg");
    bool isError = webServer.hasArg("err");
    html += "<div class='msg " + String(isError ? "msg-err" : "msg-ok") + "'>" + msg + "</div>";
  }

  // Upload form
  html += "<h2>Upload GPX File</h2>";
  html += "<form method='POST' action='/geocaches/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='gpxfile' accept='.gpx,.xml'><br>";
  html += "<button type='submit'>Upload GPX</button>";
  html += "</form>";
  html += "<p class='stats'>Max file size: 64KB | Max caches: " + String(MAX_CACHES) + "</p>";

  // Action buttons
  html += "<div style='margin:15px 0;'>";
  html += "<a href='/geocaches/download' class='btn btn-small'>Download Found Status</a> ";
  html += "<a href='/geocaches/clear?confirm=yes' class='btn btn-small btn-danger' onclick=\"return confirm('Clear all caches?')\">Clear All</a>";
  html += "</div>";

  // Current cache list
  html += "<h2>Loaded Caches (" + String(cacheListCount) + ")</h2>";

  if (cacheListCount == 0) {
    html += "<p>No caches loaded. Upload a GPX file to get started.</p>";
  } else {
    for (int i = 0; i < cacheListCount; i++) {
      GeocacheEntry& c = cacheList[i];
      if (!c.valid) continue;

      html += "<div class='cache " + String(c.found ? "found" : "notfound") + "'>";
      html += "<div class='cache-info'>";
      html += "<strong>" + String(c.gcCode) + "</strong>: " + String(c.name);
      html += "<br><span class='stats'>";
      html += "D" + String(c.difficulty, 1) + "/T" + String(c.terrain, 1);
      html += " | " + String(c.latitude, 5) + ", " + String(c.longitude, 5);
      if (c.found) html += " | <span style='color:#0f0'>FOUND</span>";
      html += "</span></div>";
      html += "<div class='cache-actions'>";
      html += "<a href='/geocaches/togglefound?idx=" + String(i) + "' class='btn btn-small " + String(c.found ? "btn-warn" : "") + "'>" + String(c.found ? "Unfound" : "Found") + "</a> ";
      html += "<a href='/geocaches/delete?idx=" + String(i) + "' class='btn btn-small btn-danger' onclick=\"return confirm('Delete this cache?')\">Del</a>";
      html += "</div></div>";
    }
  }

  html += "<br><a href='/'>Back to Home</a>";
  html += "</body></html>";

  webServer.send(200, "text/html", html);
}

// POST upload data handler - receives file chunks
void handleGeocacheUploadData() {
  HTTPUpload& upload = webServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    gpxUploadBuffer = "";
    gpxUploadSuccess = false;
    gpxUploadError = "";
    logPrintf("[GEOCACHE] Upload started: %s\n", upload.filename.c_str());

    // Validate file extension
    String filename = upload.filename;
    filename.toLowerCase();
    if (!filename.endsWith(".gpx") && !filename.endsWith(".xml")) {
      gpxUploadError = "Invalid file type. Please upload a .gpx file.";
      return;
    }
  }

  else if (upload.status == UPLOAD_FILE_WRITE) {
    // Check size limit
    if (gpxUploadBuffer.length() + upload.currentSize > GPX_MAX_FILE_SIZE) {
      gpxUploadError = "File too large. Maximum size is 64KB.";
      return;
    }

    // Append chunk to buffer
    for (size_t i = 0; i < upload.currentSize; i++) {
      gpxUploadBuffer += (char)upload.buf[i];
    }
  }

  else if (upload.status == UPLOAD_FILE_END) {
    logPrintf("[GEOCACHE] Upload complete: %d bytes\n", gpxUploadBuffer.length());

    if (gpxUploadError.length() > 0) {
      return;  // Already have an error
    }

    // Parse GPX data
    int parsed = parseGPXFromString(gpxUploadBuffer);

    if (parsed <= 0) {
      gpxUploadError = "Failed to parse GPX file. No valid waypoints found.";
      return;
    }

    // Save GPX to SD card
    if (sdAvailable) {
      // Ensure directory exists
      if (!SD.exists(GEOCACHE_DIR)) {
        SD.mkdir(GEOCACHE_DIR);
      }

      File f = sdOpenSafe(GEOCACHE_GPX_FILE, "w");
      if (f) {
        f.print(gpxUploadBuffer);
        f.close();
        recordSDSuccess();
        logPrintf("[GEOCACHE] Saved GPX to SD: %d bytes\n", gpxUploadBuffer.length());
      }
    }

    // Load any saved found status
    loadCacheFoundStatus();

    gpxUploadSuccess = true;
    logPrintf("[GEOCACHE] Loaded %d caches from upload\n", parsed);
  }

  else if (upload.status == UPLOAD_FILE_ABORTED) {
    gpxUploadError = "Upload aborted.";
    gpxUploadBuffer = "";
  }
}

// POST upload complete - send response
void handleGeocacheUploadComplete() {
  // Clear upload buffer to free RAM
  gpxUploadBuffer = "";

  String msg;
  if (gpxUploadSuccess) {
    msg = "Loaded " + String(cacheListCount) + " geocaches successfully!";
    webServer.sendHeader("Location", "/geocaches?msg=" + msg);
  } else {
    msg = gpxUploadError.length() > 0 ? gpxUploadError : "Upload failed";
    webServer.sendHeader("Location", "/geocaches?msg=" + msg + "&err=1");
  }
  webServer.send(302, "text/plain", "Redirecting...");
}

// GET /geocaches/download - Download found status CSV
void handleGeocacheDownload() {
  if (!sdAvailable || !SD.exists(GEOCACHE_FOUND_FILE)) {
    webServer.send(404, "text/plain", "No found status file available");
    return;
  }

  File f = sdOpenSafe(GEOCACHE_FOUND_FILE, "r");
  if (!f) {
    webServer.send(500, "text/plain", "Failed to open file");
    return;
  }

  webServer.sendHeader("Content-Disposition", "attachment; filename=geocache_found.csv");
  webServer.streamFile(f, "text/csv");
  f.close();
  recordSDSuccess();
}

// GET /geocaches/delete?idx=N - Delete single cache
void handleGeocacheDelete() {
  if (!webServer.hasArg("idx")) {
    webServer.sendHeader("Location", "/geocaches?msg=Missing+index&err=1");
    webServer.send(302);
    return;
  }

  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= cacheListCount) {
    webServer.sendHeader("Location", "/geocaches?msg=Invalid+index&err=1");
    webServer.send(302);
    return;
  }

  String deletedName = String(cacheList[idx].gcCode);

  // Shift remaining caches
  for (int i = idx; i < cacheListCount - 1; i++) {
    cacheList[i] = cacheList[i + 1];
  }
  cacheListCount--;

  // Update selected index if needed
  if (selectedCacheIndex >= cacheListCount) {
    selectedCacheIndex = max(0, cacheListCount - 1);
  }

  // Re-save GPX file (regenerate from memory)
  // For simplicity, we'll just mark the change happened - a full GPX rewrite would be complex
  logPrintf("[GEOCACHE] Deleted cache %s, %d remaining\n", deletedName.c_str(), cacheListCount);

  webServer.sendHeader("Location", "/geocaches?msg=Deleted+" + deletedName);
  webServer.send(302);
}

// GET /geocaches/clear?confirm=yes - Clear all caches
void handleGeocacheClear() {
  if (!webServer.hasArg("confirm") || webServer.arg("confirm") != "yes") {
    webServer.sendHeader("Location", "/geocaches?msg=Clear+requires+confirm=yes&err=1");
    webServer.send(302);
    return;
  }

  int oldCount = cacheListCount;

  // Clear memory
  cacheListCount = 0;
  selectedCacheIndex = 0;
  memset(cacheList, 0, sizeof(cacheList));

  // Delete GPX file from SD (but keep found status)
  if (sdAvailable && SD.exists(GEOCACHE_GPX_FILE)) {
    SD.remove(GEOCACHE_GPX_FILE);
    recordSDSuccess();
  }

  logPrintf("[GEOCACHE] Cleared all %d caches\n", oldCount);
  webServer.sendHeader("Location", "/geocaches?msg=Cleared+" + String(oldCount) + "+caches");
  webServer.send(302);
}

// GET /geocaches/togglefound?idx=N - Toggle found status
void handleGeocacheToggleFound() {
  if (!webServer.hasArg("idx")) {
    webServer.sendHeader("Location", "/geocaches?msg=Missing+index&err=1");
    webServer.send(302);
    return;
  }

  int idx = webServer.arg("idx").toInt();
  if (idx < 0 || idx >= cacheListCount) {
    webServer.sendHeader("Location", "/geocaches?msg=Invalid+index&err=1");
    webServer.send(302);
    return;
  }

  cacheList[idx].found = !cacheList[idx].found;
  if (cacheList[idx].found) {
    cacheList[idx].foundTime = millis() / 1000;  // Simple timestamp
  } else {
    cacheList[idx].foundTime = 0;
  }

  // Save updated found status
  saveCacheFoundStatus();

  String status = cacheList[idx].found ? "found" : "not+found";
  webServer.sendHeader("Location", "/geocaches?msg=" + String(cacheList[idx].gcCode) + "+marked+" + status);
  webServer.send(302);
}

void initWebServer() {
  if (!wifiConnected) return;
  if (webServerStarted) return;  // Already started

  Serial.print("Starting web server... ");

  // Setup mDNS
  if (MDNS.begin("fieldcompass")) {
    Serial.print("mDNS OK (fieldcompass.local) ");
  }

  // Register handlers
  webServer.on("/", handleWebRoot);
  webServer.on("/ops", handleWebOps);
  webServer.on("/gps", handleWebGPS);
  webServer.on("/env", handleWebEnv);
  webServer.on("/imu", handleWebIMU);
  webServer.on("/diags", handleWebDiags);
  webServer.on("/serial", handleWebSerial);
  webServer.on("/serial-data", handleWebSerialData);
  webServer.on("/json", handleWebJSON);
  webServer.on("/battlog", handleWebBattLog);
  webServer.on("/battlog/clear", handleWebBattLogClear);

  // Geocache manager endpoints (#70)
  webServer.on("/geocaches", HTTP_GET, handleWebGeocaches);
  webServer.on("/geocaches/upload", HTTP_POST, handleGeocacheUploadComplete, handleGeocacheUploadData);
  webServer.on("/geocaches/download", HTTP_GET, handleGeocacheDownload);
  webServer.on("/geocaches/delete", HTTP_GET, handleGeocacheDelete);
  webServer.on("/geocaches/clear", HTTP_GET, handleGeocacheClear);
  webServer.on("/geocaches/togglefound", HTTP_GET, handleGeocacheToggleFound);

  // Serial log file endpoints (#59)
  webServer.on("/logs", HTTP_GET, handleWebLogs);
  webServer.on("/logs/download", HTTP_GET, handleWebLogDownload);

  webServer.begin();
  webServerStarted = true;
  Serial.println("OK");
  Serial.print("  URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

// ============== Weather Log Statistics ==============

void updateWeatherLogStats() {
  if (!sdHealth.available) return;
  if (millis() - lastWeatherLogCheck < 60000) return;  // Check every minute
  lastWeatherLogCheck = millis();

  weatherLogFileCount = 0;
  weatherLogEntryCount = 0;

  File dir = SD.open("/weather");
  if (!dir) return;

  // Limit iterations to prevent hang on corrupted filesystem
  int maxFiles = 100;
  int filesChecked = 0;

  while (File entry = dir.openNextFile()) {
    if (filesChecked++ >= maxFiles) {
      entry.close();
      break;  // Safety limit
    }
    if (!entry.isDirectory()) {
      weatherLogFileCount++;
      // Count lines with safety limit
      int maxLines = 10000;
      int linesRead = 0;
      while (entry.available() && linesRead < maxLines) {
        if (entry.read() == '\n') weatherLogEntryCount++;
        linesRead++;
      }
    }
    entry.close();
  }
  dir.close();
  recordSDSuccess();
}

// ============== Display Sleep Functions ==============

void sleepTFT() {
  if (tftSleeping) return;

  tftSleeping = true;
  tft.sendCommand(ILI9341_SLPIN);
  #if DEBUG_SLEEP
  Serial.println("TFT sleeping");
  #endif
}

void wakeTFT() {
  if (!tftSleeping) return;

  tftSleeping = false;
  tft.sendCommand(ILI9341_SLPOUT);
  delay(120);  // ILI9341 datasheet: 120ms delay after sleep out
  tft.fillScreen(COLOR_BG);  // Clear screen on wake
  #if DEBUG_SLEEP
  Serial.println("TFT woke up");
  #endif
}

void sleepOLED() {
  if (oledSleeping || !oledAvailable) return;

  oledSleeping = true;
  oled.oled_command(SH110X_DISPLAYOFF);
  #if DEBUG_SLEEP
  Serial.println("OLED sleeping");
  #endif
}

void wakeOLED() {
  if (!oledSleeping || !oledAvailable) return;

  oledSleeping = false;
  oled.oled_command(SH110X_DISPLAYON);
  #if DEBUG_SLEEP
  Serial.println("OLED woke up");
  #endif
}

void wakeAllDisplays() {
  lastActivityTime = millis();
  wakeTFT();
  wakeOLED();
}

void checkDisplaySleep() {
  unsigned long elapsed = millis() - lastActivityTime;

  // Check OLED sleep (0 = disabled)
  if (OLED_SLEEP_TIMEOUT > 0 && !oledSleeping && oledAvailable && elapsed > OLED_SLEEP_TIMEOUT) {
    sleepOLED();
  }

  // Check TFT sleep (0 = disabled, LCD has no burn-in risk)
  if (TFT_SLEEP_TIMEOUT > 0 && !tftSleeping && elapsed > TFT_SLEEP_TIMEOUT) {
    sleepTFT();
  }
}

// ============== Button Handling ==============

void handleButtons() {
  unsigned long now = millis();
  static bool buttonCWasPressed = false;

  // Check current button states
  bool buttonA = !digitalRead(BUTTON_A);
  bool buttonB = !digitalRead(BUTTON_B);
  bool buttonC = !digitalRead(BUTTON_C);

  // Handle Button C long-press detection
  if (buttonC) {
    if (!buttonCWasPressed) {
      // Button C just pressed - start timing
      buttonCPressStart = now;
      buttonCLongPressHandled = false;
    } else if (!buttonCLongPressHandled && (now - buttonCPressStart >= LONG_PRESS_MS)) {
      // Long press detected
      buttonCLongPressHandled = true;
      handleButtonCLongPress();
    }
    buttonCWasPressed = true;
  } else {
    if (buttonCWasPressed && !buttonCLongPressHandled) {
      // Button C just released - short press
      if (now - lastButtonPress >= DEBOUNCE_MS) {
        handleButtonCShortPress();
        lastButtonPress = now;
      }
    }
    buttonCWasPressed = false;
    buttonCLongPressHandled = false;
  }

  // Debounce check for A/B buttons
  if (now - lastButtonPress < DEBOUNCE_MS) return;

  if (!buttonA && !buttonB) return;  // No A/B button pressed

  // If any display is sleeping, wake all and consume the button press
  if (tftSleeping || oledSleeping) {
    wakeAllDisplays();
    lastButtonPress = now;
    return;  // Don't process button action on wake
  }

  // Reset activity timer on any button press
  lastActivityTime = now;

  // Flush FRAM buffer on button press
  if (framAvailable && sdAvailable) framFlushToSD();

  // Reinit TFT on any button press (recovers from SPI glitch/white screen)
  tft.begin(TFT_SPI_FREQ);
  tft.setRotation(3);
  lastTFTReinit = now;

  // Handle A/B based on current screen and sub-screen
  if (currentScreen == SCREEN_GEOCACHE && geocacheSubScreen != 0) {
    // Geocache sub-screen navigation
    handleGeocacheButtons(buttonA, buttonB);
    lastButtonPress = now;
  } else {
    // Normal screen navigation
    if (buttonA) {
      currentScreen--;
      if (currentScreen < 0) currentScreen = NUM_SCREENS - 1;
      geocacheSubScreen = 0;  // Reset sub-screen when leaving
      lastButtonPress = now;
      tft.fillScreen(COLOR_BG);
    }

    if (buttonB) {
      currentScreen++;
      if (currentScreen >= NUM_SCREENS) currentScreen = 0;
      geocacheSubScreen = 0;  // Reset sub-screen when leaving
      lastButtonPress = now;
      tft.fillScreen(COLOR_BG);
    }
  }
}

// Handle geocache-specific button actions for list/details sub-screens
void handleGeocacheButtons(bool buttonA, bool buttonB) {
  if (geocacheSubScreen == 1) {
    // Cache List: A=scroll up, B=scroll down
    if (buttonA && listHighlightIndex > 0) {
      listHighlightIndex--;
      if (listHighlightIndex < listScrollOffset) {
        listScrollOffset = listHighlightIndex;
      }
    }
    if (buttonB && listHighlightIndex < cacheListCount - 1) {
      listHighlightIndex++;
      if (listHighlightIndex >= listScrollOffset + 5) {
        listScrollOffset = listHighlightIndex - 4;
      }
    }
  } else if (geocacheSubScreen == 2) {
    // Cache Details: A=prev cache, B=next cache
    if (buttonA && listHighlightIndex > 0) {
      listHighlightIndex--;
    }
    if (buttonB && listHighlightIndex < cacheListCount - 1) {
      listHighlightIndex++;
    }
  }
  tft.fillScreen(COLOR_BG);  // Clear on sub-screen change
}

// Button C short press handler
void handleButtonCShortPress() {
  // Wake displays if sleeping
  if (tftSleeping || oledSleeping) {
    wakeAllDisplays();
    return;
  }

  lastActivityTime = millis();

  if (currentScreen == SCREEN_GEOCACHE) {
    if (geocacheSubScreen == 0) {
      // Nav screen: short press goes to list
      geocacheSubScreen = 1;
      listHighlightIndex = selectedCacheIndex;
      listScrollOffset = max(0, listHighlightIndex - 2);
      tft.fillScreen(COLOR_BG);
    } else if (geocacheSubScreen == 1) {
      // List screen: short press selects cache and returns to nav
      selectedCacheIndex = listHighlightIndex;
      geocacheSubScreen = 0;
      tft.fillScreen(COLOR_BG);
    } else if (geocacheSubScreen == 2) {
      // Details screen: short press toggles found status
      if (cacheListCount > 0 && listHighlightIndex < cacheListCount) {
        cacheList[listHighlightIndex].found = !cacheList[listHighlightIndex].found;
        if (cacheList[listHighlightIndex].found) {
          cacheList[listHighlightIndex].foundTime = millis() / 1000;  // Simple timestamp
        }
        saveCacheFoundStatus();  // Persist to SD
        tft.fillScreen(COLOR_BG);
      }
    }
  }
}

// Button C long press handler
void handleButtonCLongPress() {
  // Wake displays if sleeping
  if (tftSleeping || oledSleeping) {
    wakeAllDisplays();
    return;
  }

  lastActivityTime = millis();

  if (currentScreen == SCREEN_GEOCACHE) {
    if (geocacheSubScreen == 1) {
      // List screen: long press goes to details
      geocacheSubScreen = 2;
      tft.fillScreen(COLOR_BG);
    } else if (geocacheSubScreen == 2) {
      // Details screen: long press goes back to list
      geocacheSubScreen = 1;
      tft.fillScreen(COLOR_BG);
    }
  }
}

// ============== GPS Reading ==============

void readGPS() {
  while (Serial1.available()) {
    char c = Serial1.read();
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
}

void parseNMEA(char* sentence) {
  // Parse GPRMC or GNRMC for time and position
  if (strstr(sentence, "RMC")) {
    char* token = strtok(sentence, ",");
    int field = 0;
    char status = 'V';
    float lat = 0, lon = 0;
    char latDir = 'N', lonDir = 'W';

    while (token != NULL) {
      switch (field) {
        case 1:  // Time HHMMSS.sss
          if (strlen(token) >= 6) {
            gpsData.hour = (token[0] - '0') * 10 + (token[1] - '0');
            gpsData.minute = (token[2] - '0') * 10 + (token[3] - '0');
            gpsData.second = (token[4] - '0') * 10 + (token[5] - '0');
            gpsData.timeValid = true;
          }
          break;
        case 2:  // Status A=valid, V=void
          status = token[0];
          break;
        case 3:  // Latitude
          lat = atof(token);
          break;
        case 4:  // N/S
          latDir = token[0];
          break;
        case 5:  // Longitude
          lon = atof(token);
          break;
        case 6:  // E/W
          lonDir = token[0];
          break;
        case 7:  // Speed over ground (knots)
          if (strlen(token) > 0) {
            gpsData.speedKnots = atof(token);
          }
          break;
        case 9:  // Date DDMMYY
          if (strlen(token) >= 6) {
            gpsData.day = (token[0] - '0') * 10 + (token[1] - '0');
            gpsData.month = (token[2] - '0') * 10 + (token[3] - '0');
            gpsData.year = 2000 + (token[4] - '0') * 10 + (token[5] - '0');
            gpsData.dateValid = true;
          }
          break;
      }
      token = strtok(NULL, ",");
      field++;
    }

    if (status == 'A') {
      int latDeg = (int)(lat / 100);
      float latMin = lat - (latDeg * 100);
      gpsData.latitude = latDeg + (latMin / 60.0);
      if (latDir == 'S') gpsData.latitude = -gpsData.latitude;

      int lonDeg = (int)(lon / 100);
      float lonMin = lon - (lonDeg * 100);
      gpsData.longitude = lonDeg + (lonMin / 60.0);
      if (lonDir == 'W') gpsData.longitude = -gpsData.longitude;

      gpsData.valid = true;

      // Track time to first fix (#68)
      if (!gpsHadFirstFix) {
        gpsFirstFixTime = millis();
        gpsHadFirstFix = true;
        logPrintf("[GPS] First fix acquired in %lus (TTFF)\n", gpsFirstFixTime / 1000);
      }

      // Sync RTC from GPS time (once per session, GPS is most accurate)
      if (gpsData.timeValid && gpsData.dateValid && !rtcSyncedFromGPS) {
        // Set system time from GPS (UTC)
        struct tm gpsTime;
        gpsTime.tm_year = gpsData.year - 1900;
        gpsTime.tm_mon = gpsData.month - 1;
        gpsTime.tm_mday = gpsData.day;
        gpsTime.tm_hour = gpsData.hour;
        gpsTime.tm_min = gpsData.minute;
        gpsTime.tm_sec = gpsData.second;

        time_t t = mktime(&gpsTime);
        // Apply timezone offset to get local time for system
        t += GMT_OFFSET_SEC + DAYLIGHT_OFFSET_SEC;
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);

        // Now sync RTC (stores UTC)
        syncRTCFromSystemTime("GPS");
        rtcSyncedFromGPS = true;

        logPrintf("[GPS] System time set: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  gpsData.year, gpsData.month, gpsData.day,
                  gpsData.hour, gpsData.minute, gpsData.second);
      }
    } else {
      // Track when signal is lost (for reacquiring elapsed time) (#68)
      if (gpsData.valid && gpsHadFirstFix) {
        gpsSignalLostTime = millis();
        logPrintf("[GPS] Signal lost at %lus\n", gpsSignalLostTime / 1000);
      }
      gpsData.valid = false;
    }
  }

  // Parse GPGGA or GNGGA for altitude, satellites, HDOP (#70)
  if (strstr(sentence, "GGA")) {
    char* token = strtok(sentence, ",");
    int field = 0;

    while (token != NULL) {
      if (field == 7 && strlen(token) > 0) {
        gpsData.satellites = atoi(token);  // Number of satellites (#70)
      }
      if (field == 8 && strlen(token) > 0) {
        gpsData.hdop = atof(token);        // Horizontal DOP (#70)
      }
      if (field == 9 && strlen(token) > 0) {
        gpsData.altitude = atof(token);
      }
      token = strtok(NULL, ",");
      field++;
    }
  }
}

// ============== Sensor Reading ==============

void readSHT41() {
  if (!shtAvailable) return;
  sensors_event_t humEv, tempEv;
  if (sht4.getEvent(&humEv, &tempEv)) {
    shtData.temperature = tempEv.temperature;
    shtData.humidity = humEv.relative_humidity;
  }
}

void readBME688() {
  // BSEC2 runs via callback, just need to call run() to process
  if (!envSensor.run()) {
    // Check for errors only if status is negative
    if (envSensor.status < BSEC_OK) {
      Serial.print("BSEC error: ");
      Serial.println(envSensor.status);
    }
  }
}

// Helper function to get IAQ accuracy as short text
const char* getIaqAccuracyText(uint8_t accuracy) {
  switch (accuracy) {
    case 0: return "INIT";
    case 1: return "LEARN";
    case 2: return "CAL";
    case 3: return "OK";
    default: return "?";
  }
}

// Convert hPa to inHg (inches of mercury)
float hPaToInHg(float hPa) {
  return hPa * 0.02953;
}

void readIMU() {
  sensors_event_t accel, gyro, temp, mag;

  lsm.getEvent(&accel, &gyro, &temp);
  lis.getEvent(&mag);

  imuData.accelX = accel.acceleration.x;
  imuData.accelY = accel.acceleration.y;
  imuData.accelZ = accel.acceleration.z;

  imuData.accelMag = sqrt(imuData.accelX * imuData.accelX +
                          imuData.accelY * imuData.accelY +
                          imuData.accelZ * imuData.accelZ) - 9.8;
  if (imuData.accelMag < 0) imuData.accelMag = 0;

  imuData.roll = atan2(imuData.accelY, imuData.accelZ) * 180.0 / PI;
  imuData.pitch = atan2(-imuData.accelX,
                        sqrt(imuData.accelY * imuData.accelY +
                             imuData.accelZ * imuData.accelZ)) * 180.0 / PI;

  float magX = mag.magnetic.x;
  float magY = mag.magnetic.y;

  imuData.heading = atan2(magY, magX) * 180.0 / PI;
  if (imuData.heading < 0) imuData.heading += 360;
}

// ============== Display Functions ==============

void updateDisplay() {
  static unsigned long lastUpdate = 0;

  // Update every 500ms to reduce flicker
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  // Update TFT display (if not sleeping)
  if (!tftSleeping) {
    switch (currentScreen) {
      case SCREEN_OPS:
        drawScreenOps();
        break;
      case SCREEN_COMPASS:
        drawScreenCompass();
        break;
      case SCREEN_GPS:
        drawScreenGPS();
        break;
      case SCREEN_ENV:
        drawScreenEnv();
        break;
      case SCREEN_IMU:
        drawScreenIMU();
        break;
      case SCREEN_DIAGS:
        drawScreenDiags();
        break;
      case SCREEN_GEOCACHE:
        drawScreenGeocache();
        break;
    }

    // Draw screen indicator at bottom
    drawNavBar();

    // Track TFT update for health monitoring
    lastTFTUpdate = millis();
    tftUpdateCount++;
  }

  // Update OLED display (if available and not sleeping)
  if (oledAvailable && !oledSleeping) {
    updateOLED();
  }
}

void drawHeader(const char* title) {
  tft.fillRect(0, 0, TFT_WIDTH, 30, COLOR_HEADER);
  tft.setTextColor(COLOR_BG);
  tft.setTextSize(2);
  tft.setCursor(10, 7);
  tft.print(title);
}

void drawNavBar() {
  int y = TFT_HEIGHT - 25;
  tft.fillRect(0, y, TFT_WIDTH, 25, 0x18C3);  // Dark gray

  tft.setTextColor(COLOR_TEXT);
  tft.setTextSize(2);

  // Screen indicators - adjusted spacing for 5 screens
  int startX = 60;
  int spacing = 36;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (i == currentScreen) {
      tft.fillRect(startX + i * spacing, y + 3, 28, 19, COLOR_HEADER);
      tft.setTextColor(COLOR_BG);
    } else {
      tft.setTextColor(COLOR_DIM);
    }
    tft.setCursor(startX + 6 + i * spacing, y + 5);
    tft.print(i + 1);
    tft.setTextColor(COLOR_TEXT);
  }

  // Button hints
  tft.setTextSize(1);
  tft.setCursor(10, y + 8);
  tft.print("A<");
  tft.setCursor(TFT_WIDTH - 25, y + 8);
  tft.print(">B");
}

void drawLabel(int x, int y, const char* label) {
  tft.setTextColor(COLOR_DIM);
  tft.setTextSize(2);
  tft.setCursor(x, y);
  tft.print(label);
}

void drawValue(int x, int y, const char* value, uint16_t color = COLOR_VALUE, int clearWidth = 200) {
  // Clear the value area first to prevent ghosting
  tft.fillRect(x, y, clearWidth, 18, COLOR_BG);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(x, y);
  tft.print(value);
}

void drawScreenOps() {
  drawHeader("OPERATIONAL");

  char buf[32];
  int y = 50;
  int labelX = 20;
  int valueX = 120;
  int lineH = 35;

  // Time
  drawLabel(labelX, y, "Time:");
  if (gpsData.timeValid) {
    int hour = gpsData.hour + (GMT_OFFSET_SEC / 3600);
    if (hour < 0) hour += 24;
    if (hour >= 24) hour -= 24;
    sprintf(buf, "%02d:%02d:%02d GPS", hour, gpsData.minute, gpsData.second);
    drawValue(valueX, y, buf);
  } else if (ntpSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      strftime(buf, sizeof(buf), "%H:%M:%S NTP", &timeinfo);
      drawValue(valueX, y, buf);
    }
  } else {
    drawValue(valueX, y, "--:--:-- N/A", COLOR_WARN);
  }
  y += lineH;

  // Uptime
  drawLabel(labelX, y, "Uptime:");
  unsigned long totalSec = millis() / 1000;
  int days = totalSec / 86400;
  int hours = (totalSec % 86400) / 3600;
  int mins = (totalSec % 3600) / 60;
  int secs = totalSec % 60;
  if (days > 0) {
    sprintf(buf, "%dd %02d:%02d:%02d", days, hours, mins, secs);
  } else {
    sprintf(buf, "%02d:%02d:%02d", hours, mins, secs);
  }
  drawValue(valueX, y, buf);
  y += lineH;

  // WiFi
  drawLabel(labelX, y, "WiFi:");
  if (wifiConnected) {
    drawValue(valueX, y, WiFi.SSID().c_str());
  } else {
    drawValue(valueX, y, "Disconnected", COLOR_ERROR);
  }
  y += lineH;

  // Battery
  drawLabel(labelX, y, "Battery:");
  if (batteryAvailable && isBatteryConnected()) {
    float pct = battery.cellPercent();
    float volt = battery.cellVoltage();
    sprintf(buf, "%.0f%% (%.2fV)", pct, volt);
    uint16_t color = (pct > 20) ? COLOR_VALUE : COLOR_ERROR;
    drawValue(valueX, y, buf, color);
  } else if (batteryAvailable) {
    drawValue(valueX, y, "USB Only", COLOR_DIM);
  } else {
    drawValue(valueX, y, "N/A", COLOR_DIM);
  }
}

void drawScreenGPS() {
  drawHeader("GPS");

  // Clear content area if GPS validity state changed
  static bool lastDrawnValid = false;
  if (gpsData.valid != lastDrawnValid) {
    tft.fillRect(0, 30, TFT_WIDTH, TFT_HEIGHT - 55, COLOR_BG);
    lastDrawnValid = gpsData.valid;
  }

  int y = 50;
  int labelX = 20;
  int valueX = 120;
  int lineH = 35;
  char buf[32];

  if (gpsData.valid) {
    // Latitude
    drawLabel(labelX, y, "Lat:");
    sprintf(buf, "%.6f %c", fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    drawValue(valueX, y, buf);
    y += lineH;

    // Longitude
    drawLabel(labelX, y, "Lon:");
    sprintf(buf, "%.6f %c", fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    drawValue(valueX, y, buf);
    y += lineH;

    // Altitude
    drawLabel(labelX, y, "Alt:");
    sprintf(buf, "%.1f m", gpsData.altitude);
    drawValue(valueX, y, buf);
    y += lineH;

    // Status with TTFF (#68)
    drawLabel(labelX, y, "Status:");
    if (gpsHadFirstFix) {
      sprintf(buf, "Fix OK (TTFF %lus)", gpsFirstFixTime / 1000);
      drawValue(valueX, y, buf, COLOR_VALUE);
    } else {
      drawValue(valueX, y, "Fix OK", COLOR_VALUE);
    }

  } else if (gpsData.receiving) {
    // Clear content area for acquiring state (uses raw print, not drawValue)
    tft.fillRect(0, 30, TFT_WIDTH, TFT_HEIGHT - 55, COLOR_BG);

    tft.setTextColor(COLOR_WARN);
    tft.setTextSize(2);
    tft.setCursor(60, 80);
    tft.println("Acquiring fix...");

    // Show elapsed time since boot (#68)
    unsigned long elapsed = millis() / 1000;
    tft.setCursor(60, 110);
    tft.setTextColor(COLOR_DIM);
    sprintf(buf, "Elapsed: %lum %lus", elapsed / 60, elapsed % 60);
    tft.println(buf);

    tft.setCursor(60, 140);
    tft.println("Need clear sky view");

    if (gpsData.timeValid) {
      tft.setCursor(60, 170);
      tft.setTextColor(COLOR_VALUE);
      tft.print("Time: ");
      int hour = gpsData.hour + (GMT_OFFSET_SEC / 3600);
      if (hour < 0) hour += 24;
      if (hour >= 24) hour -= 24;
      sprintf(buf, "%02d:%02d:%02d", hour, gpsData.minute, gpsData.second);
      tft.print(buf);
    }
  } else {
    // Clear content area for no-data state
    tft.fillRect(0, 30, TFT_WIDTH, TFT_HEIGHT - 55, COLOR_BG);

    tft.setTextColor(COLOR_ERROR);
    tft.setTextSize(2);
    tft.setCursor(80, 100);
    tft.println("No GPS data");
    tft.setCursor(60, 140);
    tft.setTextColor(COLOR_DIM);
    tft.println("Check connection");
  }
}

void drawScreenEnv() {
  drawHeader("ENVIRONMENT");

  int y = 42;
  int labelX = 10;
  int valueX = 70;      // Moved left for textSize 1 labels (#55)
  int lineH = 16;        // Reduced for textSize 1 (~8px char + 8px gap) (#55)
  char buf[40];

  // ENV-specific drawing at textSize 1 to prevent pressure wrap (#55)
  // Other screens continue using drawLabel/drawValue at textSize 2
  auto envLabel = [&](int x, int y, const char* label) {
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(1);
    tft.setCursor(x, y);
    tft.print(label);
  };
  auto envValue = [&](int x, int y, const char* value, uint16_t color = COLOR_VALUE) {
    tft.fillRect(x, y, 250, 10, COLOR_BG);
    tft.setTextColor(color);
    tft.setTextSize(1);
    tft.setCursor(x, y);
    tft.print(value);
  };

  if (bmeAvailable || shtAvailable) {
    // Temperature — SHT41 is primary source, BME688 fallback (#48)
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    const char* tempSrc = shtAvailable ? "SHT" : "BME";
    envLabel(labelX, y, "Temp:");
    sprintf(buf, "%.1fF (%.1fC) %s", tempF, tempC, tempSrc);
    envValue(valueX, y, buf);
    y += lineH;

    // Humidity — SHT41 is primary source, BME688 fallback (#48)
    float humid = shtAvailable ? shtData.humidity : envData.humidity;
    envLabel(labelX, y, "Humid:");
    sprintf(buf, "%.1f%% %s", humid, tempSrc);
    envValue(valueX, y, buf);
    y += lineH;

    // BME688-specific readings (IAQ, CO2, pressure, forecast)
    if (bmeAvailable) {
      // IAQ with accuracy indicator
      envLabel(labelX, y, "IAQ:");
      sprintf(buf, "%.0f [%s]", envData.iaq, getIaqAccuracyText(envData.iaqAccuracy));
      // Color based on IAQ: 0-50 good, 51-100 moderate, 101-150 poor, 151-200 unhealthy, >200 very unhealthy
      uint16_t color = COLOR_VALUE;
      if (envData.iaq > 200) color = COLOR_ERROR;
      else if (envData.iaq > 100) color = COLOR_WARN;
      envValue(valueX, y, buf, color);
      y += lineH;

      // CO2 equivalent
      envLabel(labelX, y, "CO2:");
      sprintf(buf, "%.0f ppm", envData.co2Equivalent);
      color = COLOR_VALUE;
      if (envData.co2Equivalent > 2000) color = COLOR_ERROR;
      else if (envData.co2Equivalent > 1000) color = COLOR_WARN;
      envValue(valueX, y, buf, color);
      y += lineH;

      // Pressure (station/absolute)
      envLabel(labelX, y, "Press:");
      sprintf(buf, "%.1f hPa (%.2f\")", envData.pressure, hPaToInHg(envData.pressure));
      envValue(valueX, y, buf);
      y += lineH;

      // Weather trend and forecast
      envLabel(labelX, y, "Fcst:");
      sprintf(buf, "%s %s", getTrendArrow(), weatherTrend.forecast);
      color = COLOR_VALUE;
      if (strstr(weatherTrend.forecast, "Storm")) color = COLOR_ERROR;
      else if (strstr(weatherTrend.forecast, "Rain") || strstr(weatherTrend.forecast, "Snow")) color = COLOR_WARN;
      envValue(valueX, y, buf, color);
    } else {
      // SHT41 only — no IAQ/CO2/pressure available
      envLabel(labelX, y, "IAQ:");
      envValue(valueX, y, "N/A (no BME688)", COLOR_DIM);
      y += lineH;
      envLabel(labelX, y, "Press:");
      envValue(valueX, y, "N/A (no BME688)", COLOR_DIM);
    }

  } else {
    tft.setTextColor(COLOR_ERROR);
    tft.setTextSize(2);
    tft.setCursor(60, 100);
    tft.println("No env sensors");
  }
}

void drawScreenCompass() {
  drawHeader("COMPASS");

  char buf[32];

  // === Left Panel: Text Data ===
  if (imuAvailable && magAvailable) {
    // Heading — large font
    tft.fillRect(10, 40, 130, 30, COLOR_BG);
    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(3);
    tft.setCursor(10, 42);
    sprintf(buf, "%.0f", imuData.heading);
    tft.print(buf);
    // Degree symbol
    int degX = tft.getCursorX() + 2;
    tft.drawCircle(degX + 3, 44, 3, COLOR_TEXT);

    // Cardinal direction
    tft.fillRect(10, 75, 130, 20, COLOR_BG);
    tft.setTextColor(COLOR_VALUE);
    tft.setTextSize(2);
    tft.setCursor(10, 78);
    tft.print(getCardinal(imuData.heading));
  } else {
    tft.fillRect(10, 40, 130, 55, COLOR_BG);
    tft.setTextColor(COLOR_ERROR);
    tft.setTextSize(2);
    tft.setCursor(10, 55);
    tft.print("No IMU");
  }

  // Speed from GPS
  tft.setTextColor(COLOR_DIM);
  tft.setTextSize(1);
  tft.setCursor(10, 108);
  tft.print("Speed:");

  tft.fillRect(10, 120, 130, 20, COLOR_BG);
  tft.setTextSize(2);
  if (gpsData.valid) {
    float mph = gpsData.speedKnots * 1.15078;
    tft.setTextColor(COLOR_VALUE);
    tft.setCursor(10, 120);
    sprintf(buf, "%.1f mph", mph);
    tft.print(buf);
  } else {
    tft.setTextColor(COLOR_DIM);
    tft.setCursor(10, 120);
    tft.print("-- mph");
  }

  // GPS status
  tft.fillRect(10, 150, 130, 12, COLOR_BG);
  tft.setTextSize(1);
  tft.setCursor(10, 152);
  if (gpsData.valid) {
    tft.setTextColor(COLOR_VALUE);
    sprintf(buf, "GPS OK  Sat:%d", gpsData.satellites);
    tft.print(buf);
  } else if (gpsData.receiving) {
    tft.setTextColor(COLOR_WARN);
    tft.print("GPS Acquiring...");
  } else {
    tft.setTextColor(COLOR_ERROR);
    tft.print("No GPS");
  }

  // === Right Panel: Compass Rose ===
  if (imuAvailable && magAvailable) {
    drawCompassRose(230, 125, 75, imuData.heading);
  } else {
    tft.drawCircle(230, 125, 75, COLOR_DIM);
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(3);
    tft.setCursor(222, 115);
    tft.print("?");
  }
}

void drawScreenIMU() {
  drawHeader("IMU / COMPASS");

  int y = 50;
  int labelX = 20;
  int valueX = 140;
  int lineH = 35;
  char buf[32];

  if (imuAvailable && magAvailable) {
    // Compass heading
    drawLabel(labelX, y, "Heading:");
    sprintf(buf, "%.0f %s", imuData.heading, getCardinal(imuData.heading));
    drawValue(valueX, y, buf);
    y += lineH;

    // Roll
    drawLabel(labelX, y, "Roll:");
    sprintf(buf, "%.0f deg", imuData.roll);
    drawValue(valueX, y, buf);
    y += lineH;

    // Pitch
    drawLabel(labelX, y, "Pitch:");
    sprintf(buf, "%.0f deg", imuData.pitch);
    drawValue(valueX, y, buf);
    y += lineH;

    // Acceleration
    drawLabel(labelX, y, "Accel:");
    sprintf(buf, "%.2f m/s2", imuData.accelMag);
    drawValue(valueX, y, buf);

  } else {
    tft.setTextColor(COLOR_ERROR);
    tft.setTextSize(2);
    tft.setCursor(60, 100);
    tft.println("IMU not available");
  }
}

const char* getCardinal(float heading) {
  if (heading >= 337.5 || heading < 22.5) return "N";
  if (heading >= 22.5 && heading < 67.5) return "NE";
  if (heading >= 67.5 && heading < 112.5) return "E";
  if (heading >= 112.5 && heading < 157.5) return "SE";
  if (heading >= 157.5 && heading < 202.5) return "S";
  if (heading >= 202.5 && heading < 247.5) return "SW";
  if (heading >= 247.5 && heading < 292.5) return "W";
  return "NW";
}

// Draw rotating 8-point compass rose
// cx, cy = center, radius = outer ring size, heading = device heading (degrees)
void drawCompassRose(int cx, int cy, int radius, float heading) {
  // Clear rose area
  tft.fillRect(cx - radius - 6, cy - radius - 6,
               2 * radius + 12, 2 * radius + 12, COLOR_BG);

  // Outer circle
  tft.drawCircle(cx, cy, radius, COLOR_DIM);

  // Rotation: rose turns opposite to heading so N points north
  float rotDeg = -heading;

  // Degree ticks every 30 degrees (12 ticks)
  for (int i = 0; i < 12; i++) {
    float tickAngle = radians(i * 30 + rotDeg - 90);  // -90 for screen coords
    int tickLen = (i % 3 == 0) ? 8 : 4;  // Longer at cardinals
    int outerX = cx + cos(tickAngle) * radius;
    int outerY = cy + sin(tickAngle) * radius;
    int innerX = cx + cos(tickAngle) * (radius - tickLen);
    int innerY = cy + sin(tickAngle) * (radius - tickLen);
    tft.drawLine(innerX, innerY, outerX, outerY, COLOR_DIM);
  }

  // 8 diamond needles: cardinals (longer, wider) + intercardinals (shorter, thinner)
  struct Needle {
    float angle;      // Degrees from north
    int length;       // Tip distance from center
    int halfWidth;    // Half-width at center
    uint16_t color;
  };

  Needle needles[] = {
    {  0,  70, 6, COLOR_HEADER},  // N — cyan
    { 45,  45, 4, COLOR_DIM},     // NE — gray
    { 90,  70, 6, COLOR_TEXT},    // E — white
    {135,  45, 4, COLOR_DIM},     // SE — gray
    {180,  70, 6, COLOR_ERROR},   // S — red
    {225,  45, 4, COLOR_DIM},     // SW — gray
    {270,  70, 6, COLOR_TEXT},    // W — white
    {315,  45, 4, COLOR_DIM},     // NW — gray
  };

  for (int i = 0; i < 8; i++) {
    float tipRad = radians(needles[i].angle + rotDeg - 90);
    float perpRad = tipRad + radians(90);

    // Tip point (outer end of needle)
    int tipX = cx + cos(tipRad) * needles[i].length;
    int tipY = cy + sin(tipRad) * needles[i].length;

    // Two side points at center (perpendicular to needle axis)
    int sideX1 = cx + cos(perpRad) * needles[i].halfWidth;
    int sideY1 = cy + sin(perpRad) * needles[i].halfWidth;
    int sideX2 = cx - cos(perpRad) * needles[i].halfWidth;
    int sideY2 = cy - sin(perpRad) * needles[i].halfWidth;

    // Tail point (opposite end, 1/3 length)
    float tailRad = tipRad + radians(180);
    int tailLen = needles[i].length / 3;
    int tailX = cx + cos(tailRad) * tailLen;
    int tailY = cy + sin(tailRad) * tailLen;

    // Draw as two triangles: tip half and tail half
    tft.fillTriangle(tipX, tipY, sideX1, sideY1, sideX2, sideY2, needles[i].color);
    // Tail: cardinals get dark fill, intercardinals get gray
    uint16_t tailColor = (needles[i].color == COLOR_DIM) ? COLOR_DIM : 0x2104;
    tft.fillTriangle(tailX, tailY, sideX1, sideY1, sideX2, sideY2, tailColor);
  }

  // Center dot
  tft.fillCircle(cx, cy, 4, COLOR_TEXT);
  tft.drawCircle(cx, cy, 4, COLOR_DIM);

  // Fixed lubber line at top (does NOT rotate) — orange triangle
  int lubberY = cy - radius - 3;
  tft.fillTriangle(cx, lubberY, cx - 5, lubberY - 8, cx + 5, lubberY - 8, COLOR_WARN);
}

// ============== Geocache Helper Functions (#70) ==============

// Calculate distance between two GPS coordinates using Haversine formula
float calcDistanceKm(float lat1, float lon1, float lat2, float lon2) {
  float dLat = radians(lat2 - lat1);
  float dLon = radians(lon2 - lon1);
  float a = sin(dLat/2) * sin(dLat/2) +
            cos(radians(lat1)) * cos(radians(lat2)) *
            sin(dLon/2) * sin(dLon/2);
  float c = 2 * atan2(sqrt(a), sqrt(1-a));
  return 6371.0 * c;  // Earth radius in km
}

// Calculate bearing from point 1 to point 2
float calcBearing(float lat1, float lon1, float lat2, float lon2) {
  float dLon = radians(lon2 - lon1);
  float y = sin(dLon) * cos(radians(lat2));
  float x = cos(radians(lat1)) * sin(radians(lat2)) -
            sin(radians(lat1)) * cos(radians(lat2)) * cos(dLon);
  float bearing = atan2(y, x) * 180.0 / PI;
  if (bearing < 0) bearing += 360;
  return bearing;
}

// Get GPS accuracy estimate based on HDOP
float getGpsAccuracyMeters() {
  return 3.0 * gpsData.hdop;  // Typical GPS accuracy ≈ 3m × HDOP
}

// Get color for accuracy indicator
uint16_t getAccuracyColor(float accuracyM) {
  if (accuracyM < 10) return COLOR_VALUE;  // Green - excellent
  if (accuracyM < 25) return COLOR_WARN;   // Yellow - good
  return COLOR_ERROR;                       // Red - poor
}

// Draw Google Maps style navigation triangle
void drawNavTriangle(int cx, int cy, int size, float angle, uint16_t color) {
  // angle = 0 means pointing up (north), increases clockwise
  float rad = radians(angle - 90);  // Adjust for screen coordinates

  // Front tip (pointed direction)
  int tipX = cx + cos(rad) * size;
  int tipY = cy + sin(rad) * size;

  // Rear corners (wider base for aerodynamic look)
  float rearRad1 = rad + radians(140);
  float rearRad2 = rad - radians(140);
  int rear1X = cx + cos(rearRad1) * size * 0.7;
  int rear1Y = cy + sin(rearRad1) * size * 0.7;
  int rear2X = cx + cos(rearRad2) * size * 0.7;
  int rear2Y = cy + sin(rearRad2) * size * 0.7;

  // Rear center notch (creates aerodynamic tail)
  float rearCenterRad = rad + radians(180);
  int rearCenterX = cx + cos(rearCenterRad) * size * 0.3;
  int rearCenterY = cy + sin(rearCenterRad) * size * 0.3;

  // Draw as two triangles for notched rear
  tft.fillTriangle(tipX, tipY, rear1X, rear1Y, rearCenterX, rearCenterY, color);
  tft.fillTriangle(tipX, tipY, rear2X, rear2Y, rearCenterX, rearCenterY, color);
}

// Draw pulsing search zone circle that shrinks as we get closer
void drawSearchZoneCircle(int cx, int cy, float distanceM, float accuracyM) {
  // Circle size shrinks as we get closer (visual "getting warmer")
  int maxRadius = 60;
  int minRadius = 20;
  float ratio = constrain(distanceM / accuracyM, 0.0, 1.0);
  int radius = minRadius + (int)((maxRadius - minRadius) * ratio);

  // Pulsing effect using millis()
  int pulse = (millis() / 100) % 10;  // 0-9 cycle
  int pulseOffset = (pulse < 5) ? pulse : (9 - pulse);  // 0-4-0 triangle wave
  radius += pulseOffset * 2;

  // Draw filled circle with outline
  tft.fillCircle(cx, cy, radius, COLOR_WARN);
  tft.drawCircle(cx, cy, radius + 2, COLOR_TEXT);

  // Center dot
  tft.fillCircle(cx, cy, 4, COLOR_TEXT);
}

// ============== Geocache Navigation Screen (#70) ==============

// Forward declarations for sub-screens
void drawCacheNavScreen();
void drawCacheListScreen();
void drawCacheDetailsScreen();

void drawScreenGeocache() {
  // Dispatch to appropriate sub-screen
  switch (geocacheSubScreen) {
    case 1:
      drawCacheListScreen();
      break;
    case 2:
      drawCacheDetailsScreen();
      break;
    default:
      drawCacheNavScreen();
      break;
  }
}

// Sub-screen 0: Navigation (main geocache screen)
void drawCacheNavScreen() {
  drawHeader("GEOCACHE");

  // Check if we have any caches and if selected cache is valid
  if (cacheListCount == 0 || selectedCacheIndex >= cacheListCount || !cacheList[selectedCacheIndex].valid) {
    tft.setCursor(80, 120);
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(2);
    tft.print("No cache loaded");
    drawNavBar();
    return;
  }

  // Get reference to selected cache for cleaner code
  GeocacheEntry& cache = cacheList[selectedCacheIndex];
  char buf[64];

  // Calculate distance and bearing
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float distM = distKm * 1000;
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);
  float accuracyM = getGpsAccuracyMeters();
  bool inSearchZone = (distM < accuracyM);

  // Row 1: Cache name CENTERED
  tft.setTextSize(2);
  tft.setTextColor(COLOR_TEXT);
  int nameLen = strlen(cache.name);
  int nameX = (320 - nameLen * 12) / 2;  // Center text (12px per char at size 2)
  if (nameX < 10) nameX = 10;  // Clamp to screen edge
  tft.setCursor(nameX, 35);
  tft.print(cache.name);

  // Row 2: Distance/SearchZone + D/T (left), Heading (right)
  // Clear row 2 area first to prevent ghosting
  tft.fillRect(10, 55, 300, 20, COLOR_BG);

  tft.setCursor(10, 55);
  tft.setTextSize(2);

  if (inSearchZone) {
    tft.setTextColor(COLOR_WARN);
    tft.print("SEARCH ZONE");
  } else {
    tft.setTextColor(COLOR_VALUE);
    if (useMetricUnits) {
      if (distKm >= 1.0) {
        sprintf(buf, "%.1f km", distKm);
      } else {
        sprintf(buf, "%.0f m", distM);
      }
    } else {
      float distMi = distKm * 0.621371;
      float distFt = distKm * 3280.84;
      if (distMi >= 1.0) {
        sprintf(buf, "%.1f mi", distMi);
      } else {
        sprintf(buf, "%.0f ft", distFt);
      }
    }
    tft.print(buf);
  }

  // D/T rating (after distance, same row)
  tft.setTextSize(1);
  tft.setTextColor(COLOR_DIM);
  sprintf(buf, " D:%.1f T:%.1f", cache.difficulty, cache.terrain);
  tft.print(buf);

  // Heading (right side)
  tft.setCursor(265, 55);
  tft.setTextColor(COLOR_VALUE);
  tft.setTextSize(2);
  sprintf(buf, "%d", (int)bearing);
  tft.print(buf);
  tft.setTextSize(1);
  tft.print((char)247);  // Degree symbol

  // Center: Nav triangle OR search zone pulsing circle
  // Clear center area first to prevent ghosting on rotation
  tft.fillRect(100, 75, 120, 110, COLOR_BG);

  if (inSearchZone) {
    // Pulsing circle that shrinks as we get closer
    drawSearchZoneCircle(160, 130, distM, accuracyM);
  } else {
    // Google Maps style nav triangle
    float triangleAngle = bearing - imuData.heading;
    if (triangleAngle < 0) triangleAngle += 360;
    if (triangleAngle >= 360) triangleAngle -= 360;
    drawNavTriangle(160, 130, 50, triangleAngle, COLOR_HEADER);
  }

  // GPS accuracy indicator (below center graphic)
  // Clear accuracy area first to prevent ghosting
  tft.fillRect(80, 185, 160, 20, COLOR_BG);

  uint16_t accColor = getAccuracyColor(accuracyM);
  tft.setTextColor(accColor);
  tft.setTextSize(2);

  if (useMetricUnits) {
    sprintf(buf, "+/-%.0fm", accuracyM);
  } else {
    sprintf(buf, "+/-%.0fft", accuracyM * 3.28084);
  }
  int accLen = strlen(buf);
  int accX = (320 - accLen * 12) / 2;  // Center
  tft.setCursor(accX, 185);
  tft.print(buf);

  // Bottom: Hint (teaser in normal mode, FULL in search zone)
  tft.setTextColor(COLOR_DIM);
  tft.setTextSize(1);

  if (strlen(cache.hint) > 0) {
    if (inSearchZone) {
      // FULL HINT in search zone (multi-line)
      tft.setCursor(10, 205);
      tft.print("Hint: ");
      tft.print(cache.hint);  // Full hint (may wrap)
    } else {
      // Teaser only
      tft.setCursor(10, 220);
      char hintPreview[35];
      strncpy(hintPreview, cache.hint, 30);
      hintPreview[30] = '\0';
      if (strlen(cache.hint) > 30) strcat(hintPreview, "...");
      tft.print("Hint: ");
      tft.print(hintPreview);
    }
  }

  drawNavBar();
}

// Sub-screen 1: Cache List
void drawCacheListScreen() {
  drawHeader("CACHE LIST");

  char buf[64];
  int y = 38;
  int lineH = 28;
  int maxVisible = 5;  // 5 entries visible at once

  // Show count in header area
  tft.setTextSize(1);
  tft.setTextColor(COLOR_DIM);
  sprintf(buf, "[%d/%d]", listHighlightIndex + 1, cacheListCount);
  tft.setCursor(280, 10);
  tft.print(buf);

  if (cacheListCount == 0) {
    tft.setCursor(80, 120);
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(2);
    tft.print("No caches loaded");
    drawNavBar();
    return;
  }

  // Draw visible cache entries
  for (int i = 0; i < maxVisible && (listScrollOffset + i) < cacheListCount; i++) {
    int cacheIdx = listScrollOffset + i;
    GeocacheEntry& cache = cacheList[cacheIdx];
    bool isSelected = (cacheIdx == listHighlightIndex);

    // Highlight background for selected item
    if (isSelected) {
      tft.fillRect(0, y - 2, 320, lineH, COLOR_HEADER & 0x18E3);  // Darker cyan
    }

    int x = 5;

    // Selection indicator
    tft.setTextSize(2);
    tft.setTextColor(isSelected ? COLOR_HEADER : COLOR_DIM);
    tft.setCursor(x, y + 2);
    tft.print(isSelected ? ">" : " ");
    x += 18;

    // Distance
    float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                   cache.latitude, cache.longitude);
    tft.setTextColor(COLOR_VALUE);
    tft.setTextSize(1);
    tft.setCursor(x, y + 4);
    if (useMetricUnits) {
      if (distKm >= 1.0) {
        sprintf(buf, "%4.1fkm", distKm);
      } else {
        sprintf(buf, "%4.0fm", distKm * 1000);
      }
    } else {
      float distMi = distKm * 0.621371;
      if (distMi >= 1.0) {
        sprintf(buf, "%4.1fmi", distMi);
      } else {
        sprintf(buf, "%4.0fft", distKm * 3280.84);
      }
    }
    tft.print(buf);
    x += 48;

    // Cache name (truncated)
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(x, y + 4);
    char nameBuf[20];
    strncpy(nameBuf, cache.name, 16);
    nameBuf[16] = '\0';
    if (strlen(cache.name) > 16) {
      nameBuf[14] = '.';
      nameBuf[15] = '.';
    }
    tft.print(nameBuf);
    x += 100;

    // Found checkmark
    if (cache.found) {
      tft.setTextColor(COLOR_VALUE);
      tft.setCursor(x, y + 4);
      tft.print("*");  // Checkmark indicator
    }
    x += 12;

    // D/T rating
    tft.setTextColor(COLOR_DIM);
    tft.setCursor(x, y + 4);
    sprintf(buf, "D:%d T:%d", (int)cache.difficulty, (int)cache.terrain);
    tft.print(buf);

    y += lineH;
  }

  // Draw button hints at bottom
  tft.setTextSize(1);
  tft.setTextColor(COLOR_DIM);
  tft.setCursor(10, 190);
  tft.print("[A]Up [B]Down [C]Select [C+]Details");

  drawNavBar();
}

// Sub-screen 2: Cache Details
void drawCacheDetailsScreen() {
  if (cacheListCount == 0 || listHighlightIndex >= cacheListCount) {
    drawHeader("CACHE DETAILS");
    tft.setCursor(80, 120);
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(2);
    tft.print("No cache");
    drawNavBar();
    return;
  }

  GeocacheEntry& cache = cacheList[listHighlightIndex];
  char buf[64];

  drawHeader("CACHE DETAILS");

  // Count indicator in header
  tft.setTextSize(1);
  tft.setTextColor(COLOR_DIM);
  sprintf(buf, "[%d/%d]", listHighlightIndex + 1, cacheListCount);
  tft.setCursor(280, 10);
  tft.print(buf);

  int y = 35;
  int lineH = 18;

  // Cache name (large)
  tft.setTextSize(2);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(10, y);
  // Truncate name if too long
  char nameBuf[24];
  strncpy(nameBuf, cache.name, 22);
  nameBuf[22] = '\0';
  tft.print(nameBuf);
  y += 22;

  // GC Code
  tft.setTextSize(1);
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(10, y);
  tft.print(cache.gcCode);
  y += lineH + 4;

  // Coordinates
  tft.setTextColor(COLOR_VALUE);
  tft.setCursor(10, y);
  sprintf(buf, "%.4f%c  %.4f%c",
          fabs(cache.latitude), cache.latitude >= 0 ? 'N' : 'S',
          fabs(cache.longitude), cache.longitude >= 0 ? 'E' : 'W');
  tft.print(buf);
  y += lineH;

  // Difficulty/Terrain
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(10, y);
  sprintf(buf, "Difficulty: %.1f  Terrain: %.1f", cache.difficulty, cache.terrain);
  tft.print(buf);
  y += lineH + 4;

  // Current distance/bearing
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);
  tft.setTextColor(COLOR_VALUE);
  tft.setCursor(10, y);
  if (useMetricUnits) {
    sprintf(buf, "Distance: %.2f km  Bearing: %d", distKm, (int)bearing);
  } else {
    sprintf(buf, "Distance: %.2f mi  Bearing: %d", distKm * 0.621371, (int)bearing);
  }
  tft.print(buf);
  tft.print((char)247);  // Degree symbol
  y += lineH + 4;

  // Hint (full, multi-line)
  if (strlen(cache.hint) > 0) {
    tft.setTextColor(COLOR_DIM);
    tft.setCursor(10, y);
    tft.print("Hint: ");
    tft.setTextColor(COLOR_TEXT);
    // Word wrap hint - simple approach, just print and let TFT wrap
    tft.setCursor(10, y + 10);
    // Print hint in chunks to avoid overflow
    int hintLen = strlen(cache.hint);
    int pos = 0;
    int lineY = y + 10;
    while (pos < hintLen && lineY < 175) {
      int chunkLen = min(50, hintLen - pos);  // ~50 chars per line at size 1
      char chunk[52];
      strncpy(chunk, cache.hint + pos, chunkLen);
      chunk[chunkLen] = '\0';
      tft.setCursor(10, lineY);
      tft.print(chunk);
      pos += chunkLen;
      lineY += 10;
    }
  }

  // Found status (interactive area)
  y = 180;
  tft.setCursor(10, y);
  tft.setTextSize(2);
  if (cache.found) {
    tft.setTextColor(COLOR_VALUE);
    tft.print("[* FOUND]");
  } else {
    tft.setTextColor(COLOR_DIM);
    tft.print("[ NOT FOUND ]");
  }

  // Button hints
  tft.setTextSize(1);
  tft.setTextColor(COLOR_DIM);
  tft.setCursor(10, 205);
  tft.print("[A]Prev [B]Next [C]Toggle [C+]Back");

  drawNavBar();
}

void drawScreenDiags() {
  drawHeader("DIAGNOSTICS");

  int y = 38;
  int labelX = 10;
  int valueX = 140;
  int lineH = 24;
  char buf[40];

  tft.setTextSize(1);

  // BSEC State section
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("BSEC:");
  sprintf(buf, "Load:%s Save:%s Acc:%s",
          bsecStateLoaded ? "Y" : "N",
          bsecStateSaved ? "Y" : "N",
          getIaqAccuracyText(envData.iaqAccuracy));
  tft.setTextColor(bsecStateLoaded ? COLOR_VALUE : COLOR_DIM);
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // Weather Log
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("Weather:");
  sprintf(buf, "Mem:%d Files:%d Tot:%d",
          weatherHistoryCount, weatherLogFileCount, weatherLogEntryCount);
  tft.setTextColor(COLOR_VALUE);
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // Memory
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("Heap:");
  sprintf(buf, "%lu / %lu bytes",
          (unsigned long)ESP.getFreeHeap(),
          (unsigned long)ESP.getHeapSize());
  tft.setTextColor(COLOR_VALUE);
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // Sensors status
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("Sensors:");
  sprintf(buf, "BME:%s SHT:%s IMU:%s Bat:%s FRAM:%s",
          bmeAvailable ? "Y" : "N",
          shtAvailable ? "Y" : "N",
          imuAvailable ? "Y" : "N",
          batteryAvailable ? "Y" : "N",
          framAvailable ? "Y" : "N");
  tft.setTextColor(COLOR_VALUE);
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // Temp comparison: SHT41 vs BME688 (#48)
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("Temps:");
  if (shtAvailable && bmeAvailable) {
    float shtF = shtData.temperature * 9.0 / 5.0 + 32.0;
    float bmeF = envData.temperature * 9.0 / 5.0 + 32.0;
    float deltaF = shtF - bmeF;
    sprintf(buf, "SHT:%.1fF BME:%.1fF (%+.1f)", shtF, bmeF, deltaF);
  } else if (shtAvailable) {
    float shtF = shtData.temperature * 9.0 / 5.0 + 32.0;
    sprintf(buf, "SHT:%.1fF BME:N/A", shtF);
  } else if (bmeAvailable) {
    float bmeF = envData.temperature * 9.0 / 5.0 + 32.0;
    sprintf(buf, "SHT:N/A BME:%.1fF", bmeF);
  } else {
    sprintf(buf, "No sensors");
  }
  tft.setTextColor(COLOR_VALUE);
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // GPS TTFF (#68)
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("GPS:");
  if (gpsHadFirstFix) {
    sprintf(buf, "Fix in %lus", gpsFirstFixTime / 1000);
    tft.setTextColor(COLOR_VALUE);
  } else if (gpsHadFirstReceive) {
    unsigned long elapsed = millis() / 1000;
    sprintf(buf, "Acquiring (%lum %lus)", elapsed / 60, elapsed % 60);
    tft.setTextColor(COLOR_WARN);
  } else {
    sprintf(buf, "No data");
    tft.setTextColor(COLOR_DIM);
  }
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // Storage & Display with SD health
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("Storage:");

  // Format SD status with health info
  if (sdHealth.available) {
    unsigned long ageMin = (millis() - sdHealth.lastSuccess) / 60000;
    if (sdHealth.errorCount == 0) {
      sprintf(buf, "SD:OK %lum OLED:%s",
              ageMin, oledAvailable ? "Y" : "N");
      tft.setTextColor(COLOR_VALUE);
    } else {
      sprintf(buf, "SD:WARN E:%d R:%d OLED:%s",
              sdHealth.errorCount, sdHealth.reInitCount,
              oledAvailable ? "Y" : "N");
      tft.setTextColor(COLOR_WARN);
    }
  } else {
    sprintf(buf, "SD:FAIL E:%d R:%d OLED:%s",
            sdHealth.errorCount, sdHealth.reInitCount,
            oledAvailable ? "Y" : "N");
    tft.setTextColor(COLOR_ERROR);
  }
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // Network
  tft.setTextColor(COLOR_HEADER);
  tft.setCursor(labelX, y);
  tft.print("Network:");
  if (wifiConnected) {
    sprintf(buf, "%s", WiFi.localIP().toString().c_str());
    tft.setTextColor(COLOR_VALUE);
  } else {
    sprintf(buf, "Disconnected");
    tft.setTextColor(COLOR_ERROR);
  }
  tft.setCursor(valueX - 80, y);
  tft.fillRect(valueX - 80, y, 200, 10, COLOR_BG);
  tft.print(buf);
  y += lineH;

  // Web URL hint
  if (wifiConnected) {
    tft.setTextColor(COLOR_DIM);
    tft.setCursor(labelX, y);
    tft.print("Web: http://fieldcompass.local/");
  }
}

// ============== OLED Display Functions ==============

void updateOLED() {
  oled.clearDisplay();

  // Show different content based on current screen (mirroring TFT)
  switch (currentScreen) {
    case SCREEN_OPS:
      drawOLEDScreenOps();
      break;
    case SCREEN_COMPASS:
      drawOLEDScreenCompass();
      break;
    case SCREEN_GPS:
      drawOLEDScreenGPS();
      break;
    case SCREEN_ENV:
      drawOLEDScreenEnv();
      break;
    case SCREEN_IMU:
      drawOLEDScreenIMU();
      break;
    case SCREEN_DIAGS:
      drawOLEDScreenDiags();
      break;
    case SCREEN_GEOCACHE:
      drawOLEDScreenGeocache();
      break;
  }

  // Draw screen indicator
  drawOLEDNavBar();

  oled.display();
}

void drawOLEDScreenOps() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("OPERATIONAL");

  // Time
  oled.setCursor(0, 12);
  if (gpsData.timeValid) {
    int hour = gpsData.hour + (GMT_OFFSET_SEC / 3600);
    if (hour < 0) hour += 24;
    if (hour >= 24) hour -= 24;
    sprintf(buf, "Time: %02d:%02d:%02d", hour, gpsData.minute, gpsData.second);
  } else if (ntpSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      strftime(buf, sizeof(buf), "Time: %H:%M:%S", &timeinfo);
    }
  } else {
    sprintf(buf, "Time: --:--:--");
  }
  oled.print(buf);

  // Uptime
  unsigned long totalSec = millis() / 1000;
  int hours = (totalSec % 86400) / 3600;
  int mins = (totalSec % 3600) / 60;
  int secs = totalSec % 60;
  oled.setCursor(0, 24);
  sprintf(buf, "Up: %02d:%02d:%02d", hours, mins, secs);
  oled.print(buf);

  // WiFi
  oled.setCursor(0, 36);
  if (wifiConnected) {
    oled.print("WiFi: OK");
  } else {
    oled.print("WiFi: --");
  }

  // Battery
  oled.setCursor(0, 48);
  if (batteryAvailable && isBatteryConnected()) {
    sprintf(buf, "Batt: %.0f%%", battery.cellPercent());
  } else if (batteryAvailable) {
    sprintf(buf, "Batt: USB");
  } else {
    sprintf(buf, "Batt: --");
  }
  oled.print(buf);
}

void drawOLEDScreenGPS() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("GPS");

  if (gpsData.valid) {
    oled.setCursor(0, 12);
    sprintf(buf, "Lat: %.5f", gpsData.latitude);
    oled.print(buf);

    oled.setCursor(0, 24);
    sprintf(buf, "Lon: %.5f", gpsData.longitude);
    oled.print(buf);

    oled.setCursor(0, 36);
    sprintf(buf, "Alt: %.1fm", gpsData.altitude);
    oled.print(buf);

    oled.setCursor(0, 48);
    oled.print("Status: Fix OK");
  } else if (gpsData.receiving) {
    oled.setCursor(0, 20);
    oled.print("Acquiring fix...");
    oled.setCursor(0, 36);
    oled.print("Need sky view");
  } else {
    oled.setCursor(0, 28);
    oled.print("No GPS data");
  }
}

void drawOLEDScreenEnv() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("ENVIRONMENT");

  if (bmeAvailable || shtAvailable) {
    // SHT41 temp/humidity preferred over BME688 (#48)
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    float humid = shtAvailable ? shtData.humidity : envData.humidity;

    oled.setCursor(0, 10);
    sprintf(buf, "%.1fF %.1f%% IAQ:%.0f", tempF, humid, envData.iaq);
    oled.print(buf);

    oled.setCursor(0, 22);
    sprintf(buf, "%.0fhPa %.2f\"", envData.pressure, hPaToInHg(envData.pressure));
    oled.print(buf);

    oled.setCursor(0, 34);
    sprintf(buf, "%s %s", getTrendArrow(), weatherTrend.forecast);
    oled.print(buf);

    oled.setCursor(0, 46);
    sprintf(buf, "CO2:%.0f %s", envData.co2Equivalent, shtAvailable ? "SHT" : "BME");
    oled.print(buf);
  } else {
    oled.setCursor(0, 28);
    oled.print("No env sensors");
  }
}

void drawOLEDScreenCompass() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("COMPASS");

  if (imuAvailable && magAvailable) {
    oled.setTextSize(2);
    oled.setCursor(0, 12);
    sprintf(buf, "%.0f %s", imuData.heading, getCardinal(imuData.heading));
    oled.print(buf);

    oled.setTextSize(1);
    oled.setCursor(0, 36);
    if (gpsData.valid) {
      sprintf(buf, "%.1f mph", gpsData.speedKnots * 1.15078);
    } else {
      sprintf(buf, "-- mph");
    }
    oled.print(buf);
  } else {
    oled.setCursor(0, 16);
    oled.print("No IMU");
  }
}

void drawOLEDScreenIMU() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("IMU/COMPASS");

  if (imuAvailable && magAvailable) {
    oled.setCursor(0, 12);
    sprintf(buf, "Hdg: %.0f %s", imuData.heading, getCardinal(imuData.heading));
    oled.print(buf);

    oled.setCursor(0, 24);
    sprintf(buf, "Roll: %.0f", imuData.roll);
    oled.print(buf);

    oled.setCursor(0, 36);
    sprintf(buf, "Pitch: %.0f", imuData.pitch);
    oled.print(buf);

    oled.setCursor(0, 48);
    sprintf(buf, "Accel: %.2f", imuData.accelMag);
    oled.print(buf);
  } else {
    oled.setCursor(0, 28);
    oled.print("IMU not found");
  }
}

// ============== OLED Geocache Screen (#70) ==============

void drawOLEDScreenGeocache() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("GEOCACHE");

  // Check if we have any caches and if selected cache is valid
  if (cacheListCount == 0 || selectedCacheIndex >= cacheListCount || !cacheList[selectedCacheIndex].valid) {
    oled.setCursor(0, 16);
    oled.print("No cache");
    return;
  }

  // Get reference to selected cache
  GeocacheEntry& cache = cacheList[selectedCacheIndex];

  // Distance and bearing
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);

  oled.setCursor(0, 12);
  if (useMetricUnits) {
    if (distKm >= 1.0) {
      sprintf(buf, "%.1fkm %d%c", distKm, (int)bearing, 247);
    } else {
      sprintf(buf, "%.0fm %d%c", distKm * 1000, (int)bearing, 247);
    }
  } else {
    float distMi = distKm * 0.621371;
    float distFt = distKm * 3280.84;
    if (distMi >= 1.0) {
      sprintf(buf, "%.1fmi %d%c", distMi, (int)bearing, 247);
    } else {
      sprintf(buf, "%.0fft %d%c", distFt, (int)bearing, 247);
    }
  }
  oled.print(buf);

  // Arrow direction
  float arrowAngle = bearing - imuData.heading;
  if (arrowAngle < 0) arrowAngle += 360;
  if (arrowAngle >= 360) arrowAngle -= 360;

  oled.setCursor(0, 24);
  sprintf(buf, "Arrow: %.0f%c", arrowAngle, 247);
  oled.print(buf);

  // D/T rating
  oled.setCursor(0, 36);
  sprintf(buf, "D:%.1f T:%.1f", cache.difficulty, cache.terrain);
  oled.print(buf);

  // Accuracy
  float accM = getGpsAccuracyMeters();
  oled.setCursor(0, 48);
  if (useMetricUnits) {
    sprintf(buf, "+/-%.0fm", accM);
  } else {
    sprintf(buf, "+/-%.0fft", accM * 3.28084);
  }
  oled.print(buf);
}

void drawOLEDScreenDiags() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("DIAGNOSTICS");

  oled.setCursor(0, 10);
  sprintf(buf, "BSEC: L:%c S:%c %s",
          bsecStateLoaded ? 'Y' : 'N',
          bsecStateSaved ? 'Y' : 'N',
          getIaqAccuracyText(envData.iaqAccuracy));
  oled.print(buf);

  oled.setCursor(0, 20);
  sprintf(buf, "Wx: %d/%d", weatherHistoryCount, weatherLogEntryCount);
  oled.print(buf);

  oled.setCursor(0, 30);
  sprintf(buf, "Heap: %luK", (unsigned long)ESP.getFreeHeap() / 1024);
  oled.print(buf);

  oled.setCursor(0, 40);
  if (wifiConnected) {
    oled.print(WiFi.localIP().toString());
  } else {
    oled.print("WiFi: --");
  }
}

void drawOLEDNavBar() {
  // Draw screen number indicator at bottom right
  oled.setCursor(100, 56);
  oled.setTextSize(1);
  char buf[8];
  sprintf(buf, "[%d/5]", currentScreen + 1);
  oled.print(buf);
}
