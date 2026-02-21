/*
 * Field Compass - Dual Display Firmware
 *
 * Hardware:
 * - Adafruit ESP32-S3 Feather 4MB Flash 2MB PSRAM (PID 5477)
 * - Adafruit SH1107 OLED FeatherWing 128x64 (I2C)
 * - Hosyond 3.5" ST7796U IPS TFT 480x320 with FT6336U cap touch (SPI + I2C)
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
#define FW_VERSION "0.35.2"

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <stdarg.h>
#include <Adafruit_GFX.h>
#include <TFT_eSPI.h>          // ST7796U display driver (pins in User_Setup.h)
#include <Adafruit_FT6206.h>   // FT6336U capacitive touch (I2C 0x38)
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
// Timezone handled by POSIX TZ string — see posixTZ global (#98)

// TFT Display pins (ST7796U 3.5" IPS, SPI — also configured in TFT_eSPI User_Setup.h)
// TFT_CS=18, TFT_DC=17, TFT_RST=16 defined by TFT_eSPI User_Setup.h
#define FRAM_CS   15  // A3 -> FRAM CS
#define SD_CS     10  // Adalogger FeatherWing SD slot

// Touch controller (FT6336U capacitive touch, I2C 0x38)
#define CTP_INT   14  // A4 -> Touch interrupt (active-low, CHANGE — fires on touch + release)

// Backlight control (PWM dimming)
#define TFT_BL     8  // A5 -> LED pin on display module
#define TFT_BL_PWM 255 // Default brightness (0=off, 255=full)

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

// Log level control — compile-time only (#39)
#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

#define LOG_LEVEL LOG_LEVEL_INFO  // Default: ERROR + WARN + INFO

// Screen settings
#define NUM_SCREENS 4
#define SCREEN_COMPASS   0
#define SCREEN_GEOCACHE  1  // Geocaching navigation (#70)
#define SCREEN_ENV       2
#define SCREEN_TELEMETRY 3  // Combined GPS + IMU (#97)
#define SCREEN_SETTINGS  4  // Modal overlay — outside NUM_SCREENS, not in swipe/button cycling

// Screen dimensions (landscape mode after rotation)
#define SCREEN_W 480
#define SCREEN_H 320

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

// TFT Display (ST7796U via TFT_eSPI — pins configured in User_Setup.h)
TFT_eSPI tft = TFT_eSPI();

// Sprite for flicker-free double-buffering (PSRAM-backed, 480x320x16bpp = 307KB)
TFT_eSprite spr = TFT_eSprite(&tft);
bool spriteAvailable = false;
bool forceDisplayUpdate = false;  // Skip 500ms throttle on next frame (screen change)

// Zone-based partial push system: only push changed screen regions to TFT.
// Full-frame pushSprite (~78ms SPI) causes visible flash on ST7796U mid-scan.
// Partial pushSprite of small zones (1-9ms) is invisible.
#define MAX_ZONES 20
#define ZONE_KEY_LEN 48

struct DisplayZone {
  int16_t x, y, w, h;          // Bounding rectangle on screen
  char key[ZONE_KEY_LEN];      // Formatted content string for change detection
};

static DisplayZone zonesCur[MAX_ZONES];
static DisplayZone zonesPrev[MAX_ZONES];
static uint8_t zoneCurCount = 0;
static uint8_t zonePrevCount = 0;

// Zone helper functions defined below (near display functions) to avoid
// Arduino auto-prototype ordering issues with struct types
void zoneBegin();
bool zoneMark(int16_t x, int16_t y, int16_t w, int16_t h, const char* key);
void zonePushDirty();

// Capacitive touch controller (FT6336U on I2C at 0x38)
Adafruit_FT6206 ctp = Adafruit_FT6206();
volatile bool touchDetected = false;

// Swipe gesture detection
#define SWIPE_MIN_DISTANCE  20   // Minimum px to qualify as a swipe (~4% of 480px width)
#define SWIPE_MAX_TIME_MS   600  // Maximum ms from touch-down to release
bool     swipeTracking  = false; // Currently tracking a potential swipe
int16_t  swipeStartX    = 0;     // Screen-X at touch-down (mapped from touch Y)
int16_t  swipeStartY    = 0;     // Screen-Y at touch-down (mapped from touch X)
uint32_t swipeStartTime = 0;     // millis() at touch-down

// Tap detection (for gear icon, future touch targets)
#define TAP_MAX_DISTANCE  15   // Max px movement to still count as a tap
#define TAP_MAX_TIME_MS   300  // Max ms for a tap
bool     tapFiredOnContact = false;  // Guard: true if tap already fired on touch-down

// Settings screen state
int  settingsSubScreen = 0;    // 0=menu, 1=compass cal, 2=diagnostics, ...
int  previousScreen    = 0;    // Screen to return to when exiting settings
int  settingsMenuIndex = 0;    // Currently highlighted menu item
#define SETTINGS_MENU_COUNT 6  // Configuration, Display, Compass Cal, Diagnostics, About, Factory Reset (#104)

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
int currentScreen = SCREEN_COMPASS;

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
bool touchAvailable = false;          // FT6336U capacitive touch

// Magnetometer calibration (hard-iron offsets)
float magOffsetX = 0, magOffsetY = 0, magOffsetZ = 0;
bool magCalibrated = false;
bool magCalibrating = false;
unsigned long magCalStartTime = 0;
float magCalMinX, magCalMinY, magCalMinZ;
float magCalMaxX, magCalMaxY, magCalMaxZ;
#define MAG_CAL_DURATION_MS 15000  // 15 seconds
#define MAG_MIN_MAGNITUDE 5.0      // µT — lowered for steel breadboard environment

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
bool use12Hour     = true;                                // 12-hour format default (#98)
bool useFahrenheit = true;                                // Fahrenheit default (#98)
char posixTZ[48]   = "EST5EDT,M3.2.0,M11.1.0";           // POSIX TZ string (#98)
char tzDisplayName[24] = "US Eastern";                    // Friendly TZ name (#98)
int  tzSelectedIndex   = 0;                               // Index in tzPresets[] (#98)
bool configScreenDirty = true;                            // Force redraw on entry (#98)
bool tzSelectorOpen    = false;                           // Timezone overlay active (#98)
int  tzScrollOffset    = 0;                               // Scroll position in TZ list (#98)
int  configFocusRow    = -1;                              // Button focus row: 0=TZ,1=Time,2=Temp,3=Dist (-1=none) (#98)

// Display settings (#91)
uint8_t  tftBrightness   = 255;                            // PWM 25-255, step 25
uint32_t tftSleepMs      = 0;                              // 0 = never (default)
uint32_t oledSleepMs     = 300000;                         // 5 minutes default
int      displayFocusRow = -1;                             // 0=Bright, 1=TFT timeout, 2=OLED timeout, 3=Back, 4=OK
int      compassCalFocusRow = -1;                          // 0=Start Cal, 1=Back (#89)

// Timezone presets with POSIX TZ strings (#98)
struct TZPreset {
  const char* name;
  const char* posix;
  int8_t stdOffset;  // For display: "UTC-5"
};

static const TZPreset tzPresets[] = {
  {"US Eastern",     "EST5EDT,M3.2.0,M11.1.0",        -5},
  {"US Central",     "CST6CDT,M3.2.0,M11.1.0",        -6},
  {"US Mountain",    "MST7MDT,M3.2.0,M11.1.0",        -7},
  {"US Pacific",     "PST8PDT,M3.2.0,M11.1.0",        -8},
  {"US Alaska",      "AKST9AKDT,M3.2.0,M11.1.0",      -9},
  {"US Hawaii",      "HST10",                          -10},
  {"US Arizona",     "MST7",                            -7},
  {"UTC",            "UTC0",                              0},
  {"UK / Ireland",   "GMT0BST,M3.5.0/1,M10.5.0",        0},
  {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3",      1},
  {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4",    2},
  {"Japan / Korea",  "JST-9",                             9},
  {"Australia East", "AEST-10AEDT,M10.1.0,M4.1.0/3",    10},
  {"New Zealand",    "NZST-12NZDT,M9.5.0,M4.1.0/3",    12},
};
#define TZ_PRESET_COUNT 14

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

// Log to SD + Serial only (skips web serial mirror ring buffer)
void magLogPrintln(const char* msg) {
  serialLogAppend(msg);
  serialLogAppend("\n");
  Serial.println(msg);
}

// Timestamp helper for LOG_* macros (#39)
// Returns "[HH:MM:SS] " (wall clock) or "[UUU:MM:SS] " (uptime if no time source)
const char* logTimestamp() {
  static char tsBuf[16];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    snprintf(tsBuf, sizeof(tsBuf), "[%02d:%02d:%02d] ",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    unsigned long s = millis() / 1000;
    snprintf(tsBuf, sizeof(tsBuf), "[%03lu:%02lu:%02lu] ",
             s / 3600, (s % 3600) / 60, s % 60);
  }
  return tsBuf;
}

// Severity-level log macros — compile to nothing when below LOG_LEVEL (#39)
// Usage: LOG_INFO("WiFi connected to %s", ssid);
// Output: [12:34:56] INFO  WiFi connected to tsunami

#if LOG_LEVEL >= LOG_LEVEL_ERROR
  #define LOG_ERROR(fmt, ...) logPrintf("%sERROR " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_ERROR(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
  #define LOG_WARN(fmt, ...)  logPrintf("%sWARN  " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_WARN(fmt, ...)  ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
  #define LOG_INFO(fmt, ...)  logPrintf("%sINFO  " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_INFO(fmt, ...)  ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
  #define LOG_DEBUG(fmt, ...) logPrintf("%sDEBUG " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_DEBUG(fmt, ...) ((void)0)
#endif

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
        LOG_ERROR("[LOG] SD write failed, logging disabled");
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

// ============== Touch ISR ==============

void IRAM_ATTR touchISR() {
  touchDetected = true;  // Flag only — NO I2C in ISR
}

// ============== Setup ==============

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Boot banner - capture to ring buffer for web serial
  char banner[128];
  snprintf(banner, sizeof(banner), "=================================\nField Compass Dual %s\n=================================\n\n", FW_VERSION);
  logPrintf("%s", banner);

  LOG_DEBUG("About to init TFT...");
  Serial.flush();

  // Initialize TFT first for visual feedback (TFT_eSPI handles SPI init)
  initTFT();

  LOG_DEBUG("TFT init done");
  Serial.flush();

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
  initTouch();   // FT6336U capacitive touch (I2C 0x38)

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

  // Load weather history and magnetometer calibration from SD
  if (sdAvailable) {
    loadWeatherHistory();
    loadMagCal();
    loadSettings();    // Load user prefs (#98)
    analogWrite(TFT_BL, tftBrightness);  // Apply saved brightness (#91)
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

  LOG_INFO("Log macros active (level=%d)", LOG_LEVEL);

  // Clear screen for main display
  tft.fillScreen(COLOR_BG);
}

// ============== Main Loop ==============

void loop() {
  // Handle button navigation
  handleButtons();

  // Handle touch input (interrupt-driven swipe detection)
  if (touchDetected && touchAvailable) {
    touchDetected = false;
    if (ctp.touched()) {
      TS_Point p = ctp.getPoint();
      // FT6336U reports in native portrait coords (320×480).
      // With TFT rotation=1 (landscape) + 180° panel mount:
      //   touchY (0-480) = long axis = screen horizontal
      //   touchX (0-320) = short axis = screen vertical
      int16_t screenX = 480 - p.y;  // horizontal (0-479)
      int16_t screenY = p.x;        // vertical   (0-319)

      lastActivityTime = millis();  // Reset sleep timer on touch

      if (!swipeTracking) {
        // Touch-down: start tracking
        swipeTracking  = true;
        swipeStartX    = screenX;
        swipeStartY    = screenY;
        swipeStartTime = millis();
        tapFiredOnContact = false;  // Reset guard for new touch

        // Fire-on-contact: gear icon tap (small corner hitbox — safe for immediate action)
        if (!tftSleeping && screenX >= 440 && screenY <= 34 && currentScreen != SCREEN_SETTINGS) {
          handleTap(screenX, screenY);
          tapFiredOnContact = true;
          swipeTracking = false;  // Consume gesture — don't also detect as swipe
        }
      }
    } else {
      // No touch — finger lifted (CHANGE interrupt fires on RISING edge too)
      if (swipeTracking) {
        uint32_t elapsed = millis() - swipeStartTime;
        swipeTracking = false;
        // Check for tap on release (backup path — fire-on-contact handles gear icon)
        if (!tapFiredOnContact && elapsed < TAP_MAX_TIME_MS) {
          handleTap(swipeStartX, swipeStartY);
        }
      }
    }
  }

  // Swipe timeout / completion check — poll touch state each loop iteration
  if (swipeTracking && touchAvailable) {
    if (ctp.touched()) {
      TS_Point p = ctp.getPoint();
      int16_t screenX = 480 - p.y;
      int16_t screenY = p.x;

      int16_t deltaX = screenX - swipeStartX;
      int16_t deltaY = screenY - swipeStartY;
      uint32_t elapsed = millis() - swipeStartTime;

      // Normalize deltaY for 480:320 (3:2) aspect ratio so physical angles are accurate
      // Then require 1.5:1 ratio (rejects physical angles > ~34° off horizontal)
      int16_t normDY = abs(deltaY) * 3 / 2;  // Scale 320-range up to match 480-range
      if (elapsed < SWIPE_MAX_TIME_MS && abs(deltaX) >= SWIPE_MIN_DISTANCE
          && abs(deltaX) * 2 > normDY * 3) {
        swipeTracking = false;  // Consume the gesture

        // Swipe cycling is disabled on the Settings modal screen
        if (currentScreen != SCREEN_SETTINGS) {
          if (deltaX < 0) {
            // Left swipe → next screen
            logPrintf("[SWIPE] LEFT → next screen\n");
            currentScreen++;
            if (currentScreen >= NUM_SCREENS) currentScreen = 0;
          } else {
            // Right swipe → previous screen
            logPrintf("[SWIPE] RIGHT → prev screen\n");
            currentScreen--;
            if (currentScreen < 0) currentScreen = NUM_SCREENS - 1;
          }
          geocacheSubScreen = 0;  // Reset sub-screen when swiping away
          if (spriteAvailable) forceDisplayUpdate = true;
          else tft.fillScreen(COLOR_BG);
        }

        // Debounce: wait for finger to lift before allowing next swipe
        delay(150);
        touchDetected = false;
      } else if (elapsed >= SWIPE_MAX_TIME_MS) {
        // Took too long — not a swipe (maybe a long press or tap)
        swipeTracking = false;
      }
    } else {
      // Finger lifted before swipe threshold — check if it's a tap
      swipeTracking = false;
      uint32_t elapsed = millis() - swipeStartTime;
      if (!tapFiredOnContact && elapsed < TAP_MAX_TIME_MS) {
        // Short touch with no significant movement → tap
        handleTap(swipeStartX, swipeStartY);
      }
    }
  }

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
    logPrintf("%s", buf);

    // TFT debug logging (P1 blank bug investigation)
    #if DEBUG_TFT
    unsigned long tftAge = (millis() - lastTFTUpdate) / 1000;
    unsigned long reinitAge = (millis() - lastTFTReinit) / 1000;
    snprintf(buf, sizeof(buf), "[TFT] sleep:%d scr:%d upd:%lus ago reinit:%lus ago cnt:%lu\n",
             tftSleeping, currentScreen, tftAge, reinitAge, tftUpdateCount);
    logPrintf("%s", buf);
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
      } else if (address == 0x38) {
        desc = " (FT6336U - Cap Touch)";
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
  logPrint("Initializing ST7796U TFT... ");

  tft.init();

  // Turn on backlight via PWM
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, TFT_BL_PWM);

  tft.setRotation(1);  // Landscape mode (480x320) — rotation 1 for Hosyond MSP3526 panel
  tft.fillScreen(TFT_RED);  // Flash red to confirm TFT is working
  delay(100);
  tft.fillScreen(COLOR_BG);

  lastTFTReinit = millis();  // Track init time

  // Create PSRAM-backed sprite for flicker-free double-buffering
  if (psramFound()) {
    void* ptr = spr.createSprite(SCREEN_W, SCREEN_H);
    if (ptr) {
      spriteAvailable = true;
      spr.fillSprite(COLOR_BG);
      logPrintf("OK (480x320) + Sprite (%dKB PSRAM, %dKB free)\n",
                (SCREEN_W * SCREEN_H * 2) / 1024, ESP.getFreePsram() / 1024);
    } else {
      logPrintln("OK (480x320) — sprite alloc failed, direct draw");
    }
  } else {
    logPrintln("OK (480x320) — no PSRAM, direct draw");
  }
}

// Preventive TFT re-initialization (P1 blank bug workaround)
// TFT goes blank after ~40 minutes with no errors - re-init as workaround
// With sprite mode, pushSprite overwrites every pixel each frame — self-healing.
// Only needed in direct-draw mode as a safety net.
void checkTFTHealth() {
  // Sprite mode: pushSprite(0,0) writes all 153,600 pixels every frame,
  // so any SPI glitch is corrected within 500ms. No reinit needed.
  if (spriteAvailable) return;

  unsigned long now = millis();

  // Skip if TFT is sleeping (intentional blank)
  if (tftSleeping) return;

  // Preventive re-init every 30 minutes (direct-draw mode only)
  if (now - lastTFTReinit > TFT_REINIT_INTERVAL) {
    #if DEBUG_TFT
    logPrintf("[TFT] Preventive re-init at %lus (updates:%lu)\n",
              now / 1000, tftUpdateCount);
    #endif

    // Re-initialize TFT
    tft.init();
    tft.setRotation(1);
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

void initTouch() {
  logPrint("Initializing FT6336U touch... ");

  if (!ctp.begin(40)) {  // 40 = sensitivity threshold
    logPrintln("NOT FOUND at 0x38");
    return;
  }

  touchAvailable = true;

  // Configure interrupt pin (CTP_INT is active-low, open-drain)
  pinMode(CTP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CTP_INT), touchISR, CHANGE);

  logPrintln("OK (interrupt on GPIO 14)");
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

  time_t t = mktimeUTC(&timeinfo);  // RTC stores UTC — use UTC-aware mktime (#98 bugfix)
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

// Load magnetometer calibration from SD card
void loadMagCal() {
  if (!sdHealth.available) return;

  File f = SD.open("/config/mag_cal.txt", FILE_READ);
  if (!f) return;

  char line[64];
  int idx = 0;
  while (f.available() && idx < 63) {
    char c = f.read();
    if (c == '\n' || c == '\r') break;
    line[idx++] = c;
  }
  line[idx] = '\0';
  f.close();

  float x, y, z;
  if (sscanf(line, "%f,%f,%f", &x, &y, &z) == 3) {
    magOffsetX = x;
    magOffsetY = y;
    magOffsetZ = z;
    magCalibrated = true;
    char msg[64];
    sprintf(msg, "[MAG] Calibration loaded: %.1f, %.1f, %.1f", x, y, z);
    logPrintln(msg);
  }
}

// Save magnetometer calibration to SD card
void saveMagCal() {
  if (!sdHealth.available) return;

  // Ensure /config directory exists
  if (!SD.exists("/config")) {
    SD.mkdir("/config");
  }

  File f = SD.open("/config/mag_cal.txt", FILE_WRITE);
  if (!f) {
    logPrintln("[MAG] Failed to save calibration");
    return;
  }

  char line[64];
  sprintf(line, "%.2f,%.2f,%.2f", magOffsetX, magOffsetY, magOffsetZ);
  f.println(line);
  f.close();

  char msg[80];
  sprintf(msg, "[MAG] Calibration saved: %.2f, %.2f, %.2f", magOffsetX, magOffsetY, magOffsetZ);
  logPrintln(msg);
}

// ─── Settings Persistence (#98) ─────────────────────────────────────────────

// Apply POSIX timezone to system (#98)
void applyTimezone() {
  setenv("TZ", posixTZ, 1);
  tzset();
  logPrintf("[SETTINGS] TZ applied: %s (%s)\n", tzDisplayName, posixTZ);
}

// Convert struct tm (interpreted as UTC) to time_t, ignoring active POSIX TZ (#98 bugfix)
// mktime() always treats its argument as local time; this temporarily sets UTC
// so GPS/RTC UTC values are stored correctly as time_t.
time_t mktimeUTC(struct tm* tm) {
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(tm);
  setenv("TZ", posixTZ, 1);  // Restore user's TZ
  tzset();
  return t;
}

// Format current time respecting 12/24h preference (#98)
// Returns chars written. buf must be >= 16 bytes.
int formatTimeStr(char* buf, int hour, int minute, int second, bool includeSeconds) {
  if (use12Hour) {
    const char* ampm = (hour >= 12) ? "PM" : "AM";
    int h12 = hour % 12;
    if (h12 == 0) h12 = 12;
    if (includeSeconds)
      return sprintf(buf, "%d:%02d:%02d %s", h12, minute, second, ampm);
    else
      return sprintf(buf, "%d:%02d %s", h12, minute, ampm);
  } else {
    if (includeSeconds)
      return sprintf(buf, "%02d:%02d:%02d", hour, minute, second);
    else
      return sprintf(buf, "%02d:%02d", hour, minute);
  }
}

// Load user settings from SD (#98)
void loadSettings() {
  if (!sdHealth.available) return;
  File f = SD.open("/config/settings.txt", FILE_READ);
  if (!f) {
    logPrintln("[SETTINGS] No settings file, using defaults");
    applyTimezone();  // Apply default TZ
    return;
  }

  char line[80];
  while (f.available()) {
    int idx = 0;
    while (f.available() && idx < 79) {
      char c = f.read();
      if (c == '\n' || c == '\r') break;
      line[idx++] = c;
    }
    line[idx] = '\0';
    if (idx == 0) continue;

    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    const char* key = line;
    const char* val = eq + 1;

    if (strcmp(key, "use12Hour") == 0)          use12Hour = atoi(val);
    else if (strcmp(key, "useFahrenheit") == 0)  useFahrenheit = atoi(val);
    else if (strcmp(key, "useMetricUnits") == 0) useMetricUnits = atoi(val);
    else if (strcmp(key, "posixTZ") == 0)        strncpy(posixTZ, val, sizeof(posixTZ) - 1);
    else if (strcmp(key, "tzName") == 0)         strncpy(tzDisplayName, val, sizeof(tzDisplayName) - 1);
    else if (strcmp(key, "tzIndex") == 0)        tzSelectedIndex = atoi(val);
    else if (strcmp(key, "tftBrightness") == 0)  tftBrightness = constrain(atoi(val), 25, 255);
    else if (strcmp(key, "tftSleepMs") == 0)     tftSleepMs = strtoul(val, NULL, 10);
    else if (strcmp(key, "oledSleepMs") == 0)    oledSleepMs = strtoul(val, NULL, 10);
  }
  f.close();
  applyTimezone();
  logPrintf("[SETTINGS] Loaded: 12h=%d F=%d metric=%d tz=%s\n",
            use12Hour, useFahrenheit, useMetricUnits, tzDisplayName);
}

// Save user settings to SD (#98)
void saveSettings() {
  if (!sdHealth.available) return;
  if (!SD.exists("/config")) SD.mkdir("/config");
  File f = SD.open("/config/settings.txt", FILE_WRITE);
  if (!f) { logPrintln("[SETTINGS] Failed to save settings"); return; }

  f.printf("use12Hour=%d\n", use12Hour ? 1 : 0);
  f.printf("useFahrenheit=%d\n", useFahrenheit ? 1 : 0);
  f.printf("useMetricUnits=%d\n", useMetricUnits ? 1 : 0);
  f.printf("posixTZ=%s\n", posixTZ);
  f.printf("tzName=%s\n", tzDisplayName);
  f.printf("tzIndex=%d\n", tzSelectedIndex);
  f.printf("tftBrightness=%d\n", tftBrightness);
  f.printf("tftSleepMs=%lu\n", tftSleepMs);
  f.printf("oledSleepMs=%lu\n", oledSleepMs);
  f.close();
  logPrintln("[SETTINGS] Settings saved to SD");
}

// Factory reset — delete settings file and restore compiled defaults (#104)
void factoryReset() {
  // Delete stored settings
  if (sdHealth.available && SD.exists("/config/settings.txt")) {
    SD.remove("/config/settings.txt");
  }

  // Restore compiled defaults
  useFahrenheit    = true;
  use12Hour        = true;
  useMetricUnits   = false;
  strncpy(posixTZ, "EST5EDT,M3.2.0,M11.1.0", sizeof(posixTZ) - 1);
  strncpy(tzDisplayName, "US Eastern", sizeof(tzDisplayName) - 1);
  tzSelectedIndex  = 0;
  tftBrightness    = 255;
  tftSleepMs       = 0;
  oledSleepMs      = 300000;

  // Apply
  applyTimezone();
  analogWrite(TFT_BL, tftBrightness);

  logPrintln("[SETTINGS] Factory reset — all settings restored to defaults");
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
    configTime(0, 0, NTP_SERVER);   // NTP provides UTC; POSIX TZ handles offset (#98)
    applyTimezone();                 // Ensure TZ is set after configTime

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
  html += "<title>Field Compass v";
  html += FW_VERSION;
  html += "</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta name='color-scheme' content='dark light'>";
  html += "<style>";
  html += "body{font-family:sans-serif;margin:20px;background:#1a1a1a;color:#e0e0e0;}";
  html += "h1{color:#00ffff;}";
  html += "a{color:#00ff00;display:block;padding:12px 15px;margin:8px 0;";
  html += "text-decoration:none;background:#2a2a2a;border-radius:5px;border:1px solid #444;}";
  html += "a:hover{background:#3a3a3a;border-color:#00ff00;}";
  html += "</style></head><body>";
  html += "<h1>Field Compass v";
  html += FW_VERSION;
  html += "</h1>";
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
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char tbuf[16];
      formatTimeStr(tbuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, true);
      sprintf(buf, "Time:    %s GPS\n", tbuf);
    } else {
      strcpy(buf, "Time:    --:--:-- GPS\n");
    }
  } else if (ntpSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char tbuf[16];
      formatTimeStr(tbuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, true);
      sprintf(buf, "Time:    %s NTP\n", tbuf);
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
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        char tbuf[16];
        formatTimeStr(tbuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, true);
        sprintf(buf, "Time:   %s\n", tbuf);
      } else {
        strcpy(buf, "Time:   --:--:--\n");
      }
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
    if (useFahrenheit)
      sprintf(buf, "Temp:     %.1fF (%.1fC) [%s]\n", tempF, tempC, src);
    else
      sprintf(buf, "Temp:     %.1fC (%.1fF) [%s]\n", tempC, tempF, src);
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

  // Firmware info
  html += "=== Firmware ===\n";
  sprintf(buf, "Version:  %s\n", FW_VERSION);
  html += buf;
  html += "Build:    Field Compass Dual\n";
  {
    unsigned long totalSec = millis() / 1000;
    int days = totalSec / 86400;
    int hours = (totalSec % 86400) / 3600;
    int mins = (totalSec % 3600) / 60;
    int secs = totalSec % 60;
    if (days > 0) {
      sprintf(buf, "Uptime:   %dd %02d:%02d:%02d\n", days, hours, mins, secs);
    } else {
      sprintf(buf, "Uptime:   %02d:%02d:%02d\n", hours, mins, secs);
    }
    html += buf;
  }
  if (wifiConnected) {
    html += "WiFi IP:  ";
    html += WiFi.localIP().toString();
    html += "\n";
  } else {
    html += "WiFi IP:  Disconnected\n";
  }
  html += "mDNS:     fieldcompass.local\n";

  // BSEC State
  html += "\n=== BSEC State ===\n";
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
  if (psramFound()) {
    sprintf(buf, "PSRAM:     %luK / %luK (Sprite: %s)\n",
            (unsigned long)ESP.getFreePsram() / 1024,
            (unsigned long)ESP.getPsramSize() / 1024,
            spriteAvailable ? "Active" : "Failed");
  } else {
    sprintf(buf, "PSRAM:     Not detected\n");
  }
  html += buf;
  sprintf(buf, "Sprite:    %s\n", spriteAvailable ? "480x320 double-buffer" : "Direct draw (no sprite)");
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
  sprintf(buf, "Touch:   %s\n", touchAvailable ? "OK (FT6336U)" : "N/A");
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

  // Mag Calibration
  html += "\n=== Mag Calibration ===\n";
  if (magCalibrated) {
    html += "Status:   Calibrated\n";
    sprintf(buf, "Offsets:  X=%.2f Y=%.2f Z=%.2f\n", magOffsetX, magOffsetY, magOffsetZ);
    html += buf;
  } else if (magCalibrating) {
    unsigned long elapsed = (millis() - magCalStartTime) / 1000;
    sprintf(buf, "Status:   Calibrating (%lus / 15s)\n", elapsed);
    html += buf;
  } else {
    html += "Status:   Not Calibrated\n";
    html += "          (Settings > Compass Cal)\n";
  }

  // Temp comparison SHT41 vs BME688 (#48)
  html += "\n=== Temperature Comparison ===\n";
  if (shtAvailable) {
    float shtF = shtData.temperature * 9.0 / 5.0 + 32.0;
    if (useFahrenheit)
      sprintf(buf, "SHT41:   %.1fF (%.1fC)\n", shtF, shtData.temperature);
    else
      sprintf(buf, "SHT41:   %.1fC (%.1fF)\n", shtData.temperature, shtF);
    html += buf;
    sprintf(buf, "SHT41 H: %.1f%%\n", shtData.humidity);
    html += buf;
  } else {
    html += "SHT41:   N/A\n";
  }
  if (bmeAvailable) {
    float bmeF = envData.temperature * 9.0 / 5.0 + 32.0;
    if (useFahrenheit)
      sprintf(buf, "BME688:  %.1fF (%.1fC)\n", bmeF, envData.temperature);
    else
      sprintf(buf, "BME688:  %.1fC (%.1fF)\n", envData.temperature, bmeF);
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
  html += ":root{--bg:#1a1a1a;--fg:#e0e0e0;--log-bg:#000;--log-fg:#00ff00;--btn-bg:#2a2a2a;--btn-fg:#00ff00;--btn-border:#444;--link:#00ffff;--header:#00ffff;--warn:#ffcc00;--error:#ff4444;}";
  // Light theme override
  html += "@media(prefers-color-scheme:light){:root{--bg:#f5f5f5;--fg:#222;--log-bg:#222;--log-fg:#00cc00;--btn-bg:#ddd;--btn-fg:#006600;--btn-border:#999;--link:#006666;--header:#008888;--warn:#cc8800;--error:#cc0000;}}";
  html += "body{font-family:sans-serif;margin:0;padding:10px;background:var(--bg);color:var(--fg);}";
  html += ".log{background:var(--log-bg);color:var(--log-fg);font-family:monospace;padding:10px;font-size:12px;";
  html += "height:500px;overflow-y:auto;border-radius:8px;white-space:pre-wrap;}";
  html += ".log .w{color:var(--warn);}.log .e{color:var(--error);}";
  html += ".controls{margin:10px 0;display:flex;align-items:center;gap:8px;}";
  html += "button{padding:8px 16px;background:var(--btn-bg);color:var(--btn-fg);";
  html += "border:1px solid var(--btn-border);border-radius:4px;cursor:pointer;}";
  html += "button:hover{opacity:0.8;}";
  html += ".badge{display:inline-block;padding:4px 12px;border-radius:12px;font-size:12px;";
  html += "background:#333;color:#aaa;cursor:pointer;user-select:none;}";
  html += ".badge.live{background:#004400;color:#00ff00;}";
  html += "a{color:var(--link);}";
  html += "</style></head><body>";
  html += "<h2 style='color:var(--header);margin:0 0 10px 0;'>Serial Log</h2>";
  html += "<div class='log' id='log'></div>";
  html += "<div class='controls'>";
  html += "<button onclick='copyLog()'>Copy</button>";
  html += "<button onclick='clearLog()'>Clear</button>";
  html += "<span class='badge live' id='scBtn' onclick='toggleScroll()'>Live &#x25BC;</span>";
  html += "<a href='/'>Back</a>";
  html += "</div>";
  html += "<script>";
  // Core state
  html += "var log=document.getElementById('log');";
  html += "var scBtn=document.getElementById('scBtn');";
  html += "var autoScroll=true;";
  // Line classification for colorization
  html += "function cls(l){";
  html += "if(/ERROR|FAILED/i.test(l))return 'e';";
  html += "if(/WARN|NOT FOUND|Dropout/i.test(l))return 'w';";
  html += "return '';";
  html += "}";
  // Append text line-by-line with color spans
  html += "function appendLines(t){";
  html += "var lines=t.split('\\n');";
  html += "for(var i=0;i<lines.length;i++){";
  html += "if(i===lines.length-1&&lines[i]==='')break;";
  html += "var s=document.createElement('span');";
  html += "var c=cls(lines[i]);";
  html += "if(c)s.className=c;";
  html += "s.textContent=lines[i]+(i<lines.length-1?'\\n':'');";
  html += "log.appendChild(s);";
  html += "}";
  html += "}";
  // Check if scrolled near bottom
  html += "function nearBottom(){return log.scrollHeight-log.scrollTop-log.clientHeight<30;}";
  // Scroll event: auto-detect pause/resume
  html += "log.addEventListener('scroll',function(){";
  html += "if(nearBottom()){if(!autoScroll){autoScroll=true;updBtn();}}";
  html += "else{if(autoScroll){autoScroll=false;updBtn();}}";
  html += "});";
  // Update badge appearance
  html += "function updBtn(){";
  html += "if(autoScroll){scBtn.textContent='Live \\u25BC';scBtn.className='badge live';}";
  html += "else{scBtn.textContent='Paused \\u25B6';scBtn.className='badge';}";
  html += "}";
  // Manual toggle via badge click
  html += "function toggleScroll(){";
  html += "autoScroll=!autoScroll;";
  html += "if(autoScroll)log.scrollTop=log.scrollHeight;";
  html += "updBtn();";
  html += "}";
  // Poll for new data
  html += "async function poll(){";
  html += "try{";
  html += "var r=await fetch('/serial-data');";
  html += "var t=await r.text();";
  html += "if(t.length>0){appendLines(t);if(autoScroll)log.scrollTop=log.scrollHeight;}";
  html += "}catch(e){}";
  html += "}";
  // Clear log
  html += "function clearLog(){log.innerHTML='';autoScroll=true;updBtn();}";
  // Copy log (try Clipboard API first, textarea fallback for HTTP)
  html += "function copyLog(){";
  html += "try{";
  html += "navigator.clipboard.writeText(log.textContent)";
  html += ".then(function(){alert('Copied!');})";
  html += ".catch(function(){copyFallback();});";
  html += "}catch(e){copyFallback();}";
  html += "}";
  html += "function copyFallback(){";
  html += "var ta=document.createElement('textarea');";
  html += "ta.value=log.textContent;";
  html += "ta.style.position='fixed';ta.style.left='-9999px';";
  html += "document.body.appendChild(ta);ta.select();";
  html += "try{document.execCommand('copy');alert('Copied!');}catch(e){alert('Copy failed');}";
  html += "document.body.removeChild(ta);";
  html += "}";
  // Start polling
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

  LOG_INFO("Starting web server...");

  // Setup mDNS
  if (MDNS.begin("fieldcompass")) {
    LOG_INFO("mDNS OK (fieldcompass.local)");
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
  LOG_INFO("Web server OK — http://%s/", WiFi.localIP().toString().c_str());
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
  analogWrite(TFT_BL, 0);            // Backlight off
  tft.writecommand(0x10);  // MIPI DCS Sleep In
  #if DEBUG_SLEEP
  Serial.println("TFT sleeping");
  #endif
}

void wakeTFT() {
  if (!tftSleeping) return;

  tftSleeping = false;
  tft.writecommand(0x11);  // MIPI DCS Sleep Out
  delay(120);  // ST7796U datasheet: 120ms delay after sleep out
  analogWrite(TFT_BL, tftBrightness); // Backlight on (user brightness, #91)
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

  // Check OLED sleep (0 = disabled) — uses runtime variable (#91)
  if (oledSleepMs > 0 && !oledSleeping && oledAvailable && elapsed > oledSleepMs) {
    sleepOLED();
  }

  // Check TFT sleep (0 = disabled, LCD has no burn-in risk) — uses runtime variable (#91)
  if (tftSleepMs > 0 && !tftSleeping && elapsed > tftSleepMs) {
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

  // Reinit TFT on button press only in direct-draw mode (recovers from SPI glitch/white screen)
  // With sprite double-buffering, pushSprite overwrites every pixel — self-healing
  if (!spriteAvailable) {
    tft.init();
    tft.setRotation(1);
    lastTFTReinit = now;
  }

  // Handle A/B based on current screen and sub-screen
  if (currentScreen == SCREEN_SETTINGS) {
    // Settings screen: A/B navigate menu
    handleSettingsButtons(buttonA, buttonB);
    lastButtonPress = now;
  } else if (currentScreen == SCREEN_GEOCACHE && geocacheSubScreen != 0) {
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
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
    }

    if (buttonB) {
      currentScreen++;
      if (currentScreen >= NUM_SCREENS) currentScreen = 0;
      geocacheSubScreen = 0;  // Reset sub-screen when leaving
      lastButtonPress = now;
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
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
  if (spriteAvailable) forceDisplayUpdate = true;
  else tft.fillScreen(COLOR_BG);
}

// Settings screen menu item labels
static const char* settingsMenuItems[] = {
  "Configuration",
  "Display",
  "Compass Cal",
  "Diagnostics",
  "About",
  "Factory Reset"       // #104
};

// Timeout presets for Display settings (#91)
static const uint32_t tftTimeoutPresets[]  = {0, 60000, 120000, 300000, 600000, 900000, 1800000};
static const char*    tftTimeoutLabels[]   = {"Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
static const int      TFT_TIMEOUT_COUNT    = 7;

static const uint32_t oledTimeoutPresets[] = {60000, 120000, 300000, 600000, 900000, 1800000};
static const char*    oledTimeoutLabels[]  = {"1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
static const int      OLED_TIMEOUT_COUNT   = 6;

// Handle A/B buttons on Settings screen
void handleSettingsButtons(bool buttonA, bool buttonB) {
  if (settingsSubScreen == 0) {
    // Menu: A=up, B=down
    if (buttonA && settingsMenuIndex > 0) {
      settingsMenuIndex--;
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    if (buttonB && settingsMenuIndex < SETTINGS_MENU_COUNT) {  // Includes Back button row (#98)
      settingsMenuIndex++;
      if (spriteAvailable) forceDisplayUpdate = true;
    }
  }

  // Configuration sub-screen: A/B navigation (#98)
  if (settingsSubScreen == 1) {
    if (tzSelectorOpen) {
      // TZ overlay open: A/B scroll the timezone list
      int visible = 220 / 36;  // ~6 items visible
      if (buttonA && tzScrollOffset > 0) {
        tzScrollOffset--;
        if (spriteAvailable) forceDisplayUpdate = true;
      }
      if (buttonB && tzScrollOffset + visible < TZ_PRESET_COUNT) {
        tzScrollOffset++;
        if (spriteAvailable) forceDisplayUpdate = true;
      }
    } else {
      // TZ overlay closed: A/B cycle focus through config rows + action bar (#98)
      // 0=TZ, 1=Time, 2=Temp, 3=Dist, 4=Back, 5=OK
      if (configFocusRow < 0) configFocusRow = 0;  // Initialize focus on first press
      if (buttonA && configFocusRow > 0) {
        configFocusRow--;
        if (spriteAvailable) forceDisplayUpdate = true;
      }
      if (buttonB && configFocusRow < 5) {
        configFocusRow++;
        if (spriteAvailable) forceDisplayUpdate = true;
      }
    }
  }

  // Display sub-screen: A/B = row focus navigation (#91)
  if (settingsSubScreen == 2) {
    if (displayFocusRow < 0) displayFocusRow = 0;
    if (buttonA && displayFocusRow > 0) {
      displayFocusRow--;
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    if (buttonB && displayFocusRow < 4) {
      displayFocusRow++;
      if (spriteAvailable) forceDisplayUpdate = true;
    }
  }

  // Compass Cal sub-screen: A/B = focus between Start (0) and Back (1) (#89)
  if (settingsSubScreen == 3) {
    if (compassCalFocusRow < 0) compassCalFocusRow = 0;
    if (buttonA && compassCalFocusRow > 0) {
      compassCalFocusRow--;
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    if (buttonB && compassCalFocusRow < 1) {
      compassCalFocusRow++;
      if (spriteAvailable) forceDisplayUpdate = true;
    }
  }
}

// Handle C short press on Settings screen
void handleSettingsCSelect() {
  if (settingsSubScreen == 0) {
    // Back button focused — exit settings (#98)
    if (settingsMenuIndex == SETTINGS_MENU_COUNT) {
      currentScreen = previousScreen;
      settingsSubScreen = 0;
      settingsMenuIndex = 0;
      logPrintf("[SETTINGS] Back via button C → screen %d\n", currentScreen);
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
      return;
    }
    // Menu: select highlighted item
    settingsSubScreen = settingsMenuIndex + 1;  // 1-indexed sub-screens
    configFocusRow = -1;    // Reset focus on entry (#98)
    displayFocusRow = -1;   // Reset display focus too (#91)
    compassCalFocusRow = -1; // Reset compass cal focus (#89)
    logPrintf("[SETTINGS] Selected: %s\n", settingsMenuItems[settingsMenuIndex]);
    if (spriteAvailable) forceDisplayUpdate = true;
    else tft.fillScreen(COLOR_BG);
    return;  // Prevent fall-through to sub-screen handlers (#98 bugfix)
  }

  // Configuration: C-short = toggle focused row, or OK/save if no focus (#98)
  if (settingsSubScreen == 1) {
    if (tzSelectorOpen) {
      // Confirm currently highlighted TZ
      strncpy(posixTZ, tzPresets[tzSelectedIndex].posix, sizeof(posixTZ) - 1);
      strncpy(tzDisplayName, tzPresets[tzSelectedIndex].name, sizeof(tzDisplayName) - 1);
      applyTimezone();
      tzSelectorOpen = false;
    } else if (configFocusRow >= 0) {
      // Toggle the focused config field
      switch (configFocusRow) {
        case 0:  // Timezone: open selector
          tzSelectorOpen = true;
          tzScrollOffset = max(0, tzSelectedIndex - 2);
          logPrintln("[CONFIG] TZ selector opened via button");
          break;
        case 1:  // Time format
          use12Hour = !use12Hour;
          logPrintf("[CONFIG] Time format: %s\n", use12Hour ? "12h" : "24h");
          break;
        case 2:  // Temperature
          useFahrenheit = !useFahrenheit;
          logPrintf("[CONFIG] Temp: %s\n", useFahrenheit ? "F" : "C");
          break;
        case 3:  // Distance
          useMetricUnits = !useMetricUnits;
          logPrintf("[CONFIG] Distance: %s\n", useMetricUnits ? "metric" : "imperial");
          break;
        case 4:  // Back button — discard and return
          loadSettings();
          configFocusRow = -1;
          settingsSubScreen = 0;
          logPrintln("[CONFIG] Back via button focus");
          break;
        case 5:  // OK button — save and return
          saveSettings();
          configFocusRow = -1;
          settingsSubScreen = 0;
          logPrintln("[CONFIG] OK via button focus");
          break;
      }
    } else {
      // No focus active — OK = save and return to menu
      saveSettings();
      configFocusRow = -1;
      settingsSubScreen = 0;
      logPrintln("[CONFIG] OK via button C");
    }
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Display: C-short = activate focused row (#91)
  if (settingsSubScreen == 2) {
    if (displayFocusRow >= 0) {
      switch (displayFocusRow) {
        case 0: {  // Brightness: step 25→50→...→250→255→25 (wrap)
          if (tftBrightness >= 255)      tftBrightness = 25;
          else if (tftBrightness >= 250) tftBrightness = 255;
          else                           tftBrightness += 25;
          analogWrite(TFT_BL, tftBrightness);
          logPrintf("[DISPLAY] Brightness: %d\n", tftBrightness);
          break;
        }
        case 1: {  // TFT timeout: cycle to next preset
          int idx = 0;
          for (int i = 0; i < TFT_TIMEOUT_COUNT; i++)
            if (tftTimeoutPresets[i] == tftSleepMs) { idx = i; break; }
          tftSleepMs = tftTimeoutPresets[(idx + 1) % TFT_TIMEOUT_COUNT];
          logPrintf("[DISPLAY] TFT timeout: %lu ms\n", tftSleepMs);
          break;
        }
        case 2: {  // OLED timeout: cycle to next preset
          int idx = 0;
          for (int i = 0; i < OLED_TIMEOUT_COUNT; i++)
            if (oledTimeoutPresets[i] == oledSleepMs) { idx = i; break; }
          oledSleepMs = oledTimeoutPresets[(idx + 1) % OLED_TIMEOUT_COUNT];
          logPrintf("[DISPLAY] OLED timeout: %lu ms\n", oledSleepMs);
          break;
        }
        case 3:  // Back — discard and return
          loadSettings();
          analogWrite(TFT_BL, tftBrightness);  // Restore saved brightness
          displayFocusRow = -1;
          settingsSubScreen = 0;
          logPrintln("[DISPLAY] Back via button focus");
          break;
        case 4:  // OK — save and return
          saveSettings();
          displayFocusRow = -1;
          settingsSubScreen = 0;
          logPrintln("[DISPLAY] OK via button focus");
          break;
      }
    } else {
      // No focus — default OK = save and return
      saveSettings();
      displayFocusRow = -1;
      settingsSubScreen = 0;
      logPrintln("[DISPLAY] OK via button C");
    }
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Compass Cal (3): C-short activates focused row (#89)
  if (settingsSubScreen == 3) {
    if (magCalibrating) return;  // Don't interrupt active calibration
    if (compassCalFocusRow == 0) {
      // Start Calibration
      if (magAvailable && !magCalibrating) {
        magCalibrating = true;
        magCalStartTime = millis();
        magCalMinX = magCalMinY = magCalMinZ = 99999;
        magCalMaxX = magCalMaxY = magCalMaxZ = -99999;
        logPrintln("[MAG] Calibration started from Settings");
      }
    } else if (compassCalFocusRow == 1) {
      // Back
      compassCalFocusRow = -1;
      settingsSubScreen = 0;
      logPrintln("[SETTINGS] Back (Compass Cal) via C");
    } else {
      // No focus — default Back
      compassCalFocusRow = -1;
      settingsSubScreen = 0;
    }
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Diagnostics (4): C-short = Back (read-only, no save) (#90)
  if (settingsSubScreen == 4) {
    settingsSubScreen = 0;
    logPrintln("[SETTINGS] Back (Diags) via C");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // About: C-short = Back (read-only, no save) (#92)
  if (settingsSubScreen == 5) {
    settingsSubScreen = 0;
    logPrintln("[SETTINGS] Back (About) via C");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Factory Reset (6): C-short = Back without resetting (#104)
  if (settingsSubScreen == 6) {
    settingsSubScreen = 0;
    logPrintln("[SETTINGS] Back (Factory Reset) via C");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }
}

// Handle C long press on Settings screen (back navigation)
void handleSettingsCLongPress() {
  if (settingsSubScreen > 0) {
    if (settingsSubScreen == 1) {
      // Configuration: close TZ overlay first, then discard+back (#98)
      if (tzSelectorOpen) {
        tzSelectorOpen = false;
      } else {
        loadSettings();          // Discard unsaved config changes
        configFocusRow = -1;
        settingsSubScreen = 0;
      }
    } else if (settingsSubScreen == 2) {
      // Display: discard and return (#91)
      loadSettings();
      analogWrite(TFT_BL, tftBrightness);  // Restore saved brightness
      displayFocusRow = -1;
      settingsSubScreen = 0;
    } else if (settingsSubScreen == 3) {
      // Compass Cal: cancel active calibration if running, return (#89)
      if (magCalibrating) {
        magCalibrating = false;
        logPrintln("[MAG] Calibration cancelled");
      }
      compassCalFocusRow = -1;
      settingsSubScreen = 0;
    } else {
      settingsSubScreen = 0;
    }
    logPrintf("[SETTINGS] Back to menu\n");
  } else {
    // Menu → back to previous screen
    currentScreen = previousScreen;
    settingsSubScreen = 0;
    settingsMenuIndex = 0;
    logPrintf("[SETTINGS] Exit → screen %d\n", currentScreen);
  }
  if (spriteAvailable) forceDisplayUpdate = true;
  else tft.fillScreen(COLOR_BG);
}

// Handle taps on the Configuration sub-screen (#98)
void handleConfigTap(int16_t x, int16_t y) {
  // If TZ selector overlay is open, handle it exclusively
  if (tzSelectorOpen) {
    // Overlay area: x=30..450, y=70..250 (list items region)
    if (x >= 30 && x <= 450 && y >= 70 && y < 250) {
      int itemH = 36;
      int listY = 70;  // oy(40) + title(30)
      int tappedIdx = (y - listY) / itemH + tzScrollOffset;
      if (tappedIdx >= 0 && tappedIdx < TZ_PRESET_COUNT) {
        tzSelectedIndex = tappedIdx;
        strncpy(posixTZ, tzPresets[tappedIdx].posix, sizeof(posixTZ) - 1);
        strncpy(tzDisplayName, tzPresets[tappedIdx].name, sizeof(tzDisplayName) - 1);
        applyTimezone();
        tzSelectorOpen = false;
        logPrintf("[CONFIG] TZ selected: %s\n", tzDisplayName);
      }
    } else {
      // Tap outside overlay = close it
      tzSelectorOpen = false;
    }
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Action bar: Back (x: 10-120, y: 278-312)
  if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
    loadSettings();          // Discard — reload from SD
    configFocusRow = -1;
    settingsSubScreen = 0;
    logPrintln("[CONFIG] Back (discarded)");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Action bar: OK (x: 360-470, y: 278-312)
  if (x >= 360 && x <= 470 && y >= 278 && y <= 312) {
    saveSettings();
    configFocusRow = -1;
    settingsSubScreen = 0;
    logPrintln("[CONFIG] OK (saved)");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Timezone dropdown row: y=45..75
  if (y >= 45 && y <= 75 && x >= 150) {
    tzSelectorOpen = true;
    tzScrollOffset = max(0, tzSelectedIndex - 2);  // Center selection in view
    logPrintln("[CONFIG] TZ selector opened");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Time format toggle: y=88..118
  if (y >= 88 && y <= 118) {
    if (x >= 150 && x <= 280)      { use12Hour = true;  }
    else if (x >= 290 && x <= 420) { use12Hour = false; }
    logPrintf("[CONFIG] Time format: %s\n", use12Hour ? "12h" : "24h");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Temperature toggle: y=132..162
  if (y >= 132 && y <= 162) {
    if (x >= 150 && x <= 280)      { useFahrenheit = true;  }
    else if (x >= 290 && x <= 420) { useFahrenheit = false; }
    logPrintf("[CONFIG] Temp: %s\n", useFahrenheit ? "F" : "C");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Distance toggle: y=172..202
  if (y >= 172 && y <= 202) {
    if (x >= 150 && x <= 280)      { useMetricUnits = false; }  // Imperial = left
    else if (x >= 290 && x <= 420) { useMetricUnits = true;  }  // Metric = right
    logPrintf("[CONFIG] Distance: %s\n", useMetricUnits ? "metric" : "imperial");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }
}

// Handle taps on Compass Cal sub-screen (#89)
void handleCompassCalTap(int16_t x, int16_t y) {
  if (magCalibrating) return;  // No taps during active calibration

  // Start Calibration button (x: 130-350, y: 128-162)
  if (x >= 130 && x <= 350 && y >= 128 && y <= 162 && magAvailable) {
    magCalibrating = true;
    magCalStartTime = millis();
    magCalMinX = magCalMinY = magCalMinZ = 99999;
    magCalMaxX = magCalMaxY = magCalMaxZ = -99999;
    logPrintln("[MAG] Calibration started via touch");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Back button (x: 10-120, y: 278-312)
  if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
    compassCalFocusRow = -1;
    settingsSubScreen = 0;
    logPrintln("[SETTINGS] Back (Compass Cal) via tap");
    if (spriteAvailable) forceDisplayUpdate = true;
  }
}

// Handle tap on screen (gear icon, future touch targets)
void handleTap(int16_t x, int16_t y) {
  static uint32_t lastTapTime = 0;
  if (millis() - lastTapTime < 300) return;  // Debounce: ignore taps within 300ms
  lastTapTime = millis();

  // Gear icon hitbox: right side of header bar
  if (x >= 440 && y <= 34 && currentScreen != SCREEN_SETTINGS) {
    previousScreen = currentScreen;
    currentScreen = SCREEN_SETTINGS;
    settingsSubScreen = 0;
    settingsMenuIndex = 0;
    logPrintf("[TAP] Gear icon → Settings\n");
    if (spriteAvailable) forceDisplayUpdate = true;
    else tft.fillScreen(COLOR_BG);
    return;
  }

  // Settings menu: tap on menu items (#98)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 0) {
    // Back button (x: 10-120, y: 278-312) — exit settings (#98 bugfix)
    if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
      currentScreen = previousScreen;
      settingsSubScreen = 0;
      settingsMenuIndex = 0;
      logPrintf("[SETTINGS] Back → screen %d\n", currentScreen);
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
      return;
    }
    int startY = 45, itemH = 40;
    for (int i = 0; i < SETTINGS_MENU_COUNT; i++) {
      int iy = startY + i * itemH;
      if (x >= 10 && x <= SCREEN_W - 10 && y >= iy && y <= iy + itemH - 4) {
        settingsMenuIndex = i;
        settingsSubScreen = i + 1;
        logPrintf("[TAP] Settings menu: %s\n", settingsMenuItems[i]);
        if (spriteAvailable) forceDisplayUpdate = true;
        return;
      }
    }
    return;
  }

  // Configuration sub-screen taps (#98)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 1) {
    handleConfigTap(x, y);
    return;
  }

  // Display sub-screen taps (#91)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 2) {
    handleDisplayTap(x, y);
    return;
  }

  // Compass Cal taps (3) (#89)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 3) {
    handleCompassCalTap(x, y);
    return;
  }

  // Diagnostics taps (4): Back button only (#90)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 4) {
    if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
      settingsSubScreen = 0;
      logPrintln("[SETTINGS] Back (Diags)");
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    return;
  }

  // About sub-screen taps: Back button only (#92)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 5) {
    if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
      settingsSubScreen = 0;
      logPrintln("[SETTINGS] Back (About)");
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    return;
  }

  // Factory Reset sub-screen taps (#104)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 6) {
    // Back button tap
    if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
      settingsSubScreen = 0;
      logPrintln("[SETTINGS] Back (Factory Reset)");
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    // Reset button tap
    if (x >= 360 && x <= 470 && y >= 278 && y <= 312) {
      factoryReset();
      settingsSubScreen = 0;
      logPrintln("[SETTINGS] Factory reset confirmed via tap");
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    return;
  }
}

// Button C short press handler
void handleButtonCShortPress() {
  // Wake displays if sleeping
  if (tftSleeping || oledSleeping) {
    wakeAllDisplays();
    return;
  }

  lastActivityTime = millis();

  if (currentScreen == SCREEN_SETTINGS) {
    // Settings screen: C short press = select menu item or action
    handleSettingsCSelect();
    return;
  }

  if (currentScreen == SCREEN_GEOCACHE) {
    if (geocacheSubScreen == 0) {
      // Nav screen: short press goes to list
      geocacheSubScreen = 1;
      listHighlightIndex = selectedCacheIndex;
      listScrollOffset = max(0, listHighlightIndex - 2);
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
    } else if (geocacheSubScreen == 1) {
      // List screen: short press selects cache and returns to nav
      selectedCacheIndex = listHighlightIndex;
      geocacheSubScreen = 0;
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
    } else if (geocacheSubScreen == 2) {
      // Details screen: short press toggles found status
      if (cacheListCount > 0 && listHighlightIndex < cacheListCount) {
        cacheList[listHighlightIndex].found = !cacheList[listHighlightIndex].found;
        if (cacheList[listHighlightIndex].found) {
          cacheList[listHighlightIndex].foundTime = millis() / 1000;  // Simple timestamp
        }
        saveCacheFoundStatus();  // Persist to SD
        if (spriteAvailable) forceDisplayUpdate = true;
        else tft.fillScreen(COLOR_BG);
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

  if (currentScreen == SCREEN_SETTINGS) {
    // Settings: long press = back navigation
    handleSettingsCLongPress();
    return;
  }

  // Compass calibration removed from C-long-press — now in Settings > Compass Cal (#89)

  if (currentScreen == SCREEN_GEOCACHE) {
    if (geocacheSubScreen == 1) {
      // List screen: long press goes to details
      geocacheSubScreen = 2;
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
    } else if (geocacheSubScreen == 2) {
      // Details screen: long press goes back to list
      geocacheSubScreen = 1;
      if (spriteAvailable) forceDisplayUpdate = true;
      else tft.fillScreen(COLOR_BG);
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

        time_t t = mktimeUTC(&gpsTime);  // GPS provides UTC — use UTC-aware mktime (#98 bugfix)
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
      LOG_ERROR("BSEC error: %d", envSensor.status);
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
  float magZ = mag.magnetic.z;

  // Dropout rejection: check field magnitude before using values
  float magMagnitude = sqrt(magX * magX + magY * magY + magZ * magZ);
  if (magMagnitude < MAG_MIN_MAGNITUDE) {
    // I2C dropout — keep previous heading
    static unsigned long lastDropoutLog = 0;
    if (millis() - lastDropoutLog > 5000) {
      lastDropoutLog = millis();
      char dbg[64];
      sprintf(dbg, "[MAG] Dropout (mag=%.1f uT), keeping heading", magMagnitude);
      magLogPrintln(dbg);
    }
    return;
  }

  // Calibration min/max tracking
  if (magCalibrating) {
    if (magX < magCalMinX) magCalMinX = magX;
    if (magX > magCalMaxX) magCalMaxX = magX;
    if (magY < magCalMinY) magCalMinY = magY;
    if (magY > magCalMaxY) magCalMaxY = magY;
    if (magZ < magCalMinZ) magCalMinZ = magZ;
    if (magZ > magCalMaxZ) magCalMaxZ = magZ;
  }

  // Apply hard-iron calibration offsets
  float calX = magX - magOffsetX;
  float calY = magY - magOffsetY;

  float rawHeading = atan2(calY, calX) * 180.0 / PI;
  if (rawHeading < 0) rawHeading += 360;

  // Exponential moving average with circular wrap handling
  // Alpha 0.05 = steady when still, settles in ~3-4s on rotation
  static float smoothedHeading = -1;
  if (smoothedHeading < 0) {
    smoothedHeading = rawHeading;  // First reading — no history
  } else {
    float diff = rawHeading - smoothedHeading;
    if (diff > 180) diff -= 360;
    if (diff < -180) diff += 360;
    smoothedHeading += 0.05 * diff;
    if (smoothedHeading < 0) smoothedHeading += 360;
    if (smoothedHeading >= 360) smoothedHeading -= 360;
  }
  imuData.heading = smoothedHeading;

  // Debug: raw mag values + magnitude (every 1s for diagnostic visibility)
  static unsigned long lastMagDebug = 0;
  if (millis() - lastMagDebug > 1000) {
    lastMagDebug = millis();
    char dbg[96];
    sprintf(dbg, "[MAG] X=%.1f Y=%.1f Z=%.1f  Mag=%.1fuT  Cal:%.1f,%.1f  Hdg=%.0f",
            magX, magY, magZ, magMagnitude, calX, calY, imuData.heading);
    magLogPrintln(dbg);
  }
}

// ============== Display Functions ==============

// Zone helper implementations (prototypes declared near DisplayZone struct)
void zoneBegin() {
  zoneCurCount = 0;
}

bool zoneMark(int16_t x, int16_t y, int16_t w, int16_t h, const char* key) {
  if (zoneCurCount >= MAX_ZONES) return true;
  uint8_t idx = zoneCurCount++;
  DisplayZone& z = zonesCur[idx];
  z.x = x; z.y = y; z.w = w; z.h = h;
  strncpy(z.key, key, ZONE_KEY_LEN - 1);
  z.key[ZONE_KEY_LEN - 1] = '\0';
  if (idx < zonePrevCount) {
    return strcmp(z.key, zonesPrev[idx].key) != 0;
  }
  return true;
}

void zonePushDirty() {
  if (!spriteAvailable) return;
  for (uint8_t i = 0; i < zoneCurCount; i++) {
    if (i >= zonePrevCount || strcmp(zonesCur[i].key, zonesPrev[i].key) != 0) {
      DisplayZone& z = zonesCur[i];
      spr.pushSprite(z.x, z.y, z.x, z.y, z.w, z.h);
    }
  }
  memcpy(zonesPrev, zonesCur, sizeof(DisplayZone) * zoneCurCount);
  zonePrevCount = zoneCurCount;
}

void updateDisplay() {
  static unsigned long lastUpdate = 0;

  // Save forceDisplayUpdate BEFORE clearing (race fix)
  bool wasForced = forceDisplayUpdate;
  forceDisplayUpdate = false;

  // Throttle: update every 500ms, or immediately if forced
  if (!wasForced && millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  // Update TFT display (if not sleeping)
  if (!tftSleeping) {
    TFT_eSprite* c = &spr;

    // On screen change: clear sprite + push full black frame to TFT to erase old content,
    // then invalidate all zones so the new screen's first frame pushes everything.
    static int lastDrawnScreen = -1;
    if (spriteAvailable && (currentScreen != lastDrawnScreen || wasForced)) {
      spr.fillSprite(COLOR_BG);
      spr.pushSprite(0, 0);  // Full push to clear old screen artifacts from TFT
      zonePrevCount = 0;     // Force all zones dirty on next frame
      lastDrawnScreen = currentScreen;
    }

    // Each drawScreenXxx calls zoneBegin(), registers zones with zoneMark(),
    // and only draws content for dirty zones. drawNavBar is called inside each screen.
    switch (currentScreen) {
      case SCREEN_COMPASS:   drawScreenCompass(c);   break;
      case SCREEN_GEOCACHE:  drawScreenGeocache(c);  break;
      case SCREEN_ENV:       drawScreenEnv(c);       break;
      case SCREEN_TELEMETRY: drawScreenTelemetry(c); break;
      case SCREEN_SETTINGS:  drawScreenSettings(c);  break;
    }

    // Push only dirty zones to TFT (small partial SPI transfers = no visible flash)
    zonePushDirty();

    // Track TFT update for health monitoring
    lastTFTUpdate = millis();
    tftUpdateCount++;
  }

  // Update OLED display (if available and not sleeping)
  if (oledAvailable && !oledSleeping) {
    updateOLED();
  }
}

void drawHeader(TFT_eSprite* c, const char* title) {
  c->fillRect(0, 0, SCREEN_W, 30, COLOR_HEADER);
  c->setTextColor(COLOR_BG);
  c->setTextSize(2);
  c->setCursor(10, 7);
  c->print(title);

  // Draw gear icon on cycling screens (not on Settings itself)
  if (currentScreen != SCREEN_SETTINGS) {
    int gx = 458, gy = 15;  // Center of gear icon
    int r = 8;               // Outer radius
    c->fillCircle(gx, gy, 5, COLOR_BG);       // Gear body
    c->fillCircle(gx, gy, 2, COLOR_HEADER);   // Center hole
    // Draw 6 teeth around the gear
    for (int i = 0; i < 6; i++) {
      float angle = i * PI / 3.0;
      int tx = gx + (int)(r * cos(angle));
      int ty = gy + (int)(r * sin(angle));
      c->fillCircle(tx, ty, 2, COLOR_BG);
    }
  }
}

void drawNavBar(TFT_eSprite* c) {
  int y = SCREEN_H - 25;
  c->fillRect(0, y, SCREEN_W, 25, 0x18C3);  // Dark gray

  c->setTextColor(COLOR_TEXT);
  c->setTextSize(2);

  // Screen indicators - adjusted spacing for 4 screens (#97)
  int startX = 80;
  int spacing = 40;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (i == currentScreen) {
      c->fillRect(startX + i * spacing, y + 3, 28, 19, COLOR_HEADER);
      c->setTextColor(COLOR_BG);
    } else {
      c->setTextColor(COLOR_DIM);
    }
    c->setCursor(startX + 6 + i * spacing, y + 5);
    c->print(i + 1);
    c->setTextColor(COLOR_TEXT);
  }

  // Button hints
  c->setTextSize(1);
  c->setCursor(10, y + 8);
  c->print("A<");
  c->setCursor(SCREEN_W - 25, y + 8);
  c->print(">B");
}

void drawLabel(TFT_eSprite* c, int x, int y, const char* label) {
  c->setTextColor(COLOR_DIM);
  c->setTextSize(2);
  c->setCursor(x, y);
  c->print(label);
}

void drawValue(TFT_eSprite* c, int x, int y, const char* value, uint16_t color = COLOR_VALUE, int clearWidth = 200) {
  c->fillRect(x, y, clearWidth, 18, COLOR_BG);  // Clear value area before drawing
  c->setTextColor(color);
  c->setTextSize(2);
  c->setCursor(x, y);
  c->print(value);
}

// drawScreenOps() removed — content now in Settings > About (#102)

// Combined Telemetry screen — GPS data + IMU data (#97)
void drawScreenTelemetry(TFT_eSprite* c) {
  zoneBegin();
  char buf[ZONE_KEY_LEN];

  // Column geometry
  int leftLabelX = 20;
  int leftValueX = 100;
  int rightLabelX = 250;
  int rightValueX = 340;
  int lineH = 30;

  // Zone 0: Header
  if (zoneMark(0, 0, SCREEN_W, 30, "TELEMETRY"))
    drawHeader(c, "TELEMETRY");

  // ============ GPS Section (y=36..170) ============

  // GPS mode transition clearing — force redraw on state change
  int gpsMode = gpsData.valid ? 0 : (gpsData.receiving ? 1 : 2);
  static int lastTelemetryGpsMode = -1;
  if (gpsMode != lastTelemetryGpsMode) {
    c->fillRect(0, 30, SCREEN_W, 142, COLOR_BG);  // Clear GPS section
    spr.pushSprite(0, 0);
    zonePrevCount = 0;
    lastTelemetryGpsMode = gpsMode;
  }

  // Section label
  if (zoneMark(0, 36, SCREEN_W, 14, "GPS_HDR")) {
    c->setTextColor(COLOR_HEADER);
    c->setTextSize(1);
    int labelW = 11 * 6;  // "=== GPS ===" = 11 chars
    c->setCursor((SCREEN_W - labelW) / 2, 38);
    c->print("=== GPS ===");
  }

  if (gpsData.valid) {
    int y = 56;

    // Row 1: Lat / Lon
    if (zoneMark(leftLabelX, y, 70, 18, "Lat:"))
      drawLabel(c, leftLabelX, y, "Lat:");
    sprintf(buf, "%.6f %c", fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    if (zoneMark(leftValueX, y, 140, 18, buf))
      drawValue(c, leftValueX, y, buf, COLOR_VALUE, 140);

    if (zoneMark(rightLabelX, y, 80, 18, "Lon:"))
      drawLabel(c, rightLabelX, y, "Lon:");
    sprintf(buf, "%.6f %c", fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    if (zoneMark(rightValueX, y, 140, 18, buf))
      drawValue(c, rightValueX, y, buf, COLOR_VALUE, 140);
    y += lineH;

    // Row 2: Alt / Spd
    if (zoneMark(leftLabelX, y, 70, 18, "Alt:"))
      drawLabel(c, leftLabelX, y, "Alt:");
    {
      float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
      sprintf(buf, "%.1f %s", alt, useMetricUnits ? "m" : "ft");
    }
    if (zoneMark(leftValueX, y, 140, 18, buf))
      drawValue(c, leftValueX, y, buf, COLOR_VALUE, 140);

    if (zoneMark(rightLabelX, y, 80, 18, "Spd:"))
      drawLabel(c, rightLabelX, y, "Spd:");
    {
      float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
      sprintf(buf, "%.1f %s", speed, useMetricUnits ? "km/h" : "mph");
    }
    if (zoneMark(rightValueX, y, 140, 18, buf))
      drawValue(c, rightValueX, y, buf, COLOR_VALUE, 140);
    y += lineH;

    // Row 3: Sat / HDOP
    if (zoneMark(leftLabelX, y, 70, 18, "Sat:"))
      drawLabel(c, leftLabelX, y, "Sat:");
    sprintf(buf, "%d", gpsData.satellites);
    if (zoneMark(leftValueX, y, 140, 18, buf))
      drawValue(c, leftValueX, y, buf, COLOR_VALUE, 140);

    if (zoneMark(rightLabelX, y, 80, 18, "HDOP:"))
      drawLabel(c, rightLabelX, y, "HDOP:");
    sprintf(buf, "%.1f", gpsData.hdop);
    if (zoneMark(rightValueX, y, 140, 18, buf))
      drawValue(c, rightValueX, y, buf, COLOR_VALUE, 140);
    y += lineH;

    // Row 4: Status (full width)
    if (zoneMark(leftLabelX, y, 110, 18, "Status:"))
      drawLabel(c, leftLabelX, y, "Status:");
    if (gpsHadFirstFix) {
      sprintf(buf, "Fix OK (TTFF %lus)", gpsFirstFixTime / 1000);
    } else {
      strcpy(buf, "Fix OK");
    }
    if (zoneMark(leftValueX, y, 300, 18, buf))
      drawValue(c, leftValueX, y, buf, COLOR_VALUE, 300);

  } else if (gpsData.receiving) {
    // Acquiring state
    if (zoneMark(60, 60, 300, 20, "Acquiring fix...")) {
      c->fillRect(60, 60, 300, 20, COLOR_BG);
      c->setTextColor(COLOR_WARN);
      c->setTextSize(2);
      c->setCursor(60, 60);
      c->print("Acquiring fix...");
    }

    unsigned long elapsed = millis() / 1000;
    sprintf(buf, "Elapsed: %lum %lus", elapsed / 60, elapsed % 60);
    if (zoneMark(60, 86, 300, 20, buf)) {
      c->fillRect(60, 86, 300, 20, COLOR_BG);
      c->setTextColor(COLOR_DIM);
      c->setTextSize(2);
      c->setCursor(60, 86);
      c->print(buf);
    }

    if (zoneMark(60, 112, 300, 20, "Need clear sky view")) {
      c->setTextColor(COLOR_DIM);
      c->setTextSize(2);
      c->setCursor(60, 112);
      c->print("Need clear sky view");
    }

    sprintf(buf, "Sats: %d", gpsData.satellites);
    if (zoneMark(60, 138, 300, 20, buf)) {
      c->fillRect(60, 138, 300, 20, COLOR_BG);
      c->setTextColor(COLOR_VALUE);
      c->setTextSize(2);
      c->setCursor(60, 138);
      c->print(buf);
    }

  } else {
    // No GPS
    if (zoneMark(80, 80, 300, 20, "No GPS data")) {
      c->setTextColor(COLOR_ERROR);
      c->setTextSize(2);
      c->setCursor(80, 80);
      c->print("No GPS data");
    }
    if (zoneMark(60, 116, 300, 20, "Check connection")) {
      c->setTextColor(COLOR_DIM);
      c->setTextSize(2);
      c->setCursor(60, 116);
      c->print("Check connection");
    }
  }

  // ============ Divider ============
  c->drawLine(0, 172, SCREEN_W, 172, COLOR_DIM);

  // ============ IMU Section (y=176..264) ============

  // Section label
  if (zoneMark(0, 176, SCREEN_W, 14, "IMU_HDR")) {
    c->setTextColor(COLOR_HEADER);
    c->setTextSize(1);
    int labelW = 11 * 6;  // "=== IMU ===" = 11 chars
    c->setCursor((SCREEN_W - labelW) / 2, 178);
    c->print("=== IMU ===");
  }

  if (imuAvailable && magAvailable) {
    int y = 196;

    // Row 5: Heading+Cardinal / Roll
    if (zoneMark(leftLabelX, y, 70, 18, "Hdg:"))
      drawLabel(c, leftLabelX, y, "Hdg:");
    sprintf(buf, "%.0f %s", imuData.heading, getCardinal(imuData.heading));
    if (zoneMark(leftValueX, y, 140, 18, buf))
      drawValue(c, leftValueX, y, buf, COLOR_VALUE, 140);

    if (zoneMark(rightLabelX, y, 80, 18, "Roll:"))
      drawLabel(c, rightLabelX, y, "Roll:");
    sprintf(buf, "%.0f deg", imuData.roll);
    if (zoneMark(rightValueX, y, 140, 18, buf))
      drawValue(c, rightValueX, y, buf, COLOR_VALUE, 140);
    y += lineH;

    // Row 6: Pitch / Accel
    if (zoneMark(leftLabelX, y, 70, 18, "Pitch:"))
      drawLabel(c, leftLabelX, y, "Pitch:");
    sprintf(buf, "%.0f deg", imuData.pitch);
    if (zoneMark(leftValueX, y, 140, 18, buf))
      drawValue(c, leftValueX, y, buf, COLOR_VALUE, 140);

    if (zoneMark(rightLabelX, y, 80, 18, "Accel:"))
      drawLabel(c, rightLabelX, y, "Accel:");
    sprintf(buf, "%.2f m/s2", imuData.accelMag);
    if (zoneMark(rightValueX, y, 140, 18, buf))
      drawValue(c, rightValueX, y, buf, COLOR_VALUE, 140);

  } else {
    if (zoneMark(60, 210, 300, 20, "IMU not available")) {
      c->setTextColor(COLOR_ERROR);
      c->setTextSize(2);
      c->setCursor(60, 210);
      c->print("IMU not available");
    }
  }

  // NavBar
  sprintf(buf, "nav_%d", currentScreen);
  if (zoneMark(0, SCREEN_H - 25, SCREEN_W, 25, buf))
    drawNavBar(c);
}

void drawScreenEnv(TFT_eSprite* c) {
  zoneBegin();
  char buf[ZONE_KEY_LEN];

  int y = 42;
  int labelX = 10;
  int valueX = 70;
  int lineH = 16;

  // ENV-specific lambdas at textSize 1 (#55)
  auto envLabel = [&](int x, int y, const char* label) {
    c->setTextColor(COLOR_DIM);
    c->setTextSize(1);
    c->setCursor(x, y);
    c->print(label);
  };
  auto envValue = [&](int x, int y, const char* value, uint16_t color = COLOR_VALUE) {
    c->fillRect(x, y, 250, 10, COLOR_BG);
    c->setTextColor(color);
    c->setTextSize(1);
    c->setCursor(x, y);
    c->print(value);
  };

  // Zone 0: Header
  if (zoneMark(0, 0, SCREEN_W, 30, "ENVIRONMENT"))
    drawHeader(c, "ENVIRONMENT");

  if (bmeAvailable || shtAvailable) {
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    const char* tempSrc = shtAvailable ? "SHT" : "BME";

    // Zone 1: Temp label
    if (zoneMark(labelX, y, 55, 10, "Temp:"))
      envLabel(labelX, y, "Temp:");
    // Zone 2: Temp value (#98 — respects useFahrenheit)
    if (useFahrenheit)
      sprintf(buf, "%.1f\xF7""F (%.1fC) %s", tempF, tempC, tempSrc);
    else
      sprintf(buf, "%.1f\xF7""C (%.1fF) %s", tempC, tempF, tempSrc);
    if (zoneMark(valueX, y, 250, 10, buf))
      envValue(valueX, y, buf);
    y += lineH;

    // Zone 3: Humid label
    float humid = shtAvailable ? shtData.humidity : envData.humidity;
    if (zoneMark(labelX, y, 55, 10, "Humid:"))
      envLabel(labelX, y, "Humid:");
    // Zone 4: Humid value
    sprintf(buf, "%.1f%% %s", humid, tempSrc);
    if (zoneMark(valueX, y, 250, 10, buf))
      envValue(valueX, y, buf);
    y += lineH;

    if (bmeAvailable) {
      // Zone 5: IAQ label
      if (zoneMark(labelX, y, 55, 10, "IAQ:"))
        envLabel(labelX, y, "IAQ:");
      // Zone 6: IAQ value
      sprintf(buf, "%.0f [%s]", envData.iaq, getIaqAccuracyText(envData.iaqAccuracy));
      uint16_t iaqColor = COLOR_VALUE;
      if (envData.iaq > 200) iaqColor = COLOR_ERROR;
      else if (envData.iaq > 100) iaqColor = COLOR_WARN;
      if (zoneMark(valueX, y, 250, 10, buf))
        envValue(valueX, y, buf, iaqColor);
      y += lineH;

      // Zone 7: CO2 label
      if (zoneMark(labelX, y, 55, 10, "CO2:"))
        envLabel(labelX, y, "CO2:");
      // Zone 8: CO2 value
      sprintf(buf, "%.0f ppm", envData.co2Equivalent);
      uint16_t co2Color = COLOR_VALUE;
      if (envData.co2Equivalent > 2000) co2Color = COLOR_ERROR;
      else if (envData.co2Equivalent > 1000) co2Color = COLOR_WARN;
      if (zoneMark(valueX, y, 250, 10, buf))
        envValue(valueX, y, buf, co2Color);
      y += lineH;

      // Zone 9: Pressure label
      if (zoneMark(labelX, y, 55, 10, "Press:"))
        envLabel(labelX, y, "Press:");
      // Zone 10: Pressure value
      sprintf(buf, "%.1f hPa (%.2f\")", envData.pressure, hPaToInHg(envData.pressure));
      if (zoneMark(valueX, y, 250, 10, buf))
        envValue(valueX, y, buf);
      y += lineH;

      // Zone 11: Forecast label
      if (zoneMark(labelX, y, 55, 10, "Fcst:"))
        envLabel(labelX, y, "Fcst:");
      // Zone 12: Forecast value
      sprintf(buf, "%s %s", getTrendArrow(), weatherTrend.forecast);
      uint16_t fcstColor = COLOR_VALUE;
      if (strstr(weatherTrend.forecast, "Storm")) fcstColor = COLOR_ERROR;
      else if (strstr(weatherTrend.forecast, "Rain") || strstr(weatherTrend.forecast, "Snow")) fcstColor = COLOR_WARN;
      if (zoneMark(valueX, y, 250, 10, buf))
        envValue(valueX, y, buf, fcstColor);
    } else {
      if (zoneMark(labelX, y, 55, 10, "IAQ:"))
        envLabel(labelX, y, "IAQ:");
      if (zoneMark(valueX, y, 250, 10, "N/A (no BME688)"))
        envValue(valueX, y, "N/A (no BME688)", COLOR_DIM);
      y += lineH;
      if (zoneMark(labelX, y, 55, 10, "Press:"))
        envLabel(labelX, y, "Press:");
      if (zoneMark(valueX, y, 250, 10, "N/A (no BME688) p"))
        envValue(valueX, y, "N/A (no BME688)", COLOR_DIM);
    }
  } else {
    if (zoneMark(60, 100, 300, 20, "No env sensors")) {
      c->setTextColor(COLOR_ERROR);
      c->setTextSize(2);
      c->setCursor(60, 100);
      c->println("No env sensors");
    }
  }

  // Zone 13: NavBar
  sprintf(buf, "nav_%d", currentScreen);
  if (zoneMark(0, SCREEN_H - 25, SCREEN_W, 25, buf))
    drawNavBar(c);
}

void drawScreenCompass(TFT_eSprite* c) {
  char buf[64];

  // Calibration overlay removed — now in Settings > Compass Cal (#89)

  zoneBegin();

  // Zone 0: Header
  if (zoneMark(0, 0, SCREEN_W, 30, "COMPASS"))
    drawHeader(c, "COMPASS");

  // Separator line between left panel and rose
  c->drawLine(178, 34, 178, 290, 0x2104);

  // GPS mode transition clearing — force full left-panel redraw on state change
  static int lastCompassGpsMode = -1;
  int compassGpsMode = gpsData.valid ? 0 : (gpsData.receiving ? 1 : 2);
  if (compassGpsMode != lastCompassGpsMode) {
    c->fillRect(0, 30, 178, 265, COLOR_BG);
    spr.pushSprite(0, 0);
    zonePrevCount = 0;
    lastCompassGpsMode = compassGpsMode;
  }

  // === Left Panel: Text Data ===
  if (imuAvailable && magAvailable) {
    // Zone 1: Heading + Cardinal (textSize 4) — e.g., "204° SW"
    const char* cardinal = getCardinal(imuData.heading);
    sprintf(buf, "%.0f_%s", imuData.heading, cardinal);
    if (zoneMark(8, 34, 170, 36, buf)) {
      c->fillRect(8, 34, 170, 36, COLOR_BG);
      c->setTextColor(COLOR_TEXT);
      c->setTextSize(4);
      c->setCursor(8, 36);
      c->printf("%.0f", imuData.heading);
      // Degree symbol proportional to textSize 4
      int degX = c->getCursorX() + 2;
      c->drawCircle(degX + 4, 38, 4, COLOR_TEXT);
      // Cardinal direction after degree symbol
      c->setCursor(degX + 14, 36);
      c->print(cardinal);
    }
  } else {
    // No IMU — single zone replaces heading + cardinal
    if (zoneMark(8, 34, 170, 36, "No IMU")) {
      c->fillRect(8, 34, 170, 36, COLOR_BG);
      c->setTextColor(COLOR_ERROR);
      c->setTextSize(2);
      c->setCursor(8, 45);
      c->print("No IMU");
    }
  }

  // Zone 2: GPS Coordinates (textSize 1, 2 lines)
  {
    int gpsMode = gpsData.valid ? 0 : (gpsData.receiving ? 1 : 2);
    if (gpsMode == 0) {
      sprintf(buf, "G%d_%.4f_%.4f", gpsMode, gpsData.latitude, gpsData.longitude);
    } else {
      sprintf(buf, "G%d_s%d", gpsMode, gpsData.satellites);
    }
    if (zoneMark(8, 80, 170, 28, buf)) {
      c->fillRect(8, 80, 170, 28, COLOR_BG);
      c->setTextSize(1);
      if (gpsData.valid) {
        c->setTextColor(COLOR_DIM);
        c->setCursor(8, 80);
        c->print("Lat ");
        c->setTextColor(COLOR_VALUE);
        c->printf("%.4f%c", fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
        c->setTextColor(COLOR_DIM);
        c->setCursor(8, 94);
        c->print("Lon ");
        c->setTextColor(COLOR_VALUE);
        c->printf("%.4f%c", fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
      } else if (gpsData.receiving) {
        c->setTextColor(COLOR_WARN);
        c->setCursor(8, 80);
        c->print("GPS Acquiring...");
        c->setCursor(8, 94);
        c->printf("Sats: %d", gpsData.satellites);
      } else {
        c->setTextColor(COLOR_ERROR);
        c->setCursor(8, 84);
        c->print("No GPS");
      }
    }
  }

  // Zone 3: Altitude (textSize 1) — grouped with GPS data
  {
    if (gpsData.valid) {
      float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
      sprintf(buf, "A%.0f%c", alt, useMetricUnits ? 'm' : 'f');
    } else {
      strcpy(buf, "A--");
    }
    if (zoneMark(8, 108, 170, 14, buf)) {
      c->fillRect(8, 108, 170, 14, COLOR_BG);
      c->setTextSize(1);
      c->setCursor(8, 108);
      c->setTextColor(COLOR_DIM);
      c->print("Alt ");
      if (gpsData.valid) {
        float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
        c->setTextColor(COLOR_VALUE);
        c->printf("%.0f %s", alt, useMetricUnits ? "m" : "ft");
      } else {
        c->setTextColor(COLOR_DIM);
        c->print("--");
      }
    }
  }

  // Zone 4: Speed (textSize 1) — grouped with GPS data
  {
    if (gpsData.valid) {
      float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
      sprintf(buf, "S%.1f%c", speed, useMetricUnits ? 'k' : 'm');
    } else {
      strcpy(buf, "S--");
    }
    if (zoneMark(8, 122, 170, 14, buf)) {
      c->fillRect(8, 122, 170, 14, COLOR_BG);
      c->setTextSize(1);
      c->setCursor(8, 122);
      c->setTextColor(COLOR_DIM);
      c->print("Spd ");
      if (gpsData.valid) {
        float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
        c->setTextColor(COLOR_VALUE);
        c->printf("%.1f %s", speed, useMetricUnits ? "km/h" : "mph");
      } else {
        c->setTextColor(COLOR_DIM);
        c->printf("-- %s", useMetricUnits ? "km/h" : "mph");
      }
    }
  }

  // Zone 5: Temperature (textSize 1)
  {
    bool hasTempSensor = shtAvailable || bmeAvailable;
    if (hasTempSensor) {
      float tempC = shtAvailable ? shtData.temperature : envData.temperature;
      float tempDisplay = useFahrenheit ? tempC * 9.0 / 5.0 + 32.0 : tempC;
      sprintf(buf, "T%.1f%c", tempDisplay, useFahrenheit ? 'F' : 'C');
    } else {
      strcpy(buf, "T--");
    }
    if (zoneMark(8, 144, 170, 14, buf)) {
      c->fillRect(8, 144, 170, 14, COLOR_BG);
      c->setTextSize(1);
      c->setCursor(8, 144);
      c->setTextColor(COLOR_DIM);
      c->print("Temp ");
      if (hasTempSensor) {
        float tempC = shtAvailable ? shtData.temperature : envData.temperature;
        float tempDisplay = useFahrenheit ? tempC * 9.0 / 5.0 + 32.0 : tempC;
        c->setTextColor(COLOR_VALUE);
        c->printf("%.1f", tempDisplay);
        // Degree symbol using small circle
        int degX = c->getCursorX() + 1;
        int degY = c->getCursorY();
        c->drawCircle(degX + 1, degY + 1, 1, COLOR_VALUE);
        c->setCursor(degX + 5, degY);
        c->printf("%c", useFahrenheit ? 'F' : 'C');
      } else {
        c->setTextColor(COLOR_DIM);
        c->print("--");
      }
    }
  }

  // Zone 6: Forecast + trend (textSize 1)
  {
    sprintf(buf, "F_%s_%s", getTrendArrow(), weatherTrend.forecast);
    if (zoneMark(8, 158, 170, 14, buf)) {
      c->fillRect(8, 158, 170, 14, COLOR_BG);
      c->setTextSize(1);
      c->setCursor(8, 158);
      c->setTextColor(COLOR_DIM);
      c->print("Fcst ");
      // Determine forecast color
      uint16_t fcstColor = COLOR_VALUE;  // Default green
      const char* fc = weatherTrend.forecast;
      if (strstr(fc, "Storm")) fcstColor = COLOR_ERROR;
      else if (strstr(fc, "Rain") || strstr(fc, "Snow") ||
               strstr(fc, "Unsettled") || strstr(fc, "Precip")) fcstColor = COLOR_WARN;
      else if (strcmp(fc, "Init") == 0 || strcmp(fc, "Learning") == 0 ||
               strcmp(fc, "Traveled") == 0) fcstColor = COLOR_DIM;
      c->setTextColor(fcstColor);
      c->printf("%s %s", getTrendArrow(), fc);
    }
  }

  // Zone 7: GPS Status (textSize 1)
  {
    if (gpsData.valid) {
      sprintf(buf, "GS_OK_%d_%.1f", gpsData.satellites, gpsData.hdop);
    } else if (gpsData.receiving) {
      sprintf(buf, "GS_ACQ_%d", gpsData.satellites);
    } else {
      strcpy(buf, "GS_NONE");
    }
    if (zoneMark(8, 200, 170, 14, buf)) {
      c->fillRect(8, 200, 170, 14, COLOR_BG);
      c->setTextSize(1);
      c->setCursor(8, 200);
      if (gpsData.valid) {
        c->setTextColor(COLOR_VALUE);
        c->printf("GPS OK Sat:%d HDOP:%.1f", gpsData.satellites, gpsData.hdop);
      } else if (gpsData.receiving) {
        c->setTextColor(COLOR_WARN);
        c->printf("GPS Acquiring Sat:%d", gpsData.satellites);
      } else {
        c->setTextColor(COLOR_ERROR);
        c->print("No GPS");
      }
    }
  }

  // Zone 9: Time (textSize 2)
  {
    char timeBuf[16];
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      formatTimeStr(timeBuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, false);
    } else {
      strcpy(timeBuf, "--:--");
    }
    if (zoneMark(8, 268, 170, 22, timeBuf)) {
      c->fillRect(8, 268, 170, 22, COLOR_BG);
      c->setTextSize(2);
      c->setCursor(8, 270);
      bool hasTime = (timeBuf[0] != '-');
      c->setTextColor(hasTime ? COLOR_TEXT : COLOR_DIM);
      c->print(timeBuf);
    }
  }

  // === Right Panel: Compass Rose ===
  // Zone 10: Compass rose — key on heading rounded to 2° steps
  if (imuAvailable && magAvailable) {
    int roseHeading = ((int)imuData.heading / 2) * 2;
    sprintf(buf, "rose_%d", roseHeading);
    if (zoneMark(182, 30, 298, 265, buf))
      drawCompassRose(c, 320, 162, 108, imuData.heading);
  } else {
    if (zoneMark(182, 30, 298, 265, "rose_none")) {
      c->drawCircle(320, 162, 108, COLOR_DIM);
      c->setTextColor(COLOR_DIM);
      c->setTextSize(3);
      c->setCursor(312, 152);
      c->print("?");
    }
  }

  // Zone 7: NavBar
  sprintf(buf, "nav_%d", currentScreen);
  if (zoneMark(0, SCREEN_H - 25, SCREEN_W, 25, buf))
    drawNavBar(c);
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
void drawCompassRose(TFT_eSprite* c, int cx, int cy, int radius, float heading) {
  // Clear rose area — proportional margin, clamped to content area
  int margin = radius / 5;  // 24 at r=120
  int clearY = max(30, cy - radius - margin);
  int clearBottom = min(294, cy + radius + margin);
  c->fillRect(cx - radius - margin, clearY,
              2 * (radius + margin), clearBottom - clearY, COLOR_BG);

  // Anti-aliased outer circle
  c->drawSmoothCircle(cx, cy, radius, COLOR_DIM, COLOR_BG);

  // Rotation: rose turns opposite to heading so N points north
  float rotDeg = -heading;

  // Proportional tick lengths
  int cardTickLen = radius / 10;    // 12px at r=120
  int interTickLen = radius / 20;   // 6px at r=120

  // Degree ticks every 30 degrees (12 ticks)
  for (int i = 0; i < 12; i++) {
    float tickAngle = radians(i * 30 + rotDeg - 90);  // -90 for screen coords
    int tickLen = (i % 3 == 0) ? cardTickLen : interTickLen;
    int outerX = cx + cos(tickAngle) * radius;
    int outerY = cy + sin(tickAngle) * radius;
    int innerX = cx + cos(tickAngle) * (radius - tickLen);
    int innerY = cy + sin(tickAngle) * (radius - tickLen);
    c->drawLine(innerX, innerY, outerX, outerY, COLOR_DIM);

    // Degree label just outside the ring
    int deg = i * 30;
    char lbl[4];
    sprintf(lbl, "%d", deg);
    int labelRadius = radius + 4;  // Slightly outside the circle
    int lblX = cx + cos(tickAngle) * labelRadius;
    int lblY = cy + sin(tickAngle) * labelRadius;
    int charW = strlen(lbl) * 6;  // textSize 1: 6px per char
    c->setTextSize(1);
    c->setTextColor((i % 3 == 0) ? COLOR_TEXT : COLOR_DIM);
    c->setCursor(lblX - charW / 2, lblY - 4);  // Center on point
    c->print(lbl);
  }

  // Proportional needle dimensions derived from radius
  int cardLen  = radius * 93 / 100;  // Cardinal length (93% of r)
  int interLen = radius * 60 / 100;  // Intercardinal length (60% of r)
  int cardHW   = radius * 10 / 100;  // Cardinal half-width (10% of r)
  int interHW  = radius * 6 / 100;   // Intercardinal half-width (6% of r)

  // 8 diamond needles: cardinals (longer, wider) + intercardinals (shorter, thinner)
  struct Needle {
    float angle;      // Degrees from north
    int length;       // Tip distance from center
    int halfWidth;    // Half-width at center
    uint16_t color;
  };

  Needle needles[] = {
    {  0,  cardLen,  cardHW,  COLOR_HEADER},  // N — cyan
    { 45,  interLen, interHW, COLOR_DIM},     // NE — gray
    { 90,  cardLen,  cardHW,  COLOR_TEXT},     // E — white
    {135,  interLen, interHW, COLOR_DIM},      // SE — gray
    {180,  cardLen,  cardHW,  COLOR_ERROR},    // S — red
    {225,  interLen, interHW, COLOR_DIM},      // SW — gray
    {270,  cardLen,  cardHW,  COLOR_TEXT},     // W — white
    {315,  interLen, interHW, COLOR_DIM},      // NW — gray
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
    c->fillTriangle(tipX, tipY, sideX1, sideY1, sideX2, sideY2, needles[i].color);
    // Tail: cardinals get dark fill, intercardinals get gray
    uint16_t tailColor = (needles[i].color == COLOR_DIM) ? COLOR_DIM : 0x2104;
    c->fillTriangle(tailX, tailY, sideX1, sideY1, sideX2, sideY2, tailColor);
  }

  // Proportional center hub — sized to frame needle bases
  int hubR = max(5, radius / 15);  // 7px at r=108
  c->fillCircle(cx, cy, hubR, COLOR_TEXT);
  c->drawCircle(cx, cy, hubR, COLOR_DIM);

  // Fixed lubber line at top (does NOT rotate) — proportional orange triangle
  int lubberY = cy - radius - 4;
  int lubberHW = max(5, radius / 15);   // 8px at r=120
  int lubberH  = max(8, radius / 10);   // 12px at r=120
  c->fillTriangle(cx, lubberY, cx - lubberHW, lubberY - lubberH,
                  cx + lubberHW, lubberY - lubberH, COLOR_WARN);
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
void drawNavTriangle(TFT_eSprite* c, int cx, int cy, int size, float angle, uint16_t color) {
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
  c->fillTriangle(tipX, tipY, rear1X, rear1Y, rearCenterX, rearCenterY, color);
  c->fillTriangle(tipX, tipY, rear2X, rear2Y, rearCenterX, rearCenterY, color);
}

// Draw pulsing search zone circle that shrinks as we get closer
void drawSearchZoneCircle(TFT_eSprite* c, int cx, int cy, float distanceM, float accuracyM) {
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
  c->fillCircle(cx, cy, radius, COLOR_WARN);
  c->drawCircle(cx, cy, radius + 2, COLOR_TEXT);

  // Center dot
  c->fillCircle(cx, cy, 4, COLOR_TEXT);
}

// ============== Geocache Navigation Screen (#70) ==============

// Forward declarations for sub-screens
void drawCacheNavScreen(TFT_eSprite* c);
// ─── Settings Screen ───────────────────────────────────────────────────────

// ─── Reusable Settings UI Widgets (#98) ─────────────────────────────────────

// Draw Back/OK action bar at bottom of screen
void drawActionBar(TFT_eSprite* c, bool showBack, bool showOK) {
  // Bar background: y=270 to y=320 (50px tall)
  c->fillRect(0, 270, SCREEN_W, 50, 0x18C3);

  c->setTextSize(2);
  if (showBack) {
    c->fillRoundRect(10, 278, 110, 34, 6, 0x4208);   // Dark gray button
    c->setTextColor(COLOR_TEXT);
    c->setCursor(22, 286);
    c->print("<- Back");
  }
  if (showOK) {
    c->fillRoundRect(360, 278, 110, 34, 6, 0x03E0);  // Dark green button
    c->setTextColor(COLOR_TEXT);
    c->setCursor(388, 286);
    c->print("OK  >");
  }
}

// Draw a two-option toggle row. Returns nothing; tap handling is separate.
// y = top of row, value: false=optA selected, true=optB selected
void drawToggle(TFT_eSprite* c, int y, const char* label, const char* optA, const char* optB, bool value) {
  int labelW = 140;
  int optW   = 130;
  int optH   = 30;
  int gap    = 10;

  // Label
  c->setTextSize(2);
  c->setTextColor(COLOR_DIM);
  c->setCursor(20, y + 7);
  c->print(label);

  // Option A (left) — selected when value==false
  int ax = labelW + 10;
  if (!value) {
    c->fillRoundRect(ax, y, optW, optH, 6, 0x03E0);     // Green = selected
    c->setTextColor(COLOR_TEXT);
  } else {
    c->fillRoundRect(ax, y, optW, optH, 6, 0x2104);     // Dark gray = unselected
    c->setTextColor(COLOR_DIM);
  }
  c->setCursor(ax + 10, y + 7);
  c->print(optA);

  // Option B (right) — selected when value==true
  int bx = ax + optW + gap;
  if (value) {
    c->fillRoundRect(bx, y, optW, optH, 6, 0x03E0);
    c->setTextColor(COLOR_TEXT);
  } else {
    c->fillRoundRect(bx, y, optW, optH, 6, 0x2104);
    c->setTextColor(COLOR_DIM);
  }
  c->setCursor(bx + 10, y + 7);
  c->print(optB);
}

// Draw a dropdown trigger row (label + current value + down arrow indicator)
void drawDropdown(TFT_eSprite* c, int y, const char* label, const char* currentValue) {
  int labelW = 140;

  // Label
  c->setTextSize(2);
  c->setTextColor(COLOR_DIM);
  c->setCursor(20, y + 7);
  c->print(label);

  // Value box with dropdown indicator
  int vx = labelW + 10;
  int vw = SCREEN_W - vx - 20;
  c->fillRoundRect(vx, y, vw, 30, 6, 0x2104);
  c->setTextColor(COLOR_VALUE);
  c->setCursor(vx + 10, y + 7);
  c->print(currentValue);

  // Down-arrow indicator
  c->setTextColor(COLOR_DIM);
  c->setCursor(vx + vw - 24, y + 7);
  c->print("v");
}

// Draw a scrollable selection overlay for timezone picker
void drawSelectorOverlay(TFT_eSprite* c, const TZPreset items[], int count, int selectedIdx, int scrollOffset) {
  int ox = 30, oy = 40, ow = SCREEN_W - 60, oh = 220;
  int itemH = 36;
  int visible = oh / itemH;  // ~6 items visible

  // Overlay background
  c->fillRoundRect(ox, oy, ow, oh, 8, 0x0841);       // Very dark blue
  c->drawRoundRect(ox, oy, ow, oh, 8, COLOR_DIM);     // Border

  // Title
  c->setTextSize(2);
  c->setTextColor(COLOR_HEADER);
  c->setCursor(ox + 10, oy + 6);
  c->print("Select Time Zone");

  // List items
  int listY = oy + 30;
  for (int i = 0; i < visible && (i + scrollOffset) < count; i++) {
    int idx = i + scrollOffset;
    int iy  = listY + i * itemH;

    if (idx == selectedIdx) {
      c->fillRoundRect(ox + 4, iy, ow - 8, itemH - 4, 4, 0x03E0);  // Green highlight
      c->setTextColor(COLOR_TEXT);
    } else {
      c->fillRoundRect(ox + 4, iy, ow - 8, itemH - 4, 4, 0x0841);
      c->setTextColor(COLOR_TEXT);
    }
    c->setTextSize(2);
    c->setCursor(ox + 14, iy + 8);
    char rowBuf[40];
    sprintf(rowBuf, "%s (UTC%+d)", items[idx].name, items[idx].stdOffset);
    c->print(rowBuf);
  }

  // Scroll indicators
  c->setTextColor(COLOR_DIM);
  c->setTextSize(1);
  if (scrollOffset > 0) {
    c->setCursor(ox + ow / 2 - 10, oy + 22);
    c->print("^ more");
  }
  if (scrollOffset + visible < count) {
    c->setCursor(ox + ow / 2 - 10, oy + oh - 12);
    c->print("v more");
  }
}

// ─── End Reusable Widgets ───────────────────────────────────────────────────

// Draw the Configuration sub-screen (#98)
void drawSettingsConfig(TFT_eSprite* c) {
  char buf[64];

  // Header
  if (zoneMark(0, 0, SCREEN_W, 30, "SCONFIG_HDR"))
    drawHeader(c, "CONFIGURATION");

  // Timezone dropdown row — y=45
  sprintf(buf, "SCONFIG_TZ_%d_%s_f%d", tzSelectedIndex, tzSelectorOpen ? "o" : "c", configFocusRow);
  if (zoneMark(10, 40, SCREEN_W - 20, 36, buf)) {
    drawDropdown(c, 45, "Time Zone", tzDisplayName);
    if (configFocusRow == 0) c->drawRoundRect(8, 42, SCREEN_W - 16, 34, 6, COLOR_HEADER);
  }

  // Time format toggle — y=88
  sprintf(buf, "SCONFIG_12H_%d_f%d", use12Hour ? 1 : 0, configFocusRow);
  if (zoneMark(10, 83, SCREEN_W - 20, 36, buf)) {
    drawToggle(c, 88, "Time", "12 Hour", "24 Hour", !use12Hour);
    if (configFocusRow == 1) c->drawRoundRect(8, 85, SCREEN_W - 16, 34, 6, COLOR_HEADER);
  }

  // Separator line
  if (zoneMark(20, 122, SCREEN_W - 40, 2, "SCONFIG_SEP"))
    c->drawFastHLine(20, 123, SCREEN_W - 40, COLOR_DIM);

  // Temperature toggle — y=132
  sprintf(buf, "SCONFIG_TEMP_%d_f%d", useFahrenheit ? 1 : 0, configFocusRow);
  if (zoneMark(10, 127, SCREEN_W - 20, 36, buf)) {
    drawToggle(c, 132, "Temp", "\xF7""F", "\xF7""C", !useFahrenheit);
    if (configFocusRow == 2) c->drawRoundRect(8, 129, SCREEN_W - 16, 34, 6, COLOR_HEADER);
  }

  // Distance toggle — y=172
  sprintf(buf, "SCONFIG_DIST_%d_f%d", useMetricUnits ? 1 : 0, configFocusRow);
  if (zoneMark(10, 167, SCREEN_W - 20, 36, buf)) {
    drawToggle(c, 172, "Distance", "Imperial", "Metric", useMetricUnits);
    if (configFocusRow == 3) c->drawRoundRect(8, 169, SCREEN_W - 16, 34, 6, COLOR_HEADER);
  }

  // Live preview — y=220
  {
    struct tm timeinfo;
    char timeBuf[16] = "--:--";
    if (getLocalTime(&timeinfo, 10))  // 10ms timeout — never block rendering (#98 bugfix)
      formatTimeStr(timeBuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, false);

    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    char tempBuf[16];
    if (useFahrenheit)
      sprintf(tempBuf, "%.1f\xF7""F", tempF);
    else
      sprintf(tempBuf, "%.1f\xF7""C", tempC);

    const char* distBuf = useMetricUnits ? "1.0 km" : "0.6 mi";

    sprintf(buf, "PREVIEW_%s_%s_%s", timeBuf, tempBuf, distBuf);
    if (zoneMark(10, 215, SCREEN_W - 20, 40, buf)) {
      c->fillRect(10, 215, SCREEN_W - 20, 40, COLOR_BG);
      c->setTextSize(2);
      c->setTextColor(COLOR_HEADER);
      c->setCursor(20, 225);
      c->printf("Preview:  %s  |  %s  |  %s", timeBuf, tempBuf, distBuf);
    }
  }

  // Action bar with focus highlight (#98)
  sprintf(buf, "SCONFIG_BAR_f%d", configFocusRow);
  if (zoneMark(0, 270, SCREEN_W, 50, buf)) {
    drawActionBar(c, true, true);
    if (configFocusRow == 4) c->drawRoundRect(8, 276, 114, 38, 8, COLOR_HEADER);   // Back focus
    if (configFocusRow == 5) c->drawRoundRect(358, 276, 114, 38, 8, COLOR_HEADER); // OK focus
  }

  // TZ selector overlay (drawn LAST, on top of everything)
  if (tzSelectorOpen) {
    sprintf(buf, "TZSEL_%d_%d", tzSelectedIndex, tzScrollOffset);
    if (zoneMark(30, 40, SCREEN_W - 60, 220, buf))
      drawSelectorOverlay(c, tzPresets, TZ_PRESET_COUNT, tzSelectedIndex, tzScrollOffset);
  }
}

// Factory Reset confirmation screen (#104)
void drawSettingsFactoryReset(TFT_eSprite* c) {
  char buf[64];

  // Header
  if (zoneMark(0, 0, SCREEN_W, 30, "FRESET_HDR"))
    drawHeader(c, "FACTORY RESET");

  // Warning message
  if (zoneMark(20, 80, SCREEN_W - 40, 50, "FRESET_WARN")) {
    c->setTextColor(COLOR_WARN);
    c->setTextSize(2);
    c->setCursor(20, 80);
    c->print("Reset all settings to");
    c->setCursor(20, 104);
    c->print("factory defaults?");
  }

  // Info: calibration preserved
  if (zoneMark(20, 145, SCREEN_W - 40, 50, "FRESET_INFO")) {
    c->setTextColor(COLOR_DIM);
    c->setTextSize(2);
    c->setCursor(20, 145);
    c->print("Compass calibration will");
    c->setCursor(20, 169);
    c->print("be preserved.");
  }

  // Action bar: Back / Reset
  if (zoneMark(0, 270, SCREEN_W, 50, "FRESET_BAR")) {
    c->fillRect(0, 270, SCREEN_W, 50, 0x18C3);
    c->setTextSize(2);

    // Back button (left)
    c->fillRoundRect(10, 278, 110, 34, 6, 0x4208);
    c->setTextColor(COLOR_TEXT);
    c->setCursor(22, 286);
    c->print("<- Back");

    // Reset button (right) — red to indicate destructive action
    c->fillRoundRect(360, 278, 110, 34, 6, 0x8000);  // Dark red
    c->setTextColor(COLOR_TEXT);
    c->setCursor(378, 286);
    c->print("RESET");
  }
}

void drawSettingsMenu(TFT_eSprite* c) {
  char buf[64];

  // Menu header zone
  sprintf(buf, "SETTINGS_HDR");
  if (zoneMark(0, 0, SCREEN_W, 30, buf)) {
    drawHeader(c, "SETTINGS");
  }

  // Draw menu items
  int startY = 45;
  int itemH  = 40;
  for (int i = 0; i < SETTINGS_MENU_COUNT; i++) {
    int y = startY + i * itemH;
    sprintf(buf, "SMENU_%d_%d", i, (i == settingsMenuIndex) ? 1 : 0);
    if (zoneMark(10, y, SCREEN_W - 20, itemH - 4, buf)) {
      if (i == settingsMenuIndex) {
        // Highlighted item
        c->fillRoundRect(10, y, SCREEN_W - 20, itemH - 4, 6, 0x2104);  // Dark blue-gray
        c->setTextColor(COLOR_VALUE);
      } else {
        c->fillRoundRect(10, y, SCREEN_W - 20, itemH - 4, 6, 0x0000);  // Black
        c->setTextColor(COLOR_TEXT);
      }
      c->setTextSize(2);
      c->setCursor(30, y + 10);
      c->print(settingsMenuItems[i]);
    }
  }

  // Action bar with Back button + focus highlight (#98)
  sprintf(buf, "SETTINGS_BAR_%d", settingsMenuIndex);
  if (zoneMark(0, 270, SCREEN_W, 50, buf)) {
    drawActionBar(c, true, false);   // Back only, no OK
    if (settingsMenuIndex == SETTINGS_MENU_COUNT)
      c->drawRoundRect(8, 276, 114, 38, 8, COLOR_HEADER);  // Cyan focus on Back
  }
}

void drawSettingsPlaceholder(TFT_eSprite* c, const char* title) {
  char buf[64];
  sprintf(buf, "SPLACE_%s", title);
  if (zoneMark(0, 0, SCREEN_W, 30, buf)) {
    drawHeader(c, title);
  }
  sprintf(buf, "SPLACE_BODY_%s", title);
  if (zoneMark(20, 80, SCREEN_W - 40, 60, buf)) {
    c->setTextColor(COLOR_DIM);
    c->setTextSize(2);
    c->setCursor(20, 80);
    c->print("Coming in Phase 2");
  }

  // Action bar with Back button + auto-focus highlight (#98)
  sprintf(buf, "SPLACE_BAR_%s", title);
  if (zoneMark(0, 270, SCREEN_W, 50, buf)) {
    drawActionBar(c, true, false);   // Back only, no OK
    c->drawRoundRect(8, 276, 114, 38, 8, COLOR_HEADER);  // Cyan focus — Back is auto-selected
  }
}

// ─── Display Settings Screen (#91) ─────────────────────────────────────────

// Helper: find index in timeout preset array matching a value
int findTimeoutIndex(const uint32_t presets[], int count, uint32_t value) {
  for (int i = 0; i < count; i++)
    if (presets[i] == value) return i;
  return 0;  // Default to first if not found
}

void drawSettingsDisplay(TFT_eSprite* c) {
  char buf[64];

  // Header
  if (zoneMark(0, 0, SCREEN_W, 30, "SDISPLAY_HDR"))
    drawHeader(c, "DISPLAY");

  // Row 0: Brightness — label + bar + numeric readout (y=50)
  int barX = 160, barW = 220, barH = 24, barY = 53;
  int fillW = map(constrain(tftBrightness, 25, 255), 25, 255, 0, barW);
  sprintf(buf, "SDISP_BRIGHT_%d_f%d", tftBrightness, displayFocusRow);
  if (zoneMark(10, 45, SCREEN_W - 20, 36, buf)) {
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 55);
    c->print("Brightness");
    // Bar background
    c->fillRoundRect(barX, barY, barW, barH, 4, 0x2104);
    // Filled portion
    if (fillW > 0)
      c->fillRoundRect(barX, barY, fillW, barH, 4, 0x03E0);
    // Numeric readout
    c->setTextColor(COLOR_VALUE);
    c->setCursor(barX + barW + 10, 55);
    c->printf("%d", tftBrightness);
    // Focus highlight
    if (displayFocusRow == 0)
      c->drawRoundRect(8, 47, SCREEN_W - 16, 34, 6, COLOR_HEADER);
  }

  // Separator
  if (zoneMark(20, 85, SCREEN_W - 40, 2, "SDISP_SEP1"))
    c->drawFastHLine(20, 86, SCREEN_W - 40, COLOR_DIM);

  // Row 1: TFT Sleep timeout (y=100)
  int tftIdx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
  sprintf(buf, "SDISP_TFTSLEEP_%d_f%d", tftIdx, displayFocusRow);
  if (zoneMark(10, 95, SCREEN_W - 20, 36, buf)) {
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 105);
    c->print("TFT Sleep");
    // Value in rounded rect
    int vx = 160, vw = 120;
    c->fillRoundRect(vx, 98, vw, 30, 6, 0x2104);
    c->setTextColor(COLOR_VALUE);
    c->setCursor(vx + 10, 105);
    c->print(tftTimeoutLabels[tftIdx]);
    if (displayFocusRow == 1)
      c->drawRoundRect(8, 97, SCREEN_W - 16, 34, 6, COLOR_HEADER);
  }

  // Row 2: OLED Sleep timeout (y=145)
  int oledIdx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
  sprintf(buf, "SDISP_OLEDSLEEP_%d_f%d", oledIdx, displayFocusRow);
  if (zoneMark(10, 140, SCREEN_W - 20, 36, buf)) {
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 150);
    c->print("OLED Sleep");
    int vx = 160, vw = 120;
    c->fillRoundRect(vx, 143, vw, 30, 6, 0x2104);
    c->setTextColor(COLOR_VALUE);
    c->setCursor(vx + 10, 150);
    c->print(oledTimeoutLabels[oledIdx]);
    if (displayFocusRow == 2)
      c->drawRoundRect(8, 142, SCREEN_W - 16, 34, 6, COLOR_HEADER);
  }

  // Action bar with focus highlight
  sprintf(buf, "SDISP_BAR_f%d", displayFocusRow);
  if (zoneMark(0, 270, SCREEN_W, 50, buf)) {
    drawActionBar(c, true, true);  // Back + OK
    if (displayFocusRow == 3) c->drawRoundRect(8, 276, 114, 38, 8, COLOR_HEADER);   // Back focus
    if (displayFocusRow == 4) c->drawRoundRect(358, 276, 114, 38, 8, COLOR_HEADER); // OK focus
  }
}

// Touch handler for Display settings screen (#91)
void handleDisplayTap(int x, int y) {
  // Brightness bar area (x: 160-380, y: 45-81)
  // Steps: 25,50,75,100,125,150,175,200,225,250,255 — 11 stops across 220px
  if (x >= 160 && x <= 380 && y >= 45 && y <= 81) {
    int pos = constrain(x - 160, 0, 220);  // 0-220 pixel position
    int step = (pos * 10) / 220;            // 0-10 (11 stops)
    if (step >= 10) tftBrightness = 255;
    else            tftBrightness = 25 + step * 25;  // 25,50,...250
    analogWrite(TFT_BL, tftBrightness);
    logPrintf("[DISPLAY] Brightness tap: %d\n", tftBrightness);
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // TFT timeout area (x: 160-280, y: 95-131)
  if (x >= 160 && x <= 280 && y >= 95 && y <= 131) {
    int idx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
    tftSleepMs = tftTimeoutPresets[(idx + 1) % TFT_TIMEOUT_COUNT];
    logPrintf("[DISPLAY] TFT timeout tap: %lu ms\n", tftSleepMs);
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // OLED timeout area (x: 160-280, y: 140-176)
  if (x >= 160 && x <= 280 && y >= 140 && y <= 176) {
    int idx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
    oledSleepMs = oledTimeoutPresets[(idx + 1) % OLED_TIMEOUT_COUNT];
    logPrintf("[DISPLAY] OLED timeout tap: %lu ms\n", oledSleepMs);
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Action bar: Back (x: 10-120, y: 278-312)
  if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
    loadSettings();
    analogWrite(TFT_BL, tftBrightness);  // Restore saved brightness
    displayFocusRow = -1;
    settingsSubScreen = 0;
    logPrintln("[DISPLAY] Back tap");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }

  // Action bar: OK (x: 360-470, y: 278-312)
  if (x >= 360 && x <= 470 && y >= 278 && y <= 312) {
    saveSettings();
    displayFocusRow = -1;
    settingsSubScreen = 0;
    logPrintln("[DISPLAY] OK tap");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }
}

// ─── Compass Calibration Sub-Screen (#89) ───────────────────────────────────

void drawSettingsCompassCal(TFT_eSprite* c) {
  char buf[ZONE_KEY_LEN];

  // === Calibration active state — bypass zones, full sprite push each frame ===
  static bool wasCalibrating = false;
  if (magCalibrating) {
    wasCalibrating = true;
    zoneBegin();  // Reset zones so zonePushDirty() is a no-op after return
    unsigned long elapsed = millis() - magCalStartTime;
    int remaining = (MAG_CAL_DURATION_MS - elapsed) / 1000;

    if (elapsed >= MAG_CAL_DURATION_MS) {
      // Calibration complete — compute hard-iron offsets
      magOffsetX = (magCalMaxX + magCalMinX) / 2.0;
      magOffsetY = (magCalMaxY + magCalMinY) / 2.0;
      magOffsetZ = (magCalMaxZ + magCalMinZ) / 2.0;
      magCalibrated = true;
      magCalibrating = false;
      saveMagCal();

      // Show "CAL COMPLETE" screen with offsets for 3 seconds
      spr.fillSprite(COLOR_BG);
      drawHeader(c, "CAL COMPLETE");
      c->setTextColor(COLOR_VALUE);
      c->setTextSize(2);
      c->setCursor(20, 60);
      c->print("Offsets saved:");
      c->setTextSize(2);
      c->setCursor(20, 100);
      sprintf(buf, "X: %.2f", magOffsetX);
      c->print(buf);
      c->setCursor(20, 130);
      sprintf(buf, "Y: %.2f", magOffsetY);
      c->print(buf);
      c->setCursor(20, 160);
      sprintf(buf, "Z: %.2f", magOffsetZ);
      c->print(buf);

      char msg[80];
      sprintf(msg, "[MAG] Cal complete: X=%.2f Y=%.2f Z=%.2f", magOffsetX, magOffsetY, magOffsetZ);
      logPrintln(msg);

      if (spriteAvailable) { spr.pushSprite(0, 0); }
      delay(3000);
      return;  // Returns to idle screen on next frame
    }

    // Active calibration UI with progress ring
    spr.fillSprite(COLOR_BG);
    drawHeader(c, "CALIBRATING");

    // Progress ring — center (240, 130), outer=70, inner=58
    int progressDeg = (int)((elapsed * 360UL) / MAG_CAL_DURATION_MS);

    // Background ring (full circle, dark gray)
    c->drawArc(240, 130, 70, 58, 0, 360, 0x2104, COLOR_BG, true);

    // Progress arc — starts at 12 o'clock (180° in TFT_eSPI), sweeps clockwise
    if (progressDeg > 0) {
      int arcStart = 180;
      int arcEnd = (180 + progressDeg) % 360;
      c->drawArc(240, 130, 70, 58, arcStart, arcEnd, 0x03E0, COLOR_BG, true);
    }

    // Countdown number centered inside the ring
    c->setTextColor(COLOR_TEXT);
    c->setTextSize(4);
    sprintf(buf, "%d", remaining > 0 ? remaining : 0);
    int tw = strlen(buf) * 24;  // size 4 ≈ 24px per char
    c->setCursor(240 - tw / 2, 118);
    c->print(buf);

    // "Rotate device slowly 360°" instruction below ring
    c->setTextColor(COLOR_WARN);
    c->setTextSize(2);
    c->setCursor(80, 215);
    c->print("Rotate device slowly 360");
    c->drawCircle(c->getCursorX() + 4, 217, 3, COLOR_WARN);  // Degree symbol

    // Live min/max values
    c->setTextSize(1);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 245);
    sprintf(buf, "X: %.1f to %.1f", magCalMinX < 99998 ? magCalMinX : 0, magCalMaxX > -99998 ? magCalMaxX : 0);
    c->print(buf);
    c->setCursor(20, 260);
    sprintf(buf, "Y: %.1f to %.1f", magCalMinY < 99998 ? magCalMinY : 0, magCalMaxY > -99998 ? magCalMaxY : 0);
    c->print(buf);
    c->setCursor(20, 275);
    sprintf(buf, "Z: %.1f to %.1f", magCalMinZ < 99998 ? magCalMinZ : 0, magCalMaxZ > -99998 ? magCalMaxZ : 0);
    c->print(buf);

    // Direct full push (bypasses zone system)
    if (spriteAvailable) { spr.pushSprite(0, 0); }
    return;
  }

  // Cleanup on return from calibration — force full redraw of idle screen
  if (wasCalibrating) {
    spr.fillSprite(COLOR_BG);
    spr.pushSprite(0, 0);
    zonePrevCount = 0;
    wasCalibrating = false;
  }

  // === Idle state — zone-based rendering ===

  // Auto-focus: default to row 0 (Start Calibration) on first draw
  if (compassCalFocusRow < 0) compassCalFocusRow = 0;

  // Header
  if (zoneMark(0, 0, SCREEN_W, 30, "SCAL_HDR"))
    drawHeader(c, "COMPASS CAL");

  // Status line: "Calibrated" (green) or "Not calibrated" (dim)
  sprintf(buf, "SCAL_STATUS_%d", magCalibrated ? 1 : 0);
  if (zoneMark(20, 45, SCREEN_W - 40, 20, buf)) {
    c->fillRect(20, 45, SCREEN_W - 40, 20, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 48);
    c->print("Status: ");
    if (magCalibrated) {
      c->setTextColor(COLOR_VALUE);
      c->print("Calibrated");
    } else {
      c->print("Not calibrated");
    }
  }

  // Offsets line: show values or "—"
  sprintf(buf, "SCAL_OFF_%d_%.1f_%.1f_%.1f", magCalibrated ? 1 : 0, magOffsetX, magOffsetY, magOffsetZ);
  if (zoneMark(20, 75, SCREEN_W - 40, 30, buf)) {
    c->fillRect(20, 75, SCREEN_W - 40, 30, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 78);
    c->print("Offsets: ");
    if (magCalibrated) {
      c->setTextColor(COLOR_VALUE);
      c->setTextSize(1);
      c->setCursor(20, 98);
      char offBuf[64];
      sprintf(offBuf, "X: %.2f   Y: %.2f   Z: %.2f", magOffsetX, magOffsetY, magOffsetZ);
      c->print(offBuf);
    } else {
      c->setTextColor(COLOR_DIM);
      c->print("---");
    }
  }

  // Start Calibration button (y=130, centered)
  sprintf(buf, "SCAL_START_%d_%d_%d", compassCalFocusRow, magAvailable ? 1 : 0, magCalibrating ? 1 : 0);
  if (zoneMark(120, 125, 240, 40, buf)) {
    c->fillRect(120, 125, 240, 40, COLOR_BG);  // Clear area
    if (magAvailable && !magCalibrating) {
      // Active button
      uint16_t btnColor = (compassCalFocusRow == 0) ? 0x03E0 : 0x4208;  // Green if focused, dark gray otherwise
      c->fillRoundRect(130, 128, 220, 34, 6, btnColor);
      c->setTextColor(COLOR_TEXT);
      c->setTextSize(2);
      c->setCursor(142, 136);
      c->print("Start Calibration");
      if (compassCalFocusRow == 0)
        c->drawRoundRect(128, 126, 224, 38, 8, COLOR_HEADER);  // Cyan focus ring
    } else {
      // Disabled button (IMU not available)
      c->fillRoundRect(130, 128, 220, 34, 6, 0x2104);  // Very dark gray
      c->setTextColor(COLOR_DIM);
      c->setTextSize(2);
      c->setCursor(142, 136);
      c->print("Start Calibration");
      // Show reason
      c->setTextSize(1);
      c->setTextColor(COLOR_ERROR);
      c->setCursor(170, 170);
      c->print("IMU not detected");
    }
  }

  // Action bar: Back button (y=270)
  sprintf(buf, "SCAL_BAR_%d", compassCalFocusRow);
  if (zoneMark(0, 270, SCREEN_W, 50, buf)) {
    drawActionBar(c, true, false);   // Back only, no OK
    if (compassCalFocusRow == 1)
      c->drawRoundRect(8, 276, 114, 38, 8, COLOR_HEADER);  // Cyan focus ring on Back
  }
}

// ─── About / System Info Screen (#92) ──────────────────────────────────────

void drawSettingsAbout(TFT_eSprite* c) {
  char buf[128];

  // Header
  if (zoneMark(0, 0, SCREEN_W, 30, "SABOUT_HDR"))
    drawHeader(c, "ABOUT");

  // Version (static — zone key doesn't change)
  if (zoneMark(20, 45, SCREEN_W - 40, 26, "SABOUT_VER")) {
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 50);
    c->print("Version:");
    c->setTextColor(COLOR_VALUE);
    c->setCursor(160, 50);
    c->print(FW_VERSION);
  }

  // Uptime — update every second for live display
  unsigned long uptimeSec = millis() / 1000;
  int days = uptimeSec / 86400;
  int hrs  = (uptimeSec % 86400) / 3600;
  int mins = (uptimeSec % 3600) / 60;
  int secs = uptimeSec % 60;
  sprintf(buf, "SABOUT_UP_%lu", uptimeSec);
  if (zoneMark(20, 75, SCREEN_W - 40, 26, buf)) {
    c->fillRect(20, 75, SCREEN_W - 40, 26, COLOR_BG);  // Clear before redraw
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 80);
    c->print("Uptime:");
    c->setTextColor(COLOR_VALUE);
    c->setCursor(160, 80);
    if (days > 0) c->printf("%dd %02d:%02d:%02d", days, hrs, mins, secs);
    else          c->printf("%02d:%02d:%02d", hrs, mins, secs);
  }

  // Heap
  uint32_t freeHeap = ESP.getFreeHeap() / 1024;
  uint32_t totalHeap = ESP.getHeapSize() / 1024;
  sprintf(buf, "SABOUT_HEAP_%lu", freeHeap);
  if (zoneMark(20, 105, SCREEN_W - 40, 26, buf)) {
    c->fillRect(20, 105, SCREEN_W - 40, 26, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 110);
    c->print("Heap:");
    c->setTextColor(COLOR_VALUE);
    c->setCursor(160, 110);
    c->printf("%lu / %lu KB", freeHeap, totalHeap);
  }

  // PSRAM
  uint32_t freePSRAM = ESP.getFreePsram() / 1024;
  uint32_t totalPSRAM = ESP.getPsramSize() / 1024;
  sprintf(buf, "SABOUT_PSRAM_%lu", freePSRAM);
  if (zoneMark(20, 135, SCREEN_W - 40, 26, buf)) {
    c->fillRect(20, 135, SCREEN_W - 40, 26, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 140);
    c->print("PSRAM:");
    c->setTextColor(COLOR_VALUE);
    c->setCursor(160, 140);
    c->printf("%lu / %lu KB", freePSRAM, totalPSRAM);
  }

  // Battery — use isBatteryConnected() to detect USB-only power
  bool battConn = batteryAvailable && isBatteryConnected();
  float battV = battConn ? battery.cellVoltage() : 0;
  float battP = battConn ? battery.cellPercent() : 0;
  sprintf(buf, "SABOUT_BATT_%d_%d_%d_%lu", battConn ? 1 : 0, (int)(battP * 10), (int)(battV * 100), millis() / 5000);
  if (zoneMark(20, 175, SCREEN_W - 40, 26, buf)) {
    c->fillRect(20, 175, SCREEN_W - 40, 26, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 180);
    c->print("Battery:");
    c->setTextColor(COLOR_VALUE);
    c->setCursor(160, 180);
    if (battConn)            c->printf("%.0f%% (%.2fV)", battP, battV);
    else if (batteryAvailable) c->print("USB Only");
    else                       c->print("N/A");
  }

  // WiFi — show SSID + IP
  bool wifiConn = (WiFi.status() == WL_CONNECTED);
  String wifiSSID = wifiConn ? WiFi.SSID() : "";
  sprintf(buf, "SABOUT_WIFI_%d_%s", wifiConn ? 1 : 0,
          wifiConn ? WiFi.localIP().toString().c_str() : "disc");
  if (zoneMark(20, 205, SCREEN_W - 40, 26, buf)) {
    c->fillRect(20, 205, SCREEN_W - 40, 26, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_DIM);
    c->setCursor(20, 210);
    c->print("WiFi:");
    c->setTextColor(COLOR_VALUE);
    c->setCursor(160, 210);
    if (wifiConn) c->printf("%s %s", wifiSSID.c_str(), WiFi.localIP().toString().c_str());
    else          c->print("Disconnected");
  }

  // Action bar: Back only, auto-focused
  if (zoneMark(0, 270, SCREEN_W, 50, "SABOUT_BAR")) {
    drawActionBar(c, true, false);   // Back only, no OK
    c->drawRoundRect(8, 276, 114, 38, 8, COLOR_HEADER);  // Cyan focus — auto-selected
  }
}

// ─── Diagnostics Sub-Screen (#90) ────────────────────────────────────────

void drawSettingsDiags(TFT_eSprite* c) {
  char buf[ZONE_KEY_LEN];

  int y = 38;
  int labelX = 10;
  int vX = 60;
  int lineH = 24;

  auto diagRow = [&](int y, const char* label, const char* value, uint16_t valColor) {
    c->setTextSize(1);
    c->setTextColor(COLOR_HEADER);
    c->setCursor(labelX, y);
    c->print(label);
    c->setTextColor(valColor);
    c->setCursor(vX, y);
    c->fillRect(vX, y, 260, 10, COLOR_BG);
    c->print(value);
  };

  // Header
  if (zoneMark(0, 0, SCREEN_W, 30, "SDIAGS_HDR"))
    drawHeader(c, "DIAGNOSTICS");

  // BSEC
  sprintf(buf, "SDIAGS_BSEC_%s_%s_%s",
          bsecStateLoaded ? "Y" : "N", bsecStateSaved ? "Y" : "N",
          getIaqAccuracyText(envData.iaqAccuracy));
  if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf)) {
    char val[64];
    sprintf(val, "Load:%s Save:%s Acc:%s",
            bsecStateLoaded ? "Y" : "N", bsecStateSaved ? "Y" : "N",
            getIaqAccuracyText(envData.iaqAccuracy));
    diagRow(y, "BSEC:", val, bsecStateLoaded ? COLOR_VALUE : COLOR_DIM);
  }
  y += lineH;

  // Weather
  sprintf(buf, "SDIAGS_WX_%d_%d_%d",
          weatherHistoryCount, weatherLogFileCount, weatherLogEntryCount);
  if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf)) {
    char val[64];
    sprintf(val, "Mem:%d Files:%d Tot:%d",
            weatherHistoryCount, weatherLogFileCount, weatherLogEntryCount);
    diagRow(y, "Weather:", val, COLOR_VALUE);
  }
  y += lineH;

  // Heap
  unsigned long freeH = ESP.getFreeHeap() / 1024;
  unsigned long totalH = ESP.getHeapSize() / 1024;
  sprintf(buf, "SDIAGS_HEAP_%lu", freeH);
  if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf)) {
    char val[32];
    sprintf(val, "%luK / %luK", freeH, totalH);
    diagRow(y, "Heap:", val, COLOR_VALUE);
  }
  y += lineH;

  // PSRAM
  if (psramFound()) {
    unsigned long freeP = ESP.getFreePsram() / 1024;
    unsigned long totalP = ESP.getPsramSize() / 1024;
    sprintf(buf, "SDIAGS_PSRAM_%lu", freeP);
    if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf)) {
      char val[48];
      sprintf(val, "%luK / %luK  Spr:%s", freeP, totalP, spriteAvailable ? "Y" : "N");
      diagRow(y, "PSRAM:", val, COLOR_VALUE);
    }
  } else {
    if (zoneMark(labelX, y, SCREEN_W - 20, 10, "SDIAGS_PSRAM_NA"))
      diagRow(y, "PSRAM:", "Not available", COLOR_DIM);
  }
  y += lineH;

  // Sensors
  sprintf(buf, "SDIAGS_SENS_%d%d%d%d%d%d",
          bmeAvailable, shtAvailable, imuAvailable,
          batteryAvailable, framAvailable, touchAvailable);
  if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf)) {
    char val[64];
    sprintf(val, "BME:%s SHT:%s IMU:%s Bat:%s FRAM:%s CTP:%s",
            bmeAvailable ? "Y" : "N", shtAvailable ? "Y" : "N",
            imuAvailable ? "Y" : "N", batteryAvailable ? "Y" : "N",
            framAvailable ? "Y" : "N", touchAvailable ? "Y" : "N");
    diagRow(y, "Sensors:", val, COLOR_VALUE);
  }
  y += lineH;

  // Temps
  {
    char val[64];
    if (shtAvailable && bmeAvailable) {
      float shtF = shtData.temperature * 9.0 / 5.0 + 32.0;
      float bmeF = envData.temperature * 9.0 / 5.0 + 32.0;
      if (useFahrenheit)
        sprintf(val, "SHT:%.1fF BME:%.1fF (%+.1f)", shtF, bmeF, shtF - bmeF);
      else
        sprintf(val, "SHT:%.1fC BME:%.1fC (%+.1f)", shtData.temperature, envData.temperature, shtData.temperature - envData.temperature);
    } else if (shtAvailable) {
      if (useFahrenheit)
        sprintf(val, "SHT:%.1fF BME:N/A", shtData.temperature * 9.0 / 5.0 + 32.0);
      else
        sprintf(val, "SHT:%.1fC BME:N/A", shtData.temperature);
    } else if (bmeAvailable) {
      if (useFahrenheit)
        sprintf(val, "SHT:N/A BME:%.1fF", envData.temperature * 9.0 / 5.0 + 32.0);
      else
        sprintf(val, "SHT:N/A BME:%.1fC", envData.temperature);
    } else {
      strcpy(val, "No sensors");
    }
    sprintf(buf, "SDIAGS_TEMP_%s", val);
    if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf))
      diagRow(y, "Temps:", val, COLOR_VALUE);
  }
  y += lineH;

  // GPS
  {
    uint16_t gpsColor = COLOR_VALUE;
    char val[48];
    if (gpsHadFirstFix) {
      sprintf(val, "Fix in %lus", gpsFirstFixTime / 1000);
    } else if (gpsHadFirstReceive) {
      unsigned long elapsed = millis() / 1000;
      sprintf(val, "Acquiring (%lum %lus)", elapsed / 60, elapsed % 60);
      gpsColor = COLOR_WARN;
    } else {
      strcpy(val, "No data"); gpsColor = COLOR_DIM;
    }
    sprintf(buf, "SDIAGS_GPS_%s", val);
    if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf))
      diagRow(y, "GPS:", val, gpsColor);
  }
  y += lineH;

  // MagCal
  {
    char val[48];
    if (magCalibrated) {
      sprintf(val, "%.1f, %.1f, %.1f", magOffsetX, magOffsetY, magOffsetZ);
    } else {
      strcpy(val, "None (Settings > Compass Cal)");
    }
    sprintf(buf, "SDIAGS_MAG_%d", magCalibrated);
    if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf))
      diagRow(y, "MagCal:", val, magCalibrated ? COLOR_VALUE : COLOR_DIM);
  }
  y += lineH;

  // Storage
  {
    uint16_t sdColor = COLOR_VALUE;
    char val[64];
    if (sdHealth.available) {
      unsigned long ageMin = (millis() - sdHealth.lastSuccess) / 60000;
      if (sdHealth.errorCount == 0) {
        sprintf(val, "SD:OK %lum OLED:%s", ageMin, oledAvailable ? "Y" : "N");
      } else {
        sprintf(val, "SD:WARN E:%d R:%d OLED:%s",
                sdHealth.errorCount, sdHealth.reInitCount, oledAvailable ? "Y" : "N");
        sdColor = COLOR_WARN;
      }
    } else {
      sprintf(val, "SD:FAIL E:%d R:%d OLED:%s",
              sdHealth.errorCount, sdHealth.reInitCount, oledAvailable ? "Y" : "N");
      sdColor = COLOR_ERROR;
    }
    sprintf(buf, "SDIAGS_SD_%s", val);
    if (zoneMark(labelX, y, SCREEN_W - 20, 10, buf))
      diagRow(y, "Storage:", val, sdColor);
  }
  y += lineH;

  // Network removed — redundant with About screen; see scrolling enhancement issue

  // Web URL
  const char* webKey = wifiConnected ? "SDIAGS_WEB_Y" : "SDIAGS_WEB_N";
  if (zoneMark(labelX, y, SCREEN_W - 20, 10, webKey)) {
    if (wifiConnected) {
      c->setTextColor(COLOR_DIM);
      c->setTextSize(1);
      c->setCursor(labelX, y);
      c->print("Web: http://fieldcompass.local/");
    }
  }

  // Action bar: Back only (read-only screen)
  if (zoneMark(0, 270, SCREEN_W, 50, "SDIAGS_BAR")) {
    drawActionBar(c, true, false);
    c->drawRoundRect(8, 276, 114, 38, 8, COLOR_HEADER);  // Auto-focus on Back
  }
}

void drawScreenSettings(TFT_eSprite* c) {
  zoneBegin();

  // Track sub-screen changes — clear and push to erase old artifacts
  static int lastSettingsSubScreen = -1;
  if (settingsSubScreen != lastSettingsSubScreen) {
    c->fillRect(0, 30, SCREEN_W, SCREEN_H - 30, COLOR_BG);
    spr.pushSprite(0, 0);      // Full push to clear old content
    zonePrevCount = 0;         // Force all zones dirty
    lastSettingsSubScreen = settingsSubScreen;
  }

  switch (settingsSubScreen) {
    case 1:  drawSettingsConfig(c);                        break;  // Configuration (#98)
    case 2:  drawSettingsDisplay(c);                       break;  // Display (#91)
    case 3:  drawSettingsCompassCal(c);                     break;  // Compass Cal (#89)
    case 4:  drawSettingsDiags(c);                          break;  // Diagnostics (#90)
    case 5:  drawSettingsAbout(c);                         break;  // About (#92)
    case 6:  drawSettingsFactoryReset(c);                  break;  // Factory Reset (#104)
    default: drawSettingsMenu(c);                          break;
  }
}

// ─── Geocache Screen ───────────────────────────────────────────────────────

void drawCacheListScreen(TFT_eSprite* c);
void drawCacheDetailsScreen(TFT_eSprite* c);

void drawScreenGeocache(TFT_eSprite* c) {
  zoneBegin();

  // Track sub-screen changes — clear and push to erase old sub-screen artifacts
  static int lastGeoSubScreen = -1;
  if (geocacheSubScreen != lastGeoSubScreen) {
    c->fillRect(0, 30, SCREEN_W, SCREEN_H - 30, COLOR_BG);
    spr.pushSprite(0, 0);  // Full push to clear old sub-screen artifacts from TFT
    zonePrevCount = 0;      // Force all zones dirty
    lastGeoSubScreen = geocacheSubScreen;
  }

  // Dispatch to appropriate sub-screen
  switch (geocacheSubScreen) {
    case 1:
      drawCacheListScreen(c);
      break;
    case 2:
      drawCacheDetailsScreen(c);
      break;
    default:
      drawCacheNavScreen(c);
      break;
  }
}

// Sub-screen 0: Navigation (main geocache screen)
void drawCacheNavScreen(TFT_eSprite* c) {
  char buf[ZONE_KEY_LEN];

  // Zone: Header
  if (zoneMark(0, 0, SCREEN_W, 30, "GEOCACHE"))
    drawHeader(c, "GEOCACHE");

  // Check if we have any caches and if selected cache is valid
  if (cacheListCount == 0 || selectedCacheIndex >= cacheListCount || !cacheList[selectedCacheIndex].valid) {
    if (zoneMark(0, 30, SCREEN_W, SCREEN_H - 30, "no_cache")) {
      c->setCursor(80, 120);
      c->setTextColor(COLOR_DIM);
      c->setTextSize(2);
      c->print("No cache loaded");
    }
    return;
  }

  // Get reference to selected cache for cleaner code
  GeocacheEntry& cache = cacheList[selectedCacheIndex];

  // Calculate distance and bearing
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float distM = distKm * 1000;
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);
  float accuracyM = getGpsAccuracyMeters();
  bool inSearchZone = (distM < accuracyM);

  // Zone: Cache name (changes when selected cache changes)
  snprintf(buf, ZONE_KEY_LEN, "n_%d_%s", selectedCacheIndex, cache.name);
  if (zoneMark(0, 33, SCREEN_W, 20, buf)) {
    c->fillRect(0, 33, SCREEN_W, 20, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_TEXT);
    int nameLen = strlen(cache.name);
    int nameX = (SCREEN_W - nameLen * 12) / 2;
    if (nameX < 10) nameX = 10;
    c->setCursor(nameX, 35);
    c->print(cache.name);
  }

  // Zone: Distance + D/T row (left side, 260px wide)
  if (inSearchZone) {
    snprintf(buf, ZONE_KEY_LEN, "SEARCHZONE D:%.1f T:%.1f", cache.difficulty, cache.terrain);
  } else {
    char distBuf[20];
    if (useMetricUnits) {
      if (distKm >= 1.0) sprintf(distBuf, "%.1f km", distKm);
      else sprintf(distBuf, "%.0f m", distM);
    } else {
      float distMi = distKm * 0.621371;
      if (distMi >= 1.0) sprintf(distBuf, "%.1f mi", distMi);
      else sprintf(distBuf, "%.0f ft", distKm * 3280.84);
    }
    snprintf(buf, ZONE_KEY_LEN, "%s D:%.1f T:%.1f", distBuf, cache.difficulty, cache.terrain);
  }
  if (zoneMark(10, 55, 250, 20, buf)) {
    c->fillRect(10, 55, 250, 20, COLOR_BG);
    c->setCursor(10, 55);
    c->setTextSize(2);
    if (inSearchZone) {
      c->setTextColor(COLOR_WARN);
      c->print("SEARCH ZONE");
    } else {
      c->setTextColor(COLOR_VALUE);
      // Re-format distance for drawing
      if (useMetricUnits) {
        if (distKm >= 1.0) sprintf(buf, "%.1f km", distKm);
        else sprintf(buf, "%.0f m", distM);
      } else {
        float distMi = distKm * 0.621371;
        if (distMi >= 1.0) sprintf(buf, "%.1f mi", distMi);
        else sprintf(buf, "%.0f ft", distKm * 3280.84);
      }
      c->print(buf);
    }
    c->setTextSize(1);
    c->setTextColor(COLOR_DIM);
    sprintf(buf, " D:%.1f T:%.1f", cache.difficulty, cache.terrain);
    c->print(buf);
  }

  // Zone: Bearing (right side)
  snprintf(buf, ZONE_KEY_LEN, "brg_%d", (int)bearing);
  if (zoneMark(260, 55, 60, 20, buf)) {
    c->fillRect(260, 55, 60, 20, COLOR_BG);
    c->setCursor(265, 55);
    c->setTextColor(COLOR_VALUE);
    c->setTextSize(2);
    sprintf(buf, "%d", (int)bearing);
    c->print(buf);
    c->setTextSize(1);
    c->print((char)247);  // Degree symbol
  }

  // Zone: Nav graphic (triangle or search zone circle)
  // Key on rounded bearing (2°), heading (2°), and search zone state
  int roundedBrg = ((int)bearing / 2) * 2;
  int roundedHdg = ((int)imuData.heading / 2) * 2;
  int roundedDist = (int)(distM / 2) * 2;  // Round to 2m for search zone pulse
  if (inSearchZone)
    snprintf(buf, ZONE_KEY_LEN, "nav_sz_%d_%d", roundedDist, (int)accuracyM);
  else
    snprintf(buf, ZONE_KEY_LEN, "nav_%d_%d", roundedBrg, roundedHdg);
  if (zoneMark(100, 75, 120, 110, buf)) {
    c->fillRect(100, 75, 120, 110, COLOR_BG);
    if (inSearchZone) {
      drawSearchZoneCircle(c, 160, 130, distM, accuracyM);
    } else {
      float triangleAngle = bearing - imuData.heading;
      if (triangleAngle < 0) triangleAngle += 360;
      if (triangleAngle >= 360) triangleAngle -= 360;
      drawNavTriangle(c, 160, 130, 50, triangleAngle, COLOR_HEADER);
    }
  }

  // Zone: GPS accuracy
  if (useMetricUnits)
    snprintf(buf, ZONE_KEY_LEN, "+/-%.0fm", accuracyM);
  else
    snprintf(buf, ZONE_KEY_LEN, "+/-%.0fft", accuracyM * 3.28084);
  if (zoneMark(80, 185, 160, 20, buf)) {
    c->fillRect(80, 185, 160, 20, COLOR_BG);
    uint16_t accColor = getAccuracyColor(accuracyM);
    c->setTextColor(accColor);
    c->setTextSize(2);
    // buf already has formatted accuracy from zone key
    int accLen = strlen(buf);
    int accX = (SCREEN_W - accLen * 12) / 2;
    c->setCursor(accX, 185);
    c->print(buf);
  }

  // Zone: Hint area
  // Key includes search zone state and hint content (truncated for key)
  if (strlen(cache.hint) > 0) {
    char hintKey[ZONE_KEY_LEN];
    snprintf(hintKey, ZONE_KEY_LEN, "hint_%d_%.20s", inSearchZone ? 1 : 0, cache.hint);
    if (zoneMark(10, 200, SCREEN_W - 20, 30, hintKey)) {
      c->fillRect(10, 200, SCREEN_W - 20, 30, COLOR_BG);
      c->setTextColor(COLOR_DIM);
      c->setTextSize(1);
      if (inSearchZone) {
        c->setCursor(10, 205);
        c->print("Hint: ");
        c->print(cache.hint);
      } else {
        c->setCursor(10, 220);
        char hintPreview[35];
        strncpy(hintPreview, cache.hint, 30);
        hintPreview[30] = '\0';
        if (strlen(cache.hint) > 30) strcat(hintPreview, "...");
        c->print("Hint: ");
        c->print(hintPreview);
      }
    }
  }
}

// Sub-screen 1: Cache List
void drawCacheListScreen(TFT_eSprite* c) {
  char buf[ZONE_KEY_LEN];

  // Zone: Header
  if (zoneMark(0, 0, SCREEN_W, 30, "CACHE LIST"))
    drawHeader(c, "CACHE LIST");

  // Zone: Count indicator
  snprintf(buf, ZONE_KEY_LEN, "[%d/%d]", listHighlightIndex + 1, cacheListCount);
  if (zoneMark(270, 8, 50, 12, buf)) {
    c->fillRect(270, 8, 50, 12, COLOR_BG);
    c->setTextSize(1);
    c->setTextColor(COLOR_DIM);
    c->setCursor(280, 10);
    c->print(buf);
  }

  if (cacheListCount == 0) {
    if (zoneMark(0, 30, SCREEN_W, SCREEN_H - 30, "no_caches")) {
      c->setCursor(80, 120);
      c->setTextColor(COLOR_DIM);
      c->setTextSize(2);
      c->print("No caches loaded");
    }
    return;
  }

  // Zone: Entire list area — key on scroll offset + highlight + distances
  // The list has interleaved selection highlighting, distances, and names.
  // Treat the whole visible list as one zone keyed on state that affects it.
  int y = 38;
  int lineH = 28;
  int maxVisible = 5;

  // Build a composite key from scroll state + all visible row data
  char listKey[ZONE_KEY_LEN];
  snprintf(listKey, ZONE_KEY_LEN, "l_%d_%d", listScrollOffset, listHighlightIndex);
  // Append abbreviated distance data for visible rows
  for (int i = 0; i < maxVisible && (listScrollOffset + i) < cacheListCount; i++) {
    int cacheIdx = listScrollOffset + i;
    float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                   cacheList[cacheIdx].latitude, cacheList[cacheIdx].longitude);
    char d[8];
    snprintf(d, 8, "%.0f", distKm * 1000);
    // Append if space allows
    int curLen = strlen(listKey);
    if (curLen + strlen(d) + 1 < ZONE_KEY_LEN) {
      listKey[curLen] = '_';
      strcpy(listKey + curLen + 1, d);
    }
  }

  if (zoneMark(0, 36, SCREEN_W, maxVisible * lineH + 4, listKey)) {
    // Clear list area
    c->fillRect(0, 36, SCREEN_W, maxVisible * lineH + 4, COLOR_BG);

    for (int i = 0; i < maxVisible && (listScrollOffset + i) < cacheListCount; i++) {
      int cacheIdx = listScrollOffset + i;
      GeocacheEntry& cache = cacheList[cacheIdx];
      bool isSelected = (cacheIdx == listHighlightIndex);

      if (isSelected) {
        c->fillRect(0, y - 2, SCREEN_W, lineH, COLOR_HEADER & 0x18E3);
      }

      int x = 5;

      c->setTextSize(2);
      c->setTextColor(isSelected ? COLOR_HEADER : COLOR_DIM);
      c->setCursor(x, y + 2);
      c->print(isSelected ? ">" : " ");
      x += 18;

      float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                     cache.latitude, cache.longitude);
      c->setTextColor(COLOR_VALUE);
      c->setTextSize(1);
      c->setCursor(x, y + 4);
      if (useMetricUnits) {
        if (distKm >= 1.0) sprintf(buf, "%4.1fkm", distKm);
        else sprintf(buf, "%4.0fm", distKm * 1000);
      } else {
        float distMi = distKm * 0.621371;
        if (distMi >= 1.0) sprintf(buf, "%4.1fmi", distMi);
        else sprintf(buf, "%4.0fft", distKm * 3280.84);
      }
      c->print(buf);
      x += 48;

      c->setTextColor(COLOR_TEXT);
      c->setCursor(x, y + 4);
      char nameBuf[20];
      strncpy(nameBuf, cache.name, 16);
      nameBuf[16] = '\0';
      if (strlen(cache.name) > 16) {
        nameBuf[14] = '.';
        nameBuf[15] = '.';
      }
      c->print(nameBuf);
      x += 100;

      if (cache.found) {
        c->setTextColor(COLOR_VALUE);
        c->setCursor(x, y + 4);
        c->print("*");
      }
      x += 12;

      c->setTextColor(COLOR_DIM);
      c->setCursor(x, y + 4);
      sprintf(buf, "D:%d T:%d", (int)cache.difficulty, (int)cache.terrain);
      c->print(buf);

      y += lineH;
    }
  }

  // Zone: Button hints (static)
  if (zoneMark(10, 188, SCREEN_W - 20, 12, "list_hints")) {
    c->setTextSize(1);
    c->setTextColor(COLOR_DIM);
    c->setCursor(10, 190);
    c->print("[A]Up [B]Down [C]Select [C+]Details");
  }
}

// Sub-screen 2: Cache Details
void drawCacheDetailsScreen(TFT_eSprite* c) {
  char buf[ZONE_KEY_LEN];

  if (cacheListCount == 0 || listHighlightIndex >= cacheListCount) {
    if (zoneMark(0, 0, SCREEN_W, 30, "CACHE DETAILS"))
      drawHeader(c, "CACHE DETAILS");
    if (zoneMark(0, 30, SCREEN_W, SCREEN_H - 30, "no_cache_d")) {
      c->setCursor(80, 120);
      c->setTextColor(COLOR_DIM);
      c->setTextSize(2);
      c->print("No cache");
    }
    return;
  }

  GeocacheEntry& cache = cacheList[listHighlightIndex];

  // Zone: Header
  if (zoneMark(0, 0, SCREEN_W, 30, "CACHE DETAILS"))
    drawHeader(c, "CACHE DETAILS");

  // Zone: Count indicator
  snprintf(buf, ZONE_KEY_LEN, "[%d/%d]", listHighlightIndex + 1, cacheListCount);
  if (zoneMark(270, 8, 50, 12, buf)) {
    c->fillRect(270, 8, 50, 12, COLOR_BG);
    c->setTextSize(1);
    c->setTextColor(COLOR_DIM);
    c->setCursor(280, 10);
    c->print(buf);
  }

  int y = 35;
  int lineH = 18;

  // Zone: Cache name (changes when selected cache changes)
  snprintf(buf, ZONE_KEY_LEN, "dn_%d", listHighlightIndex);
  if (zoneMark(10, y, SCREEN_W - 20, 20, buf)) {
    c->fillRect(10, y, SCREEN_W - 20, 20, COLOR_BG);
    c->setTextSize(2);
    c->setTextColor(COLOR_TEXT);
    c->setCursor(10, y);
    char nameBuf[24];
    strncpy(nameBuf, cache.name, 22);
    nameBuf[22] = '\0';
    c->print(nameBuf);
  }
  y += 22;

  // Zone: GC Code (changes with cache selection)
  snprintf(buf, ZONE_KEY_LEN, "gc_%s", cache.gcCode);
  if (zoneMark(10, y, 200, 12, buf)) {
    c->fillRect(10, y, 200, 12, COLOR_BG);
    c->setTextSize(1);
    c->setTextColor(COLOR_HEADER);
    c->setCursor(10, y);
    c->print(cache.gcCode);
  }
  y += lineH + 4;

  // Zone: Coordinates (static per cache)
  snprintf(buf, ZONE_KEY_LEN, "%.4f%c %.4f%c",
           fabs(cache.latitude), cache.latitude >= 0 ? 'N' : 'S',
           fabs(cache.longitude), cache.longitude >= 0 ? 'E' : 'W');
  if (zoneMark(10, y, SCREEN_W - 20, 12, buf)) {
    c->fillRect(10, y, SCREEN_W - 20, 12, COLOR_BG);
    c->setTextColor(COLOR_VALUE);
    c->setTextSize(1);
    c->setCursor(10, y);
    c->print(buf);
  }
  y += lineH;

  // Zone: D/T (static per cache)
  snprintf(buf, ZONE_KEY_LEN, "D:%.1f T:%.1f", cache.difficulty, cache.terrain);
  if (zoneMark(10, y, SCREEN_W - 20, 12, buf)) {
    c->fillRect(10, y, SCREEN_W - 20, 12, COLOR_BG);
    c->setTextColor(COLOR_TEXT);
    c->setTextSize(1);
    c->setCursor(10, y);
    sprintf(buf, "Difficulty: %.1f  Terrain: %.1f", cache.difficulty, cache.terrain);
    c->print(buf);
  }
  y += lineH + 4;

  // Zone: Distance/Bearing (dynamic - changes with GPS position)
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);
  if (useMetricUnits)
    snprintf(buf, ZONE_KEY_LEN, "%.2fkm %d", distKm, (int)bearing);
  else
    snprintf(buf, ZONE_KEY_LEN, "%.2fmi %d", distKm * 0.621371, (int)bearing);
  if (zoneMark(10, y, SCREEN_W - 20, 12, buf)) {
    c->fillRect(10, y, SCREEN_W - 20, 12, COLOR_BG);
    c->setTextColor(COLOR_VALUE);
    c->setTextSize(1);
    c->setCursor(10, y);
    if (useMetricUnits)
      sprintf(buf, "Distance: %.2f km  Bearing: %d", distKm, (int)bearing);
    else
      sprintf(buf, "Distance: %.2f mi  Bearing: %d", distKm * 0.621371, (int)bearing);
    c->print(buf);
    c->print((char)247);
  }
  y += lineH + 4;

  // Zone: Hint (static per cache)
  if (strlen(cache.hint) > 0) {
    snprintf(buf, ZONE_KEY_LEN, "dh_%d_%.20s", listHighlightIndex, cache.hint);
    if (zoneMark(10, y, SCREEN_W - 20, 50, buf)) {
      c->fillRect(10, y, SCREEN_W - 20, 50, COLOR_BG);
      c->setTextColor(COLOR_DIM);
      c->setTextSize(1);
      c->setCursor(10, y);
      c->print("Hint: ");
      c->setTextColor(COLOR_TEXT);
      c->setCursor(10, y + 10);
      int hintLen = strlen(cache.hint);
      int pos = 0;
      int lineY = y + 10;
      while (pos < hintLen && lineY < 175) {
        int chunkLen = min(50, hintLen - pos);
        char chunk[52];
        strncpy(chunk, cache.hint + pos, chunkLen);
        chunk[chunkLen] = '\0';
        c->setCursor(10, lineY);
        c->print(chunk);
        pos += chunkLen;
        lineY += 10;
      }
    }
  }

  // Zone: Found status (changes on toggle)
  snprintf(buf, ZONE_KEY_LEN, "found_%d_%d", listHighlightIndex, cache.found ? 1 : 0);
  if (zoneMark(10, 178, SCREEN_W - 20, 20, buf)) {
    c->fillRect(10, 178, SCREEN_W - 20, 20, COLOR_BG);
    c->setCursor(10, 180);
    c->setTextSize(2);
    if (cache.found) {
      c->setTextColor(COLOR_VALUE);
      c->print("[* FOUND]");
    } else {
      c->setTextColor(COLOR_DIM);
      c->print("[ NOT FOUND ]");
    }
  }

  // Zone: Button hints (static)
  if (zoneMark(10, 203, SCREEN_W - 20, 12, "detail_hints")) {
    c->setTextSize(1);
    c->setTextColor(COLOR_DIM);
    c->setCursor(10, 205);
    c->print("[A]Prev [B]Next [C]Toggle [C+]Back");
  }
}

// drawScreenDiags() removed — content migrated to drawSettingsDiags() (#90)

// ============== OLED Display Functions ==============

void updateOLED() {
  oled.clearDisplay();

  // Show different content based on current screen (mirroring TFT)
  switch (currentScreen) {
    case SCREEN_COMPASS:
      drawOLEDScreenCompass();
      break;
    case SCREEN_GEOCACHE:
      drawOLEDScreenGeocache();
      break;
    case SCREEN_ENV:
      drawOLEDScreenEnv();
      break;
    case SCREEN_TELEMETRY:
      drawOLEDScreenTelemetry();
      break;
    case SCREEN_SETTINGS:
      oled.setTextSize(1);
      oled.setCursor(0, 0);
      oled.print("SETTINGS");
      break;
  }

  // Draw screen indicator
  drawOLEDNavBar();

  oled.display();
}

// drawOLEDScreenOps() removed — content now in Settings > About (#102)

// OLED Telemetry — combined GPS + IMU (#97)
void drawOLEDScreenTelemetry() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("TELEMETRY");

  // GPS section (rows 1-3)
  if (gpsData.valid) {
    oled.setCursor(0, 10);
    sprintf(buf, "%.5f %c", fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    oled.print(buf);

    oled.setCursor(0, 20);
    sprintf(buf, "%.5f %c", fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    oled.print(buf);

    float altVal = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
    char altUnit = useMetricUnits ? 'm' : 'f';
    oled.setCursor(0, 30);
    sprintf(buf, "%.0f%c S:%d H:%.1f", altVal, altUnit, gpsData.satellites, gpsData.hdop);
    oled.print(buf);
  } else if (gpsData.receiving) {
    oled.setCursor(0, 15);
    oled.print("Acquiring fix...");
    oled.setCursor(0, 25);
    sprintf(buf, "Sat: %d", gpsData.satellites);
    oled.print(buf);
  } else {
    oled.setCursor(0, 18);
    oled.print("No GPS data");
  }

  // IMU section (rows 4-5)
  if (imuAvailable && magAvailable) {
    oled.setCursor(0, 42);
    sprintf(buf, "%.0f%s R:%.0f P:%.0f", imuData.heading, getCardinal(imuData.heading), imuData.roll, imuData.pitch);
    oled.print(buf);

    oled.setCursor(0, 52);
    sprintf(buf, "Accel:%.2f m/s2", imuData.accelMag);
    oled.print(buf);
  } else {
    oled.setCursor(0, 46);
    oled.print("No IMU");
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
    if (useFahrenheit)
      sprintf(buf, "%.1fF %.1f%% IAQ:%.0f", tempF, humid, envData.iaq);
    else
      sprintf(buf, "%.1fC %.1f%% IAQ:%.0f", tempC, humid, envData.iaq);
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

// drawOLEDScreenDiags() removed — diagnostics now in Settings (#90)

void drawOLEDNavBar() {
  // Draw screen number indicator at bottom right
  oled.setCursor(100, 56);
  oled.setTextSize(1);
  char buf[8];
  sprintf(buf, "[%d/%d]", currentScreen + 1, NUM_SCREENS);
  oled.print(buf);
}
