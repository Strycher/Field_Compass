/*
 * Field Compass - Multi-Screen Firmware
 *
 * Hardware:
 * - Adafruit ESP32-S3 Feather 8MB
 * - Adafruit FeatherWing OLED 128x64 (I2C)
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
 *
 * Issues: #2-#25
 */

#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <Adafruit_GFX.h>
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

// NTP configuration
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = -18000;  // EST = UTC-5
const int DAYLIGHT_OFFSET_SEC = 3600; // DST +1 hour

// OLED FeatherWing button pins (active LOW) for ESP32-S3 Feather
// ESP32-S3 uses same mapping as M0/M4/nRF52840
#define BUTTON_A 9
#define BUTTON_B 6
#define BUTTON_C 5

// GPS Serial configuration
// ESP32-S3 Feather: Use hardware RX/TX pins on Feather header
#define GPS_BAUD 9600
// Using Arduino pin definitions - RX and TX are defined in board variant
#define GPS_RX RX
#define GPS_TX TX

// Screen count
#define NUM_SCREENS 4
#define SCREEN_OPS 0
#define SCREEN_GPS 1
#define SCREEN_ENV 2
#define SCREEN_IMU 3

// Debounce time in ms
#define DEBOUNCE_MS 200

// Display sleep timeout (ms) - 3 minutes for OLED (high burn-in risk)
#define DISPLAY_SLEEP_TIMEOUT 180000

// ============== Global Objects ==============

// Display
Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);

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

// Display sleep state
bool displaySleeping = false;
unsigned long lastActivityTime = 0;

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
  Serial.println("Field Compass v0.3");
  Serial.println("=================================\n");

  // Initialize I2C
  Wire.begin();

  // Scan I2C bus
  scanI2C();

  // Initialize all hardware
  initDisplay();
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
}

// ============== Main Loop ==============

void loop() {
  // Handle button navigation
  handleButtons();

  // Check display sleep timeout
  checkDisplaySleep();

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

void initDisplay() {
  Serial.print("Initializing OLED... ");

  if (!display.begin(0x3C, true)) {
    if (!display.begin(0x3D, true)) {
      Serial.println("FAILED!");
      return;
    }
  }

  display.setRotation(1);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Field Compass v0.3");
  display.println("Initializing...");
  display.display();

  Serial.println("OK");
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

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Field Compass v0.3");
  display.println();
  display.println("Connecting to WiFi...");
  display.display();

  // Try first network
  WiFi.begin(WIFI_SSID_1, WIFI_PASS_1);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  // Try second network if first failed
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print(" trying 5G");
    WiFi.begin(WIFI_SSID_2, WIFI_PASS_2);
    attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
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
}

// ============== Display Sleep Functions ==============

void sleepDisplay() {
  if (displaySleeping) return;

  displaySleeping = true;
  display.oled_command(SH110X_DISPLAYOFF);
  Serial.println("Display sleeping");
}

void wakeDisplay() {
  if (!displaySleeping) return;

  displaySleeping = false;
  display.oled_command(SH110X_DISPLAYON);
  lastActivityTime = millis();
  Serial.println("Display woke up");
}

void checkDisplaySleep() {
  // Don't sleep if already sleeping
  if (displaySleeping) return;

  // Check if timeout exceeded
  if (millis() - lastActivityTime > DISPLAY_SLEEP_TIMEOUT) {
    sleepDisplay();
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

  // If display is sleeping, wake it and consume the button press
  if (displaySleeping) {
    wakeDisplay();
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
    Serial.print("Screen: ");
    Serial.println(currentScreen + 1);
  }

  // Button B - Next screen
  if (buttonB) {
    currentScreen++;
    if (currentScreen >= NUM_SCREENS) currentScreen = 0;
    lastButtonPress = now;
    Serial.print("Screen: ");
    Serial.println(currentScreen + 1);
  }

  // Button C - Reserved
  if (buttonC) {
    lastButtonPress = now;
    // No action
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
  // Debug: print RMC and GGA sentences to see fix status
  if (strstr(sentence, "RMC") || strstr(sentence, "GGA")) {
    Serial.print("GPS: ");
    Serial.println(sentence);
  }

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
            // Time is valid even without position fix (from GPS clock/backup battery)
            gpsData.timeValid = true;
          }
          break;
        case 2:  // Status A=valid position, V=no position fix
          status = token[0];
          break;
        case 3:  // Latitude DDMM.MMMM
          lat = atof(token);
          break;
        case 4:  // N/S
          latDir = token[0];
          break;
        case 5:  // Longitude DDDMM.MMMM
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
      // Convert DDMM.MMMM to decimal degrees
      int latDeg = (int)(lat / 100);
      float latMin = lat - (latDeg * 100);
      gpsData.latitude = latDeg + (latMin / 60.0);
      if (latDir == 'S') gpsData.latitude = -gpsData.latitude;

      int lonDeg = (int)(lon / 100);
      float lonMin = lon - (lonDeg * 100);
      gpsData.longitude = lonDeg + (lonMin / 60.0);
      if (lonDir == 'W') gpsData.longitude = -gpsData.longitude;

      gpsData.valid = true;
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
    envData.pressure = bme.pressure / 100.0;  // Convert to hPa
    envData.gasResistance = bme.gas_resistance / 1000.0;  // Convert to kOhm
  }
}

void readIMU() {
  sensors_event_t accel, gyro, temp, mag;

  lsm.getEvent(&accel, &gyro, &temp);
  lis.getEvent(&mag);

  // Store raw accel
  imuData.accelX = accel.acceleration.x;
  imuData.accelY = accel.acceleration.y;
  imuData.accelZ = accel.acceleration.z;

  // Calculate linear acceleration magnitude (subtract ~9.8 for gravity)
  imuData.accelMag = sqrt(imuData.accelX * imuData.accelX +
                          imuData.accelY * imuData.accelY +
                          imuData.accelZ * imuData.accelZ) - 9.8;
  if (imuData.accelMag < 0) imuData.accelMag = 0;

  // Calculate roll and pitch from accelerometer
  imuData.roll = atan2(imuData.accelY, imuData.accelZ) * 180.0 / PI;
  imuData.pitch = atan2(-imuData.accelX,
                        sqrt(imuData.accelY * imuData.accelY +
                             imuData.accelZ * imuData.accelZ)) * 180.0 / PI;

  // Calculate compass heading from magnetometer
  float magX = mag.magnetic.x;
  float magY = mag.magnetic.y;

  imuData.heading = atan2(magY, magX) * 180.0 / PI;
  if (imuData.heading < 0) imuData.heading += 360;
}

// ============== Display Functions ==============

void updateDisplay() {
  static unsigned long lastUpdate = 0;

  // Don't update display when sleeping
  if (displaySleeping) return;

  // Update every 250ms
  if (millis() - lastUpdate < 250) return;
  lastUpdate = millis();

  display.clearDisplay();
  display.setCursor(0, 0);

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
  display.setCursor(0, 57);
  display.print("[");
  for (int i = 0; i < NUM_SCREENS; i++) {
    display.print(i == currentScreen ? (char)('1' + i) : '-');
  }
  display.print("] A<  B>");

  display.display();
}

void drawScreenOps() {
  display.println("== OPERATIONAL ==");
  display.println();

  // Time
  display.print("Time: ");
  if (gpsData.timeValid) {
    // Apply timezone offset to GPS time (UTC)
    int hour = gpsData.hour + (GMT_OFFSET_SEC / 3600);
    if (hour < 0) hour += 24;
    if (hour >= 24) hour -= 24;
    if (hour < 10) display.print("0");
    display.print(hour);
    display.print(":");
    if (gpsData.minute < 10) display.print("0");
    display.print(gpsData.minute);
    display.print(":");
    if (gpsData.second < 10) display.print("0");
    display.print(gpsData.second);
    display.println(" GPS");
  } else if (ntpSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[12];
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
      display.print(timeStr);
      display.println(" NTP");
    }
  } else {
    display.println("--:--:-- N/A");
  }

  // Uptime
  unsigned long totalSec = millis() / 1000;
  int days = totalSec / 86400;
  int hours = (totalSec % 86400) / 3600;
  int mins = (totalSec % 3600) / 60;
  int secs = totalSec % 60;

  display.print("Up: ");
  if (days > 0) {
    display.print(days);
    display.print("d ");
  }
  if (hours < 10) display.print("0");
  display.print(hours);
  display.print(":");
  if (mins < 10) display.print("0");
  display.print(mins);
  display.print(":");
  if (secs < 10) display.print("0");
  display.println(secs);

  // WiFi
  display.print("WiFi: ");
  if (wifiConnected) {
    display.println(WiFi.SSID());
  } else {
    display.println("Disconnected");
  }

  // Battery
  display.print("Batt: ");
  if (batteryAvailable) {
    float pct = battery.cellPercent();
    display.print(pct, 0);
    display.print("% (");
    display.print(battery.cellVoltage(), 2);
    display.println("V)");
  } else {
    display.println("N/A");
  }
}

void drawScreenGPS() {
  display.println("== GPS ==");
  display.println();

  if (gpsData.valid) {
    display.print("Lat:  ");
    display.print(abs(gpsData.latitude), 6);
    display.println(gpsData.latitude >= 0 ? " N" : " S");

    display.print("Lon: ");
    display.print(abs(gpsData.longitude), 6);
    display.println(gpsData.longitude >= 0 ? " E" : " W");

    display.print("Alt:  ");
    display.print(gpsData.altitude, 1);
    display.println(" m");
  } else if (gpsData.receiving) {
    display.println("Acquiring fix...");
    display.println();
    display.println("(Need sky view)");
  } else {
    display.println("No GPS data");
    display.println();
    display.println("Check connection");
  }
}

void drawScreenEnv() {
  display.println("== ENVIRONMENT ==");
  display.println();

  if (bmeAvailable) {
    // Temperature in F and C
    float tempF = envData.temperature * 9.0 / 5.0 + 32.0;
    display.print("Temp: ");
    display.print(tempF, 1);
    display.print("F (");
    display.print(envData.temperature, 1);
    display.println("C)");

    display.print("Hum:  ");
    display.print(envData.humidity, 1);
    display.println("%");

    display.print("Pres: ");
    display.print(envData.pressure, 1);
    display.println(" hPa");

    display.print("Gas:  ");
    display.print(envData.gasResistance, 1);
    display.println(" kOhm");
  } else {
    display.println("BME688 not found");
  }
}

void drawScreenIMU() {
  display.println("== IMU/COMPASS ==");
  display.println();

  if (imuAvailable && magAvailable) {
    // Compass heading with cardinal
    display.print("Hdg: ");
    display.print(imuData.heading, 0);
    display.print(" ");
    display.println(getCardinal(imuData.heading));

    // Orientation
    display.print("Roll:  ");
    display.print(imuData.roll, 0);
    display.println(" deg");

    display.print("Pitch: ");
    display.print(imuData.pitch, 0);
    display.println(" deg");

    // Linear acceleration
    display.print("Accel: ");
    display.print(imuData.accelMag, 2);
    display.println(" m/s2");
  } else {
    display.println("IMU not available");
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
