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
#define FW_VERSION "0.48.1"

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

// ============== LVGL Integration (#105 — coexistence phase) ==============
#include <lvgl.h>
#include <esp_timer.h>              // For esp_timer_get_time() used by LVGL tick

// Set to 1 to enable LVGL test rendering (label in corner during boot).
// Set to 0 for normal operation where sprite pipeline handles all rendering.
#define LVGL_TEST_MODE 0

// LVGL display object and draw buffers (PSRAM-backed for performance)
static lv_display_t* lvglDisplay = NULL;

// Two 480x50 partial-render buffers in PSRAM (~48KB each, 96KB total)
// Allocated dynamically in initLVGL() via heap_caps_malloc(MALLOC_CAP_SPIRAM)
#define LVGL_BUF_LINES 50
#define LVGL_BUF_SIZE  (480 * LVGL_BUF_LINES * sizeof(uint16_t))
static uint8_t* lvglBuf1 = NULL;
static uint8_t* lvglBuf2 = NULL;

// LVGL initialization flag
static bool lvglAvailable = false;

// LVGL input devices (#106)
static lv_indev_t* lvglTouchIndev = NULL;   // FT6336U → LV_INDEV_TYPE_POINTER
static lv_indev_t* lvglEncoderIndev = NULL; // Buttons → LV_INDEV_TYPE_ENCODER
static lv_group_t* lvglGroup = NULL;        // Focus group for encoder navigation

// LVGL named styles — Field Compass theme (#107)
static lv_style_t fcStyleHeader;   // Cyan, XL (24) — screen titles
static lv_style_t fcStyleValue;    // Green, LG (20) — sensor values
static lv_style_t fcStyleHero;     // Green, HERO (32) — large numbers
static lv_style_t fcStyleBody;     // White, MD (18) — body text
static lv_style_t fcStyleLabel;    // Gray, SM (16) — secondary labels
static lv_style_t fcStyleWarn;     // Orange, MD (18) — warnings
static lv_style_t fcStyleError;    // Red, MD (18) — errors

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
#define FRAM_SETTINGS_ADDR  0x046C0    // User settings backup (after weather ring)
#define FRAM_SETTINGS_SIZE  128        // Allocated block size
#define FRAM_SETTINGS_MAGIC 0x53544E47 // "STNG" in ASCII
#define FRAM_SETTINGS_VER   1

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
#define GPS_STALE_MS  5000   // No bytes for 5s → clear receiving/valid (#115)
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

// LVGL color palette — RGB888 equivalents of RGB565 defines above (#107)
#define FC_COLOR_BG       lv_color_hex(0x000000)   // Black
#define FC_COLOR_TEXT     lv_color_hex(0xFFFFFF)   // White
#define FC_COLOR_HEADER   lv_color_hex(0x00FFFF)   // Cyan
#define FC_COLOR_VALUE    lv_color_hex(0x00FF00)   // Green
#define FC_COLOR_WARN     lv_color_hex(0xFF6400)   // Orange
#define FC_COLOR_ERROR    lv_color_hex(0xFF0000)   // Red
#define FC_COLOR_DIM      lv_color_hex(0x7B7B7B)   // Gray

// LVGL font aliases — semantic sizes for Field Compass UI (#107)
#define FC_FONT_XS    &lv_font_montserrat_14   // Fine labels, status text
#define FC_FONT_SM    &lv_font_montserrat_16   // Body text, list items
#define FC_FONT_MD    &lv_font_montserrat_18   // Standard values (theme default)
#define FC_FONT_LG    &lv_font_montserrat_20   // Emphasized values
#define FC_FONT_XL    &lv_font_montserrat_24   // Section headers
#define FC_FONT_XXL   &lv_font_montserrat_28   // Large headers
#define FC_FONT_HERO  &lv_font_montserrat_32   // Hero values (heading, telemetry)

// Widget-specific background colors — RGB888 from RGB565 (#108)
#define FC_COLOR_W_BAR      lv_color_hex(0x181818)   // Nav/action bar bg (0x18C3)
#define FC_COLOR_W_BTN      lv_color_hex(0x424242)   // Inactive button bg (0x4208)
#define FC_COLOR_W_OK       lv_color_hex(0x007D00)   // OK/selected bg (0x03E0)
#define FC_COLOR_W_INACTIVE lv_color_hex(0x212121)   // Unselected items (0x2104)
#define FC_COLOR_W_OVERLAY  lv_color_hex(0x080808)   // Modal overlay bg (0x0841)

// ============== Global Objects ==============

// TFT Display (ST7796U via TFT_eSPI — pins configured in User_Setup.h)
TFT_eSPI tft = TFT_eSPI();

// TFT_eSprite removed — LVGL handles all TFT rendering (#114)

// Zone-based partial push system removed — LVGL handles dirty tracking (#114)

// Capacitive touch controller (FT6336U on I2C at 0x38)
Adafruit_FT6206 ctp = Adafruit_FT6206();
volatile bool touchDetected = false;

// Legacy swipe/tap detection removed — LVGL gesture + click callbacks (#113)

// Settings screen state
int  settingsSubScreen = 0;    // 0=menu, 1=compass cal, 2=diagnostics, ...
int  previousScreen    = 0;    // Screen to return to when exiting settings
#define SETTINGS_MENU_COUNT 6  // Configuration, Display, Compass Cal, Diagnostics, About, Factory Reset (#104)
static const char* settingsMenuItems[] = {
  "Configuration",
  "Display",
  "Compass Cal",
  "Diagnostics",
  "About",
  "Factory Reset"
};

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
static unsigned long gpsLastByteTime = 0;    // Timestamp of last serial byte (#115)
static bool gpsDebugEnabled = false;          // Runtime GPS debug logging (#115)
// Multi-constellation RMC cycle tracking (#115)
static bool gprmcFixThisCycle = false;
static bool gnrmcFixThisCycle = false;
static unsigned long lastRmcCycleTime = 0;

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

// Display settings (#91)
uint8_t  tftBrightness   = 255;                            // PWM 25-255, step 25
uint32_t tftSleepMs      = 0;                              // 0 = never (default)
uint32_t oledSleepMs     = 300000;                         // 5 minutes default

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
enum SDIndicatorState { SD_IND_OK, SD_IND_ERROR, SD_IND_MISSING };  // (#120)
static bool settingsLoadedFromSD = false;  // Deferred load flag (#118)

#define SD_MAX_CONSECUTIVE_FAILURES 3
#define SD_MAX_REINIT_ATTEMPTS 10
#define SD_REINIT_COOLDOWN 15000  // Wait 15s between re-init attempts (#116)

// Forward declarations for SD health functions (defined after initSD)
void recordSDSuccess();
void recordSDError(SDErrorType err);
bool shouldAttemptReInit();
bool trySDReInit();
File sdOpenSafe(const char* path, const char* mode, bool silent = false);

// Forward declarations for FRAM settings backup (#118)
bool loadSettingsFromFRAM();
void saveSettingsToFRAM();

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

// ============== LVGL Tick Callback ==============

// LVGL needs a tick source to track elapsed time for animations/timers.
// On ESP32-S3, esp_timer_get_time() returns microseconds since boot.
static uint32_t lvglTickCb(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);  // Convert µs to ms
}

// ============== LVGL Flush Callback ==============

// Called by LVGL when a rendered region is ready to be sent to the display.
// Uses TFT_eSPI's SPI transaction-safe pushColors with byte swap.
void lvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  // swap=true: byte-swaps from LVGL native little-endian to ST7796U big-endian
  tft.pushColors((uint16_t*)px_map, w * h, true);
  tft.endWrite();

  lv_display_flush_ready(disp);
}

// ============== LVGL Log Callback ==============

#if LV_USE_LOG != 0
void lvglLogCb(lv_log_level_t level, const char* buf) {
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}
#endif

// ============== LVGL Touch Read Callback (#106) ==============

// Called by LVGL's indev timer (~33ms). Polls FT6336U over I2C.
// I2C reads are non-destructive — both legacy and LVGL read the same hardware.
// Touch diagnostics: track press/release transitions for debugging (#112)
static uint32_t touchPressCount = 0;
static uint32_t touchReleaseCount = 0;
static bool     touchWasPressed = false;
static int32_t  lastTouchX = -1;          // Last LVGL-space touch X (for diagnostics)
static int32_t  lastTouchY = -1;          // Last LVGL-space touch Y (for diagnostics)

void lvglTouchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
  (void)indev;

  if (!touchAvailable) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  if (ctp.touched()) {
    TS_Point p = ctp.getPoint();
    // Same coordinate transform as legacy pipeline:
    data->point.x = (int32_t)(480 - p.y);  // horizontal 0-479
    data->point.y = (int32_t)(p.x);        // vertical   0-319
    data->state   = LV_INDEV_STATE_PRESSED;
    lastTouchX = data->point.x;
    lastTouchY = data->point.y;
    lastActivityTime = millis();  // DIAG: keep TFT awake on LVGL screens too
    if (!touchWasPressed) {
      touchPressCount++;
      touchWasPressed = true;
      logPrintf("[TOUCH] PRESS @(%ld,%ld) scr=%d sub=%d\n",
                data->point.x, data->point.y, currentScreen, settingsSubScreen);
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    if (touchWasPressed) {
      touchReleaseCount++;
      touchWasPressed = false;
      // DIAG: log what LVGL thinks is pressed at touch coordinates
      lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &data->point);
      logPrintf("[TOUCH] RELEASE #%lu → hit_obj=%p (scr=%d sub=%d)\n",
                touchPressCount, (void*)hit, currentScreen, settingsSubScreen);
    }
  }
}

// ============== LVGL Encoder Read Callback (#106) ==============

// Maps A/B/C buttons to LVGL encoder: A=prev(-1), B=next(+1), C=enter.
// Edge detection emits a single enc_diff pulse per press.
void lvglEncoderReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
  (void)indev;

  static bool prevA = false, prevB = false, prevC = false;

  bool curA = !digitalRead(BUTTON_A);  // active LOW
  bool curB = !digitalRead(BUTTON_B);
  bool curC = !digitalRead(BUTTON_C);

  int16_t diff = 0;

  // Edge detect: fire once on press-down
  if (curA && !prevA) diff = -1;   // A = previous
  if (curB && !prevB) diff = +1;   // B = next

  data->enc_diff = diff;
  data->state = curC ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

  prevA = curA;
  prevB = curB;
  prevC = curC;
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

  // Deselect ALL SPI slave CS pins BEFORE any SPI bus activity (#116)
  // Without this, SD_CS (GPIO 10) and FRAM_CS (GPIO 15) float during TFT init,
  // allowing 80MHz TFT traffic to corrupt idle SPI slaves on the shared bus.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(FRAM_CS, OUTPUT);
  digitalWrite(FRAM_CS, HIGH);
  logPrintln("[SPI] CS pins pre-set HIGH: SD_CS=10, FRAM_CS=15");

  LOG_DEBUG("About to init TFT...");
  Serial.flush();

  // Initialize TFT first for visual feedback (TFT_eSPI handles SPI init)
  initTFT();

  LOG_DEBUG("TFT init done");
  Serial.flush();

  // Initialize LVGL (coexistence: runs alongside sprite pipeline)
  initLVGL();

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
  // CS pins already set HIGH at top of setup() — SPI.begin() re-confirms SD_CS
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  logPrintf("[SPI] Bus started: SCK=%d MISO=%d MOSI=%d SS=%d\n",
            SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
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

static unsigned long loopCount = 0;  // Loop frequency counter (#112)

void loop() {
  loopCount++;

  // Handle button navigation
  handleButtons();

  // Touch input handled by LVGL indev (lvglTouchReadCb) — no manual polling needed (#113)

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

  // Deferred settings load: if SD wasn't available at boot but recovered
  // via re-init, load settings now so user prefs aren't lost (#118)
  if (sdAvailable && !settingsLoadedFromSD) {
    logPrintln("[SETTINGS] SD recovered — loading deferred settings");
    loadSettings();
    analogWrite(TFT_BL, tftBrightness);  // Apply restored brightness
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

    // Loop speed + touch diagnostics (#112)
    static unsigned long loopCountLast = 0;
    unsigned long loopHz = (loopCount - loopCountLast);  // loops in last 10s
    loopCountLast = loopCount;
    logPrintf("[LOOP] %luHz touch:%lu/%lu (press/release)\n",
             loopHz / 10, touchPressCount, touchReleaseCount);

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

  // LVGL timer handler — process animations, redraws, timers (#109)
  if (lvglAvailable && !tftSleeping) {
    lv_timer_handler();
  }

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

  // TFT_eSprite removed — PSRAM now used for LVGL draw buffers only (#114)
  logPrintf("OK (480x320, PSRAM: %dKB free)\n", ESP.getFreePsram() / 1024);
}

// ============== Field Compass LVGL Theme (#107) ==============
void initFCTheme() {
  // Initialize default dark theme with cyan primary, green secondary
  lv_theme_t* theme = lv_theme_default_init(
      lvglDisplay,
      FC_COLOR_HEADER,    // primary — focus rings, active elements
      FC_COLOR_VALUE,     // secondary — accents, toggles
      true,               // dark mode
      FC_FONT_MD          // default app font = 18px Montserrat
  );
  lv_display_set_theme(lvglDisplay, theme);

  // Screen background: black
  lv_obj_set_style_bg_color(lv_screen_active(), FC_COLOR_BG, 0);

  // Initialize named styles
  lv_style_init(&fcStyleHeader);
  lv_style_set_text_color(&fcStyleHeader, FC_COLOR_HEADER);
  lv_style_set_text_font(&fcStyleHeader, FC_FONT_XL);

  lv_style_init(&fcStyleValue);
  lv_style_set_text_color(&fcStyleValue, FC_COLOR_VALUE);
  lv_style_set_text_font(&fcStyleValue, FC_FONT_LG);

  lv_style_init(&fcStyleHero);
  lv_style_set_text_color(&fcStyleHero, FC_COLOR_VALUE);
  lv_style_set_text_font(&fcStyleHero, FC_FONT_HERO);

  lv_style_init(&fcStyleBody);
  lv_style_set_text_color(&fcStyleBody, FC_COLOR_TEXT);
  lv_style_set_text_font(&fcStyleBody, FC_FONT_MD);

  lv_style_init(&fcStyleLabel);
  lv_style_set_text_color(&fcStyleLabel, FC_COLOR_DIM);
  lv_style_set_text_font(&fcStyleLabel, FC_FONT_SM);

  lv_style_init(&fcStyleWarn);
  lv_style_set_text_color(&fcStyleWarn, FC_COLOR_WARN);
  lv_style_set_text_font(&fcStyleWarn, FC_FONT_MD);

  lv_style_init(&fcStyleError);
  lv_style_set_text_color(&fcStyleError, FC_COLOR_ERROR);
  lv_style_set_text_font(&fcStyleError, FC_FONT_MD);

  logPrintln("[LVGL] Field Compass theme initialized (7 styles)");
}

// ============== FC Widget Library (#108) ==============

// Forward declarations for callbacks used in widgets (#113)
static void gearIconClickCb(lv_event_t* e);
static void screenGestureCb(lv_event_t* e);

// --- Header Bar: 30px cyan bar with title + gear icon ---
lv_obj_t* fcHeaderCreate(lv_obj_t* parent, const char* title) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 30);
  lv_obj_set_pos(cont, 0, 0);
  lv_obj_set_style_bg_color(cont, FC_COLOR_HEADER, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: title label — black on cyan
  lv_obj_t* titleLbl = lv_label_create(cont);
  lv_label_set_text(titleLbl, title);
  lv_obj_set_style_text_color(titleLbl, FC_COLOR_BG, 0);
  lv_obj_set_style_text_font(titleLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(titleLbl, 10, 7);

  // Child [1]: gear button — tappable settings icon
  lv_obj_t* gearBtn = lv_button_create(cont);
  lv_obj_remove_style_all(gearBtn);
  lv_obj_set_size(gearBtn, 30, 30);
  lv_obj_align(gearBtn, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_opa(gearBtn, LV_OPA_TRANSP, 0);

  lv_obj_t* gearLbl = lv_label_create(gearBtn);
  lv_label_set_text(gearLbl, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_color(gearLbl, FC_COLOR_BG, 0);
  lv_obj_set_style_text_font(gearLbl, FC_FONT_SM, 0);
  lv_obj_center(gearLbl);

  // Gear icon click → Settings (#113) — callback defined in navigation section
  lv_obj_add_event_cb(gearBtn, gearIconClickCb, LV_EVENT_CLICKED, NULL);

  // Child [2]: SD status indicator — hidden when healthy (#120)
  lv_obj_t* sdLbl = lv_label_create(cont);
  lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD);
  lv_obj_set_style_text_font(sdLbl, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(sdLbl, lv_color_hex(0x808080), 0);  // default gray
  lv_obj_align(sdLbl, LV_ALIGN_RIGHT_MID, -38, 0);
  // Starts visible — updateSDIndicators() sets color per state (#120)

  return cont;
}

void fcHeaderSetTitle(lv_obj_t* header, const char* title) {
  lv_obj_t* titleLbl = lv_obj_get_child(header, 0);
  if (titleLbl) lv_label_set_text(titleLbl, title);
}

void fcHeaderSetSDStatus(lv_obj_t* header, SDIndicatorState state) {
  if (!header) return;
  lv_obj_t* sdLbl = lv_obj_get_child(header, 2);  // child[2] = SD indicator
  if (!sdLbl) return;
  lv_obj_clear_flag(sdLbl, LV_OBJ_FLAG_HIDDEN);
  if (state == SD_IND_OK) {
    lv_obj_set_style_text_color(sdLbl, lv_color_hex(0x2E7D32), 0); // dim green
    lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD);
  } else if (state == SD_IND_ERROR) {
    lv_obj_set_style_text_color(sdLbl, FC_COLOR_ERROR, 0);         // red
    lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD " " LV_SYMBOL_CLOSE);
  } else {  // SD_IND_MISSING
    lv_obj_set_style_text_color(sdLbl, lv_color_hex(0x808080), 0); // gray
    lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD);
  }
}

// --- Nav Bar: 25px dark gray bar with numbered screen dots ---
lv_obj_t* fcNavBarCreate(lv_obj_t* parent, uint8_t screenCount, uint8_t activeIdx) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 25);
  lv_obj_set_pos(cont, 0, SCREEN_H - 25);
  lv_obj_set_style_bg_color(cont, FC_COLOR_W_BAR, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: "A<" hint
  lv_obj_t* hintL = lv_label_create(cont);
  lv_label_set_text(hintL, "A<");
  lv_obj_set_style_text_color(hintL, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(hintL, FC_FONT_XS, 0);
  lv_obj_set_pos(hintL, 10, 5);

  // Children [1..N]: numbered dots
  int startX = 80;
  int spacing = 40;
  for (uint8_t i = 0; i < screenCount; i++) {
    lv_obj_t* dot = lv_obj_create(cont);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 28, 19);
    lv_obj_set_pos(dot, startX + i * spacing, 3);
    lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                          | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

    bool active = (i == activeIdx);
    if (active) {
      lv_obj_set_style_bg_color(dot, FC_COLOR_HEADER, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    lv_obj_t* numLbl = lv_label_create(dot);
    char num[2] = { (char)('1' + i), '\0' };
    lv_label_set_text(numLbl, num);
    lv_obj_set_style_text_font(numLbl, FC_FONT_SM, 0);
    lv_obj_set_style_text_color(numLbl, active ? FC_COLOR_BG : FC_COLOR_DIM, 0);
    lv_obj_center(numLbl);
  }

  // Child [N+1]: ">B" hint
  lv_obj_t* hintR = lv_label_create(cont);
  lv_label_set_text(hintR, ">B");
  lv_obj_set_style_text_color(hintR, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(hintR, FC_FONT_XS, 0);
  lv_obj_set_pos(hintR, SCREEN_W - 30, 5);

  return cont;
}

void fcNavBarSetActive(lv_obj_t* navBar, uint8_t activeIdx) {
  uint32_t count = lv_obj_get_child_count(navBar);
  // Children: [0]=hintL, [1..N-2]=dots, [N-1]=hintR
  for (uint32_t i = 1; i < count - 1; i++) {
    lv_obj_t* dot = lv_obj_get_child(navBar, i);
    bool active = ((i - 1) == activeIdx);

    if (active) {
      lv_obj_set_style_bg_color(dot, FC_COLOR_HEADER, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    } else {
      lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
    }

    lv_obj_t* numLbl = lv_obj_get_child(dot, 0);
    lv_obj_set_style_text_color(numLbl, active ? FC_COLOR_BG : FC_COLOR_DIM, 0);
  }
}

// --- Action Bar: 50px dark gray bar with Back/OK buttons ---
lv_obj_t* fcActionBarCreate(lv_obj_t* parent, bool showBack, bool showOK) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 50);
  lv_obj_set_pos(cont, 0, 270);
  lv_obj_set_style_bg_color(cont, FC_COLOR_W_BAR, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: Back button — 40px tall with 15px extended hit area for reliable
  // finger targeting on 3.5" capacitive display (touches land ~5-14px above visual)
  lv_obj_t* backBtn = lv_button_create(cont);
  lv_obj_set_size(backBtn, 120, 40);
  lv_obj_set_pos(backBtn, 5, 5);
  lv_obj_set_style_bg_color(backBtn, FC_COLOR_W_BTN, 0);
  lv_obj_set_style_radius(backBtn, 6, 0);
  lv_obj_set_ext_click_area(backBtn, 15);  // +15px invisible hit padding all sides
  if (!showBack) lv_obj_add_flag(backBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(backLbl, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(backLbl, FC_FONT_SM, 0);
  lv_obj_center(backLbl);

  // Child [1]: OK button — same enlarged sizing + extended hit area
  lv_obj_t* okBtn = lv_button_create(cont);
  lv_obj_set_size(okBtn, 120, 40);
  lv_obj_set_pos(okBtn, 355, 5);
  lv_obj_set_style_bg_color(okBtn, FC_COLOR_W_OK, 0);
  lv_obj_set_style_radius(okBtn, 6, 0);
  lv_obj_set_ext_click_area(okBtn, 15);  // +15px invisible hit padding all sides
  if (!showOK) lv_obj_add_flag(okBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* okLbl = lv_label_create(okBtn);
  lv_label_set_text(okLbl, "OK " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(okLbl, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(okLbl, FC_FONT_SM, 0);
  lv_obj_center(okLbl);

  return cont;
}

// --- Toggle: two-option selector with green=selected ---

// Internal: update toggle visuals based on value
static void fcToggleUpdateVisuals(lv_obj_t* cont, bool value) {
  lv_obj_t* btnA = lv_obj_get_child(cont, 1);
  lv_obj_t* btnB = lv_obj_get_child(cont, 2);

  lv_obj_set_style_bg_color(btnA, value ? FC_COLOR_W_INACTIVE : FC_COLOR_W_OK, 0);
  lv_obj_set_style_bg_color(btnB, value ? FC_COLOR_W_OK : FC_COLOR_W_INACTIVE, 0);

  lv_obj_t* lblA = lv_obj_get_child(btnA, 0);
  lv_obj_t* lblB = lv_obj_get_child(btnB, 0);
  lv_obj_set_style_text_color(lblA, value ? FC_COLOR_DIM : FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_color(lblB, value ? FC_COLOR_TEXT : FC_COLOR_DIM, 0);
}

// Internal: toggle click handler
static void fcToggleClickCb(lv_event_t* e) {
  lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* cont = lv_obj_get_parent(btn);
  lv_obj_t* btnB = lv_obj_get_child(cont, 2);

  bool newVal = (btn == btnB);
  lv_obj_set_user_data(cont, (void*)(intptr_t)newVal);
  fcToggleUpdateVisuals(cont, newVal);
  lv_obj_send_event(cont, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t* fcToggleCreate(lv_obj_t* parent, int16_t y,
                          const char* label, const char* optA,
                          const char* optB, bool value) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W - 20, 30);
  lv_obj_set_pos(cont, 10, y);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: label
  lv_obj_t* lbl = lv_label_create(cont);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(lbl, FC_FONT_SM, 0);
  lv_obj_set_pos(lbl, 10, 7);

  // Child [1]: option A button (130px wide — no ext_click_area needed)
  int ax = 140;
  lv_obj_t* btnA = lv_button_create(cont);
  lv_obj_set_size(btnA, 130, 30);
  lv_obj_set_pos(btnA, ax, 0);
  lv_obj_set_style_radius(btnA, 6, 0);
  lv_obj_add_event_cb(btnA, fcToggleClickCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lblA = lv_label_create(btnA);
  lv_label_set_text(lblA, optA);
  lv_obj_set_style_text_font(lblA, FC_FONT_SM, 0);
  lv_obj_center(lblA);

  // Child [2]: option B button (130px wide — no ext_click_area needed)
  int bx = ax + 130 + 10;
  lv_obj_t* btnB = lv_button_create(cont);
  lv_obj_set_size(btnB, 130, 30);
  lv_obj_set_pos(btnB, bx, 0);
  lv_obj_set_style_radius(btnB, 6, 0);
  lv_obj_add_event_cb(btnB, fcToggleClickCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lblB = lv_label_create(btnB);
  lv_label_set_text(lblB, optB);
  lv_obj_set_style_text_font(lblB, FC_FONT_SM, 0);
  lv_obj_center(lblB);

  // Set initial value and visuals
  lv_obj_set_user_data(cont, (void*)(intptr_t)value);
  fcToggleUpdateVisuals(cont, value);

  return cont;
}

bool fcToggleGetValue(lv_obj_t* toggle) {
  return (bool)(intptr_t)lv_obj_get_user_data(toggle);
}

void fcToggleSetValue(lv_obj_t* toggle, bool value) {
  lv_obj_set_user_data(toggle, (void*)(intptr_t)value);
  fcToggleUpdateVisuals(toggle, value);
}

// --- Dropdown: single button covering full row (#117 rewrite v0.46.3) ---
// Uses lv_button_create (not lv_obj_create) to match the proven click pattern
// used by menu buttons, toggles, and action bar — eliminates hit-test ambiguity.
// Children: [0]=label, [1]=value text, [2]=arrow
lv_obj_t* fcDropdownCreate(lv_obj_t* parent, int16_t y,
                            const char* label, const char* initialValue) {
  lv_obj_t* btn = lv_button_create(parent);
  lv_obj_set_size(btn, SCREEN_W - 20, 36);
  lv_obj_set_pos(btn, 10, y);
  lv_obj_set_style_bg_color(btn, FC_COLOR_W_INACTIVE, 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_clear_flag(btn, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: label text (left side)
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(lbl, FC_FONT_SM, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

  // Child [1]: value text (center-right)
  lv_obj_t* valLbl = lv_label_create(btn);
  lv_label_set_text(valLbl, initialValue);
  lv_obj_set_style_text_color(valLbl, FC_COLOR_VALUE, 0);
  lv_obj_set_style_text_font(valLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(valLbl, 150, 10);

  // Child [2]: down arrow (far right)
  lv_obj_t* arrowLbl = lv_label_create(btn);
  lv_label_set_text(arrowLbl, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(arrowLbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(arrowLbl, FC_FONT_XS, 0);
  lv_obj_align(arrowLbl, LV_ALIGN_RIGHT_MID, -5, 0);

  // Store selected index in user_data (default 0)
  lv_obj_set_user_data(btn, (void*)(intptr_t)0);

  return btn;
}

int fcDropdownGetIndex(lv_obj_t* dropdown) {
  return (int)(intptr_t)lv_obj_get_user_data(dropdown);
}

void fcDropdownSetValue(lv_obj_t* dropdown, int idx, const char* text) {
  lv_obj_set_user_data(dropdown, (void*)(intptr_t)idx);
  // Child [1] is the value label directly (flat structure, no nested button)
  lv_obj_t* valLbl = lv_obj_get_child(dropdown, 1);
  lv_label_set_text(valLbl, text);
}

// --- List Picker: modal scrollable selection overlay ---

// Track active list picker overlay — prevents stacking (#119)
static lv_obj_t* fcListPickerActiveOverlay = NULL;

// Internal: list item click handler — select and close
static void fcListPickerItemCb(lv_event_t* e) {
  lv_obj_t* itemBtn = (lv_obj_t*)lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(itemBtn);

  // Navigate up: itemBtn -> list -> box -> overlay
  lv_obj_t* list = lv_obj_get_parent(itemBtn);
  lv_obj_t* box = lv_obj_get_parent(list);
  lv_obj_t* overlay = lv_obj_get_parent(box);

  // Get caller stored in overlay user_data
  lv_obj_t* caller = (lv_obj_t*)lv_obj_get_user_data(overlay);

  // Notify caller with selected index
  if (caller) {
    lv_obj_send_event(caller, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
  }

  // Close: async-delete overlay to avoid use-after-free during event callback (#112)
  fcListPickerActiveOverlay = NULL;  // Clear tracker before delete (#119)
  lv_obj_delete_async(overlay);
}

lv_obj_t* fcListPickerOpen(const char* title, const char** items,
                            int count, int selectedIdx, lv_obj_t* caller) {
  // Guard: destroy any existing overlay before creating a new one (#119)
  if (fcListPickerActiveOverlay != NULL) {
    logPrintln("[LVGL/MEM] Closing stale list picker overlay before opening new one");
    lv_obj_delete(fcListPickerActiveOverlay);
    fcListPickerActiveOverlay = NULL;
  }

  // Safety check: ensure enough LVGL heap for overlay (~count*2+4 objects) (#119)
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  logPrintf("[LVGL/MEM] ListPicker open '%s' (%d items): free=%lu frag=%d%%\n",
            title, count, (unsigned long)mon.free_size, mon.frag_pct);
  if (mon.free_size < 8192) {  // 8KB minimum — raised from 4KB after OOM crash (#119)
    logPrintf("[LVGL/MEM] ABORT: insufficient heap (%lu < 8192) for picker!\n",
              (unsigned long)mon.free_size);
    return NULL;
  }

  // Full-screen semi-transparent overlay
  lv_obj_t* overlay = lv_obj_create(lv_screen_active());
  if (!overlay) { logPrintln("[LVGL/MEM] ListPicker overlay alloc failed"); return NULL; }
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_user_data(overlay, (void*)caller);

  // Modal container: 420x220, centered
  lv_obj_t* box = lv_obj_create(overlay);
  if (!box) { logPrintln("[LVGL/MEM] ListPicker box alloc failed"); lv_obj_delete(overlay); return NULL; }
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 420, 220);
  lv_obj_center(box);
  lv_obj_set_style_bg_color(box, FC_COLOR_W_OVERLAY, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, FC_COLOR_DIM, 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  // Title label
  lv_obj_t* titleLbl = lv_label_create(box);
  if (!titleLbl) { lv_obj_delete(overlay); return NULL; }
  lv_label_set_text(titleLbl, title);
  lv_obj_set_style_text_color(titleLbl, FC_COLOR_HEADER, 0);
  lv_obj_set_style_text_font(titleLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(titleLbl, 10, 6);

  // Scrollable list area
  lv_obj_t* listArea = lv_obj_create(box);
  if (!listArea) { lv_obj_delete(overlay); return NULL; }
  lv_obj_remove_style_all(listArea);
  lv_obj_set_size(listArea, 410, 180);
  lv_obj_set_pos(listArea, 5, 32);
  lv_obj_set_style_bg_opa(listArea, LV_OPA_TRANSP, 0);
  lv_obj_set_flex_flow(listArea, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(listArea, 4, 0);

  // List items
  for (int i = 0; i < count; i++) {
    lv_obj_t* itemBtn = lv_button_create(listArea);
    if (!itemBtn) break;  // OOM mid-loop — stop gracefully
    lv_obj_set_size(itemBtn, 400, 32);
    lv_obj_set_style_bg_color(itemBtn,
      (i == selectedIdx) ? FC_COLOR_W_OK : FC_COLOR_W_OVERLAY, 0);
    lv_obj_set_style_radius(itemBtn, 4, 0);
    lv_obj_set_style_min_height(itemBtn, 32, 0);
    lv_obj_set_user_data(itemBtn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(itemBtn, fcListPickerItemCb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* itemLbl = lv_label_create(itemBtn);
    if (!itemLbl) break;
    lv_label_set_text(itemLbl, items[i]);
    lv_obj_set_style_text_color(itemLbl, FC_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(itemLbl, FC_FONT_SM, 0);
    lv_obj_align(itemLbl, LV_ALIGN_LEFT_MID, 10, 0);
  }

  // Scroll to selected item
  if (selectedIdx > 0 && selectedIdx < count) {
    lv_obj_t* selBtn = lv_obj_get_child(listArea, selectedIdx);
    if (selBtn) lv_obj_scroll_to_view(selBtn, LV_ANIM_OFF);
  }

  fcListPickerActiveOverlay = overlay;
  return overlay;
}

// ============== LVGL Compass Screen (#109) ==============

// Compass screen container and child widgets
static lv_obj_t* compassScr      = NULL;  // Root container (full screen)
static lv_obj_t* compassHeader   = NULL;  // fcHeader widget
static lv_obj_t* compassNavBar   = NULL;  // fcNavBar widget
static lv_obj_t* compassRoseObj  = NULL;  // Custom draw rose area
static lv_obj_t* compassLblHdg   = NULL;  // "204° SW"
static lv_obj_t* compassLblLat   = NULL;  // "Lat 39.3525N"
static lv_obj_t* compassLblLon   = NULL;  // "Lon 84.3825W"
static lv_obj_t* compassLblAlt   = NULL;  // "Alt 820 ft"
static lv_obj_t* compassLblSpd   = NULL;  // "Spd 2.3 mph"
static lv_obj_t* compassLblTemp  = NULL;  // "Temp 72.5°F"
static lv_obj_t* compassLblFcst  = NULL;  // "↑ Fair"
static lv_obj_t* compassLblGps   = NULL;  // "GPS OK Sat:8"
static lv_obj_t* compassLblTime  = NULL;  // "3:42:15 PM"

// Cardinal direction labels (rotate with heading around rose)
static lv_obj_t* compassLblN = NULL;
static lv_obj_t* compassLblE = NULL;
static lv_obj_t* compassLblS = NULL;
static lv_obj_t* compassLblW = NULL;

// Compass rose draw callback (forward declare — implemented in Task 2)
static void compassRoseDrawCb(lv_event_t* e);

// Last drawn heading for invalidation threshold
static float compassLastHeading = -999.0f;

// ============== LVGL Geocache Screen (#110) ==============

// Root + sub-screen containers
static lv_obj_t* geocacheScr = NULL;        // Root container
static lv_obj_t* geocacheNavCtr = NULL;      // Nav sub-screen container
static lv_obj_t* geocacheListCtr = NULL;     // List sub-screen container
static lv_obj_t* geocacheDetailsCtr = NULL;  // Details sub-screen container

// Nav sub-screen labels
static lv_obj_t* gcNavHeader = NULL;
static lv_obj_t* gcNavNavBar = NULL;
static lv_obj_t* gcNavLblName = NULL;
static lv_obj_t* gcNavLblDist = NULL;
static lv_obj_t* gcNavLblDT = NULL;
static lv_obj_t* gcNavLblBearing = NULL;
static lv_obj_t* gcNavGraphicObj = NULL;     // Custom draw area
static lv_obj_t* gcNavLblAccuracy = NULL;
static lv_obj_t* gcNavLblHint = NULL;

// List sub-screen handles
static lv_obj_t* gcListHeader = NULL;
static lv_obj_t* gcListNavBar = NULL;
static lv_obj_t* gcListLblCount = NULL;
static lv_obj_t* gcListScrollCtr = NULL;
static lv_obj_t* gcListLblHints = NULL;

// List row handles (MAX_CACHES=20)
static lv_obj_t* gcListRows[MAX_CACHES] = {};
static lv_obj_t* gcListRowSelector[MAX_CACHES] = {};
static lv_obj_t* gcListRowDist[MAX_CACHES] = {};
static lv_obj_t* gcListRowName[MAX_CACHES] = {};
static lv_obj_t* gcListRowFound[MAX_CACHES] = {};
static lv_obj_t* gcListRowDT[MAX_CACHES] = {};

// Details sub-screen labels
static lv_obj_t* gcDetHeader = NULL;
static lv_obj_t* gcDetNavBar = NULL;
static lv_obj_t* gcDetLblCount = NULL;
static lv_obj_t* gcDetLblName = NULL;
static lv_obj_t* gcDetLblGC = NULL;
static lv_obj_t* gcDetLblCoords = NULL;
static lv_obj_t* gcDetLblDT = NULL;
static lv_obj_t* gcDetLblDist = NULL;
static lv_obj_t* gcDetLblHintLabel = NULL;
static lv_obj_t* gcDetLblHint = NULL;
static lv_obj_t* gcDetLblFound = NULL;
static lv_obj_t* gcDetLblHints = NULL;

// Nav graphic state tracking
static float gcNavLastBearing = -999;
static float gcNavLastHeading = -999;
static bool  gcNavLastInZone = false;
static int32_t gcNavPulseRadius = 0;  // For search zone animation

// Forward declarations (#110)
static void geocacheNavDrawCb(lv_event_t* e);

// ============== LVGL Environment Screen (#111) ==============

// Root container
static lv_obj_t* envScr = NULL;

// Header + NavBar
static lv_obj_t* envHeader = NULL;
static lv_obj_t* envNavBar = NULL;

// Label pairs: dim label + colored value
static lv_obj_t* envLblTempLabel = NULL;
static lv_obj_t* envLblTempValue = NULL;
static lv_obj_t* envLblHumidLabel = NULL;
static lv_obj_t* envLblHumidValue = NULL;
static lv_obj_t* envLblIaqLabel = NULL;
static lv_obj_t* envLblIaqValue = NULL;
static lv_obj_t* envLblCo2Label = NULL;
static lv_obj_t* envLblCo2Value = NULL;
static lv_obj_t* envLblPressLabel = NULL;
static lv_obj_t* envLblPressValue = NULL;
static lv_obj_t* envLblFcstLabel = NULL;
static lv_obj_t* envLblFcstValue = NULL;

// Error state label (no sensors)
static lv_obj_t* envLblNoSensors = NULL;

// ============== LVGL Telemetry Screen (#111) ==============

// Root container
static lv_obj_t* telemetryScr = NULL;

// Header + NavBar
static lv_obj_t* telHeader = NULL;
static lv_obj_t* telNavBar = NULL;

// Section labels
static lv_obj_t* telLblGpsSection = NULL;
static lv_obj_t* telLblImuSection = NULL;

// GPS data labels (left column: label+value, right column: label+value)
static lv_obj_t* telLblLatLabel = NULL;
static lv_obj_t* telLblLatValue = NULL;
static lv_obj_t* telLblLonLabel = NULL;
static lv_obj_t* telLblLonValue = NULL;
static lv_obj_t* telLblAltLabel = NULL;
static lv_obj_t* telLblAltValue = NULL;
static lv_obj_t* telLblSpdLabel = NULL;
static lv_obj_t* telLblSpdValue = NULL;
static lv_obj_t* telLblSatLabel = NULL;
static lv_obj_t* telLblSatValue = NULL;
static lv_obj_t* telLblHdopLabel = NULL;
static lv_obj_t* telLblHdopValue = NULL;
static lv_obj_t* telLblStatusLabel = NULL;
static lv_obj_t* telLblStatusValue = NULL;

// GPS acquiring/error state labels
static lv_obj_t* telLblGpsAcquiring = NULL;
static lv_obj_t* telLblGpsElapsed = NULL;
static lv_obj_t* telLblGpsSkyHint = NULL;
static lv_obj_t* telLblGpsSatCount = NULL;
static lv_obj_t* telLblGpsNoData = NULL;
static lv_obj_t* telLblGpsCheckConn = NULL;

// Divider line
static lv_obj_t* telDivider = NULL;

// IMU data labels
static lv_obj_t* telLblHdgLabel = NULL;
static lv_obj_t* telLblHdgValue = NULL;
static lv_obj_t* telLblRollLabel = NULL;
static lv_obj_t* telLblRollValue = NULL;
static lv_obj_t* telLblPitchLabel = NULL;
static lv_obj_t* telLblPitchValue = NULL;
static lv_obj_t* telLblAccelLabel = NULL;
static lv_obj_t* telLblAccelValue = NULL;

// IMU error state
static lv_obj_t* telLblNoImu = NULL;

// ============== LVGL Settings Screen (#112) ==============

// Root + sub-screen containers
static lv_obj_t* settingsScr       = NULL;
static lv_obj_t* settingsMenuCtr   = NULL;
static lv_obj_t* settingsConfigCtr  = NULL;
static lv_obj_t* settingsDisplayCtr = NULL;
static lv_obj_t* settingsCalCtr     = NULL;
static lv_obj_t* settingsDiagsCtr   = NULL;
static lv_obj_t* settingsAboutCtr   = NULL;
static lv_obj_t* settingsResetCtr   = NULL;
static lv_obj_t* settingsMenuBtns[6];

// Config sub-screen widget pointers (#112)
static lv_obj_t* cfgTzDropdown   = NULL;
static lv_obj_t* cfgTimeToggle   = NULL;
static lv_obj_t* cfgTempToggle   = NULL;
static lv_obj_t* cfgDistToggle   = NULL;
static lv_obj_t* cfgPreviewLabel = NULL;

// Display sub-screen widget pointers (#112)
static lv_obj_t* dispBrightnessSlider = NULL;
static lv_obj_t* dispBrightnessLabel  = NULL;
static lv_obj_t* dispTftDropdown      = NULL;
static lv_obj_t* dispOledDropdown     = NULL;

// Compass Cal sub-screen widget pointers (#112)
// Idle state
static lv_obj_t* calStatusLabel   = NULL;
static lv_obj_t* calOffsetsLabel  = NULL;
static lv_obj_t* calStartBtn      = NULL;
static lv_obj_t* calIdleActBar    = NULL;
// Active state (hidden during idle)
static lv_obj_t* calArc            = NULL;
static lv_obj_t* calCountdownLabel = NULL;
static lv_obj_t* calInstructLabel  = NULL;
static lv_obj_t* calMinMaxLabel    = NULL;

// Diagnostics sub-screen widget pointers (#112)
static lv_obj_t* diagValueLabels[11];  // 11 value labels updated each frame

// About sub-screen widget pointers (#112)
static lv_obj_t* aboutValueLabels[6];  // 6 value labels (some live-updating)

void buildCompassScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  compassScr = lv_obj_create(NULL);
  lv_obj_set_size(compassScr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(compassScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(compassScr, 0, 0);
  lv_obj_set_style_radius(compassScr, 0, 0);
  lv_obj_set_style_pad_all(compassScr, 0, 0);
  lv_obj_clear_flag(compassScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(compassScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Header bar (reuse #108 widget)
  compassHeader = fcHeaderCreate(compassScr, "COMPASS");

  // Vertical separator line between panels
  lv_obj_t* sep = lv_obj_create(compassScr);
  lv_obj_remove_style_all(sep);
  lv_obj_set_size(sep, 1, 256);
  lv_obj_set_pos(sep, 178, 34);
  lv_obj_set_style_bg_color(sep, lv_color_hex(0x212121), 0);
  lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

  // === Left Panel Labels ===

  // Heading + cardinal (large green text)
  compassLblHdg = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblHdg, 8, 34);
  lv_obj_set_style_text_font(compassLblHdg, FC_FONT_XXL, 0);
  lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblHdg, "--");

  // Latitude
  compassLblLat = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblLat, 8, 72);
  lv_obj_set_style_text_font(compassLblLat, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblLat, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblLat, "Lat --");

  // Longitude
  compassLblLon = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblLon, 8, 90);
  lv_obj_set_style_text_font(compassLblLon, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblLon, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblLon, "Lon --");

  // Altitude
  compassLblAlt = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblAlt, 8, 112);
  lv_obj_set_style_text_font(compassLblAlt, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblAlt, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblAlt, "Alt --");

  // Speed
  compassLblSpd = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblSpd, 8, 132);
  lv_obj_set_style_text_font(compassLblSpd, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblSpd, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblSpd, "Spd --");

  // Temperature
  compassLblTemp = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblTemp, 8, 152);
  lv_obj_set_style_text_font(compassLblTemp, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblTemp, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblTemp, "Temp --");

  // Forecast
  compassLblFcst = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblFcst, 8, 172);
  lv_obj_set_style_text_font(compassLblFcst, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblFcst, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblFcst, "Fcst --");

  // GPS status
  compassLblGps = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblGps, 8, 200);
  lv_obj_set_style_text_font(compassLblGps, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(compassLblGps, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblGps, "GPS --");

  // Time (bottom of left panel)
  compassLblTime = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblTime, 8, 268);
  lv_obj_set_style_text_font(compassLblTime, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(compassLblTime, FC_COLOR_TEXT, 0);
  lv_label_set_text(compassLblTime, "--:--");

  // === Right Panel: Compass Rose (custom draw) ===
  compassRoseObj = lv_obj_create(compassScr);
  lv_obj_remove_style_all(compassRoseObj);
  lv_obj_set_size(compassRoseObj, 298, 260);
  lv_obj_set_pos(compassRoseObj, 182, 30);
  lv_obj_clear_flag(compassRoseObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(compassRoseObj, compassRoseDrawCb, LV_EVENT_DRAW_MAIN, NULL);

  // Lubber line — fixed orange triangle at top of rose (does not rotate)
  // Uses a small filled rectangle as approximation; future polish can use draw callback
  lv_obj_t* lubber = lv_obj_create(compassScr);
  lv_obj_remove_style_all(lubber);
  lv_obj_set_size(lubber, 14, 10);
  lv_obj_set_pos(lubber, 324, 40);  // Just above outer ring (top at y=52), below header (y=30)
  lv_obj_set_style_bg_color(lubber, FC_COLOR_WARN, 0);
  lv_obj_set_style_bg_opa(lubber, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(lubber, 2, 0);

  // Cardinal direction labels (N/E/S/W — positioned dynamically in updateCompassData)
  const char* cardinals[] = {"N", "E", "S", "W"};
  lv_obj_t** cardLbls[] = {&compassLblN, &compassLblE, &compassLblS, &compassLblW};
  lv_color_t cardColors[] = {
    lv_color_hex(0x00FFFF),  // N cyan (matches north needle)
    lv_color_hex(0xFFFFFF),  // E white
    lv_color_hex(0xFF0000),  // S red (matches south needle)
    lv_color_hex(0xFFFFFF),  // W white
  };
  for (int i = 0; i < 4; i++) {
    *cardLbls[i] = lv_label_create(compassScr);
    lv_label_set_text(*cardLbls[i], cardinals[i]);
    lv_obj_set_style_text_font(*cardLbls[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(*cardLbls[i], cardColors[i], 0);
    lv_obj_set_pos(*cardLbls[i], -50, -50);  // Off-screen until first heading update
  }

  // NavBar at bottom
  compassNavBar = fcNavBarCreate(compassScr, NUM_SCREENS, SCREEN_COMPASS);


  logPrintln("[LVGL] Compass screen built (#109)");
}

// Compass rose custom draw callback — anti-aliased via LVGL primitives (#109)
static void compassRoseDrawCb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);

  // Rose geometry — center of the 298x260 container
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  int32_t objW = lv_obj_get_width(obj);
  int32_t objH = lv_obj_get_height(obj);
  int32_t cx = coords.x1 + objW / 2;
  int32_t cy = coords.y1 + objH / 2;
  int32_t radius = 108;

  float heading = compassLastHeading;
  if (heading < 0) heading = 0;
  float rotDeg = -heading;

  // --- Outer ring (anti-aliased arc) ---
  lv_draw_arc_dsc_t arcDsc;
  lv_draw_arc_dsc_init(&arcDsc);
  arcDsc.center.x = cx;
  arcDsc.center.y = cy;
  arcDsc.radius = radius;
  arcDsc.width = 2;
  arcDsc.start_angle = 0;
  arcDsc.end_angle = 360;
  arcDsc.color = lv_color_hex(0x808080);
  arcDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &arcDsc);

  // --- Tick marks (12 ticks every 30°) ---
  int tickCardLen = radius / 10;
  int tickInterLen = radius / 20;

  for (int i = 0; i < 12; i++) {
    float tickAngle = (i * 30.0f + rotDeg - 90.0f) * (float)M_PI / 180.0f;
    int tickLen = (i % 3 == 0) ? tickCardLen : tickInterLen;

    lv_draw_line_dsc_t lineDsc;
    lv_draw_line_dsc_init(&lineDsc);
    lineDsc.p1.x = cx + (int32_t)(cosf(tickAngle) * (radius - tickLen));
    lineDsc.p1.y = cy + (int32_t)(sinf(tickAngle) * (radius - tickLen));
    lineDsc.p2.x = cx + (int32_t)(cosf(tickAngle) * radius);
    lineDsc.p2.y = cy + (int32_t)(sinf(tickAngle) * radius);
    lineDsc.width = (i % 3 == 0) ? 2 : 1;
    lineDsc.color = lv_color_hex(0x808080);
    lineDsc.opa = LV_OPA_COVER;
    lv_draw_line(layer, &lineDsc);
  }

  // --- 8 Diamond needles ---
  struct {
    float angle;
    int length;
    int halfWidth;
    lv_color_t color;
    lv_color_t tailColor;
  } needles[] = {
    {  0, radius*93/100, radius*10/100, lv_color_hex(0x00FFFF), lv_color_hex(0x212121)},  // N cyan
    { 45, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // NE gray
    { 90, radius*93/100, radius*10/100, lv_color_hex(0xFFFFFF), lv_color_hex(0x212121)},  // E white
    {135, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // SE gray
    {180, radius*93/100, radius*10/100, lv_color_hex(0xFF0000), lv_color_hex(0x212121)},  // S red
    {225, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // SW gray
    {270, radius*93/100, radius*10/100, lv_color_hex(0xFFFFFF), lv_color_hex(0x212121)},  // W white
    {315, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // NW gray
  };

  for (int i = 0; i < 8; i++) {
    float tipRad = (needles[i].angle + rotDeg - 90.0f) * (float)M_PI / 180.0f;
    float perpRad = tipRad + (float)M_PI / 2.0f;

    int32_t tipX = cx + (int32_t)(cosf(tipRad) * needles[i].length);
    int32_t tipY = cy + (int32_t)(sinf(tipRad) * needles[i].length);

    int32_t sX1 = cx + (int32_t)(cosf(perpRad) * needles[i].halfWidth);
    int32_t sY1 = cy + (int32_t)(sinf(perpRad) * needles[i].halfWidth);
    int32_t sX2 = cx - (int32_t)(cosf(perpRad) * needles[i].halfWidth);
    int32_t sY2 = cy - (int32_t)(sinf(perpRad) * needles[i].halfWidth);

    float tailRad = tipRad + (float)M_PI;
    int tailLen = needles[i].length / 3;
    int32_t tailX = cx + (int32_t)(cosf(tailRad) * tailLen);
    int32_t tailY = cy + (int32_t)(sinf(tailRad) * tailLen);

    // Tip triangle (front half of diamond)
    lv_draw_triangle_dsc_t triDsc;
    lv_draw_triangle_dsc_init(&triDsc);
    triDsc.p[0].x = tipX;  triDsc.p[0].y = tipY;
    triDsc.p[1].x = sX1;   triDsc.p[1].y = sY1;
    triDsc.p[2].x = sX2;   triDsc.p[2].y = sY2;
    triDsc.color = needles[i].color;
    triDsc.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &triDsc);

    // Tail triangle (back half of diamond)
    triDsc.p[0].x = tailX; triDsc.p[0].y = tailY;
    triDsc.color = needles[i].tailColor;
    lv_draw_triangle(layer, &triDsc);
  }

  // --- Center hub (filled circle via thick arc) ---
  int hubR = max(5, (int)(radius / 15));
  lv_draw_arc_dsc_t hubDsc;
  lv_draw_arc_dsc_init(&hubDsc);
  hubDsc.center.x = cx;
  hubDsc.center.y = cy;
  hubDsc.radius = hubR;
  hubDsc.width = hubR;
  hubDsc.start_angle = 0;
  hubDsc.end_angle = 360;
  hubDsc.color = lv_color_hex(0xFFFFFF);
  hubDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &hubDsc);

  // Hub outline
  lv_draw_arc_dsc_t hubOutDsc;
  lv_draw_arc_dsc_init(&hubOutDsc);
  hubOutDsc.center.x = cx;
  hubOutDsc.center.y = cy;
  hubOutDsc.radius = hubR;
  hubOutDsc.width = 1;
  hubOutDsc.start_angle = 0;
  hubOutDsc.end_angle = 360;
  hubOutDsc.color = lv_color_hex(0x808080);
  hubOutDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &hubOutDsc);
}

// Update left panel labels from sensor data (called from updateDisplay at 2Hz)
void updateCompassData() {
  char buf[64];

  // Section 1: Heading + cardinal
  if (imuAvailable && magAvailable) {
    const char* card = getCardinal(imuData.heading);
    lv_label_set_text_fmt(compassLblHdg, "%.0f\xC2\xB0 %s", imuData.heading, card);
    lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_VALUE, 0);
  } else {
    lv_label_set_text(compassLblHdg, "No IMU");
    lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_ERROR, 0);
  }

  // Section 2: GPS coordinates
  if (gpsData.valid) {
    lv_label_set_text_fmt(compassLblLat, "Lat %.4f%c",
      fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    lv_label_set_text_fmt(compassLblLon, "Lon %.4f%c",
      fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    lv_obj_set_style_text_color(compassLblLat, FC_COLOR_VALUE, 0);
    lv_obj_set_style_text_color(compassLblLon, FC_COLOR_VALUE, 0);
  } else if (gpsData.receiving) {
    lv_label_set_text(compassLblLat, "GPS Acquiring...");
    lv_label_set_text_fmt(compassLblLon, "Sats: %d", gpsData.satellites);
    lv_obj_set_style_text_color(compassLblLat, FC_COLOR_WARN, 0);
    lv_obj_set_style_text_color(compassLblLon, FC_COLOR_WARN, 0);
  } else {
    lv_label_set_text(compassLblLat, "No GPS");
    lv_label_set_text(compassLblLon, "");
    lv_obj_set_style_text_color(compassLblLat, FC_COLOR_ERROR, 0);
  }

  // Section 3: Altitude
  if (gpsData.valid) {
    float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
    lv_label_set_text_fmt(compassLblAlt, "Alt %.0f %s",
      alt, useMetricUnits ? "m" : "ft");
  } else {
    lv_label_set_text(compassLblAlt, "Alt --");
  }

  // Section 4: Speed
  if (gpsData.valid) {
    float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
    lv_label_set_text_fmt(compassLblSpd, "Spd %.1f %s",
      speed, useMetricUnits ? "km/h" : "mph");
  } else {
    lv_label_set_text_fmt(compassLblSpd, "Spd -- %s",
      useMetricUnits ? "km/h" : "mph");
  }

  // Section 5: Temperature
  bool hasTempSensor = shtAvailable || bmeAvailable;
  if (hasTempSensor) {
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempDisplay = useFahrenheit ? tempC * 9.0 / 5.0 + 32.0 : tempC;
    lv_label_set_text_fmt(compassLblTemp, "Temp %.1f\xC2\xB0%c",
      tempDisplay, useFahrenheit ? 'F' : 'C');
  } else {
    lv_label_set_text(compassLblTemp, "Temp --");
  }

  // Section 6: Forecast with color coding
  const char* fc = weatherTrend.forecast;
  lv_color_t fcstColor = FC_COLOR_VALUE;
  if (strstr(fc, "Storm")) fcstColor = FC_COLOR_ERROR;
  else if (strstr(fc, "Rain") || strstr(fc, "Snow") ||
           strstr(fc, "Unsettled") || strstr(fc, "Precip")) fcstColor = FC_COLOR_WARN;
  else if (strcmp(fc, "Init") == 0 || strcmp(fc, "Learning") == 0 ||
           strcmp(fc, "Traveled") == 0) fcstColor = FC_COLOR_DIM;
  lv_obj_set_style_text_color(compassLblFcst, fcstColor, 0);
  lv_label_set_text_fmt(compassLblFcst, "Fcst %s %s", getTrendArrow(), fc);

  // Section 7: GPS status
  if (gpsData.valid) {
    lv_label_set_text_fmt(compassLblGps, "GPS OK Sat:%d HDOP:%.1f",
      gpsData.satellites, gpsData.hdop);
    lv_obj_set_style_text_color(compassLblGps, FC_COLOR_VALUE, 0);
  } else if (gpsData.receiving) {
    lv_label_set_text_fmt(compassLblGps, "GPS Acquiring Sat:%d",
      gpsData.satellites);
    lv_obj_set_style_text_color(compassLblGps, FC_COLOR_WARN, 0);
  } else {
    lv_label_set_text(compassLblGps, "No GPS");
    lv_obj_set_style_text_color(compassLblGps, FC_COLOR_ERROR, 0);
  }

  // Section 8: Time
  char timeBuf[16];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    formatTimeStr(timeBuf, timeinfo.tm_hour, timeinfo.tm_min,
                  timeinfo.tm_sec, false);
  } else {
    strcpy(timeBuf, "--:--");
  }
  lv_label_set_text(compassLblTime, timeBuf);

  // Section 9: Invalidate compass rose if heading changed >2°
  if (imuAvailable && magAvailable) {
    float diff = fabs(imuData.heading - compassLastHeading);
    if (diff > 180) diff = 360 - diff;  // Wrap-around
    if (diff >= 2.0f) {
      compassLastHeading = imuData.heading;
      lv_obj_invalidate(compassRoseObj);

      // Section 10: Reposition cardinal direction labels around rose
      if (compassLblN) {
        int32_t roseCx = 331;  // Center of rose (182 + 298/2)
        int32_t roseCy = 160;  // Center of rose (30 + 260/2)
        int32_t labelR = 120;  // Just outside outer ring (radius=108 + padding)
        float cardAngles[] = {0, 90, 180, 270};
        lv_obj_t* cardObjs[] = {compassLblN, compassLblE, compassLblS, compassLblW};
        float rot = -compassLastHeading;

        for (int ci = 0; ci < 4; ci++) {
          float rad = (cardAngles[ci] + rot - 90.0f) * (float)M_PI / 180.0f;
          int32_t lx = roseCx + (int32_t)(cosf(rad) * labelR) - 5;  // ~half char width
          int32_t ly = roseCy + (int32_t)(sinf(rad) * labelR) - 8;  // ~half char height
          lv_obj_set_pos(cardObjs[ci], lx, ly);
        }
      }
    }
  }
}

// ============== LVGL Geocache Nav Draw Callback (#110) ==============

static void geocacheNavDrawCb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  int32_t cx = (coords.x1 + coords.x2) / 2;
  int32_t cy = (coords.y1 + coords.y2) / 2;

  // Need valid cache and GPS data to draw
  if (cacheListCount == 0 || !cacheList[selectedCacheIndex].valid) return;
  if (!gpsData.valid) return;

  GeocacheEntry& cache = cacheList[selectedCacheIndex];
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float distM = distKm * 1000.0f;
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);
  float accuracyM = getGpsAccuracyMeters();
  bool inSearchZone = (distM < accuracyM);

  if (inSearchZone) {
    // === Search Zone Circle (pulsing) ===
    float ratio = (accuracyM > 0) ? (distM / accuracyM) : 0;
    int32_t baseR = 20 + (int32_t)((1.0f - ratio) * 40.0f);  // 20-60px
    int32_t r = baseR + gcNavPulseRadius;

    // Filled orange circle
    lv_draw_arc_dsc_t arcDsc;
    lv_draw_arc_dsc_init(&arcDsc);
    arcDsc.center.x = cx;
    arcDsc.center.y = cy;
    arcDsc.radius = r;
    arcDsc.start_angle = 0;
    arcDsc.end_angle = 360;
    arcDsc.color = FC_COLOR_WARN;
    arcDsc.opa = LV_OPA_COVER;
    arcDsc.width = r;  // Filled
    lv_draw_arc(layer, &arcDsc);

    // White outline
    lv_draw_arc_dsc_t outDsc;
    lv_draw_arc_dsc_init(&outDsc);
    outDsc.center.x = cx;
    outDsc.center.y = cy;
    outDsc.radius = r + 2;
    outDsc.start_angle = 0;
    outDsc.end_angle = 360;
    outDsc.color = FC_COLOR_TEXT;
    outDsc.opa = LV_OPA_COVER;
    outDsc.width = 2;
    lv_draw_arc(layer, &outDsc);

    // Center dot
    lv_draw_arc_dsc_t dotDsc;
    lv_draw_arc_dsc_init(&dotDsc);
    dotDsc.center.x = cx;
    dotDsc.center.y = cy;
    dotDsc.radius = 4;
    dotDsc.start_angle = 0;
    dotDsc.end_angle = 360;
    dotDsc.color = FC_COLOR_TEXT;
    dotDsc.opa = LV_OPA_COVER;
    dotDsc.width = 4;
    lv_draw_arc(layer, &dotDsc);
  } else {
    // === Direction Arrow (same math as legacy drawNavTriangle) ===
    float triangleAngle = bearing - imuData.heading;
    if (triangleAngle < 0) triangleAngle += 360;
    if (triangleAngle >= 360) triangleAngle -= 360;

    int size = 50;
    float rad = (triangleAngle - 90.0f) * (float)M_PI / 180.0f;

    // Tip point
    int32_t tipX = cx + (int32_t)(cosf(rad) * size);
    int32_t tipY = cy + (int32_t)(sinf(rad) * size);

    // Rear corners (±140° from tip direction)
    float rear1Rad = rad + 140.0f * (float)M_PI / 180.0f;
    float rear2Rad = rad - 140.0f * (float)M_PI / 180.0f;
    int32_t rear1X = cx + (int32_t)(cosf(rear1Rad) * size * 0.7f);
    int32_t rear1Y = cy + (int32_t)(sinf(rear1Rad) * size * 0.7f);
    int32_t rear2X = cx + (int32_t)(cosf(rear2Rad) * size * 0.7f);
    int32_t rear2Y = cy + (int32_t)(sinf(rear2Rad) * size * 0.7f);

    // Rear center notch
    float rearCRad = rad + 180.0f * (float)M_PI / 180.0f;
    int32_t rearCX = cx + (int32_t)(cosf(rearCRad) * size * 0.3f);
    int32_t rearCY = cy + (int32_t)(sinf(rearCRad) * size * 0.3f);

    lv_color_t arrowColor = FC_COLOR_HEADER;  // Cyan

    // Triangle 1: tip → rear1 → rearCenter
    lv_draw_triangle_dsc_t tri1;
    lv_draw_triangle_dsc_init(&tri1);
    tri1.p[0].x = tipX; tri1.p[0].y = tipY;
    tri1.p[1].x = rear1X; tri1.p[1].y = rear1Y;
    tri1.p[2].x = rearCX; tri1.p[2].y = rearCY;
    tri1.color = arrowColor;
    tri1.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &tri1);

    // Triangle 2: tip → rear2 → rearCenter
    lv_draw_triangle_dsc_t tri2;
    lv_draw_triangle_dsc_init(&tri2);
    tri2.p[0].x = tipX; tri2.p[0].y = tipY;
    tri2.p[1].x = rear2X; tri2.p[1].y = rear2Y;
    tri2.p[2].x = rearCX; tri2.p[2].y = rearCY;
    tri2.color = arrowColor;
    tri2.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &tri2);
  }
}

// ============== LVGL Geocache Screen Builder (#110) ==============

void buildGeocacheScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  geocacheScr = lv_obj_create(NULL);
  lv_obj_set_size(geocacheScr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(geocacheScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(geocacheScr, 0, 0);
  lv_obj_set_style_radius(geocacheScr, 0, 0);
  lv_obj_set_style_pad_all(geocacheScr, 0, 0);
  lv_obj_clear_flag(geocacheScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(geocacheScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Nav sub-screen container (sub 0)
  geocacheNavCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheNavCtr);
  lv_obj_set_size(geocacheNavCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheNavCtr, 0, 0);
  lv_obj_clear_flag(geocacheNavCtr, LV_OBJ_FLAG_SCROLLABLE);

  // === Nav Sub-screen widgets (sub 0) ===
  gcNavHeader = fcHeaderCreate(geocacheNavCtr, "GEOCACHE");

  // Cache name (centered)
  gcNavLblName = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblName, 0, 33);
  lv_obj_set_width(gcNavLblName, SCREEN_W);
  lv_obj_set_style_text_font(gcNavLblName, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(gcNavLblName, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_align(gcNavLblName, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcNavLblName, "No cache loaded");

  // Distance
  gcNavLblDist = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblDist, 8, 57);
  lv_obj_set_style_text_font(gcNavLblDist, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcNavLblDist, "--");

  // Difficulty / Terrain
  gcNavLblDT = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblDT, 140, 57);
  lv_obj_set_style_text_font(gcNavLblDT, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcNavLblDT, FC_COLOR_DIM, 0);
  lv_label_set_text(gcNavLblDT, "");

  // Bearing
  gcNavLblBearing = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblBearing, 300, 57);
  lv_obj_set_style_text_font(gcNavLblBearing, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcNavLblBearing, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcNavLblBearing, "");

  // Nav graphic area (custom draw — triangle or search zone)
  gcNavGraphicObj = lv_obj_create(geocacheNavCtr);
  lv_obj_remove_style_all(gcNavGraphicObj);
  lv_obj_set_size(gcNavGraphicObj, 200, 120);
  lv_obj_set_pos(gcNavGraphicObj, 140, 78);
  lv_obj_clear_flag(gcNavGraphicObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(gcNavGraphicObj, geocacheNavDrawCb, LV_EVENT_DRAW_MAIN, NULL);

  // Pulse animation for search zone circle
  static lv_anim_t pulseAnim;
  lv_anim_init(&pulseAnim);
  lv_anim_set_var(&pulseAnim, gcNavGraphicObj);
  lv_anim_set_values(&pulseAnim, 0, 8);
  lv_anim_set_duration(&pulseAnim, 1000);
  lv_anim_set_playback_duration(&pulseAnim, 1000);
  lv_anim_set_repeat_count(&pulseAnim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&pulseAnim, [](void* obj, int32_t val) {
    gcNavPulseRadius = val;
    if (geocacheSubScreen == 0 && gcNavLastInZone) {
      lv_obj_invalidate((lv_obj_t*)obj);
    }
  });
  lv_anim_start(&pulseAnim);

  // Accuracy
  gcNavLblAccuracy = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblAccuracy, 0, 200);
  lv_obj_set_width(gcNavLblAccuracy, SCREEN_W);
  lv_obj_set_style_text_font(gcNavLblAccuracy, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcNavLblAccuracy, FC_COLOR_VALUE, 0);
  lv_obj_set_style_text_align(gcNavLblAccuracy, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcNavLblAccuracy, "");

  // Hint
  gcNavLblHint = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblHint, 8, 222);
  lv_obj_set_width(gcNavLblHint, SCREEN_W - 16);
  lv_obj_set_style_text_font(gcNavLblHint, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcNavLblHint, FC_COLOR_DIM, 0);
  lv_label_set_long_mode(gcNavLblHint, LV_LABEL_LONG_WRAP);
  lv_label_set_text(gcNavLblHint, "");

  // Nav bar
  gcNavNavBar = fcNavBarCreate(geocacheNavCtr, NUM_SCREENS, SCREEN_GEOCACHE);

  // List sub-screen container (sub 1)
  geocacheListCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheListCtr);
  lv_obj_set_size(geocacheListCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheListCtr, 0, 0);
  lv_obj_clear_flag(geocacheListCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);

  // === List Sub-screen widgets (sub 1) ===
  gcListHeader = fcHeaderCreate(geocacheListCtr, "CACHE LIST");

  // Count label in header area
  gcListLblCount = lv_label_create(geocacheListCtr);
  lv_obj_set_pos(gcListLblCount, 360, 7);
  lv_obj_set_style_text_font(gcListLblCount, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcListLblCount, FC_COLOR_DIM, 0);
  lv_label_set_text(gcListLblCount, "");

  // Scrollable list container
  gcListScrollCtr = lv_obj_create(geocacheListCtr);
  lv_obj_remove_style_all(gcListScrollCtr);
  lv_obj_set_size(gcListScrollCtr, SCREEN_W, 228);
  lv_obj_set_pos(gcListScrollCtr, 0, 33);
  lv_obj_set_style_bg_color(gcListScrollCtr, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(gcListScrollCtr, LV_OPA_COVER, 0);
  lv_obj_add_flag(gcListScrollCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_row(gcListScrollCtr, 2, 0);
  lv_obj_set_flex_flow(gcListScrollCtr, LV_FLEX_FLOW_COLUMN);

  // Pre-create all MAX_CACHES rows
  for (int i = 0; i < MAX_CACHES; i++) {
    gcListRows[i] = lv_obj_create(gcListScrollCtr);
    lv_obj_remove_style_all(gcListRows[i]);
    lv_obj_set_size(gcListRows[i], 460, 28);
    lv_obj_clear_flag(gcListRows[i], LV_OBJ_FLAG_SCROLLABLE);

    // Selector ">"
    gcListRowSelector[i] = lv_label_create(gcListRows[i]);
    lv_obj_set_pos(gcListRowSelector[i], 4, 4);
    lv_obj_set_style_text_font(gcListRowSelector[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(gcListRowSelector[i], FC_COLOR_HEADER, 0);
    lv_label_set_text(gcListRowSelector[i], "");

    // Distance
    gcListRowDist[i] = lv_label_create(gcListRows[i]);
    lv_obj_set_pos(gcListRowDist[i], 20, 4);
    lv_obj_set_style_text_font(gcListRowDist[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(gcListRowDist[i], FC_COLOR_VALUE, 0);
    lv_label_set_text(gcListRowDist[i], "");

    // Name
    gcListRowName[i] = lv_label_create(gcListRows[i]);
    lv_obj_set_pos(gcListRowName[i], 95, 4);
    lv_obj_set_width(gcListRowName[i], 180);
    lv_obj_set_style_text_font(gcListRowName[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(gcListRowName[i], FC_COLOR_TEXT, 0);
    lv_label_set_long_mode(gcListRowName[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(gcListRowName[i], "");

    // Found badge
    gcListRowFound[i] = lv_label_create(gcListRows[i]);
    lv_obj_set_pos(gcListRowFound[i], 280, 4);
    lv_obj_set_style_text_font(gcListRowFound[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(gcListRowFound[i], FC_COLOR_VALUE, 0);
    lv_label_set_text(gcListRowFound[i], "");

    // D/T
    gcListRowDT[i] = lv_label_create(gcListRows[i]);
    lv_obj_set_pos(gcListRowDT[i], 300, 4);
    lv_obj_set_style_text_font(gcListRowDT[i], FC_FONT_XS, 0);
    lv_obj_set_style_text_color(gcListRowDT[i], FC_COLOR_DIM, 0);
    lv_label_set_text(gcListRowDT[i], "");

    // Hide rows beyond current cache count
    lv_obj_add_flag(gcListRows[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Button hints
  gcListLblHints = lv_label_create(geocacheListCtr);
  lv_obj_set_pos(gcListLblHints, 0, 265);
  lv_obj_set_width(gcListLblHints, SCREEN_W);
  lv_obj_set_style_text_font(gcListLblHints, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcListLblHints, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_align(gcListLblHints, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcListLblHints, "[A]Up [B]Down [C]Select [C+]Details");

  // Nav bar
  gcListNavBar = fcNavBarCreate(geocacheListCtr, NUM_SCREENS, SCREEN_GEOCACHE);

  // Details sub-screen container (sub 2)
  geocacheDetailsCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheDetailsCtr);
  lv_obj_set_size(geocacheDetailsCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheDetailsCtr, 0, 0);
  lv_obj_clear_flag(geocacheDetailsCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);

  // === Details Sub-screen widgets (sub 2) ===
  gcDetHeader = fcHeaderCreate(geocacheDetailsCtr, "CACHE DETAILS");

  // Count in header area
  gcDetLblCount = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblCount, 360, 7);
  lv_obj_set_style_text_font(gcDetLblCount, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcDetLblCount, FC_COLOR_DIM, 0);
  lv_label_set_text(gcDetLblCount, "");

  // Cache name
  gcDetLblName = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblName, 8, 35);
  lv_obj_set_width(gcDetLblName, SCREEN_W - 16);
  lv_obj_set_style_text_font(gcDetLblName, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(gcDetLblName, FC_COLOR_TEXT, 0);
  lv_label_set_long_mode(gcDetLblName, LV_LABEL_LONG_CLIP);
  lv_label_set_text(gcDetLblName, "");

  // GC code
  gcDetLblGC = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblGC, 8, 57);
  lv_obj_set_style_text_font(gcDetLblGC, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcDetLblGC, FC_COLOR_HEADER, 0);
  lv_label_set_text(gcDetLblGC, "");

  // Coordinates
  gcDetLblCoords = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblCoords, 8, 79);
  lv_obj_set_style_text_font(gcDetLblCoords, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblCoords, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcDetLblCoords, "");

  // Difficulty/Terrain
  gcDetLblDT = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblDT, 8, 97);
  lv_obj_set_style_text_font(gcDetLblDT, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblDT, FC_COLOR_DIM, 0);
  lv_label_set_text(gcDetLblDT, "");

  // Distance + bearing (dynamic)
  gcDetLblDist = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblDist, 8, 119);
  lv_obj_set_style_text_font(gcDetLblDist, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblDist, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcDetLblDist, "");

  // Hint label
  gcDetLblHintLabel = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblHintLabel, 8, 141);
  lv_obj_set_style_text_font(gcDetLblHintLabel, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblHintLabel, FC_COLOR_DIM, 0);
  lv_label_set_text(gcDetLblHintLabel, "Hint:");

  // Hint text (wrapped)
  gcDetLblHint = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblHint, 8, 159);
  lv_obj_set_width(gcDetLblHint, SCREEN_W - 16);
  lv_obj_set_style_text_font(gcDetLblHint, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblHint, FC_COLOR_DIM, 0);
  lv_label_set_long_mode(gcDetLblHint, LV_LABEL_LONG_WRAP);
  lv_label_set_text(gcDetLblHint, "");

  // Found status
  gcDetLblFound = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblFound, 0, 200);
  lv_obj_set_width(gcDetLblFound, SCREEN_W);
  lv_obj_set_style_text_font(gcDetLblFound, FC_FONT_MD, 0);
  lv_obj_set_style_text_align(gcDetLblFound, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcDetLblFound, "");

  // Button hints
  gcDetLblHints = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblHints, 0, 265);
  lv_obj_set_width(gcDetLblHints, SCREEN_W);
  lv_obj_set_style_text_font(gcDetLblHints, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcDetLblHints, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_align(gcDetLblHints, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcDetLblHints, "[A]Prev [B]Next [C]Toggle [C+]Back");

  // Nav bar
  gcDetNavBar = fcNavBarCreate(geocacheDetailsCtr, NUM_SCREENS, SCREEN_GEOCACHE);


  logPrintln("[LVGL] Geocache screen built (#110)");
}

// ============== LVGL Geocache Accuracy Color (#110) ==============

static lv_color_t getLvglAccuracyColor(float accuracyM) {
  if (accuracyM < 10.0f) return FC_COLOR_VALUE;   // Green — excellent
  if (accuracyM < 25.0f) return FC_COLOR_WARN;    // Orange — good
  return FC_COLOR_ERROR;                           // Red — poor
}

// ============== LVGL Geocache Data Update (#110) ==============

void updateGeocacheData() {
  if (!geocacheScr) return;

  // Sub-screen visibility switching
  if (geocacheSubScreen == 0) {
    lv_obj_clear_flag(geocacheNavCtr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);
  } else if (geocacheSubScreen == 1) {
    lv_obj_add_flag(geocacheNavCtr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(geocacheNavCtr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);
  }

  // === NAV sub-screen data (sub 0) ===
  if (geocacheSubScreen == 0) {
    if (cacheListCount == 0 || !cacheList[selectedCacheIndex].valid) {
      lv_label_set_text(gcNavLblName, "No cache loaded");
      lv_label_set_text(gcNavLblDist, "");
      lv_label_set_text(gcNavLblDT, "");
      lv_label_set_text(gcNavLblBearing, "");
      lv_label_set_text(gcNavLblAccuracy, "");
      lv_label_set_text(gcNavLblHint, "Upload caches via web interface");
      return;
    }

    GeocacheEntry& cache = cacheList[selectedCacheIndex];
    lv_label_set_text(gcNavLblName, cache.name);
    lv_label_set_text_fmt(gcNavLblDT, "D:%.1f T:%.1f", cache.difficulty, cache.terrain);

    if (gpsData.valid) {
      float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                      cache.latitude, cache.longitude);
      float distM = distKm * 1000.0f;
      float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                                   cache.latitude, cache.longitude);
      float accuracyM = getGpsAccuracyMeters();
      bool inSearchZone = (distM < accuracyM);

      // Distance
      if (inSearchZone) {
        lv_label_set_text(gcNavLblDist, "SEARCH ZONE");
        lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_WARN, 0);
      } else if (useMetricUnits) {
        if (distKm >= 1.0f) lv_label_set_text_fmt(gcNavLblDist, "%.1f km", distKm);
        else lv_label_set_text_fmt(gcNavLblDist, "%d m", (int)distM);
        lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_VALUE, 0);
      } else {
        float distMi = distKm * 0.621371f;
        float distFt = distM * 3.28084f;
        if (distMi >= 0.1f) lv_label_set_text_fmt(gcNavLblDist, "%.1f mi", distMi);
        else lv_label_set_text_fmt(gcNavLblDist, "%d ft", (int)distFt);
        lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_VALUE, 0);
      }

      // Bearing
      lv_label_set_text_fmt(gcNavLblBearing, "%d\xC2\xB0", (int)bearing);

      // Accuracy (color-coded)
      if (useMetricUnits) {
        lv_label_set_text_fmt(gcNavLblAccuracy, "+/-%dm", (int)accuracyM);
      } else {
        lv_label_set_text_fmt(gcNavLblAccuracy, "+/-%dft", (int)(accuracyM * 3.28084f));
      }
      lv_obj_set_style_text_color(gcNavLblAccuracy, getLvglAccuracyColor(accuracyM), 0);

      // Hint (full in search zone, truncated otherwise)
      if (inSearchZone) {
        lv_label_set_text(gcNavLblHint, cache.hint);
      } else {
        char hintPreview[32];
        strncpy(hintPreview, cache.hint, 30);
        hintPreview[30] = '\0';
        if (strlen(cache.hint) > 30) strcat(hintPreview, "..");
        lv_label_set_text(gcNavLblHint, hintPreview);
      }

      // Invalidate nav graphic on bearing/heading change (2° threshold)
      float bDiff = fabs(bearing - gcNavLastBearing);
      float hDiff = fabs(imuData.heading - gcNavLastHeading);
      if (bDiff > 180) bDiff = 360 - bDiff;
      if (hDiff > 180) hDiff = 360 - hDiff;
      if (bDiff >= 2.0f || hDiff >= 2.0f || inSearchZone != gcNavLastInZone) {
        gcNavLastBearing = bearing;
        gcNavLastHeading = imuData.heading;
        gcNavLastInZone = inSearchZone;
        lv_obj_invalidate(gcNavGraphicObj);
      }
    } else {
      lv_label_set_text(gcNavLblDist, "Acquiring GPS...");
      lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_WARN, 0);
      lv_label_set_text(gcNavLblBearing, "");
      lv_label_set_text(gcNavLblAccuracy, "");
    }
    fcNavBarSetActive(gcNavNavBar, currentScreen);
  }

  // === LIST sub-screen data (sub 1) ===
  if (geocacheSubScreen == 1) {
    lv_label_set_text_fmt(gcListLblCount, "[%d/%d]", listHighlightIndex + 1, cacheListCount);

    for (int i = 0; i < MAX_CACHES; i++) {
      if (i >= cacheListCount) {
        lv_obj_add_flag(gcListRows[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_clear_flag(gcListRows[i], LV_OBJ_FLAG_HIDDEN);
      GeocacheEntry& c = cacheList[i];

      // Selector
      lv_label_set_text(gcListRowSelector[i], (i == listHighlightIndex) ? ">" : " ");
      lv_obj_set_style_text_color(gcListRowSelector[i],
        (i == listHighlightIndex) ? FC_COLOR_HEADER : FC_COLOR_DIM, 0);

      // Highlight row background
      if (i == listHighlightIndex) {
        lv_obj_set_style_bg_color(gcListRows[i], lv_color_hex(0x1A1A2E), 0);
        lv_obj_set_style_bg_opa(gcListRows[i], LV_OPA_COVER, 0);
      } else {
        lv_obj_set_style_bg_opa(gcListRows[i], LV_OPA_TRANSP, 0);
      }

      // Distance
      if (gpsData.valid) {
        float dk = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                   c.latitude, c.longitude);
        if (useMetricUnits) {
          if (dk >= 1.0f) lv_label_set_text_fmt(gcListRowDist[i], "%.1fkm", dk);
          else lv_label_set_text_fmt(gcListRowDist[i], "%dm", (int)(dk * 1000));
        } else {
          float mi = dk * 0.621371f;
          if (mi >= 0.1f) lv_label_set_text_fmt(gcListRowDist[i], "%.1fmi", mi);
          else lv_label_set_text_fmt(gcListRowDist[i], "%dft", (int)(dk * 3280.84f));
        }
      } else {
        lv_label_set_text(gcListRowDist[i], "--");
      }

      // Name (truncated to 16 chars)
      char nameBuf[20];
      strncpy(nameBuf, c.name, 16);
      nameBuf[16] = '\0';
      if (strlen(c.name) > 16) { nameBuf[14] = '.'; nameBuf[15] = '.'; nameBuf[16] = '\0'; }
      lv_label_set_text(gcListRowName[i], nameBuf);

      // Found badge
      lv_label_set_text(gcListRowFound[i], c.found ? "*" : "");

      // D/T
      lv_label_set_text_fmt(gcListRowDT[i], "D:%d T:%d", (int)c.difficulty, (int)c.terrain);
    }

    // Scroll highlighted row into view
    if (listHighlightIndex < cacheListCount) {
      lv_obj_scroll_to_view(gcListRows[listHighlightIndex], LV_ANIM_ON);
    }
    fcNavBarSetActive(gcListNavBar, currentScreen);
  }

  // === DETAILS sub-screen data (sub 2) ===
  if (geocacheSubScreen == 2) {
    if (listHighlightIndex >= cacheListCount) return;
    GeocacheEntry& c = cacheList[listHighlightIndex];

    lv_label_set_text_fmt(gcDetLblCount, "[%d/%d]", listHighlightIndex + 1, cacheListCount);
    lv_label_set_text(gcDetLblName, c.name);
    lv_label_set_text(gcDetLblGC, c.gcCode);
    lv_label_set_text_fmt(gcDetLblCoords, "%.4f%c %.4f%c",
      fabs(c.latitude), c.latitude >= 0 ? 'N' : 'S',
      fabs(c.longitude), c.longitude >= 0 ? 'E' : 'W');
    lv_label_set_text_fmt(gcDetLblDT, "Difficulty: %.1f  Terrain: %.1f",
      c.difficulty, c.terrain);

    // Dynamic distance
    if (gpsData.valid) {
      float dk = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 c.latitude, c.longitude);
      float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                                   c.latitude, c.longitude);
      if (useMetricUnits) {
        lv_label_set_text_fmt(gcDetLblDist, "%.2f km  Bearing: %d\xC2\xB0", dk, (int)bearing);
      } else {
        lv_label_set_text_fmt(gcDetLblDist, "%.2f mi  Bearing: %d\xC2\xB0",
          dk * 0.621371f, (int)bearing);
      }
    } else {
      lv_label_set_text(gcDetLblDist, "GPS not available");
    }

    lv_label_set_text(gcDetLblHint, c.hint);

    // Found status (color-coded)
    if (c.found) {
      lv_label_set_text(gcDetLblFound, "[* FOUND]");
      lv_obj_set_style_text_color(gcDetLblFound, FC_COLOR_VALUE, 0);
    } else {
      lv_label_set_text(gcDetLblFound, "[ NOT FOUND ]");
      lv_obj_set_style_text_color(gcDetLblFound, FC_COLOR_DIM, 0);
    }
    fcNavBarSetActive(gcDetNavBar, currentScreen);
  }
}

// ============== LVGL Environment Screen Builder (#111) ==============

void buildEnvScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  envScr = lv_obj_create(NULL);
  lv_obj_set_size(envScr, 480, 320);
  lv_obj_set_style_bg_color(envScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(envScr, 0, 0);
  lv_obj_set_style_radius(envScr, 0, 0);
  lv_obj_set_style_pad_all(envScr, 0, 0);
  lv_obj_clear_flag(envScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(envScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Header
  envHeader = fcHeaderCreate(envScr, "ENVIRONMENT");

  // Helper lambda for label pairs
  int labelX = 10;
  int valueX = 80;
  int y = 42;
  int lineH = 28;

  auto makeLabelPair = [&](lv_obj_t** lblOut, lv_obj_t** valOut,
                           const char* labelText, int row) {
    int rowY = y + row * lineH;

    *lblOut = lv_label_create(envScr);
    lv_label_set_text(*lblOut, labelText);
    lv_obj_set_pos(*lblOut, labelX, rowY);
    lv_obj_set_style_text_font(*lblOut, FC_FONT_SM, 0);
    lv_obj_set_style_text_color(*lblOut, FC_COLOR_DIM, 0);

    *valOut = lv_label_create(envScr);
    lv_label_set_text(*valOut, "---");
    lv_obj_set_pos(*valOut, valueX, rowY);
    lv_obj_set_style_text_font(*valOut, FC_FONT_MD, 0);
    lv_obj_set_style_text_color(*valOut, FC_COLOR_VALUE, 0);
  };

  makeLabelPair(&envLblTempLabel,  &envLblTempValue,  "Temp:",  0);
  makeLabelPair(&envLblHumidLabel, &envLblHumidValue, "Humid:", 1);
  makeLabelPair(&envLblIaqLabel,   &envLblIaqValue,   "IAQ:",   2);
  makeLabelPair(&envLblCo2Label,   &envLblCo2Value,   "CO2:",   3);
  makeLabelPair(&envLblPressLabel, &envLblPressValue, "Press:", 4);
  makeLabelPair(&envLblFcstLabel,  &envLblFcstValue,  "Fcst:",  5);

  // Error label (hidden unless no sensors at all)
  envLblNoSensors = lv_label_create(envScr);
  lv_label_set_text(envLblNoSensors, "No env sensors");
  lv_obj_set_style_text_font(envLblNoSensors, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(envLblNoSensors, FC_COLOR_ERROR, 0);
  lv_obj_center(envLblNoSensors);
  lv_obj_add_flag(envLblNoSensors, LV_OBJ_FLAG_HIDDEN);

  // NavBar
  envNavBar = fcNavBarCreate(envScr, NUM_SCREENS, SCREEN_ENV);
}

// ============== LVGL Telemetry Screen Builder (#111) ==============

void buildTelemetryScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  telemetryScr = lv_obj_create(NULL);
  lv_obj_set_size(telemetryScr, 480, 320);
  lv_obj_set_style_bg_color(telemetryScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(telemetryScr, 0, 0);
  lv_obj_set_style_radius(telemetryScr, 0, 0);
  lv_obj_set_style_pad_all(telemetryScr, 0, 0);
  lv_obj_clear_flag(telemetryScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(telemetryScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Header
  telHeader = fcHeaderCreate(telemetryScr, "TELEMETRY");

  // Column geometry
  int leftLabelX = 20;
  int leftValueX = 110;
  int rightLabelX = 250;
  int rightValueX = 350;
  int lineH = 28;

  // Helper lambda for label creation
  auto makeLabel = [&](lv_obj_t** out, const char* text, int x, int yPos,
                       const lv_font_t* font, lv_color_t color) {
    *out = lv_label_create(telemetryScr);
    lv_label_set_text(*out, text);
    lv_obj_set_pos(*out, x, yPos);
    lv_obj_set_style_text_font(*out, font, 0);
    lv_obj_set_style_text_color(*out, color, 0);
  };

  // === GPS Section ===
  telLblGpsSection = lv_label_create(telemetryScr);
  lv_label_set_text(telLblGpsSection, "GPS");
  lv_obj_set_style_text_font(telLblGpsSection, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(telLblGpsSection, FC_COLOR_HEADER, 0);
  lv_obj_set_width(telLblGpsSection, 480);
  lv_obj_set_style_text_align(telLblGpsSection, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(telLblGpsSection, 0, 36);

  // GPS data rows (y starts at 56)
  int gy = 56;

  // Row 1: Lat / Lon
  makeLabel(&telLblLatLabel, "Lat:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblLatValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblLonLabel, "Lon:", rightLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblLonValue, "---", rightValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  gy += lineH;

  // Row 2: Alt / Spd
  makeLabel(&telLblAltLabel, "Alt:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblAltValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblSpdLabel, "Spd:", rightLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblSpdValue, "---", rightValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  gy += lineH;

  // Row 3: Sat / HDOP
  makeLabel(&telLblSatLabel, "Sat:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblSatValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblHdopLabel, "HDOP:", rightLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblHdopValue, "---", rightValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  gy += lineH;

  // Row 4: Status (full width)
  makeLabel(&telLblStatusLabel, "Status:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblStatusValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);

  // GPS acquiring state labels (hidden by default)
  makeLabel(&telLblGpsAcquiring, "Acquiring fix...", 60, 70, FC_FONT_LG, FC_COLOR_WARN);
  lv_obj_add_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsElapsed, "Elapsed: 0m 0s", 60, 100, FC_FONT_LG, FC_COLOR_DIM);
  lv_obj_add_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsSkyHint, "Need clear sky view", 60, 130, FC_FONT_MD, FC_COLOR_DIM);
  lv_obj_add_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsSatCount, "Sats: 0", 60, 147, FC_FONT_LG, FC_COLOR_VALUE);
  lv_obj_add_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);

  // GPS no-data state labels (hidden by default)
  makeLabel(&telLblGpsNoData, "No GPS data", 80, 80, FC_FONT_LG, FC_COLOR_ERROR);
  lv_obj_add_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsCheckConn, "Check connection", 60, 116, FC_FONT_LG, FC_COLOR_DIM);
  lv_obj_add_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);

  // === Divider ===
  telDivider = lv_obj_create(telemetryScr);
  lv_obj_set_size(telDivider, 460, 1);
  lv_obj_set_pos(telDivider, 10, 172);
  lv_obj_set_style_bg_color(telDivider, FC_COLOR_DIM, 0);
  lv_obj_set_style_bg_opa(telDivider, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(telDivider, 0, 0);
  lv_obj_set_style_radius(telDivider, 0, 0);
  lv_obj_set_style_pad_all(telDivider, 0, 0);
  lv_obj_clear_flag(telDivider, LV_OBJ_FLAG_SCROLLABLE);

  // === IMU Section ===
  telLblImuSection = lv_label_create(telemetryScr);
  lv_label_set_text(telLblImuSection, "IMU");
  lv_obj_set_style_text_font(telLblImuSection, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(telLblImuSection, FC_COLOR_HEADER, 0);
  lv_obj_set_width(telLblImuSection, 480);
  lv_obj_set_style_text_align(telLblImuSection, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(telLblImuSection, 0, 178);

  int iy = 198;

  // Row 5: Heading / Roll
  makeLabel(&telLblHdgLabel, "Hdg:", leftLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblHdgValue, "---", leftValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblRollLabel, "Roll:", rightLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblRollValue, "---", rightValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);
  iy += lineH;

  // Row 6: Pitch / Accel
  makeLabel(&telLblPitchLabel, "Pitch:", leftLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblPitchValue, "---", leftValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblAccelLabel, "Accel:", rightLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblAccelValue, "---", rightValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);

  // IMU not available label (hidden by default)
  makeLabel(&telLblNoImu, "IMU not available", 60, 210, FC_FONT_LG, FC_COLOR_ERROR);
  lv_obj_add_flag(telLblNoImu, LV_OBJ_FLAG_HIDDEN);

  // NavBar
  telNavBar = fcNavBarCreate(telemetryScr, NUM_SCREENS, SCREEN_TELEMETRY);
}

// ============== LVGL Environment Data Update (#111) ==============

void updateEnvData() {
  if (!envScr) return;
  char buf[80];

  fcNavBarSetActive(envNavBar, currentScreen);

  if (!bmeAvailable && !shtAvailable) {
    // No sensors at all — show error, hide all rows
    lv_obj_clear_flag(envLblNoSensors, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblTempLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblTempValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblHumidLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblHumidValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblIaqLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblIaqValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblCo2Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblCo2Value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblPressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblPressValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstValue, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  // Sensors available — hide error, show temp+humid rows
  lv_obj_add_flag(envLblNoSensors, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblTempLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblTempValue, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblHumidLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblHumidValue, LV_OBJ_FLAG_HIDDEN);

  float tempC = shtAvailable ? shtData.temperature : envData.temperature;
  float tempF = tempC * 9.0 / 5.0 + 32.0;
  const char* tempSrc = shtAvailable ? "SHT" : "BME";

  // Temp value (respects useFahrenheit) — UTF-8 degree sign \xC2\xB0
  if (useFahrenheit)
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""F (%.1fC) %s", tempF, tempC, tempSrc);
  else
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""C (%.1fF) %s", tempC, tempF, tempSrc);
  lv_label_set_text(envLblTempValue, buf);

  // Humidity
  float humid = shtAvailable ? shtData.humidity : envData.humidity;
  snprintf(buf, sizeof(buf), "%.1f%% %s", humid, tempSrc);
  lv_label_set_text(envLblHumidValue, buf);

  if (bmeAvailable) {
    // Show BME-only rows
    lv_obj_clear_flag(envLblIaqLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblIaqValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblCo2Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblCo2Value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblPressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblPressValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblFcstLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblFcstValue, LV_OBJ_FLAG_HIDDEN);

    // IAQ with color coding
    snprintf(buf, sizeof(buf), "%.0f [%s]", envData.iaq, getIaqAccuracyText(envData.iaqAccuracy));
    lv_label_set_text(envLblIaqValue, buf);
    if (envData.iaq > 200)
      lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_ERROR, 0);
    else if (envData.iaq > 100)
      lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_WARN, 0);
    else
      lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_VALUE, 0);

    // CO2 with color coding
    snprintf(buf, sizeof(buf), "%.0f ppm", envData.co2Equivalent);
    lv_label_set_text(envLblCo2Value, buf);
    if (envData.co2Equivalent > 2000)
      lv_obj_set_style_text_color(envLblCo2Value, FC_COLOR_ERROR, 0);
    else if (envData.co2Equivalent > 1000)
      lv_obj_set_style_text_color(envLblCo2Value, FC_COLOR_WARN, 0);
    else
      lv_obj_set_style_text_color(envLblCo2Value, FC_COLOR_VALUE, 0);

    // Pressure
    snprintf(buf, sizeof(buf), "%.1f hPa (%.2f\")", envData.pressure, hPaToInHg(envData.pressure));
    lv_label_set_text(envLblPressValue, buf);

    // Forecast with color coding
    snprintf(buf, sizeof(buf), "%s %s", getTrendArrow(), weatherTrend.forecast);
    lv_label_set_text(envLblFcstValue, buf);
    if (strstr(weatherTrend.forecast, "Storm"))
      lv_obj_set_style_text_color(envLblFcstValue, FC_COLOR_ERROR, 0);
    else if (strstr(weatherTrend.forecast, "Rain") || strstr(weatherTrend.forecast, "Snow"))
      lv_obj_set_style_text_color(envLblFcstValue, FC_COLOR_WARN, 0);
    else
      lv_obj_set_style_text_color(envLblFcstValue, FC_COLOR_VALUE, 0);

  } else {
    // No BME — show N/A for BME-only rows
    lv_obj_clear_flag(envLblIaqLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblIaqValue, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(envLblIaqValue, "N/A (no BME688)");
    lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_DIM, 0);

    lv_obj_clear_flag(envLblPressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblPressValue, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(envLblPressValue, "N/A (no BME688)");
    lv_obj_set_style_text_color(envLblPressValue, FC_COLOR_DIM, 0);

    // Hide CO2 and Forecast when no BME
    lv_obj_add_flag(envLblCo2Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblCo2Value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstValue, LV_OBJ_FLAG_HIDDEN);
  }
}

// ============== LVGL Telemetry Data Update (#111) ==============

// Helper: show or hide GPS data row labels
static void telShowGpsDataRows(bool show) {
  lv_obj_t* gpsLabels[] = {
    telLblLatLabel, telLblLatValue, telLblLonLabel, telLblLonValue,
    telLblAltLabel, telLblAltValue, telLblSpdLabel, telLblSpdValue,
    telLblSatLabel, telLblSatValue, telLblHdopLabel, telLblHdopValue,
    telLblStatusLabel, telLblStatusValue
  };
  for (auto lbl : gpsLabels) {
    if (show)
      lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

static void telShowImuDataRows(bool show) {
  lv_obj_t* imuLabels[] = {
    telLblHdgLabel, telLblHdgValue, telLblRollLabel, telLblRollValue,
    telLblPitchLabel, telLblPitchValue, telLblAccelLabel, telLblAccelValue
  };
  for (auto lbl : imuLabels) {
    if (show)
      lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

void updateTelemetryData() {
  if (!telemetryScr) return;
  char buf[80];

  fcNavBarSetActive(telNavBar, currentScreen);

  // === GPS Section ===
  if (gpsData.valid) {
    // Show data rows, hide acquiring/error labels
    telShowGpsDataRows(true);
    lv_obj_add_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);

    // Lat
    snprintf(buf, sizeof(buf), "%.6f %c", fabs(gpsData.latitude),
             gpsData.latitude >= 0 ? 'N' : 'S');
    lv_label_set_text(telLblLatValue, buf);

    // Lon
    snprintf(buf, sizeof(buf), "%.6f %c", fabs(gpsData.longitude),
             gpsData.longitude >= 0 ? 'E' : 'W');
    lv_label_set_text(telLblLonValue, buf);

    // Alt (respects useMetricUnits)
    float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
    snprintf(buf, sizeof(buf), "%.1f %s", alt, useMetricUnits ? "m" : "ft");
    lv_label_set_text(telLblAltValue, buf);

    // Speed
    float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
    snprintf(buf, sizeof(buf), "%.1f %s", speed, useMetricUnits ? "km/h" : "mph");
    lv_label_set_text(telLblSpdValue, buf);

    // Satellites
    snprintf(buf, sizeof(buf), "%d", gpsData.satellites);
    lv_label_set_text(telLblSatValue, buf);

    // HDOP
    snprintf(buf, sizeof(buf), "%.1f", gpsData.hdop);
    lv_label_set_text(telLblHdopValue, buf);

    // Status
    if (gpsHadFirstFix)
      snprintf(buf, sizeof(buf), "Fix OK (TTFF %lus)", gpsFirstFixTime / 1000);
    else
      strcpy(buf, "Fix OK");
    lv_label_set_text(telLblStatusValue, buf);

  } else if (gpsData.receiving) {
    // Acquiring — hide data rows, show acquiring labels
    telShowGpsDataRows(false);
    lv_obj_add_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);

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

    snprintf(buf, sizeof(buf), "Sats: %d", gpsData.satellites);
    lv_label_set_text(telLblGpsSatCount, buf);

  } else {
    // No GPS — hide data rows + acquiring, show error
    telShowGpsDataRows(false);
    lv_obj_add_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);
  }

  // === IMU Section ===
  if (imuAvailable && magAvailable) {
    telShowImuDataRows(true);
    lv_obj_add_flag(telLblNoImu, LV_OBJ_FLAG_HIDDEN);

    snprintf(buf, sizeof(buf), "%.0f %s", imuData.heading, getCardinal(imuData.heading));
    lv_label_set_text(telLblHdgValue, buf);

    snprintf(buf, sizeof(buf), "%.0f deg", imuData.roll);
    lv_label_set_text(telLblRollValue, buf);

    snprintf(buf, sizeof(buf), "%.0f deg", imuData.pitch);
    lv_label_set_text(telLblPitchValue, buf);

    snprintf(buf, sizeof(buf), "%.2f m/s2", imuData.accelMag);
    lv_label_set_text(telLblAccelValue, buf);

  } else {
    telShowImuDataRows(false);
    lv_obj_clear_flag(telLblNoImu, LV_OBJ_FLAG_HIDDEN);
  }
}

// ============== LVGL Settings Screen Functions (#112) ==============

// LVGL heap diagnostic — log memory state on Settings interactions (#119)
static void logLvglHeap(const char* context) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  logPrintf("[LVGL/MEM] %s: free=%lu used=%d%% frag=%d%%\n",
            context, (unsigned long)mon.free_size,
            mon.used_pct, mon.frag_pct);
  // Warn if dangerously low
  if (mon.free_size < 8192) {
    logPrintf("[LVGL/MEM] WARNING: <8KB free! Alloc may fail.\n");
  }
}

// Settings menu button click — navigate to sub-screen
static void settingsMenuBtnCb(lv_event_t* e) {
  lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
  int subScreen = (int)(intptr_t)lv_obj_get_user_data(btn);
  settingsSubScreen = subScreen;
  logPrintf("[SETTINGS/LVGL] Menu → sub-screen %d\n", subScreen);
  logLvglHeap("menu→sub");  // Track heap on every navigation (#119)
}

// Settings Back button — return to previous screen
static void settingsBackToScreenCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("settings→exit");  // Log heap BEFORE save+exit (#119)
  navigateFromSettings();
}

// --- Configuration sub-screen callbacks (#112) ---

// Timezone picker selection — fired by fcListPickerOpen via LV_EVENT_VALUE_CHANGED on cfgTzDropdown
static void cfgTzSelectedCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_param(e);
  if (idx < 0 || idx >= TZ_PRESET_COUNT) return;
  tzSelectedIndex = idx;
  strncpy(posixTZ, tzPresets[idx].posix, sizeof(posixTZ) - 1);
  strncpy(tzDisplayName, tzPresets[idx].name, sizeof(tzDisplayName) - 1);
  fcDropdownSetValue(cfgTzDropdown, idx, tzDisplayName);
  logPrintf("[CONFIG/LVGL] TZ selected: %s\n", tzDisplayName);
}

// Timezone dropdown click — opens list picker for TZ selection (#117/#119)
static void cfgTzDropdownCb(lv_event_t* e) {
  (void)e;
  logPrintf("[CONFIG/LVGL] TZ dropdown CLICKED — opening picker\n");
  static const char* tzNames[TZ_PRESET_COUNT];
  for (int i = 0; i < TZ_PRESET_COUNT; i++) {
    tzNames[i] = tzPresets[i].name;
  }
  lv_obj_t* picker = fcListPickerOpen("Time Zone", tzNames, TZ_PRESET_COUNT,
                                       tzSelectedIndex, cfgTzDropdown);
  if (!picker) {
    logPrintf("[CONFIG/LVGL] TZ picker FAILED to open (OOM?)\n");
  }
}

// Toggle change — update globals immediately for live preview
static void cfgToggleCb(lv_event_t* e) {
  (void)e;
  use12Hour      = (fcToggleGetValue(cfgTimeToggle) == 0);   // 0=12Hour
  useFahrenheit  = (fcToggleGetValue(cfgTempToggle) == 0);   // 0=degF
  useMetricUnits = (fcToggleGetValue(cfgDistToggle) == 1);   // 1=Metric
}

// Config Back — discard changes, reload from SD
static void cfgBackCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("cfg←back");  // (#119)
  loadSettings();  // Discard changes, reload from SD
  // Reset toggle visuals to match reloaded values
  fcToggleSetValue(cfgTimeToggle, use12Hour ? 0 : 1);
  fcToggleSetValue(cfgTempToggle, useFahrenheit ? 0 : 1);
  fcToggleSetValue(cfgDistToggle, useMetricUnits ? 1 : 0);
  fcDropdownSetValue(cfgTzDropdown, tzSelectedIndex, tzDisplayName);
  settingsSubScreen = 0;
}

// Config OK — apply and save
static void cfgOKCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("cfg←ok");  // (#119)
  applyTimezone();
  saveSettings();
  settingsSubScreen = 0;
}

// Forward declarations for display callbacks (#112)
extern const uint32_t tftTimeoutPresets[];
extern const char*    tftTimeoutLabels[];
extern const int      TFT_TIMEOUT_COUNT;
extern const uint32_t oledTimeoutPresets[];
extern const char*    oledTimeoutLabels[];
extern const int      OLED_TIMEOUT_COUNT;
int findTimeoutIndex(const uint32_t presets[], int count, uint32_t value);

// Display sub-screen callbacks (#112)
static void dispBrightnessChangedCb(lv_event_t* e) {
  lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
  int val = lv_slider_get_value(slider);
  tftBrightness = (uint8_t)val;
  analogWrite(TFT_BL, tftBrightness);
  if (dispBrightnessLabel) lv_label_set_text_fmt(dispBrightnessLabel, "%d", val);
}

static void dispTftDropdownCb(lv_event_t* e) {
  (void)e;
  logPrintf("[DISPLAY/LVGL] TFT dropdown CLICKED\n");
  int idx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
  fcListPickerOpen("TFT Sleep", tftTimeoutLabels, TFT_TIMEOUT_COUNT, idx, dispTftDropdown);
}

static void dispTftSelectedCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_param(e);
  if (idx < 0 || idx >= TFT_TIMEOUT_COUNT) return;
  tftSleepMs = tftTimeoutPresets[idx];
  fcDropdownSetValue(dispTftDropdown, idx, tftTimeoutLabels[idx]);
  logPrintf("[DISPLAY/LVGL] TFT sleep: %s\n", tftTimeoutLabels[idx]);
}

static void dispOledDropdownCb(lv_event_t* e) {
  (void)e;
  logPrintf("[DISPLAY/LVGL] OLED dropdown CLICKED\n");
  int idx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
  fcListPickerOpen("OLED Sleep", oledTimeoutLabels, OLED_TIMEOUT_COUNT, idx, dispOledDropdown);
}

static void dispOledSelectedCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_param(e);
  if (idx < 0 || idx >= OLED_TIMEOUT_COUNT) return;
  oledSleepMs = oledTimeoutPresets[idx];
  fcDropdownSetValue(dispOledDropdown, idx, oledTimeoutLabels[idx]);
  logPrintf("[DISPLAY/LVGL] OLED sleep: %s\n", oledTimeoutLabels[idx]);
}

static void dispBackCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("disp←back");  // (#119)
  loadSettings();  // Restore saved values
  analogWrite(TFT_BL, tftBrightness);  // Apply restored brightness
  // Sync slider + label to restored values
  if (dispBrightnessSlider) lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
  if (dispBrightnessLabel)  lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);
  // Sync dropdown labels
  if (dispTftDropdown) {
    int idx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
    fcDropdownSetValue(dispTftDropdown, idx, tftTimeoutLabels[idx]);
  }
  if (dispOledDropdown) {
    int idx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
    fcDropdownSetValue(dispOledDropdown, idx, oledTimeoutLabels[idx]);
  }
  settingsSubScreen = 0;
}

static void dispOKCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("disp←ok");  // (#119)
  saveSettings();
  settingsSubScreen = 0;
}

// Compass Cal sub-screen callbacks (#112)
static void calStartBtnCb(lv_event_t* e) {
  (void)e;
  if (!magAvailable || magCalibrating) return;
  magCalibrating = true;
  magCalStartTime = millis();
  magCalMinX = magCalMinY = magCalMinZ = 99999;
  magCalMaxX = magCalMaxY = magCalMaxZ = -99999;
  logPrintln("[MAG/LVGL] Calibration started");
}

static void calBackCb(lv_event_t* e) {
  (void)e;
  if (magCalibrating) {
    magCalibrating = false;
    logPrintln("[MAG/LVGL] Calibration cancelled");
  }
  settingsSubScreen = 0;
}

// Diagnostics sub-screen callback (#112)
static void diagsBackCb(lv_event_t* e) {
  (void)e;
  settingsSubScreen = 0;
}

// About sub-screen callback (#112)
static void aboutBackCb(lv_event_t* e) {
  (void)e;
  settingsSubScreen = 0;
}

// Factory Reset sub-screen callbacks (#112)
static void resetBackCb(lv_event_t* e) {
  (void)e;
  settingsSubScreen = 0;
}

static void resetBtnCb(lv_event_t* e) {
  (void)e;
  factoryReset();
  // Sync all LVGL widgets to restored defaults
  if (cfgTimeToggle)  fcToggleSetValue(cfgTimeToggle, use12Hour ? 0 : 1);
  if (cfgTempToggle)  fcToggleSetValue(cfgTempToggle, useFahrenheit ? 0 : 1);
  if (cfgDistToggle)  fcToggleSetValue(cfgDistToggle, useMetricUnits ? 1 : 0);
  if (cfgTzDropdown)  fcDropdownSetValue(cfgTzDropdown, tzSelectedIndex, tzDisplayName);
  if (dispBrightnessSlider) lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
  if (dispBrightnessLabel)  lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);
  if (dispTftDropdown) {
    int idx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
    fcDropdownSetValue(dispTftDropdown, idx, tftTimeoutLabels[idx]);
  }
  if (dispOledDropdown) {
    int idx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
    fcDropdownSetValue(dispOledDropdown, idx, oledTimeoutLabels[idx]);
  }
  settingsSubScreen = 0;
  logPrintln("[SETTINGS/LVGL] Factory reset executed");
}

// Update settings sub-screen visibility
void updateSettingsData() {
  if (!settingsScr) return;

  // Only toggle container visibility when the sub-screen CHANGES (#117)
  // Hiding+re-showing the active container every 500ms cancels LVGL's
  // press tracking on child buttons, preventing CLICKED events.
  static int prevSubScreen = -1;  // force first-run update
  if (settingsSubScreen != prevSubScreen) {
    // Hide all containers
    if (settingsMenuCtr)   lv_obj_add_flag(settingsMenuCtr,   LV_OBJ_FLAG_HIDDEN);
    if (settingsConfigCtr)  lv_obj_add_flag(settingsConfigCtr,  LV_OBJ_FLAG_HIDDEN);
    if (settingsDisplayCtr) lv_obj_add_flag(settingsDisplayCtr, LV_OBJ_FLAG_HIDDEN);
    if (settingsCalCtr)     lv_obj_add_flag(settingsCalCtr,     LV_OBJ_FLAG_HIDDEN);
    if (settingsDiagsCtr)   lv_obj_add_flag(settingsDiagsCtr,   LV_OBJ_FLAG_HIDDEN);
    if (settingsAboutCtr)   lv_obj_add_flag(settingsAboutCtr,   LV_OBJ_FLAG_HIDDEN);
    if (settingsResetCtr)   lv_obj_add_flag(settingsResetCtr,   LV_OBJ_FLAG_HIDDEN);

    // Show the active sub-screen container
    switch (settingsSubScreen) {
      case 0: if (settingsMenuCtr)    lv_obj_clear_flag(settingsMenuCtr,    LV_OBJ_FLAG_HIDDEN); break;
      case 1: if (settingsConfigCtr)  lv_obj_clear_flag(settingsConfigCtr,  LV_OBJ_FLAG_HIDDEN); break;
      case 2: if (settingsDisplayCtr) lv_obj_clear_flag(settingsDisplayCtr, LV_OBJ_FLAG_HIDDEN); break;
      case 3: if (settingsCalCtr)     lv_obj_clear_flag(settingsCalCtr,     LV_OBJ_FLAG_HIDDEN); break;
      case 4: if (settingsDiagsCtr)   lv_obj_clear_flag(settingsDiagsCtr,   LV_OBJ_FLAG_HIDDEN); break;
      case 5: if (settingsAboutCtr)   lv_obj_clear_flag(settingsAboutCtr,   LV_OBJ_FLAG_HIDDEN); break;
      case 6: if (settingsResetCtr)   lv_obj_clear_flag(settingsResetCtr,   LV_OBJ_FLAG_HIDDEN); break;
    }
    prevSubScreen = settingsSubScreen;
    logPrintf("[SETTINGS] Sub-screen changed to %d\n", settingsSubScreen);
  }

  // --- Live data updates (run every 500ms regardless of visibility) ---
  switch (settingsSubScreen) {
    case 1:
      if (settingsConfigCtr) {
        // Live preview: time | temp | distance (#112)
        if (cfgPreviewLabel) {
          char prev[80];
          // Time preview using formatTimeStr (respects use12Hour)
          struct tm timeinfo;
          char timeBuf[16] = "--:--";
          if (getLocalTime(&timeinfo, 10))
            formatTimeStr(timeBuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, false);
          // Temp preview
          float tempC = shtAvailable ? shtData.temperature : envData.temperature;
          float dispTemp = useFahrenheit ? (tempC * 9.0f / 5.0f + 32.0f) : tempC;
          char tempBuf[16];
          snprintf(tempBuf, sizeof(tempBuf), "%.1f\xC2\xB0%s", dispTemp, useFahrenheit ? "F" : "C");
          // Distance preview
          const char* distEx = useMetricUnits ? "1.0 km" : "0.6 mi";
          snprintf(prev, sizeof(prev), "%s  |  %s  |  %s", timeBuf, tempBuf, distEx);
          lv_label_set_text(cfgPreviewLabel, prev);
        }
      }
      break;
    case 2:
      if (settingsDisplayCtr) {
        // Keep brightness widgets in sync if changed externally
        if (dispBrightnessSlider) lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
        if (dispBrightnessLabel) lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);
      }
      break;
    case 3:
      if (settingsCalCtr) {
        if (magCalibrating) {
          // === ACTIVE CALIBRATION STATE ===
          // Hide idle widgets
          if (calStatusLabel)  lv_obj_add_flag(calStatusLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calOffsetsLabel) lv_obj_add_flag(calOffsetsLabel, LV_OBJ_FLAG_HIDDEN);
          if (calStartBtn)     lv_obj_add_flag(calStartBtn,     LV_OBJ_FLAG_HIDDEN);
          if (calIdleActBar)   lv_obj_add_flag(calIdleActBar,   LV_OBJ_FLAG_HIDDEN);
          // Show active widgets
          if (calArc)            lv_obj_clear_flag(calArc,            LV_OBJ_FLAG_HIDDEN);
          if (calCountdownLabel) lv_obj_clear_flag(calCountdownLabel, LV_OBJ_FLAG_HIDDEN);
          if (calInstructLabel)  lv_obj_clear_flag(calInstructLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calMinMaxLabel)    lv_obj_clear_flag(calMinMaxLabel,    LV_OBJ_FLAG_HIDDEN);

          unsigned long elapsed = millis() - magCalStartTime;
          int pct = (int)((elapsed * 100UL) / MAG_CAL_DURATION_MS);
          if (pct > 100) pct = 100;
          int remaining = ((int)MAG_CAL_DURATION_MS - (int)elapsed) / 1000;
          if (remaining < 0) remaining = 0;

          // Update arc progress
          if (calArc) lv_arc_set_value(calArc, pct);
          // Update countdown
          if (calCountdownLabel) lv_label_set_text_fmt(calCountdownLabel, "%d", remaining);
          // Update min/max
          if (calMinMaxLabel) {
            char mmBuf[128];
            snprintf(mmBuf, sizeof(mmBuf),
              "X: %.1f to %.1f\nY: %.1f to %.1f\nZ: %.1f to %.1f",
              magCalMinX < 99998 ? magCalMinX : 0.0f, magCalMaxX > -99998 ? magCalMaxX : 0.0f,
              magCalMinY < 99998 ? magCalMinY : 0.0f, magCalMaxY > -99998 ? magCalMaxY : 0.0f,
              magCalMinZ < 99998 ? magCalMinZ : 0.0f, magCalMaxZ > -99998 ? magCalMaxZ : 0.0f);
            lv_label_set_text(calMinMaxLabel, mmBuf);
          }

          // Check completion
          if (elapsed >= MAG_CAL_DURATION_MS) {
            // Compute hard-iron offsets
            magOffsetX = (magCalMaxX + magCalMinX) / 2.0f;
            magOffsetY = (magCalMaxY + magCalMinY) / 2.0f;
            magOffsetZ = (magCalMaxZ + magCalMinZ) / 2.0f;
            magCalibrated = true;
            magCalibrating = false;
            saveMagCal();
            logPrintf("[MAG/LVGL] Cal complete: X=%.2f Y=%.2f Z=%.2f\n", magOffsetX, magOffsetY, magOffsetZ);
            // Show completion briefly — idle widgets will show on next frame
          }

        } else {
          // === IDLE STATE ===
          // Show idle widgets
          if (calStatusLabel)  lv_obj_clear_flag(calStatusLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calOffsetsLabel) lv_obj_clear_flag(calOffsetsLabel, LV_OBJ_FLAG_HIDDEN);
          if (calStartBtn)     lv_obj_clear_flag(calStartBtn,     LV_OBJ_FLAG_HIDDEN);
          if (calIdleActBar)   lv_obj_clear_flag(calIdleActBar,   LV_OBJ_FLAG_HIDDEN);

          // Update start button state — magAvailable may have changed since build time (#112)
          if (calStartBtn) {
            if (magAvailable) {
              lv_obj_clear_state(calStartBtn, LV_STATE_DISABLED);
              lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x007D00), 0);
            } else {
              lv_obj_add_state(calStartBtn, LV_STATE_DISABLED);
              lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x424242), 0);
            }
          }

          // Hide active widgets
          if (calArc)            lv_obj_add_flag(calArc,            LV_OBJ_FLAG_HIDDEN);
          if (calCountdownLabel) lv_obj_add_flag(calCountdownLabel, LV_OBJ_FLAG_HIDDEN);
          if (calInstructLabel)  lv_obj_add_flag(calInstructLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calMinMaxLabel)    lv_obj_add_flag(calMinMaxLabel,    LV_OBJ_FLAG_HIDDEN);

          // Update status
          if (calStatusLabel) {
            if (magCalibrated) {
              lv_label_set_text(calStatusLabel, "Status: Calibrated");
              lv_obj_set_style_text_color(calStatusLabel, FC_COLOR_VALUE, 0);  // Green
            } else {
              lv_label_set_text(calStatusLabel, "Status: Not calibrated");
              lv_obj_set_style_text_color(calStatusLabel, FC_COLOR_DIM, 0);
            }
          }
          // Update offsets
          if (calOffsetsLabel) {
            if (magCalibrated) {
              char offBuf[64];
              snprintf(offBuf, sizeof(offBuf), "X: %.2f  Y: %.2f  Z: %.2f", magOffsetX, magOffsetY, magOffsetZ);
              lv_label_set_text(calOffsetsLabel, offBuf);
              lv_obj_set_style_text_color(calOffsetsLabel, FC_COLOR_VALUE, 0);
            } else {
              lv_label_set_text(calOffsetsLabel, "Offsets: ---");
              lv_obj_set_style_text_color(calOffsetsLabel, FC_COLOR_DIM, 0);
            }
          }
        }
      }
      break;
    case 4:
      if (settingsDiagsCtr) {
        char dBuf[80];

        // [0] BSEC
        snprintf(dBuf, sizeof(dBuf), "Load:%s Save:%s Acc:%s",
          bsecStateLoaded ? "Y" : "N", bsecStateSaved ? "Y" : "N",
          getIaqAccuracyText(envData.iaqAccuracy));
        lv_label_set_text(diagValueLabels[0], dBuf);
        lv_obj_set_style_text_color(diagValueLabels[0], bsecStateLoaded ? FC_COLOR_VALUE : FC_COLOR_DIM, 0);

        // [1] Weather
        snprintf(dBuf, sizeof(dBuf), "Mem:%d Files:%d Tot:%d",
          weatherHistoryCount, weatherLogFileCount, weatherLogEntryCount);
        lv_label_set_text(diagValueLabels[1], dBuf);

        // [2] Heap
        snprintf(dBuf, sizeof(dBuf), "%luK / %luK",
          (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getHeapSize() / 1024));
        lv_label_set_text(diagValueLabels[2], dBuf);

        // [3] PSRAM
        if (psramFound()) {
          snprintf(dBuf, sizeof(dBuf), "%luK / %luK",
            (unsigned long)(ESP.getFreePsram() / 1024), (unsigned long)(ESP.getPsramSize() / 1024));
        } else {
          snprintf(dBuf, sizeof(dBuf), "Not available");
        }
        lv_label_set_text(diagValueLabels[3], dBuf);
        lv_obj_set_style_text_color(diagValueLabels[3], psramFound() ? FC_COLOR_VALUE : FC_COLOR_DIM, 0);

        // [4] Sensors
        snprintf(dBuf, sizeof(dBuf), "BME:%s SHT:%s IMU:%s Bat:%s FRAM:%s CTP:%s",
          bmeAvailable ? "Y" : "N", shtAvailable ? "Y" : "N",
          imuAvailable ? "Y" : "N", batteryAvailable ? "Y" : "N",
          framAvailable ? "Y" : "N", touchAvailable ? "Y" : "N");
        lv_label_set_text(diagValueLabels[4], dBuf);

        // [5] Temps
        if (shtAvailable && bmeAvailable) {
          float shtT = useFahrenheit ? (shtData.temperature * 9.0f / 5.0f + 32.0f) : shtData.temperature;
          float bmeT = useFahrenheit ? (envData.temperature * 9.0f / 5.0f + 32.0f) : envData.temperature;
          snprintf(dBuf, sizeof(dBuf), "SHT:%.1f%s BME:%.1f%s (%+.1f)",
            shtT, useFahrenheit ? "F" : "C", bmeT, useFahrenheit ? "F" : "C", shtT - bmeT);
        } else if (shtAvailable) {
          float t = useFahrenheit ? (shtData.temperature * 9.0f / 5.0f + 32.0f) : shtData.temperature;
          snprintf(dBuf, sizeof(dBuf), "SHT:%.1f%s BME:N/A", t, useFahrenheit ? "F" : "C");
        } else if (bmeAvailable) {
          float t = useFahrenheit ? (envData.temperature * 9.0f / 5.0f + 32.0f) : envData.temperature;
          snprintf(dBuf, sizeof(dBuf), "SHT:N/A BME:%.1f%s", t, useFahrenheit ? "F" : "C");
        } else {
          snprintf(dBuf, sizeof(dBuf), "No sensors");
        }
        lv_label_set_text(diagValueLabels[5], dBuf);

        // [6] GPS
        if (gpsHadFirstFix) {
          snprintf(dBuf, sizeof(dBuf), "Fix in %lus", gpsFirstFixTime / 1000);
          lv_obj_set_style_text_color(diagValueLabels[6], FC_COLOR_VALUE, 0);
        } else if (gpsHadFirstReceive) {
          unsigned long elapsed = millis() / 1000;
          snprintf(dBuf, sizeof(dBuf), "Acquiring (%lum %lus)", elapsed / 60, elapsed % 60);
          lv_obj_set_style_text_color(diagValueLabels[6], FC_COLOR_WARN, 0);
        } else {
          snprintf(dBuf, sizeof(dBuf), "No data");
          lv_obj_set_style_text_color(diagValueLabels[6], FC_COLOR_DIM, 0);
        }
        lv_label_set_text(diagValueLabels[6], dBuf);

        // [7] MagCal
        if (magCalibrated) {
          snprintf(dBuf, sizeof(dBuf), "%.1f, %.1f, %.1f", magOffsetX, magOffsetY, magOffsetZ);
          lv_obj_set_style_text_color(diagValueLabels[7], FC_COLOR_VALUE, 0);
        } else {
          snprintf(dBuf, sizeof(dBuf), "None");
          lv_obj_set_style_text_color(diagValueLabels[7], FC_COLOR_DIM, 0);
        }
        lv_label_set_text(diagValueLabels[7], dBuf);

        // [8] Storage
        if (sdHealth.available) {
          unsigned long ageMin = (millis() - sdHealth.lastSuccess) / 60000;
          if (sdHealth.errorCount == 0)
            snprintf(dBuf, sizeof(dBuf), "SD:OK %lum OLED:%s", ageMin, oledAvailable ? "Y" : "N");
          else {
            snprintf(dBuf, sizeof(dBuf), "SD:WARN E:%d R:%d OLED:%s",
              sdHealth.errorCount, sdHealth.reInitCount, oledAvailable ? "Y" : "N");
            lv_obj_set_style_text_color(diagValueLabels[8], FC_COLOR_WARN, 0);
          }
        } else {
          snprintf(dBuf, sizeof(dBuf), "SD:FAIL E:%d R:%d OLED:%s",
            sdHealth.errorCount, sdHealth.reInitCount, oledAvailable ? "Y" : "N");
          lv_obj_set_style_text_color(diagValueLabels[8], FC_COLOR_ERROR, 0);
        }
        if (sdHealth.available && sdHealth.errorCount == 0)
          lv_obj_set_style_text_color(diagValueLabels[8], FC_COLOR_VALUE, 0);
        lv_label_set_text(diagValueLabels[8], dBuf);

        // [9] Web URL
        if (wifiConnected) {
          snprintf(dBuf, sizeof(dBuf), "http://fieldcompass.local/");
          lv_obj_set_style_text_color(diagValueLabels[9], FC_COLOR_DIM, 0);
        } else {
          snprintf(dBuf, sizeof(dBuf), "Not connected");
          lv_obj_set_style_text_color(diagValueLabels[9], FC_COLOR_DIM, 0);
        }
        lv_label_set_text(diagValueLabels[9], dBuf);

        // [10] Touch — last press coordinates + count (for debugging #117)
        if (lastTouchX >= 0) {
          snprintf(dBuf, sizeof(dBuf), "(%ld,%ld) #%lu",
            lastTouchX, lastTouchY, (unsigned long)touchPressCount);
        } else {
          snprintf(dBuf, sizeof(dBuf), "No press yet");
        }
        lv_label_set_text(diagValueLabels[10], dBuf);
        lv_obj_set_style_text_color(diagValueLabels[10],
          lastTouchX >= 0 ? FC_COLOR_VALUE : FC_COLOR_DIM, 0);
      }
      break;
    case 5:
      if (settingsAboutCtr) {
        char aBuf[64];

        // [0] Version — static, already set in build

        // [1] Uptime
        {
          unsigned long uptimeSec = millis() / 1000;
          int days = uptimeSec / 86400;
          int hrs  = (uptimeSec % 86400) / 3600;
          int mins = (uptimeSec % 3600) / 60;
          int secs = uptimeSec % 60;
          if (days > 0) snprintf(aBuf, sizeof(aBuf), "%dd %02d:%02d:%02d", days, hrs, mins, secs);
          else          snprintf(aBuf, sizeof(aBuf), "%02d:%02d:%02d", hrs, mins, secs);
          lv_label_set_text(aboutValueLabels[1], aBuf);
        }

        // [2] Heap
        snprintf(aBuf, sizeof(aBuf), "%lu / %lu KB",
          (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getHeapSize() / 1024));
        lv_label_set_text(aboutValueLabels[2], aBuf);

        // [3] PSRAM
        snprintf(aBuf, sizeof(aBuf), "%lu / %lu KB",
          (unsigned long)(ESP.getFreePsram() / 1024), (unsigned long)(ESP.getPsramSize() / 1024));
        lv_label_set_text(aboutValueLabels[3], aBuf);

        // [4] Battery
        {
          bool battConn = batteryAvailable && isBatteryConnected();
          if (battConn) {
            float pct = battery.cellPercent();
            float v   = battery.cellVoltage();
            snprintf(aBuf, sizeof(aBuf), "%.0f%% (%.2fV)", pct, v);
          } else if (batteryAvailable) {
            snprintf(aBuf, sizeof(aBuf), "USB Only");
          } else {
            snprintf(aBuf, sizeof(aBuf), "N/A");
          }
          lv_label_set_text(aboutValueLabels[4], aBuf);
        }

        // [5] WiFi
        {
          bool wConn = (WiFi.status() == WL_CONNECTED);
          if (wConn) {
            snprintf(aBuf, sizeof(aBuf), "%s %s",
              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
          } else {
            snprintf(aBuf, sizeof(aBuf), "Disconnected");
          }
          lv_label_set_text(aboutValueLabels[5], aBuf);
        }
      }
      break;
  }
}

void buildSettingsScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  settingsScr = lv_obj_create(NULL);
  lv_obj_set_size(settingsScr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(settingsScr, 0, 0);
  lv_obj_set_style_radius(settingsScr, 0, 0);
  lv_obj_set_style_pad_all(settingsScr, 0, 0);
  lv_obj_clear_flag(settingsScr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Menu container (sub-screen 0)
  settingsMenuCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsMenuCtr);
  lv_obj_set_size(settingsMenuCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(settingsMenuCtr, 0, 0);
  lv_obj_set_style_bg_color(settingsMenuCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsMenuCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsMenuCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Header
  fcHeaderCreate(settingsMenuCtr, "SETTINGS");

  // Flex container for menu buttons
  lv_obj_t* menuList = lv_obj_create(settingsMenuCtr);
  lv_obj_remove_style_all(menuList);
  lv_obj_set_size(menuList, 460, 235);
  lv_obj_set_pos(menuList, 10, 35);
  lv_obj_set_style_pad_row(menuList, 1, 0);
  lv_obj_set_flex_flow(menuList, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(menuList, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // 6 menu buttons — 38px tall with NO ext_click_area (#117)
  // Root cause: ext_click_area on adjacent buttons creates OVERLAPPING hit
  // zones (16px overlap with 4px gap + 10px ext), causing LVGL to pick the
  // lower button. Fix: tall buttons, minimal gap, zero ext_click_area.
  // Math: 6×38 + 5×1 = 233px fits in 235px container
  for (int i = 0; i < SETTINGS_MENU_COUNT; i++) {
    lv_obj_t* btn = lv_button_create(menuList);
    lv_obj_set_size(btn, 440, 38);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x424242), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    // NO ext_click_area — prevents overlap with adjacent buttons

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, settingsMenuItems[i]);
    lv_obj_set_style_text_font(lbl, FC_FONT_MD, 0);
    lv_obj_set_style_text_color(lbl, FC_COLOR_TEXT, 0);
    lv_obj_center(lbl);

    lv_obj_set_user_data(btn, (void*)(intptr_t)(i + 1));
    lv_obj_add_event_cb(btn, settingsMenuBtnCb, LV_EVENT_CLICKED, NULL);
    settingsMenuBtns[i] = btn;
  }

  // Action bar with Back button only
  lv_obj_t* actBar = fcActionBarCreate(settingsMenuCtr, true, false);
  lv_obj_t* backBtn = lv_obj_get_child(actBar, 0);
  lv_obj_add_event_cb(backBtn, settingsBackToScreenCb, LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 1: Configuration (#112) ---
  settingsConfigCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsConfigCtr);
  lv_obj_set_size(settingsConfigCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsConfigCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsConfigCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsConfigCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsConfigCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsConfigCtr, "CONFIGURATION");

  // Timezone dropdown — click on ENTIRE row opens picker (#117/#119)
  cfgTzDropdown = fcDropdownCreate(settingsConfigCtr, 45, "Time Zone", tzDisplayName);
  lv_obj_add_event_cb(cfgTzDropdown, cfgTzDropdownCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(cfgTzDropdown, cfgTzSelectedCb, LV_EVENT_VALUE_CHANGED, NULL);
  // Time format toggle: 0=12Hour(left), 1=24Hour(right)
  cfgTimeToggle = fcToggleCreate(settingsConfigCtr, 95, "Time", "12 Hour", "24 Hour", use12Hour ? 0 : 1);
  lv_obj_add_event_cb(cfgTimeToggle, cfgToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Temperature unit toggle: 0=degF(left), 1=degC(right)
  cfgTempToggle = fcToggleCreate(settingsConfigCtr, 145, "Temp", "\xC2\xB0""F", "\xC2\xB0""C", useFahrenheit ? 0 : 1);
  lv_obj_add_event_cb(cfgTempToggle, cfgToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Distance unit toggle: 0=Imperial(left), 1=Metric(right)
  cfgDistToggle = fcToggleCreate(settingsConfigCtr, 195, "Distance", "Imperial", "Metric", useMetricUnits ? 1 : 0);
  lv_obj_add_event_cb(cfgDistToggle, cfgToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Live preview label
  cfgPreviewLabel = lv_label_create(settingsConfigCtr);
  lv_obj_set_pos(cfgPreviewLabel, 20, 235);
  lv_obj_set_style_text_font(cfgPreviewLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(cfgPreviewLabel, lv_color_hex(0x808080), 0);
  lv_label_set_text(cfgPreviewLabel, "");

  // Action bar with Back and OK
  lv_obj_t* cfgActBar = fcActionBarCreate(settingsConfigCtr, true, true);
  lv_obj_t* cfgBack = lv_obj_get_child(cfgActBar, 0);
  lv_obj_t* cfgOK   = lv_obj_get_child(cfgActBar, 1);
  lv_obj_add_event_cb(cfgBack, cfgBackCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(cfgOK,   cfgOKCb,   LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 2: Display (#112) ---
  settingsDisplayCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsDisplayCtr);
  lv_obj_set_size(settingsDisplayCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsDisplayCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsDisplayCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsDisplayCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsDisplayCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsDisplayCtr, "DISPLAY");

  // Brightness label
  lv_obj_t* brightLbl = lv_label_create(settingsDisplayCtr);
  lv_label_set_text(brightLbl, "Brightness");
  lv_obj_set_style_text_color(brightLbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(brightLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(brightLbl, 20, 53);

  // Brightness slider: range 25-255, initial = tftBrightness
  dispBrightnessSlider = lv_slider_create(settingsDisplayCtr);
  lv_obj_set_size(dispBrightnessSlider, 250, 30);
  lv_obj_set_pos(dispBrightnessSlider, 140, 47);
  lv_slider_set_range(dispBrightnessSlider, 25, 255);
  lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
  // Style: green indicator, dark gray track, white knob
  lv_obj_set_style_bg_color(dispBrightnessSlider, lv_color_hex(0x2A2A2A), 0);           // track bg
  lv_obj_set_style_bg_color(dispBrightnessSlider, lv_color_hex(0x007D00), LV_PART_INDICATOR); // green fill
  lv_obj_set_style_bg_color(dispBrightnessSlider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);     // white knob
  lv_obj_set_style_pad_all(dispBrightnessSlider, 8, LV_PART_KNOB);  // bigger knob touch target
  lv_obj_add_event_cb(dispBrightnessSlider, dispBrightnessChangedCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Brightness numeric readout (to the right of slider)
  dispBrightnessLabel = lv_label_create(settingsDisplayCtr);
  lv_obj_set_pos(dispBrightnessLabel, 375, 53);
  lv_obj_set_style_text_color(dispBrightnessLabel, FC_COLOR_VALUE, 0);
  lv_obj_set_style_text_font(dispBrightnessLabel, FC_FONT_SM, 0);
  lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);

  // TFT Sleep timeout dropdown — click on entire row (#117)
  int tftIdx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
  dispTftDropdown = fcDropdownCreate(settingsDisplayCtr, 105, "TFT Sleep", tftTimeoutLabels[tftIdx]);
  lv_obj_add_event_cb(dispTftDropdown, dispTftDropdownCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(dispTftDropdown, dispTftSelectedCb, LV_EVENT_VALUE_CHANGED, NULL);

  // OLED Sleep timeout dropdown — click on entire row (#117)
  int oledIdx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
  dispOledDropdown = fcDropdownCreate(settingsDisplayCtr, 155, "OLED Sleep", oledTimeoutLabels[oledIdx]);
  lv_obj_add_event_cb(dispOledDropdown, dispOledDropdownCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(dispOledDropdown, dispOledSelectedCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Action bar with Back + OK
  lv_obj_t* dispActBar = fcActionBarCreate(settingsDisplayCtr, true, true);
  lv_obj_t* dispBack = lv_obj_get_child(dispActBar, 0);
  lv_obj_t* dispOK   = lv_obj_get_child(dispActBar, 1);
  lv_obj_add_event_cb(dispBack, dispBackCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(dispOK,   dispOKCb,   LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 3: Compass Calibration (#112) ---
  settingsCalCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsCalCtr);
  lv_obj_set_size(settingsCalCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsCalCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsCalCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsCalCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsCalCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsCalCtr, "COMPASS CAL");

  // === Idle state widgets ===

  // Status label
  calStatusLabel = lv_label_create(settingsCalCtr);
  lv_obj_set_pos(calStatusLabel, 20, 50);
  lv_obj_set_style_text_font(calStatusLabel, FC_FONT_MD, 0);
  lv_label_set_text(calStatusLabel, "Status: ---");

  // Offsets label
  calOffsetsLabel = lv_label_create(settingsCalCtr);
  lv_obj_set_pos(calOffsetsLabel, 20, 80);
  lv_obj_set_style_text_font(calOffsetsLabel, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(calOffsetsLabel, FC_COLOR_DIM, 0);
  lv_label_set_text(calOffsetsLabel, "Offsets: ---");

  // Start Calibration button
  calStartBtn = lv_button_create(settingsCalCtr);
  lv_obj_set_size(calStartBtn, 220, 40);
  lv_obj_set_pos(calStartBtn, 130, 130);
  lv_obj_set_style_radius(calStartBtn, 6, 0);
  if (magAvailable) {
    lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x007D00), 0);  // Green
  } else {
    lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x424242), 0);  // Gray disabled
    lv_obj_add_state(calStartBtn, LV_STATE_DISABLED);
  }
  lv_obj_add_event_cb(calStartBtn, calStartBtnCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* calBtnLbl = lv_label_create(calStartBtn);
  lv_label_set_text(calBtnLbl, "Start Calibration");
  lv_obj_set_style_text_font(calBtnLbl, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(calBtnLbl, FC_COLOR_TEXT, 0);
  lv_obj_center(calBtnLbl);

  // Idle action bar with Back only
  calIdleActBar = fcActionBarCreate(settingsCalCtr, true, false);
  lv_obj_t* calBack = lv_obj_get_child(calIdleActBar, 0);
  lv_obj_add_event_cb(calBack, calBackCb, LV_EVENT_CLICKED, NULL);

  // === Active calibration widgets (hidden initially) ===

  // Progress arc
  calArc = lv_arc_create(settingsCalCtr);
  lv_obj_set_size(calArc, 140, 140);
  lv_obj_set_pos(calArc, 170, 55);
  lv_arc_set_range(calArc, 0, 100);
  lv_arc_set_value(calArc, 0);
  lv_arc_set_bg_angles(calArc, 0, 360);
  lv_obj_remove_style(calArc, NULL, LV_PART_KNOB);  // Hide knob
  lv_obj_clear_flag(calArc, LV_OBJ_FLAG_CLICKABLE);  // Not interactive
  lv_obj_set_style_arc_color(calArc, lv_color_hex(0x2A2A2A), LV_PART_MAIN);      // Background arc
  lv_obj_set_style_arc_color(calArc, lv_color_hex(0x007D00), LV_PART_INDICATOR);  // Green progress
  lv_obj_set_style_arc_width(calArc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(calArc, 12, LV_PART_INDICATOR);
  lv_obj_add_flag(calArc, LV_OBJ_FLAG_HIDDEN);

  // Countdown label (centered in arc)
  calCountdownLabel = lv_label_create(settingsCalCtr);
  lv_obj_set_pos(calCountdownLabel, 225, 105);  // Centered in arc area
  lv_obj_set_style_text_font(calCountdownLabel, FC_FONT_HERO, 0);  // 32px
  lv_obj_set_style_text_color(calCountdownLabel, FC_COLOR_TEXT, 0);
  lv_label_set_text(calCountdownLabel, "15");
  lv_obj_add_flag(calCountdownLabel, LV_OBJ_FLAG_HIDDEN);

  // Instruction label
  calInstructLabel = lv_label_create(settingsCalCtr);
  lv_obj_set_pos(calInstructLabel, 80, 210);
  lv_obj_set_style_text_font(calInstructLabel, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(calInstructLabel, FC_COLOR_WARN, 0);  // Orange
  lv_label_set_text(calInstructLabel, "Rotate device slowly 360\xC2\xB0");
  lv_obj_add_flag(calInstructLabel, LV_OBJ_FLAG_HIDDEN);

  // Min/max label
  calMinMaxLabel = lv_label_create(settingsCalCtr);
  lv_obj_set_pos(calMinMaxLabel, 20, 245);
  lv_obj_set_style_text_font(calMinMaxLabel, FC_FONT_XS, 0);  // 14px
  lv_obj_set_style_text_color(calMinMaxLabel, FC_COLOR_DIM, 0);
  lv_label_set_text(calMinMaxLabel, "");
  lv_obj_add_flag(calMinMaxLabel, LV_OBJ_FLAG_HIDDEN);

  // --- Sub-screen 4: Diagnostics (#112) ---
  settingsDiagsCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsDiagsCtr);
  lv_obj_set_size(settingsDiagsCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsDiagsCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsDiagsCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsDiagsCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsDiagsCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsDiagsCtr, "DIAGNOSTICS");

  // 11 label-value rows
  static const char* diagLabels[11] = {
    "BSEC:", "Weather:", "Heap:", "PSRAM:", "Sensors:",
    "Temps:", "GPS:", "MagCal:", "Storage:", "Web:", "Touch:"
  };

  int diagY = 38;
  int diagLineH = 21;
  for (int i = 0; i < 11; i++) {
    // Cyan label
    lv_obj_t* lbl = lv_label_create(settingsDiagsCtr);
    lv_label_set_text(lbl, diagLabels[i]);
    lv_obj_set_pos(lbl, 10, diagY + i * diagLineH);
    lv_obj_set_style_text_font(lbl, FC_FONT_XS, 0);
    lv_obj_set_style_text_color(lbl, FC_COLOR_HEADER, 0);

    // Value label (updated each frame)
    diagValueLabels[i] = lv_label_create(settingsDiagsCtr);
    lv_label_set_text(diagValueLabels[i], "---");
    lv_obj_set_pos(diagValueLabels[i], 70, diagY + i * diagLineH);
    lv_obj_set_style_text_font(diagValueLabels[i], FC_FONT_XS, 0);
    lv_obj_set_style_text_color(diagValueLabels[i], FC_COLOR_VALUE, 0);
  }

  // Action bar with Back only
  lv_obj_t* diagsActBar = fcActionBarCreate(settingsDiagsCtr, true, false);
  lv_obj_t* diagsBack = lv_obj_get_child(diagsActBar, 0);
  lv_obj_add_event_cb(diagsBack, diagsBackCb, LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 5: About (#112) ---
  settingsAboutCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsAboutCtr);
  lv_obj_set_size(settingsAboutCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsAboutCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsAboutCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsAboutCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsAboutCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsAboutCtr, "ABOUT");

  // 6 label-value rows
  static const char* aboutLabels[6] = {
    "Version:", "Uptime:", "Heap:", "PSRAM:", "Battery:", "WiFi:"
  };

  int aboutY = 50;
  int aboutLineH = 30;
  for (int i = 0; i < 6; i++) {
    lv_obj_t* lbl = lv_label_create(settingsAboutCtr);
    lv_label_set_text(lbl, aboutLabels[i]);
    lv_obj_set_pos(lbl, 20, aboutY + i * aboutLineH);
    lv_obj_set_style_text_font(lbl, FC_FONT_MD, 0);
    lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);

    aboutValueLabels[i] = lv_label_create(settingsAboutCtr);
    lv_label_set_text(aboutValueLabels[i], "---");
    lv_obj_set_pos(aboutValueLabels[i], 160, aboutY + i * aboutLineH);
    lv_obj_set_style_text_font(aboutValueLabels[i], FC_FONT_MD, 0);
    lv_obj_set_style_text_color(aboutValueLabels[i], FC_COLOR_VALUE, 0);
  }

  // Set version (static, never changes)
  lv_label_set_text(aboutValueLabels[0], FW_VERSION);

  // Action bar with Back only
  lv_obj_t* aboutActBar = fcActionBarCreate(settingsAboutCtr, true, false);
  lv_obj_t* aboutBack = lv_obj_get_child(aboutActBar, 0);
  lv_obj_add_event_cb(aboutBack, aboutBackCb, LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 6: Factory Reset (#112) ---
  settingsResetCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsResetCtr);
  lv_obj_set_size(settingsResetCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsResetCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsResetCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsResetCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsResetCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsResetCtr, "FACTORY RESET");

  // Warning text (orange)
  lv_obj_t* resetWarn = lv_label_create(settingsResetCtr);
  lv_label_set_text(resetWarn, "Reset all settings to\nfactory defaults?");
  lv_obj_set_pos(resetWarn, 20, 90);
  lv_obj_set_style_text_font(resetWarn, FC_FONT_LG, 0);  // 20px
  lv_obj_set_style_text_color(resetWarn, FC_COLOR_WARN, 0);  // Orange

  // Info text (dim)
  lv_obj_t* resetInfo = lv_label_create(settingsResetCtr);
  lv_label_set_text(resetInfo, "Compass calibration will\nbe preserved.");
  lv_obj_set_pos(resetInfo, 20, 155);
  lv_obj_set_style_text_font(resetInfo, FC_FONT_SM, 0);  // 16px
  lv_obj_set_style_text_color(resetInfo, FC_COLOR_DIM, 0);

  // Action bar with Back only
  lv_obj_t* resetActBar = fcActionBarCreate(settingsResetCtr, true, false);
  lv_obj_t* resetBack = lv_obj_get_child(resetActBar, 0);
  lv_obj_add_event_cb(resetBack, resetBackCb, LV_EVENT_CLICKED, NULL);

  // Red "Reset" button at OK button position
  lv_obj_t* resetBtn = lv_button_create(settingsResetCtr);
  lv_obj_set_size(resetBtn, 100, 40);
  lv_obj_set_pos(resetBtn, 360, 275);
  lv_obj_set_style_bg_color(resetBtn, lv_color_hex(0xCC0000), 0);  // Red
  lv_obj_set_style_radius(resetBtn, 8, 0);
  lv_obj_add_event_cb(resetBtn, resetBtnCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* resetBtnLbl = lv_label_create(resetBtn);
  lv_label_set_text(resetBtnLbl, "Reset");
  lv_obj_set_style_text_font(resetBtnLbl, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(resetBtnLbl, FC_COLOR_TEXT, 0);
  lv_obj_center(resetBtnLbl);
}

// ============== LVGL Initialization (#105) ==============

void initLVGL() {
  logPrint("Initializing LVGL 9.5... ");

  // Core init
  lv_init();

  // Register tick source (esp_timer µs → ms)
  lv_tick_set_cb(lvglTickCb);

  // Register log callback
  #if LV_USE_LOG != 0
  lv_log_register_print_cb(lvglLogCb);
  #endif

  // Allocate draw buffers in PSRAM (two 480×50 partial-render buffers)
  if (psramFound()) {
    lvglBuf1 = (uint8_t*)heap_caps_malloc(LVGL_BUF_SIZE, MALLOC_CAP_SPIRAM);
    lvglBuf2 = (uint8_t*)heap_caps_malloc(LVGL_BUF_SIZE, MALLOC_CAP_SPIRAM);
  }
  if (!lvglBuf1 || !lvglBuf2) {
    logPrintln("FAIL — PSRAM buffer alloc");
    if (lvglBuf1) { heap_caps_free(lvglBuf1); lvglBuf1 = NULL; }
    if (lvglBuf2) { heap_caps_free(lvglBuf2); lvglBuf2 = NULL; }
    return;
  }

  // Create display (480×320 landscape, matching TFT rotation 1)
  lvglDisplay = lv_display_create(480, 320);
  if (!lvglDisplay) {
    logPrintln("FAIL — display create");
    heap_caps_free(lvglBuf1); lvglBuf1 = NULL;
    heap_caps_free(lvglBuf2); lvglBuf2 = NULL;
    return;
  }

  // Set flush callback and double-buffered partial rendering
  lv_display_set_flush_cb(lvglDisplay, lvglFlushCb);
  lv_display_set_buffers(lvglDisplay, lvglBuf1, lvglBuf2,
                         LVGL_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lvglAvailable = true;
  logPrintf("OK (buf: 2x%dKB PSRAM, %dKB free)\n",
            LVGL_BUF_SIZE / 1024, ESP.getFreePsram() / 1024);

  // Create touch input device (FT6336U → pointer) (#106)
  lvglTouchIndev = lv_indev_create();
  if (lvglTouchIndev) {
    lv_indev_set_type(lvglTouchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvglTouchIndev, lvglTouchReadCb);
    lv_indev_set_display(lvglTouchIndev, lvglDisplay);
    // Increase scroll threshold: default 10px is too sensitive for capacitive touch
    // jitter — finger movement during tap is typically 5-15px. 50px ensures taps
    // aren't misinterpreted as scroll gestures on non-scrollable containers. (#112)
    lv_indev_set_scroll_limit(lvglTouchIndev, 50);
    logPrintln("[LVGL] Touch indev created (pointer, scroll_limit=50)");
  }

  // Create encoder input device (buttons A/B/C → encoder) (#106)
  lvglEncoderIndev = lv_indev_create();
  if (lvglEncoderIndev) {
    lv_indev_set_type(lvglEncoderIndev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(lvglEncoderIndev, lvglEncoderReadCb);
    lv_indev_set_display(lvglEncoderIndev, lvglDisplay);

    // Create focus group for encoder navigation
    lvglGroup = lv_group_create();
    if (lvglGroup) {
      lv_indev_set_group(lvglEncoderIndev, lvglGroup);
      lv_group_set_default(lvglGroup);  // New widgets auto-join this group
      logPrintln("[LVGL] Encoder indev + group created");
    }
  }

  // Initialize Field Compass theme and named styles (#107)
  initFCTheme();

  // Disable scrolling on root screen — LVGL's scroll chain walks up to this
  // from any widget, triggering unwanted scroll-vs-click detection (#112)
  lv_obj_clear_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

  // Build LVGL compass screen (#109)
  buildCompassScreen();

  // Build LVGL geocache screen (#110)
  buildGeocacheScreen();

  // Build LVGL environment screen (#111)
  buildEnvScreen();

  // Build LVGL telemetry screen (#111)
  buildTelemetryScreen();

  // Build LVGL settings screen (#112)
  buildSettingsScreen();

  // Load compass as the initial active screen (#113)
  lv_screen_load(compassScr);

  // Widget library demo screen (#108): all 6 widgets
  #if LVGL_TEST_MODE
  {
    // Header with title + gear icon
    lv_obj_t* header = fcHeaderCreate(lv_screen_active(), "WIDGET DEMO");

    // Toggle: 12h/24h time format
    lv_obj_t* toggle1 = fcToggleCreate(lv_screen_active(), 45,
      "Time", "12-Hour", "24-Hour", false);

    // Toggle: temp unit
    lv_obj_t* toggle2 = fcToggleCreate(lv_screen_active(), 85,
      "Temp", "\xC2\xB0""F", "\xC2\xB0""C", false);

    // Dropdown: timezone (static items for demo)
    lv_obj_t* dropdown = fcDropdownCreate(lv_screen_active(), 130,
      "Zone", "Eastern (UTC-5)");

    // Action bar with Back + OK
    lv_obj_t* actionBar = fcActionBarCreate(lv_screen_active(), true, true);

    // Nav bar with 4 screens, screen 1 active
    lv_obj_t* navBar = fcNavBarCreate(lv_screen_active(), 4, 0);

    lv_timer_handler();  // Render to TFT
    delay(5000);         // Hold for visual confirmation
  }
  logPrintln("[LVGL] Widget demo rendered (LVGL_TEST_MODE=1)");
  #endif
}

// Preventive TFT re-initialization (P1 blank bug workaround)
// LVGL continuously repaints, so blank-screen is self-healing.
// Kept as a safety net — re-init every 30 minutes + invalidate LVGL.
void checkTFTHealth() {
  unsigned long now = millis();
  if (tftSleeping) return;

  if (now - lastTFTReinit > TFT_REINIT_INTERVAL) {
    #if DEBUG_TFT
    logPrintf("[TFT] Preventive re-init at %lus (updates:%lu)\n",
              now / 1000, tftUpdateCount);
    #endif
    tft.init();
    tft.setRotation(1);
    analogWrite(TFT_BL, tftBrightness);
    lv_obj_invalidate(lv_screen_active());  // Force LVGL full repaint
    lastTFTReinit = now;
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
  delay(100);  // Let UART settle before sending command
  Serial1.println("$PMTK101*32");  // Hot restart — ensures search is active (#115)
  logPrintln("OK (9600 baud, PMTK101 sent)");
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

  // Ensure SD_CS is HIGH before init (defense-in-depth — also set at top of setup)
  digitalWrite(SD_CS, HIGH);

  // Use explicit SPI instance and conservative 4MHz clock to avoid bus
  // speed conflicts with TFT_eSPI running at 80MHz on same FSPI bus (#116)
  const int SD_SPI_FREQ = 4000000;  // 4MHz — maximum reliability on shared bus
  const int SD_INIT_RETRIES = 5;    // Increased from 3 → 5 (#116)

  bool mounted = false;
  for (int attempt = 1; attempt <= SD_INIT_RETRIES; attempt++) {
    if (attempt > 1) {
      // Progressive backoff: end previous attempt cleanly, wait longer each retry
      SD.end();
      delay(100 * attempt);  // 200ms, 300ms, 400ms, 500ms
    }
    if (SD.begin(SD_CS, SPI, SD_SPI_FREQ)) {
      mounted = true;
      break;
    }
    logPrintf("retry %d/%d... ", attempt, SD_INIT_RETRIES);
  }

  if (!mounted) {
    logPrintln("NOT FOUND (after retries)");
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

  // Full SPI bus reset: end SD, end SPI, re-init SPI, then re-mount SD (#116)
  // This clears any stale bus state from TFT_eSPI's 80MHz DMA transfers
  SD.end();
  SPI.end();
  delay(100);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  delay(100);  // Allow bus + card to settle

  // Try to re-initialize with conservative 4MHz clock (#116)
  if (SD.begin(SD_CS, SPI, 4000000)) {
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

  // Try open with one retry on failure (bus contention mitigation #116)
  File f;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (strcmp(mode, "r") == 0 || strcmp(mode, FILE_READ) == 0) {
      f = SD.open(path, FILE_READ);
    } else if (strcmp(mode, "w") == 0 || strcmp(mode, FILE_WRITE) == 0) {
      f = SD.open(path, FILE_WRITE);
    } else if (strcmp(mode, "a") == 0) {
      f = SD.open(path, FILE_APPEND);
    } else {
      f = SD.open(path);  // Default mode
    }
    if (f) break;  // Success
    if (attempt == 0) delay(50);  // Brief settle before retry
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

// ============== FRAM Settings Backup (#118) ==============

// XOR-32 checksum over raw bytes (excludes the checksum field itself)
static uint32_t framSettingsChecksum(const FRAMSettings& s) {
  const uint8_t* p = (const uint8_t*)&s;
  size_t len = offsetof(FRAMSettings, checksum);  // everything before checksum
  uint32_t ck = 0;
  for (size_t i = 0; i < len; i++) ck ^= ((uint32_t)p[i]) << ((i & 3) * 8);
  return ck;
}

void saveSettingsToFRAM() {
  if (!framAvailable) return;

  FRAMSettings s;
  memset(&s, 0, sizeof(s));
  s.magic          = FRAM_SETTINGS_MAGIC;
  s.version        = FRAM_SETTINGS_VER;
  s.use12Hour      = use12Hour ? 1 : 0;
  s.useFahrenheit  = useFahrenheit ? 1 : 0;
  s.useMetricUnits = useMetricUnits ? 1 : 0;
  strncpy(s.posixTZ, posixTZ, sizeof(s.posixTZ) - 1);
  strncpy(s.tzDisplayName, tzDisplayName, sizeof(s.tzDisplayName) - 1);
  s.tzSelectedIndex = (int8_t)tzSelectedIndex;
  s.tftBrightness   = tftBrightness;
  s.tftSleepMs      = tftSleepMs;
  s.oledSleepMs     = oledSleepMs;
  s.checksum        = framSettingsChecksum(s);

  const uint8_t* data = (const uint8_t*)&s;
  for (size_t i = 0; i < sizeof(s); i++) {
    fram.write8(FRAM_SETTINGS_ADDR + i, data[i]);
  }
  logPrintln("[SETTINGS] Saved to FRAM");
}

bool loadSettingsFromFRAM() {
  if (!framAvailable) return false;

  FRAMSettings s;
  uint8_t* data = (uint8_t*)&s;
  for (size_t i = 0; i < sizeof(s); i++) {
    data[i] = fram.read8(FRAM_SETTINGS_ADDR + i);
  }

  if (s.magic != FRAM_SETTINGS_MAGIC || s.version != FRAM_SETTINGS_VER) return false;
  if (s.checksum != framSettingsChecksum(s)) {
    logPrintln("[SETTINGS] FRAM checksum mismatch, ignoring");
    return false;
  }

  use12Hour       = s.use12Hour;
  useFahrenheit   = s.useFahrenheit;
  useMetricUnits  = s.useMetricUnits;
  strncpy(posixTZ, s.posixTZ, sizeof(posixTZ) - 1);
  strncpy(tzDisplayName, s.tzDisplayName, sizeof(tzDisplayName) - 1);
  tzSelectedIndex = s.tzSelectedIndex;
  tftBrightness   = constrain(s.tftBrightness, 25, 255);
  tftSleepMs      = s.tftSleepMs;
  oledSleepMs     = s.oledSleepMs;

  logPrintf("[SETTINGS] Loaded from FRAM: 12h=%d F=%d metric=%d tz=%s bright=%d\n",
            use12Hour, useFahrenheit, useMetricUnits, tzDisplayName, tftBrightness);
  return true;
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

// Load user settings — try SD first, fall back to FRAM (#98, #118)
void loadSettings() {
  if (!sdHealth.available) {
    // SD unavailable — try FRAM backup (#118)
    if (loadSettingsFromFRAM()) {
      settingsLoadedFromSD = true;  // Treat FRAM load as success for deferred-load flag
      applyTimezone();
      return;
    }
    logPrintln("[SETTINGS] SD + FRAM unavailable, using defaults");
    applyTimezone();
    return;
  }

  File f = sdOpenSafe("/config/settings.txt", "r", true);  // silent — normal on first boot
  if (!f) {
    // No SD file — try FRAM backup (#118)
    if (loadSettingsFromFRAM()) {
      settingsLoadedFromSD = true;
      applyTimezone();
      return;
    }
    logPrintln("[SETTINGS] No settings file or FRAM backup, using defaults");
    applyTimezone();
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
  settingsLoadedFromSD = true;
  applyTimezone();
  logPrintf("[SETTINGS] Loaded: 12h=%d F=%d metric=%d tz=%s bright=%d\n",
            use12Hour, useFahrenheit, useMetricUnits, tzDisplayName, tftBrightness);
}

// Save user settings to SD + FRAM (#98, #118)
// SD: atomic write via temp file (.tmp → rename) to prevent truncation loss.
// FRAM: instant backup — survives SD mount failures on next boot.
void saveSettings() {
  // Always save to FRAM first — instant, no SD dependency (#118)
  saveSettingsToFRAM();

  if (!sdHealth.available) {
    logPrintln("[SETTINGS] SD unavailable, attempting re-init before save...");
    trySDReInit();  // Try to recover SD before giving up (#118)
    if (!sdHealth.available) {
      logPrintln("[SETTINGS] SD still unavailable — saved to FRAM only");
      return;
    }
  }
  if (!SD.exists("/config")) SD.mkdir("/config");

  const char* tmpPath = "/config/settings.tmp";
  const char* finalPath = "/config/settings.txt";

  File f = sdOpenSafe(tmpPath, "w");
  if (!f) {
    logPrintln("[SETTINGS] Failed to open temp file — saved to FRAM only");
    return;
  }

  f.printf("use12Hour=%d\n", use12Hour ? 1 : 0);
  f.printf("useFahrenheit=%d\n", useFahrenheit ? 1 : 0);
  f.printf("useMetricUnits=%d\n", useMetricUnits ? 1 : 0);
  f.printf("posixTZ=%s\n", posixTZ);
  f.printf("tzName=%s\n", tzDisplayName);
  f.printf("tzIndex=%d\n", tzSelectedIndex);
  f.printf("tftBrightness=%d\n", tftBrightness);
  f.printf("tftSleepMs=%lu\n", tftSleepMs);
  f.printf("oledSleepMs=%lu\n", oledSleepMs);
  f.flush();
  f.close();

  // Atomic swap: remove old, rename temp to final
  if (SD.exists(finalPath)) SD.remove(finalPath);
  SD.rename(tmpPath, finalPath);
  logPrintln("[SETTINGS] Settings saved to SD + FRAM");
}

// Factory reset — delete settings file and restore compiled defaults (#104)
void factoryReset() {
  // Delete stored settings from SD
  if (sdHealth.available && SD.exists("/config/settings.txt")) {
    SD.remove("/config/settings.txt");
  }

  // Clear FRAM settings backup (#118) — zero the magic so it won't be loaded
  if (framAvailable) {
    for (size_t i = 0; i < FRAM_SETTINGS_SIZE; i++) {
      fram.write8(FRAM_SETTINGS_ADDR + i, 0);
    }
    logPrintln("[SETTINGS] FRAM settings cleared");
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
    sprintf(buf, "PSRAM:     %luK / %luK\n",
            (unsigned long)ESP.getFreePsram() / 1024,
            (unsigned long)ESP.getPsramSize() / 1024);
  } else {
    sprintf(buf, "PSRAM:     Not detected\n");
  }
  html += buf;
  sprintf(buf, "Render:    LVGL 9.5 (2x%dKB PSRAM buffers)\n", LVGL_BUF_SIZE / 1024);
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
  // GPS debug/reset endpoints (#115)
  webServer.on("/gps/debug", HTTP_GET, handleWebGpsDebug);
  webServer.on("/gps/reset", HTTP_GET, handleWebGpsReset);
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
  lv_obj_invalidate(lv_screen_active());  // Force LVGL full repaint on wake (#113)
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

// ============== LVGL Screen Navigation (#113) ==============

// Screen table for main screens (Settings is modal, not in cycle)
static lv_obj_t** const mainScreens[] = { &compassScr, &geocacheScr, &envScr, &telemetryScr };

void navigateScreen(int delta) {
  int next = (currentScreen + delta + NUM_SCREENS) % NUM_SCREENS;
  lv_scr_load_anim_t anim = (delta > 0)
      ? LV_SCR_LOAD_ANIM_OVER_LEFT : LV_SCR_LOAD_ANIM_OVER_RIGHT;
  lv_screen_load_anim(*mainScreens[next], anim, 200, 0, false);
  currentScreen = next;
  geocacheSubScreen = 0;
}

void navigateToSettings() {
  previousScreen = currentScreen;
  currentScreen = SCREEN_SETTINGS;
  settingsSubScreen = 0;
  lv_screen_load_anim(settingsScr, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0, false);
  logPrintf("[NAV] → Settings (from screen %d)\n", previousScreen);
}

void navigateFromSettings() {
  saveSettings();
  settingsSubScreen = 0;
  currentScreen = previousScreen;
  lv_screen_load_anim(*mainScreens[currentScreen], LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
  logPrintf("[NAV] Settings → screen %d\n", currentScreen);
}

// LVGL gesture callback — swipe left/right on main screens (#113)
static void screenGestureCb(lv_event_t* e) {
  (void)e;
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  if (dir == LV_DIR_LEFT)       navigateScreen(1);
  else if (dir == LV_DIR_RIGHT) navigateScreen(-1);
}

// Gear icon click — navigate to Settings (#113)
static void gearIconClickCb(lv_event_t* e) {
  (void)e;
  if (currentScreen == SCREEN_SETTINGS) return;
  navigateToSettings();
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

  // Handle A/B based on current screen and sub-screen
  if (currentScreen == SCREEN_GEOCACHE && geocacheSubScreen != 0) {
    // Geocache sub-screen navigation
    handleGeocacheButtons(buttonA, buttonB);
    lastButtonPress = now;
  } else {
    // Normal screen navigation via LVGL animated transitions (#113)
    if (buttonA) {
      navigateScreen(-1);
      lastButtonPress = now;
    }
    if (buttonB) {
      navigateScreen(1);
      lastButtonPress = now;
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
}

// Timeout presets for Display settings (#91)
const uint32_t tftTimeoutPresets[]  = {0, 60000, 120000, 300000, 600000, 900000, 1800000};
const char*    tftTimeoutLabels[]   = {"Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
const int      TFT_TIMEOUT_COUNT    = 7;

const uint32_t oledTimeoutPresets[] = {60000, 120000, 300000, 600000, 900000, 1800000};
const char*    oledTimeoutLabels[]  = {"1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
const int      OLED_TIMEOUT_COUNT   = 6;

// Helper: find index in timeout preset array matching a value
int findTimeoutIndex(const uint32_t presets[], int count, uint32_t value) {
  for (int i = 0; i < count; i++)
    if (presets[i] == value) return i;
  return 0;  // Default to first if not found
}

// handleTap removed — gear icon now uses LVGL gearIconClickCb (#113)

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
    } else if (geocacheSubScreen == 1) {
      // List screen: short press selects cache and returns to nav
      selectedCacheIndex = listHighlightIndex;
      geocacheSubScreen = 0;
    } else if (geocacheSubScreen == 2) {
      // Details screen: short press toggles found status
      if (cacheListCount > 0 && listHighlightIndex < cacheListCount) {
        cacheList[listHighlightIndex].found = !cacheList[listHighlightIndex].found;
        if (cacheList[listHighlightIndex].found) {
          cacheList[listHighlightIndex].foundTime = millis() / 1000;  // Simple timestamp
        }
        saveCacheFoundStatus();  // Persist to SD
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
    } else if (geocacheSubScreen == 2) {
      // Details screen: long press goes back to list
      geocacheSubScreen = 1;
    }
  }
}

// ============== GPS Reading ==============

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

// Zone helper implementations removed — LVGL handles dirty tracking (#114)

// Update SD card status indicator on all screen headers (#120)
void updateSDIndicators() {
  SDIndicatorState state;
  if (sdAvailable && sdHealth.consecutiveFailures == 0) {
    state = SD_IND_OK;
  } else if (sdAvailable) {
    state = SD_IND_ERROR;  // mounted but experiencing errors
  } else {
    state = SD_IND_MISSING; // never mounted or fully failed
  }
  fcHeaderSetSDStatus(compassHeader, state);
  fcHeaderSetSDStatus(gcNavHeader, state);
  fcHeaderSetSDStatus(envHeader, state);
  fcHeaderSetSDStatus(telHeader, state);
}

void updateDisplay() {
  static unsigned long lastUpdate = 0;

  // Throttle: update data every 500ms (LVGL rendering runs independently)
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  // Update TFT display data (if not sleeping)
  if (!tftSleeping) {
    // Update SD card status indicator on all screen headers (#120)
    updateSDIndicators();

    // Update active screen data — LVGL handles rendering via lv_timer_handler()
    switch (currentScreen) {
      case SCREEN_COMPASS:   updateCompassData(); fcNavBarSetActive(compassNavBar, currentScreen); break;
      case SCREEN_GEOCACHE:  updateGeocacheData(); break;
      case SCREEN_ENV:       updateEnvData(); break;
      case SCREEN_TELEMETRY: updateTelemetryData(); break;
      case SCREEN_SETTINGS:  updateSettingsData(); break;
    }

    // Track TFT update for health monitoring
    lastTFTUpdate = millis();
    tftUpdateCount++;
  }

  // Update OLED display (if available and not sleeping)
  if (oledAvailable && !oledSleeping) {
    updateOLED();
  }
}

// ============== Utility Functions (preserved from legacy) ==============

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

// Legacy TFT_eSprite draw functions removed — all rendering via LVGL (#114)
// Removed: drawHeader, drawNavBar, drawLabel, drawValue, drawScreenTelemetry,
//   drawScreenEnv, drawScreenCompass, drawCompassRose (legacy sprite version),
//   drawNavTriangle, drawSearchZoneCircle, drawScreenGeocache,
//   drawCacheNavScreen, drawCacheListScreen, drawCacheDetailsScreen (~1,300 lines)

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
