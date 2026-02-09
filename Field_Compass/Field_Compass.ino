/*
 * Field Compass - Dual Display Firmware
 *
 * Hardware:
 * - Adafruit ESP32-S3 Feather 8MB w.FL
 * - Adafruit SH1107 OLED FeatherWing 128x64 (I2C)
 * - Adafruit ST7789 2.0" TFT 320x240 (SPI via EYESPI breakout)
 * - Adafruit Ultimate GPS FeatherWing PA1616D (Serial)
 * - Adafruit BME688 (I2C - STEMMA QT)
 * - Adafruit LSM6DSOX + LIS3MDL 9-DoF IMU (I2C - STEMMA QT)
 *
 * Screens:
 * 1. Operational Info (time, uptime, WiFi, battery)
 * 2. GPS Info (coordinates, altitude, address)
 * 3. BME688 Environmental (temp, humidity, pressure, air quality)
 * 4. IMU/Compass (heading, orientation, acceleration)
 *
 * Navigation: Button A = prev screen, Button B = next screen
 * Display Sleep: OLED 3 min, TFT 15 min (button press wakes)
 *
 * Issues: #44, #46
 */

#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_BME680.h>
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_LIS3MDL.h>
#include <Adafruit_MAX1704X.h>

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
#define TFT_RST   12  // A2 -> RST
#define TOUCH_CS  14  // A3 -> TSCS (for future use)

// Button pins (directly wired, active LOW)
#define BUTTON_A 9
#define BUTTON_B 6
#define BUTTON_C 5

// GPS Serial configuration
#define GPS_BAUD 9600
#define GPS_RX RX
#define GPS_TX TX

// Screen settings
#define NUM_SCREENS 4
#define SCREEN_OPS 0
#define SCREEN_GPS 1
#define SCREEN_ENV 2
#define SCREEN_IMU 3

// TFT dimensions
#define TFT_WIDTH 320
#define TFT_HEIGHT 240

// Debounce time in ms
#define DEBOUNCE_MS 200

// WiFi reconnect interval (ms)
#define WIFI_RECONNECT_INTERVAL 30000

// Display sleep timeouts (ms)
#define TFT_SLEEP_TIMEOUT  900000   // 15 minutes for TFT (low burn-in risk)
#define OLED_SLEEP_TIMEOUT 180000   // 3 minutes for OLED (high burn-in risk)

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
Adafruit_BME680 bme;
Adafruit_LSM6DSOX lsm;
Adafruit_LIS3MDL lis;
Adafruit_MAX17048 battery;

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

// OLED availability
bool oledAvailable = false;

// Sensor availability flags
bool bmeAvailable = false;
bool imuAvailable = false;
bool magAvailable = false;
bool batteryAvailable = false;
bool wifiConnected = false;
bool ntpSynced = false;

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

// BME688 data
struct {
  float temperature = 0;
  float humidity = 0;
  float pressure = 0;
  float gasResistance = 0;
} envData;

// ============== Setup ==============

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=================================");
  Serial.println("Field Compass Dual v0.5");
  Serial.println("=================================\n");

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
  tft.println("v0.5 Dual");
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
  initWiFi();

  // Setup buttons
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

  Serial.println("\nSetup complete!\n");

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

  // Update sensor data (even when display sleeping)
  readGPS();
  if (bmeAvailable) readBME688();
  if (imuAvailable && magAvailable) readIMU();

  // Update display based on current screen
  updateDisplay();

  delay(50);
}

// ============== I2C Scanner ==============

void scanI2C() {
  Serial.println("Scanning I2C bus...");

  int deviceCount = 0;
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("  Found device at 0x");
      if (address < 16) Serial.print("0");
      Serial.print(address, HEX);

      if (address == 0x3C || address == 0x3D) {
        Serial.print(" (OLED Display)");
      } else if (address == 0x76 || address == 0x77) {
        Serial.print(" (BME688)");
      } else if (address == 0x6A || address == 0x6B) {
        Serial.print(" (LSM6DSOX - Accel/Gyro)");
      } else if (address == 0x1C || address == 0x1E) {
        Serial.print(" (LIS3MDL - Magnetometer)");
      } else if (address == 0x36) {
        Serial.print(" (MAX17048 - Battery Gauge)");
      }
      Serial.println();
      deviceCount++;
    }
  }
  Serial.print("  Total devices: ");
  Serial.println(deviceCount);
  Serial.println();
}

// ============== Initialization Functions ==============

void initTFT() {
  Serial.print("Initializing ST7789 TFT... ");

  tft.init(TFT_HEIGHT, TFT_WIDTH);  // 240x320, but we use landscape
  tft.setRotation(1);  // Landscape mode
  tft.fillScreen(COLOR_BG);

  Serial.println("OK (320x240)");
}

void initOLED() {
  Serial.print("Initializing OLED... ");

  if (!oled.begin(0x3C, true)) {
    if (!oled.begin(0x3D, true)) {
      Serial.println("NOT FOUND");
      return;
    }
  }

  oled.setRotation(1);
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.display();

  oledAvailable = true;
  Serial.println("OK (128x64)");
}

void initGPS() {
  Serial.print("Initializing GPS on RX=");
  Serial.print(GPS_RX);
  Serial.print(", TX=");
  Serial.print(GPS_TX);
  Serial.print("... ");
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("OK (9600 baud)");
}

void initBME688() {
  Serial.print("Initializing BME688... ");

  if (!bme.begin(0x77)) {
    if (!bme.begin(0x76)) {
      Serial.println("NOT FOUND");
      return;
    }
  }

  // Configure oversampling and filter
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);  // 320°C for 150ms

  bmeAvailable = true;
  Serial.println("OK");
}

void initIMU() {
  Serial.print("Initializing LSM6DSOX... ");

  if (!lsm.begin_I2C(0x6A)) {
    if (!lsm.begin_I2C(0x6B)) {
      Serial.println("NOT FOUND");
      return;
    }
  }

  lsm.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
  lsm.setGyroRange(LSM6DS_GYRO_RANGE_500_DPS);
  lsm.setAccelDataRate(LSM6DS_RATE_104_HZ);
  lsm.setGyroDataRate(LSM6DS_RATE_104_HZ);

  imuAvailable = true;
  Serial.println("OK");

  Serial.print("Initializing LIS3MDL... ");

  if (!lis.begin_I2C(0x1C)) {
    if (!lis.begin_I2C(0x1E)) {
      Serial.println("NOT FOUND");
      return;
    }
  }

  lis.setPerformanceMode(LIS3MDL_MEDIUMMODE);
  lis.setOperationMode(LIS3MDL_CONTINUOUSMODE);
  lis.setDataRate(LIS3MDL_DATARATE_155_HZ);
  lis.setRange(LIS3MDL_RANGE_4_GAUSS);

  magAvailable = true;
  Serial.println("OK");
}

void initBattery() {
  Serial.print("Initializing MAX17048... ");

  if (!battery.begin()) {
    Serial.println("NOT FOUND");
    return;
  }

  batteryAvailable = true;
  Serial.println("OK");
}

void initWiFi() {
  Serial.print("Connecting to WiFi");

  // Try networks in order
  const char* ssids[] = {WIFI_SSID_1, WIFI_SSID_2, WIFI_SSID_3};
  const char* passwords[] = {WIFI_PASS_1, WIFI_PASS_2, WIFI_PASS_3};

  for (int net = 0; net < 3; net++) {
    Serial.print(" [");
    Serial.print(ssids[net]);
    Serial.print("]");

    WiFi.begin(ssids[net], passwords[net]);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 15) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) break;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println(" OK");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());

    // Sync NTP time
    Serial.print("Syncing NTP time... ");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      ntpSynced = true;
      Serial.println("OK");
    } else {
      Serial.println("FAILED");
    }
  } else {
    Serial.println(" FAILED");
  }

  lastWiFiAttempt = millis();
}

void checkWiFi() {
  // Update connection status
  wifiConnected = (WiFi.status() == WL_CONNECTED);

  // Attempt reconnect if disconnected
  if (!wifiConnected && (millis() - lastWiFiAttempt > WIFI_RECONNECT_INTERVAL)) {
    Serial.println("WiFi disconnected, attempting reconnect...");
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
      Serial.println("WiFi reconnected!");
    }
  }
}

// ============== Display Sleep Functions ==============

void sleepTFT() {
  if (tftSleeping) return;

  tftSleeping = true;
  tft.enableSleep(true);
  Serial.println("TFT sleeping");
}

void wakeTFT() {
  if (!tftSleeping) return;

  tftSleeping = false;
  tft.enableSleep(false);
  tft.fillScreen(COLOR_BG);  // Clear screen on wake
  Serial.println("TFT woke up");
}

void sleepOLED() {
  if (oledSleeping || !oledAvailable) return;

  oledSleeping = true;
  oled.oled_command(SH110X_DISPLAYOFF);
  Serial.println("OLED sleeping");
}

void wakeOLED() {
  if (!oledSleeping || !oledAvailable) return;

  oledSleeping = false;
  oled.oled_command(SH110X_DISPLAYON);
  Serial.println("OLED woke up");
}

void wakeAllDisplays() {
  lastActivityTime = millis();
  wakeTFT();
  wakeOLED();
}

void checkDisplaySleep() {
  unsigned long elapsed = millis() - lastActivityTime;

  // Check OLED sleep (3 minutes)
  if (!oledSleeping && oledAvailable && elapsed > OLED_SLEEP_TIMEOUT) {
    sleepOLED();
  }

  // Check TFT sleep (15 minutes)
  if (!tftSleeping && elapsed > TFT_SLEEP_TIMEOUT) {
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
    Serial.print("Screen: ");
    Serial.println(currentScreen + 1);
  }

  // Button B - Next screen
  if (buttonB) {
    currentScreen++;
    if (currentScreen >= NUM_SCREENS) currentScreen = 0;
    lastButtonPress = now;
    tft.fillScreen(COLOR_BG);  // Clear screen on change
    Serial.print("Screen: ");
    Serial.println(currentScreen + 1);
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

    if (c == '\n') {
      gpsBuffer[gpsBufferIndex] = '\0';
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
    } else {
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
  if (bme.performReading()) {
    envData.temperature = bme.temperature;
    envData.humidity = bme.humidity;
    envData.pressure = bme.pressure / 100.0;
    envData.gasResistance = bme.gas_resistance / 1000.0;
  }
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
    }

    // Draw screen indicator at bottom
    drawNavBar();
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

  // Screen indicators
  int startX = 80;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (i == currentScreen) {
      tft.fillRect(startX + i * 40, y + 3, 30, 19, COLOR_HEADER);
      tft.setTextColor(COLOR_BG);
    } else {
      tft.setTextColor(COLOR_DIM);
    }
    tft.setCursor(startX + 8 + i * 40, y + 5);
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
  if (batteryAvailable) {
    float pct = battery.cellPercent();
    float volt = battery.cellVoltage();
    sprintf(buf, "%.0f%% (%.2fV)", pct, volt);
    uint16_t color = (pct > 20) ? COLOR_VALUE : COLOR_ERROR;
    drawValue(valueX, y, buf, color);
  } else {
    drawValue(valueX, y, "N/A", COLOR_DIM);
  }
}

void drawScreenGPS() {
  drawHeader("GPS");

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

    // Status
    drawLabel(labelX, y, "Status:");
    drawValue(valueX, y, "Fix OK", COLOR_VALUE);

  } else if (gpsData.receiving) {
    tft.setTextColor(COLOR_WARN);
    tft.setTextSize(2);
    tft.setCursor(60, 80);
    tft.println("Acquiring fix...");
    tft.setCursor(60, 120);
    tft.setTextColor(COLOR_DIM);
    tft.println("Need clear sky view");

    if (gpsData.timeValid) {
      tft.setCursor(60, 160);
      tft.setTextColor(COLOR_VALUE);
      tft.print("Time: ");
      int hour = gpsData.hour + (GMT_OFFSET_SEC / 3600);
      if (hour < 0) hour += 24;
      if (hour >= 24) hour -= 24;
      sprintf(buf, "%02d:%02d:%02d", hour, gpsData.minute, gpsData.second);
      tft.print(buf);
    }
  } else {
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

  int y = 50;
  int labelX = 20;
  int valueX = 120;
  int lineH = 35;
  char buf[32];

  if (bmeAvailable) {
    // Temperature
    float tempF = envData.temperature * 9.0 / 5.0 + 32.0;
    drawLabel(labelX, y, "Temp:");
    sprintf(buf, "%.1fF (%.1fC)", tempF, envData.temperature);
    drawValue(valueX, y, buf);
    y += lineH;

    // Humidity
    drawLabel(labelX, y, "Humid:");
    sprintf(buf, "%.1f%%", envData.humidity);
    drawValue(valueX, y, buf);
    y += lineH;

    // Pressure
    drawLabel(labelX, y, "Press:");
    sprintf(buf, "%.1f hPa", envData.pressure);
    drawValue(valueX, y, buf);
    y += lineH;

    // Gas/Air Quality
    drawLabel(labelX, y, "Gas:");
    sprintf(buf, "%.1f kOhm", envData.gasResistance);
    // Color based on resistance (higher = better)
    uint16_t color = COLOR_VALUE;
    if (envData.gasResistance < 50) color = COLOR_ERROR;
    else if (envData.gasResistance < 100) color = COLOR_WARN;
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
  if (batteryAvailable) {
    sprintf(buf, "Batt: %.0f%%", battery.cellPercent());
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

    oled.setCursor(0, 12);
    sprintf(buf, "Temp: %.1fF", tempF);
    oled.print(buf);

    oled.setCursor(0, 24);
    sprintf(buf, "Humid: %.1f%%", envData.humidity);
    oled.print(buf);

    oled.setCursor(0, 36);
    sprintf(buf, "Press: %.0fhPa", envData.pressure);
    oled.print(buf);

    oled.setCursor(0, 48);
    sprintf(buf, "Gas: %.0fkOhm", envData.gasResistance);
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

void drawOLEDNavBar() {
  // Draw screen number indicator at bottom right
  oled.setCursor(100, 56);
  oled.setTextSize(1);
  char buf[8];
  sprintf(buf, "[%d/4]", currentScreen + 1);
  oled.print(buf);
}
