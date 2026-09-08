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

#include "fc_version.h"   // FW_VERSION

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <stdarg.h>
#include <Adafruit_GFX.h>
#include <TFT_eSPI.h>          // ST7796U display driver (pins in platformio.ini build_flags, #184)
#include <Adafruit_FT6206.h>   // FT6336U capacitive touch (I2C 0x38)
#include <Adafruit_SH110X.h>
#include <bsec2.h>

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
#include "fc_theme.h"
#include "geo.h"
#include "ui_widgets.h"
#include "fc_config.h"
#include "logging.h"
#include "fram.h"
#include "settings.h"
#include "rtc.h"
#include "gps.h"
#include "imu.h"
#include "env.h"
#include "battery.h"
#include "weather.h"
#include "geocache.h"
#include "oled.h"
#include "display.h"
#include "ui_state.h"
#include "touch.h"
#include "i2c_health.h"   // reset reason at boot; the units own their device records (#289)
#include "web.h"
#include "lvgl_port.h"
#include "oled_screens.h"
#include "screen_compass.h"
#include "screen_env.h"
#include "screen_telemetry.h"
#include "screen_geocache.h"
#include "screen_settings.h"
#include "navigation.h"


// ============== Configuration ==============


// TFT Display pins (ST7796U 3.5" IPS, SPI) — TFT_CS=18, TFT_DC=17, TFT_RST=16 are
// defined in platformio.ini build_flags under USER_SETUP_LOADED=1, which tells
// TFT_eSPI to skip its own header selection entirely. Not User_Setup.h (#184).
// FRAM_CS, SD_CS, SPI_*, TFT_BL, CTP_INT and the button pins live in fc_config.h (E4).

// Debounce time in ms
#define DEBOUNCE_MS 200


// Serial log to SD (#59)
#define LOG_FLUSH_INTERVAL    5000     // Flush SD buffer every 5 seconds (ms)
#define LOG_ROTATION_INTERVAL 3600000  // Check rotation hourly (ms)


// Watchdog configuration (auto-reset on hang)
#define WDT_TIMEOUT_SEC 30  // Reset if loop hangs for 30 seconds

// ============== Global State ==============
// Every peripheral object and every screen lives in its own unit now (E4, #212);
// what is left here belongs to setup, loop and the buttons.


// Button debounce
unsigned long lastButtonPress = 0;


// Periodic status logging
static unsigned long lastStatusLog = 0;
#define STATUS_LOG_INTERVAL 10000  // Log status every 10 seconds


// Battery logging to SD card
static unsigned long lastBattLog = 0;


// Button C long-press tracking
unsigned long buttonCPressStart = 0;
bool buttonCLongPressHandled = false;
#define LONG_PRESS_MS 800             // 800ms for long press


// ============== Setup ==============

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Boot banner - capture to ring buffer for web serial
  char banner[128];
  snprintf(banner, sizeof(banner), "=================================\nField Compass Dual %s\n=================================\n\n", FW_VERSION);
  logPrintf("%s", banner);
  logResetReason();   // panic / watchdog / brownout / power-on -- so a crash boot is attributable (#289)

#ifdef FC_LDO2_PIN
  // UM FeatherS3 only (#283): the second LDO feeds the vertical STEMMA QT
  // connector. Bring it up before anything touches I2C. See fc_config.h.
  pinMode(FC_LDO2_PIN, OUTPUT);
  digitalWrite(FC_LDO2_PIN, HIGH);
  logPrintf("[PWR] LDO2 enabled on GPIO %d\n", FC_LDO2_PIN);
#endif

  // Deselect ALL SPI slave CS pins BEFORE any SPI bus activity (#116)
  // Without this, SD_CS and FRAM_CS float during TFT init, allowing TFT
  // traffic to corrupt idle SPI slaves on the shared bus.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(FRAM_CS, OUTPUT);
  digitalWrite(FRAM_CS, HIGH);
  logPrintf("[SPI] CS pins pre-set HIGH: SD_CS=%d, FRAM_CS=%d\n", SD_CS, FRAM_CS);

  LOG_DEBUG("About to init TFT...");
  Serial.flush();

  // Initialize TFT first for visual feedback (TFT_eSPI handles SPI init)
  initTFT();

  LOG_DEBUG("TFT init done");
  Serial.flush();

  // Initialize LVGL (coexistence: runs alongside sprite pipeline)
  initLVGL();

  // Initialize I2C (I2C1: the variant's SDA/SCL = header + first STEMMA QT)
  Wire.begin();
#ifdef FC_I2C2_SDA
  // UM FeatherS3[D] only: the second STEMMA QT connector is its own bus.
  Wire1.begin(FC_I2C2_SDA, FC_I2C2_SCL);
  logPrintf("[I2C] I2C1 on SDA=%d SCL=%d, I2C2 on SDA=%d SCL=%d\n",
            SDA, SCL, FC_I2C2_SDA, FC_I2C2_SCL);
#endif

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

  // SD and FRAM share the bus TFT_eSPI already started (SCK/MISO/MOSI from
  // its build_flags, the same 36/37/35 on both boards). One SPIClass per
  // controller: with USE_FSPI_PORT (#284) TFT_eSPI owns its own SPIClass on
  // GPSPI2, and starting the Arduino SPI object on the same controller was a
  // second driver on one bus. Its instance is used everywhere instead.
  logPrintf("[SPI] Using TFT_eSPI's bus instance: SCK=%d MISO=%d MOSI=%d (SD_CS=%d FRAM_CS=%d)\n",
            SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS, FRAM_CS);
  initSD();
  initFRAM();   // SPI FRAM 256KB (shared bus with TFT/SD)
  initRTC();    // Adalogger RTC - sets system time if RTC has valid time
  initSerialLog(rtcAvailable);  // Serial log to SD (#59) - needs SD + RTC; RTC state passed in (#263)
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
  // Arduino core 3.x starts the task watchdog itself before setup() runs
  // (sdkconfig: CONFIG_ESP_TASK_WDT_INIT=1, CONFIG_ESP_TASK_WDT_TIMEOUT_S=5),
  // so esp_task_wdt_init() returns ESP_ERR_INVALID_STATE here and the 5 s
  // default silently stayed in force -- long enough for the WiFi reconnect
  // wait to trip it and reboot the board every ~70 s (#285). Reconfigure
  // the running watchdog instead, and never claim a timeout we did not get.
  esp_err_t wdtErr = esp_task_wdt_init(&wdt_config);
  if (wdtErr == ESP_ERR_INVALID_STATE) {
    wdtErr = esp_task_wdt_reconfigure(&wdt_config);
  }
  esp_err_t wdtAddErr = esp_task_wdt_add(NULL);  // Subscribe loopTask
  if (wdtErr == ESP_OK && wdtAddErr == ESP_OK) {
    logPrintf("Watchdog enabled: %ds timeout\n", WDT_TIMEOUT_SEC);
  } else {
    logPrintf("Watchdog: config %s, subscribe %s -- core default timeout stays in force\n",
              esp_err_to_name(wdtErr), esp_err_to_name(wdtAddErr));
  }

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

  // I2C health (#289): a device that stops answering is dropped after
  // I2C_FAIL_LIMIT consecutive failures (its reads above stop), re-probed
  // every I2C_REPROBE_MS, and re-initialised when it answers again.
  serviceIMU();
  serviceSHT41();
  serviceBME688();
  serviceBattery();
  serviceTouch();

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
    snprintf(buf, sizeof(buf), "[%lus] T:%.1fF H:%.0f%% IAQ:%.0f(%s) Hdg:%.0f %s Sat:%d HDOP:%.1f TTFF:%lus Batt:%.3fV/%.2f%%/%+.1f%%hr\n",
             millis() / 1000,
             logTempC * 9.0 / 5.0 + 32.0,
             logHumid,
             envData.iaq, getIaqQualityText(envData.iaq),
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
  } else if (tftSleeping && touchPollForWake()) {
    // LVGL is parked while the panel sleeps, and it is the only reader of the
    // touch chip, so without this a tap could never wake the TFT and only the
    // wing buttons could (#290). Same wake path the buttons use.
    logPrintln("[SLEEP] Woken by touch");
    wakeAllDisplays();
  }

  // Update display based on current screen
  updateDisplay();

  // Reset watchdog timer (proves loop is not hung)
  esp_task_wdt_reset();

  delay(5);  // 5ms RTOS yield — keeps touch/LVGL responsive (~120Hz) (#121)
}

// ============== I2C Scanner ==============

static int scanI2CBus(TwoWire &bus, const char *label);

void scanI2C() {
  int total = scanI2CBus(Wire, "I2C1 (Wire)");
#ifdef FC_I2C2_SDA
  total += scanI2CBus(Wire1, "I2C2 (Wire1)");
#endif
#if defined(ARDUINO_FEATHERS3)
  if (total == 0) {
    // Diagnostic (#283): devices are on the bus but nothing answered. One
    // boot log says whether the lines are held low, crossed, or too slow:
    // idle levels with the peripheral detached, a rescan with SDA/SCL
    // swapped, and a rescan at 10 kHz. Then the bus is restored.
    Wire.end();
    pinMode(SDA, INPUT_PULLUP);
    pinMode(SCL, INPUT_PULLUP);
    delay(5);
    logPrintf("[I2C] idle levels with I2C1 detached: SDA(GPIO%d)=%d SCL(GPIO%d)=%d  (1 = released)\n",
              SDA, digitalRead(SDA), SCL, digitalRead(SCL));
    Wire.begin(SCL, SDA);   // deliberately crossed
    scanI2CBus(Wire, "I2C1 with SDA/SCL SWAPPED (diagnostic)");
    Wire.end();
    Wire.begin();
    Wire.setClock(10000);
    scanI2CBus(Wire, "I2C1 at 10 kHz (diagnostic)");
    Wire.end();
    Wire.begin();           // back to the variant's SDA/SCL at the default clock
  }
#endif
  logPrintf("  Total devices: %d\n\n", total);
}

// One pass over 0x01-0x7E on the given bus; returns the device count.
static int scanI2CBus(TwoWire &bus, const char *label) {
  logPrintf("Scanning %s...\n", label);

  int deviceCount = 0;
  for (byte address = 1; address < 127; address++) {
    bus.beginTransmission(address);
    byte error = bus.endTransmission();

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
  logPrintf("  %s: %d device(s)\n", label, deviceCount);
  return deviceCount;
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
    // Scroll threshold: prevent tap jitter from triggering scroll. (#112)
    lv_indev_set_scroll_limit(lvglTouchIndev, 20);
    // Gesture threshold: default 50px is too high for 480px/3.5" screen.
    // FT6336U has hardware filtering — 10px safely above residual jitter. (#121)
    lv_indev_set_gesture_min_distance(lvglTouchIndev, 10);
    logPrintln("[LVGL] Touch indev created (scroll_limit=20, gesture_min=10)");
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
    if (SD.begin(SD_CS, TFT_eSPI::getSPIinstance(), SD_SPI_FREQ)) {
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
  if (currentScreen == SCREEN_SETTINGS) {
    // Settings: A/B scroll the active sub-screen content (#101)
    lv_obj_t* scroll = (settingsSubScreen >= 0 && settingsSubScreen < 7)
                       ? settingsScrollAreas[settingsSubScreen] : NULL;
    if (scroll) {
      int step = SETTINGS_SCROLL_STEP;
      if (buttonA) lv_obj_scroll_by(scroll, 0, step, LV_ANIM_ON);   // Scroll up
      if (buttonB) lv_obj_scroll_by(scroll, 0, -step, LV_ANIM_ON);  // Scroll down
    }
    lastButtonPress = now;
  } else if (currentScreen == SCREEN_GEOCACHE && geocacheSubScreen != 0) {
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
      // List screen: short press selects cache and returns to nav (#122: map through filter)
      if (gcFilteredCount > 0 && listHighlightIndex < gcFilteredCount)
        selectedCacheIndex = gcFilteredIndices[listHighlightIndex];
      geocacheSubScreen = 0;
    } else if (geocacheSubScreen == 2) {
      // Details screen: short press toggles found status (#122: map through filter)
      if (gcFilteredCount > 0 && listHighlightIndex < gcFilteredCount) {
        int ci = gcFilteredIndices[listHighlightIndex];
        cacheList[ci].found = !cacheList[ci].found;
        if (cacheList[ci].found) {
          cacheList[ci].foundTime = millis() / 1000;  // Simple timestamp
        }
        saveCacheFoundStatus();  // Persist to SD
        gcApplyFilters();        // Re-filter after found status change (#122)
      }
    } else if (geocacheSubScreen == 3) {
      // Filter screen: short press acts as Back (#122)
      gcApplyFilters();
      listHighlightIndex = 0;
      geocacheSubScreen = 1;
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

