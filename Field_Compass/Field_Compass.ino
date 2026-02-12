/*
 * Field Compass - Dual Display Firmware
 *
 * Hardware:
 * - Adafruit ESP32-S3 Feather 8MB w.FL
 * - Adafruit SH1107 OLED FeatherWing 128x64 (I2C)
 * - Adafruit ST7789 2.0" TFT 320x240 (SPI via EYESPI breakout)
 * - Adafruit Ultimate GPS FeatherWing PA1616D (Serial)
 * - Adafruit BME688 (I2C - STEMMA QT) with BSEC2
 * - Adafruit LSM6DSOX + LIS3MDL 9-DoF IMU (I2C - STEMMA QT)
 *
 * Screens:
 * 1. Operational Info (time, uptime, WiFi, battery)
 * 2. GPS Info (coordinates, altitude, address)
 * 3. BME688 Environmental (temp, humidity, pressure, IAQ, CO2)
 * 4. IMU/Compass (heading, orientation, acceleration)
 * 5. Diagnostics (BSEC state, weather log, system info)
 *
 * Navigation: Button A = prev screen, Button B = next screen
 * Display Sleep: OLED 3 min, TFT 15 min (button press wakes)
 *
 * Issues: #44, #46, #47
 */

// Firmware version
#define FW_VERSION "0.13"

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <stdarg.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SH110X.h>
#include <bsec2.h>

// BSEC2 IAQ config for BME680/688 at 3.3V, 3-second sample rate, 4-day calibration
const uint8_t bsec2_config[] = {
  #include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};

#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_LIS3MDL.h>
#include <Adafruit_MAX1704X.h>
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

// TFT Display pins (EYESPI breakout)
#define TFT_CS    18  // A0 -> TCS
#define TFT_DC    17  // A1 -> DC
#define TFT_RST   16  // A2 -> RST
#define TOUCH_CS  15  // A3 -> TSCS (for future use)
#define SD_CS     14  // A4 -> SDCS (TFT MicroSD slot)

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
#define NUM_SCREENS 5
#define SCREEN_OPS 0
#define SCREEN_GPS 1
#define SCREEN_ENV 2
#define SCREEN_IMU 3
#define SCREEN_DIAGS 4

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

// TFT Display
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// OLED Display
Adafruit_SH1107 oled = Adafruit_SH1107(64, 128, &Wire);

// Sensors
Bsec2 envSensor;
Adafruit_LSM6DSOX lsm;
Adafruit_LIS3MDL lis;
Adafruit_MAX17048 battery;

// Web Server
WebServer webServer(WEB_SERVER_PORT);

// Serial ring buffer for web streaming
static char serialRing[SERIAL_RING_SIZE];
static volatile uint16_t serialRingHead = 0;
static volatile uint16_t serialRingTail = 0;

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
bool imuAvailable = false;
bool magAvailable = false;
bool batteryAvailable = false;
bool sdAvailable = false;
bool wifiConnected = false;
bool ntpSynced = false;
bool webServerStarted = false;

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
  int hour = 0;
  int minute = 0;
  int second = 0;
  bool timeValid = false;
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

// Custom print that captures to ring buffer
void logPrint(const char* msg) {
  serialRingAppend(msg);
  Serial.print(msg);
}

void logPrintln(const char* msg) {
  serialRingAppend(msg);
  serialRingAppend("\n");
  Serial.println(msg);
}

void logPrintf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  serialRingAppend(buf);
  Serial.print(buf);
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

  // Initialize SPI for TFT
  SPI.begin();

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
  initIMU();
  initBattery();
  initSD();
  initWiFi();

  // Load weather history and BSEC state from SD
  if (sdAvailable) {
    loadWeatherHistory();
    if (bmeAvailable) {
      loadBsecState();
    }
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
  if (imuAvailable && magAvailable) readIMU();

  // Weather logging (every 5 minutes)
  if (sdAvailable && bmeAvailable && (millis() - lastWeatherLog > WEATHER_LOG_INTERVAL)) {
    logWeatherReading();
    calculateWeatherTrend();
    lastWeatherLog = millis();
  }

  // Update weather log statistics periodically
  updateWeatherLogStats();

  // Battery logging to SD card (every 10 seconds)
  if (sdAvailable && batteryAvailable && (millis() - lastBattLog > BATT_LOG_INTERVAL)) {
    logBatteryToSD();
    lastBattLog = millis();
  }

  // Periodic status logging for web serial monitor
  if (millis() - lastStatusLog > STATUS_LOG_INTERVAL) {
    char buf[256];
    float battV = batteryAvailable ? battery.cellVoltage() : 0;
    float battP = batteryAvailable ? battery.cellPercent() : 0;
    float battR = batteryAvailable ? battery.chargeRate() : 0;
    // Include GPS TTFF in status (#68)
    const char* gpsStatus = gpsHadFirstFix ? "GPS:OK" : (gpsHadFirstReceive ? "GPS:--" : "GPS:NO");
    snprintf(buf, sizeof(buf), "[%lus] T:%.1fF H:%.0f%% IAQ:%.0f Hdg:%.0f %s TTFF:%lus Batt:%.3fV/%.2f%%/%+.1f%%hr\n",
             millis() / 1000,
             envData.temperature * 9.0 / 5.0 + 32.0,
             envData.humidity,
             envData.iaq,
             imuData.heading,
             gpsStatus,
             gpsHadFirstFix ? gpsFirstFixTime / 1000 : 0,
             battV, battP, battR);
    serialRingAppend(buf);
    Serial.print(buf);

    // TFT debug logging (P1 blank bug investigation)
    #if DEBUG_TFT
    unsigned long tftAge = (millis() - lastTFTUpdate) / 1000;
    unsigned long reinitAge = (millis() - lastTFTReinit) / 1000;
    snprintf(buf, sizeof(buf), "[TFT] sleep:%d scr:%d upd:%lus ago reinit:%lus ago cnt:%lu\n",
             tftSleeping, currentScreen, tftAge, reinitAge, tftUpdateCount);
    serialRingAppend(buf);
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
      }
      logPrintf("  Found device at 0x%02X%s\n", address, desc);
      deviceCount++;
    }
  }
  logPrintf("  Total devices: %d\n\n", deviceCount);
}

// ============== Initialization Functions ==============

void initTFT() {
  logPrint("Initializing ST7789 TFT... ");

  tft.init(TFT_HEIGHT, TFT_WIDTH);  // 240x320, but we use landscape
  tft.setRotation(1);  // Landscape mode
  tft.fillScreen(ST77XX_RED);  // Flash red to confirm TFT is working
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
    tft.init(TFT_HEIGHT, TFT_WIDTH);
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

  // BSEC state persistence: save when accuracy reaches level 3 for first time
  static uint8_t lastAccuracy = 0;
  if (envData.iaqAccuracy == 3 && lastAccuracy < 3) {
    saveBsecState();
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
void logWeatherReading() {
  if (!sdHealth.available || !bmeAvailable) return;

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
  reading.temp = envData.temperature;
  reading.humidity = envData.humidity;

  // Add to in-memory buffer
  addToWeatherHistory(reading);

  // Write to SD card (silent fail OK - weather logging is non-critical)
  char filename[32];
  getWeatherFilename(filename, 0);

  File file = sdOpenSafe(filename, "a", true);  // silent fail
  if (file) {
    file.printf("%lu,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                timestamp, lat, lon,
                reading.pressure, reading.temp, reading.humidity);
    file.close();
    recordSDSuccess();
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
  float t = envData.temperature;
  float h = envData.humidity;
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
  html += "<a href='/serial'>Serial Monitor</a>";
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
  if (bmeAvailable) {
    float tempF = envData.temperature * 9.0 / 5.0 + 32.0;
    sprintf(buf, "Temp:     %.1fF (%.1fC)\n", tempF, envData.temperature);
    html += buf;
    sprintf(buf, "Humidity: %.1f%%\n", envData.humidity);
    html += buf;
    sprintf(buf, "IAQ:      %.0f [%s]\n", envData.iaq, getIaqAccuracyText(envData.iaqAccuracy));
    html += buf;
    sprintf(buf, "CO2:      %.0f ppm\n", envData.co2Equivalent);
    html += buf;
    sprintf(buf, "Pressure: %.1f hPa (%.2f\")\n", envData.pressure, hPaToInHg(envData.pressure));
    html += buf;
    sprintf(buf, "Forecast: %s %s\n", getTrendArrow(), weatherTrend.forecast);
    html += buf;
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

void handleWebJSON() {
  char buf[1024];
  bool battConnected = batteryAvailable && isBatteryConnected();
  snprintf(buf, sizeof(buf),
    "{"
    "\"gps\":{\"valid\":%s,\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f},"
    "\"env\":{\"temp\":%.1f,\"humidity\":%.1f,\"pressure\":%.1f,\"iaq\":%.0f,\"co2\":%.0f,\"accuracy\":%d},"
    "\"imu\":{\"heading\":%.1f,\"roll\":%.1f,\"pitch\":%.1f,\"accel\":%.2f},"
    "\"system\":{\"uptime\":%lu,\"wifi\":%s,\"battery\":%.1f,\"batteryConnected\":%s,\"heap\":%lu}"
    "}",
    gpsData.valid ? "true" : "false", gpsData.latitude, gpsData.longitude, gpsData.altitude,
    envData.temperature, envData.humidity, envData.pressure, envData.iaq, envData.co2Equivalent, envData.iaqAccuracy,
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
  tft.enableSleep(true);
  #if DEBUG_SLEEP
  Serial.println("TFT sleeping");
  #endif
}

void wakeTFT() {
  if (!tftSleeping) return;

  tftSleeping = false;
  tft.enableSleep(false);
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

  // Debounce check
  if (now - lastButtonPress < DEBOUNCE_MS) return;

  // Check if any button is pressed
  bool buttonA = !digitalRead(BUTTON_A);
  bool buttonB = !digitalRead(BUTTON_B);
  bool buttonC = !digitalRead(BUTTON_C);

  if (!buttonA && !buttonB && !buttonC) return;  // No button pressed

  // If any display is sleeping, wake all and consume the button press
  if (tftSleeping || oledSleeping) {
    wakeAllDisplays();
    lastButtonPress = now;
    return;  // Don't process button action on wake
  }

  // Reset activity timer on any button press
  lastActivityTime = now;

  // Button A - Previous screen
  if (buttonA) {
    currentScreen--;
    if (currentScreen < 0) currentScreen = NUM_SCREENS - 1;
    lastButtonPress = now;
    tft.fillScreen(COLOR_BG);  // Clear screen on change
  }

  // Button B - Next screen
  if (buttonB) {
    currentScreen++;
    if (currentScreen >= NUM_SCREENS) currentScreen = 0;
    lastButtonPress = now;
    tft.fillScreen(COLOR_BG);  // Clear screen on change
  }

  // Button C - Reserved
  if (buttonC) {
    lastButtonPress = now;
    // No action yet
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
    } else {
      // Track when signal is lost (for reacquiring elapsed time) (#68)
      if (gpsData.valid && gpsHadFirstFix) {
        gpsSignalLostTime = millis();
        logPrintf("[GPS] Signal lost at %lus\n", gpsSignalLostTime / 1000);
      }
      gpsData.valid = false;
    }
  }

  // Parse GPGGA or GNGGA for altitude
  if (strstr(sentence, "GGA")) {
    char* token = strtok(sentence, ",");
    int field = 0;

    while (token != NULL) {
      if (field == 9 && strlen(token) > 0) {
        gpsData.altitude = atof(token);
      }
      token = strtok(NULL, ",");
      field++;
    }
  }
}

// ============== Sensor Reading ==============

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
  int labelX = 20;
  int valueX = 110;
  int lineH = 28;
  char buf[40];

  if (bmeAvailable) {
    // Temperature (compensated)
    float tempF = envData.temperature * 9.0 / 5.0 + 32.0;
    drawLabel(labelX, y, "Temp:");
    sprintf(buf, "%.1fF (%.1fC)", tempF, envData.temperature);
    drawValue(valueX, y, buf);
    y += lineH;

    // Humidity (compensated)
    drawLabel(labelX, y, "Humid:");
    sprintf(buf, "%.1f%%", envData.humidity);
    drawValue(valueX, y, buf);
    y += lineH;

    // IAQ with accuracy indicator
    drawLabel(labelX, y, "IAQ:");
    sprintf(buf, "%.0f [%s]", envData.iaq, getIaqAccuracyText(envData.iaqAccuracy));
    // Color based on IAQ: 0-50 good, 51-100 moderate, 101-150 poor, 151-200 unhealthy, >200 very unhealthy
    uint16_t color = COLOR_VALUE;
    if (envData.iaq > 200) color = COLOR_ERROR;
    else if (envData.iaq > 100) color = COLOR_WARN;
    drawValue(valueX, y, buf, color);
    y += lineH;

    // CO2 equivalent
    drawLabel(labelX, y, "CO2:");
    sprintf(buf, "%.0f ppm", envData.co2Equivalent);
    color = COLOR_VALUE;
    if (envData.co2Equivalent > 2000) color = COLOR_ERROR;
    else if (envData.co2Equivalent > 1000) color = COLOR_WARN;
    drawValue(valueX, y, buf, color);
    y += lineH;

    // Pressure (station/absolute)
    drawLabel(labelX, y, "Press:");
    sprintf(buf, "%.1f hPa (%.2f\")", envData.pressure, hPaToInHg(envData.pressure));
    drawValue(valueX, y, buf);
    y += lineH;

    // Weather trend and forecast
    drawLabel(labelX, y, "Fcst:");
    sprintf(buf, "%s %s", getTrendArrow(), weatherTrend.forecast);
    color = COLOR_VALUE;
    if (strstr(weatherTrend.forecast, "Storm")) color = COLOR_ERROR;
    else if (strstr(weatherTrend.forecast, "Rain") || strstr(weatherTrend.forecast, "Snow")) color = COLOR_WARN;
    drawValue(valueX, y, buf, color);

  } else {
    tft.setTextColor(COLOR_ERROR);
    tft.setTextSize(2);
    tft.setCursor(60, 100);
    tft.println("BME688 not found");
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
  sprintf(buf, "BME:%s IMU:%s Mag:%s Bat:%s",
          bmeAvailable ? "Y" : "N",
          imuAvailable ? "Y" : "N",
          magAvailable ? "Y" : "N",
          batteryAvailable ? "Y" : "N");
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

  if (bmeAvailable) {
    float tempF = envData.temperature * 9.0 / 5.0 + 32.0;

    oled.setCursor(0, 10);
    sprintf(buf, "%.1fF %.1f%% IAQ:%.0f", tempF, envData.humidity, envData.iaq);
    oled.print(buf);

    oled.setCursor(0, 22);
    sprintf(buf, "%.0fhPa %.2f\"", envData.pressure, hPaToInHg(envData.pressure));
    oled.print(buf);

    oled.setCursor(0, 34);
    sprintf(buf, "%s %s", getTrendArrow(), weatherTrend.forecast);
    oled.print(buf);

    oled.setCursor(0, 46);
    sprintf(buf, "CO2:%.0f", envData.co2Equivalent);
    oled.print(buf);
  } else {
    oled.setCursor(0, 28);
    oled.print("BME688 not found");
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
