# LVGL Environment + Telemetry Screens Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate `drawScreenEnv()` and `drawScreenTelemetry()` from legacy TFT_eSprite to LVGL labels with the established show/hide coexistence pattern.

**Architecture:** Two new root containers (`envScr`, `telemetryScr`) with static `lv_label` objects positioned absolutely. Data updates via `updateEnvData()` and `updateTelemetryData()` called from the existing LVGL routing block in `updateDisplay()`. Same show/hide pattern as compass (#109) and geocache (#110).

**Tech Stack:** LVGL 9.5.0, ESP32-S3, existing FC widget library (fcHeaderCreate, fcNavBarCreate), FC color/font defines.

---

### Task 1: Scaffold — Widget Handles + Root Containers

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:1863` (insert after geocache handles, before `buildCompassScreen()` at line 1920)
- Modify: `Field_Compass/Field_Compass.ino:3003` (insert after `buildGeocacheScreen()` call in `initLVGL()`)

**Step 1: Add static widget handles for Environment screen**

Insert after the geocache section (after line 1918), before `buildCompassScreen()`:

```cpp
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
```

**Step 2: Add static widget handles for Telemetry screen**

Insert immediately after Environment handles:

```cpp
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
```

**Step 3: Add `buildEnvScreen()` and `buildTelemetryScreen()` stubs**

Insert after `buildGeocacheScreen()` function (after the geocache screen builder ends, before `initLVGL`):

```cpp
// ============== LVGL Environment Screen Builder (#111) ==============

void buildEnvScreen() {
  // Root container — full screen, hidden by default
  envScr = lv_obj_create(lv_screen_active());
  lv_obj_set_size(envScr, 480, 320);
  lv_obj_set_pos(envScr, 0, 0);
  lv_obj_set_style_bg_color(envScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(envScr, 0, 0);
  lv_obj_set_style_radius(envScr, 0, 0);
  lv_obj_set_style_pad_all(envScr, 0, 0);
  lv_obj_add_flag(envScr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envScr, LV_OBJ_FLAG_SCROLLABLE);
}

// ============== LVGL Telemetry Screen Builder (#111) ==============

void buildTelemetryScreen() {
  // Root container — full screen, hidden by default
  telemetryScr = lv_obj_create(lv_screen_active());
  lv_obj_set_size(telemetryScr, 480, 320);
  lv_obj_set_pos(telemetryScr, 0, 0);
  lv_obj_set_style_bg_color(telemetryScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(telemetryScr, 0, 0);
  lv_obj_set_style_radius(telemetryScr, 0, 0);
  lv_obj_set_style_pad_all(telemetryScr, 0, 0);
  lv_obj_add_flag(telemetryScr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(telemetryScr, LV_OBJ_FLAG_SCROLLABLE);
}
```

**Step 4: Call builders from `initLVGL()`**

After `buildGeocacheScreen();` at line ~3003, add:

```cpp
  // Build LVGL environment screen (#111)
  buildEnvScreen();

  // Build LVGL telemetry screen (#111)
  buildTelemetryScreen();
```

**Step 5: Compile and verify**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Compiles with ~0-500 byte increase from baseline (empty containers).

**Step 6: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat(#111): Scaffold env + telemetry LVGL screens with handles and root containers"
```

---

### Task 2: Environment Screen — Labels

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — `buildEnvScreen()` function

**Step 1: Populate `buildEnvScreen()` with all labels**

Replace the empty `buildEnvScreen()` body (after the root container setup) with:

```cpp
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
```

**Step 2: Compile and verify**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Compiles, small flash increase from label objects.

**Step 3: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat(#111): Environment screen labels and layout"
```

---

### Task 3: Telemetry Screen — Labels

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — `buildTelemetryScreen()` function

**Step 1: Populate `buildTelemetryScreen()` with all labels**

Replace the empty `buildTelemetryScreen()` body (after root container setup) with:

```cpp
  // Header
  telHeader = fcHeaderCreate(telemetryScr, "TELEMETRY");

  // Column geometry
  int leftLabelX = 20;
  int leftValueX = 110;
  int rightLabelX = 250;
  int rightValueX = 350;
  int lineH = 28;

  // Helper lambda for label-value pair
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

  makeLabel(&telLblGpsSatCount, "Sats: 0", 60, 155, FC_FONT_LG, FC_COLOR_VALUE);
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
```

**Step 2: Compile and verify**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Compiles, moderate flash increase from ~28 label objects.

**Step 3: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat(#111): Telemetry screen labels, GPS states, and IMU layout"
```

---

### Task 4: `updateEnvData()` — Data Update Function

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — insert after `buildTelemetryScreen()`, before `initLVGL()`

**Step 1: Add `updateEnvData()` function**

```cpp
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

  // Temp value (respects useFahrenheit)
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
```

**Note on degree symbol:** Legacy uses `\xF7` (CP437 ÷ character hack for TFT_eSPI). LVGL uses UTF-8, so we use `\xC2\xB0` (proper Unicode degree sign °). The Montserrat font includes this glyph.

**Step 2: Compile and verify**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Compiles successfully.

**Step 3: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat(#111): updateEnvData() with color-coded thresholds"
```

---

### Task 5: `updateTelemetryData()` — Data Update Function

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — insert after `updateEnvData()`

**Step 1: Add helper to show/hide a batch of label pairs**

```cpp
// ============== LVGL Telemetry Data Update (#111) ==============

// Helper: show or hide GPS data row labels
static void telShowGpsDataRows(bool show) {
  lv_obj_flag_t op = show ? (lv_obj_flag_t)0 : LV_OBJ_FLAG_HIDDEN;
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
```

**Step 2: Add `updateTelemetryData()` function**

```cpp
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

    unsigned long elapsed = millis() / 1000;
    snprintf(buf, sizeof(buf), "Elapsed: %lum %lus", elapsed / 60, elapsed % 60);
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
```

**Step 2: Compile and verify**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Compiles successfully.

**Step 3: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat(#111): updateTelemetryData() with GPS states and IMU data"
```

---

### Task 6: Coexistence Wiring in `updateDisplay()`

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:6736-6761` (the existing LVGL show/hide and routing block)

**Step 1: Add show/hide blocks for env and telemetry**

After the geocache show/hide block (line ~6750), insert:

```cpp
    // Show/hide LVGL environment screen based on currentScreen (#111)
    if (envScr) {
      if (currentScreen == SCREEN_ENV)
        lv_obj_clear_flag(envScr, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(envScr, LV_OBJ_FLAG_HIDDEN);
    }

    // Show/hide LVGL telemetry screen based on currentScreen (#111)
    if (telemetryScr) {
      if (currentScreen == SCREEN_TELEMETRY)
        lv_obj_clear_flag(telemetryScr, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(telemetryScr, LV_OBJ_FLAG_HIDDEN);
    }
```

**Step 2: Expand the LVGL routing condition**

Change (line ~6753-6754):
```cpp
    if ((currentScreen == SCREEN_COMPASS && compassScr) ||
        (currentScreen == SCREEN_GEOCACHE && geocacheScr)) {
```

To:
```cpp
    if ((currentScreen == SCREEN_COMPASS && compassScr) ||
        (currentScreen == SCREEN_GEOCACHE && geocacheScr) ||
        (currentScreen == SCREEN_ENV && envScr) ||
        (currentScreen == SCREEN_TELEMETRY && telemetryScr)) {
```

**Step 3: Add update function calls inside the routing block**

After the geocache `else if` (line ~6760), add:

```cpp
      } else if (currentScreen == SCREEN_ENV) {
        updateEnvData();
      } else if (currentScreen == SCREEN_TELEMETRY) {
        updateTelemetryData();
      }
```

**Step 4: Compile and verify**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Compiles successfully. This is the first version that will actually render on device.

**Step 5: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat(#111): Coexistence wiring for env + telemetry in updateDisplay()"
```

---

### Task 7: Version Bump + Upload + Verify

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:28` — FW_VERSION

**Step 1: Bump version**

Change line 28:
```cpp
#define FW_VERSION "0.42.0"
```
To:
```cpp
#define FW_VERSION "0.43.0"
```

**Step 2: Compile final**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Compiles. Note flash/SRAM sizes for comparison.

**Step 3: Upload to device**

Run: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 Field_Compass/`
Expected: Upload succeeds.

**Step 4: Verify stability via serial**

Monitor serial for 30+ seconds. Check for:
- Zero LVGL errors/assertions
- Zero reboots
- Navigate to Environment screen (button B) — verify labels render
- Navigate to Telemetry screen — verify GPS/IMU data renders
- Navigate back to Compass — verify it still works

**Step 5: Commit + Tag + Push**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.43.0 — LVGL environment + telemetry screen migration (#111)"
git tag -a v0.43.0 -m "LVGL environment + telemetry screens (#111)"
git push origin main --tags
```

**Step 6: Close issue**

```bash
gh issue close 111 --comment "Completed in v0.43.0. Both screens migrated to LVGL with label-value pairs, color-coded thresholds, GPS state handling, and unit preference support."
```

**Step 7: Update MEMORY.md**

Update version to v0.43.0, add section for #111 completion with flash/SRAM sizes.
