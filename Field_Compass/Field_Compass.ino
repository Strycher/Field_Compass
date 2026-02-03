/*
 * Field Compass - Hardware Validation Sketch
 *
 * Tests communication with:
 * - Adafruit FeatherWing OLED 128x64 (I2C)
 * - Adafruit Ultimate GPS FeatherWing PA1616D (Serial)
 *
 * Hardware: Adafruit ESP32-S3 Feather 8MB
 *
 * Issue: #1
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// OLED FeatherWing configuration
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RESET -1  // No reset pin

// OLED FeatherWing button pins (active LOW) for ESP32-S3 Feather
#define BUTTON_A 5
#define BUTTON_B 6
#define BUTTON_C 9

// GPS Serial configuration
// ESP32-S3 Feather uses Serial1 for GPS FeatherWing
#define GPS_BAUD 9600

// Create display object - SH1107 is the actual controller on newer OLED FeatherWings
Adafruit_SH1107 display = Adafruit_SH1107(OLED_HEIGHT, OLED_WIDTH, &Wire);

// Buffer for GPS data
char gpsBuffer[128];
int gpsBufferIndex = 0;

void setup() {
  // Initialize USB Serial for debug output
  Serial.begin(115200);
  delay(1000);  // Give USB serial time to connect

  Serial.println("=================================");
  Serial.println("Field Compass - Hardware Validation");
  Serial.println("=================================");
  Serial.println();

  // Initialize I2C
  Wire.begin();

  // Scan I2C bus
  scanI2C();

  // Initialize OLED
  initOLED();

  // Initialize GPS Serial
  initGPS();

  // Setup button pins
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

  Serial.println();
  Serial.println("Setup complete. Monitoring GPS and buttons...");
  Serial.println();
}

void loop() {
  // Check for GPS data
  readGPS();

  // Check buttons and update display
  updateDisplay();

  delay(100);
}

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

      // Identify known devices
      if (address == 0x3C || address == 0x3D) {
        Serial.print(" (OLED Display)");
      }
      Serial.println();
      deviceCount++;
    }
  }

  if (deviceCount == 0) {
    Serial.println("  No I2C devices found!");
  } else {
    Serial.print("  Total devices: ");
    Serial.println(deviceCount);
  }
  Serial.println();
}

void initOLED() {
  Serial.println("Initializing OLED...");

  // Try default address 0x3C first
  if (!display.begin(0x3C, true)) {
    Serial.println("  OLED not found at 0x3C, trying 0x3D...");
    if (!display.begin(0x3D, true)) {
      Serial.println("  ERROR: OLED initialization failed!");
      return;
    }
  }

  Serial.println("  OLED initialized successfully");

  // Configure display
  display.setRotation(1);  // Horizontal orientation
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  // Show startup message
  display.setCursor(0, 0);
  display.println("Field Compass");
  display.println("Hardware Validation");
  display.println();
  display.println("Waiting for GPS...");
  display.display();
}

void initGPS() {
  Serial.println("Initializing GPS...");

  // Initialize Serial1 for GPS
  // ESP32-S3 Feather: RX=16, TX=17 for FeatherWing
  Serial1.begin(GPS_BAUD, SERIAL_8N1, 16, 17);

  Serial.println("  GPS serial initialized at 9600 baud");
  Serial.println("  Waiting for NMEA sentences...");
}

void readGPS() {
  while (Serial1.available()) {
    char c = Serial1.read();

    // Build line buffer
    if (c == '\n') {
      gpsBuffer[gpsBufferIndex] = '\0';

      // Only print GPRMC or GPGGA sentences (position data)
      if (strstr(gpsBuffer, "GPRMC") || strstr(gpsBuffer, "GPGGA") ||
          strstr(gpsBuffer, "GNRMC") || strstr(gpsBuffer, "GNGGA")) {
        Serial.print("GPS: ");
        Serial.println(gpsBuffer);
      }

      gpsBufferIndex = 0;
    } else if (c != '\r' && gpsBufferIndex < sizeof(gpsBuffer) - 1) {
      gpsBuffer[gpsBufferIndex++] = c;
    }
  }
}

void updateDisplay() {
  static unsigned long lastUpdate = 0;
  static bool buttonAPressed = false;
  static bool buttonBPressed = false;
  static bool buttonCPressed = false;

  // Read button states (active LOW)
  bool currentA = !digitalRead(BUTTON_A);
  bool currentB = !digitalRead(BUTTON_B);
  bool currentC = !digitalRead(BUTTON_C);

  // Log button presses
  if (currentA && !buttonAPressed) {
    Serial.println("Button A pressed");
  }
  if (currentB && !buttonBPressed) {
    Serial.println("Button B pressed");
  }
  if (currentC && !buttonCPressed) {
    Serial.println("Button C pressed");
  }

  buttonAPressed = currentA;
  buttonBPressed = currentB;
  buttonCPressed = currentC;

  // Update display every 500ms
  if (millis() - lastUpdate > 500) {
    lastUpdate = millis();

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Field Compass v0.1");
    display.println("------------------");

    // Show uptime
    unsigned long seconds = millis() / 1000;
    display.print("Uptime: ");
    display.print(seconds);
    display.println("s");

    // Show button states
    display.println();
    display.print("Buttons: ");
    display.print(currentA ? "A" : "-");
    display.print(currentB ? "B" : "-");
    display.println(currentC ? "C" : "-");

    // Show GPS status
    display.println();
    if (gpsBufferIndex > 0 || strlen(gpsBuffer) > 0) {
      display.println("GPS: Receiving");
    } else {
      display.println("GPS: Waiting...");
    }

    display.display();
  }
}
