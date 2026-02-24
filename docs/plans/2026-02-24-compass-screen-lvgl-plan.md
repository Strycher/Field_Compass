# Compass Screen LVGL Migration — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the legacy TFT_eSprite compass screen with LVGL-native rendering, featuring anti-aliased compass rose and label-based data panel.

**Architecture:** Custom draw event (`LV_EVENT_DRAW_MAIN`) on a plain `lv_obj_t` for the compass rose; `lv_label` column for the left data panel. Coexistence wiring skips the legacy `drawScreenCompass()` when LVGL is active.

**Tech Stack:** LVGL 9.5.0, ESP32-S3, `lv_draw_arc/triangle/line` primitives, FC widget library (#108)

---

### Task 1: Coexistence Wiring + Left Panel Labels (v0.40.1)

**Goal:** Enable `lv_timer_handler()` in production, build compass screen container with all 9 data labels, wire screen switching to show/hide LVGL objects.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`
  - Line 63: `LVGL_TEST_MODE` stays 0 (production)
  - Line 1315-1320: Remove `#if LVGL_TEST_MODE` guard around `lv_timer_handler()`
  - Line 1835: Add compass screen builder after widget library section
  - Line 5654-5660: Modify switch statement to skip legacy compass draw
  - Line 28: Bump `FW_VERSION` to `"0.40.1"`

**Step 1: Enable lv_timer_handler() in production**

At line 1315-1320 in `loop()`, replace the `#if LVGL_TEST_MODE` guard:

```cpp
// BEFORE:
  // LVGL timer handler — process animations, redraws, timers
  #if LVGL_TEST_MODE
  if (lvglAvailable && !tftSleeping) {
    lv_timer_handler();
  }
  #endif

// AFTER:
  // LVGL timer handler — process animations, redraws, timers (#109)
  if (lvglAvailable && !tftSleeping) {
    lv_timer_handler();
  }
```

**Step 2: Add compass screen globals and builder**

After the FC Widget Library section (after line 1834, before `initLVGL()`), add:

```cpp
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

// Compass rose draw callback (forward declare — implemented in Task 2)
static void compassRoseDrawCb(lv_event_t* e);

// Last drawn heading for invalidation threshold
static float compassLastHeading = -999.0f;

void buildCompassScreen() {
  // Root container — full screen, no scrolling, black background
  compassScr = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(compassScr);
  lv_obj_set_size(compassScr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(compassScr, 0, 0);
  lv_obj_set_style_bg_color(compassScr, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(compassScr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(compassScr, LV_OBJ_FLAG_SCROLLABLE);

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

  // === Right Panel: Compass Rose placeholder ===
  // (Custom draw added in Task 2 — for now, just an empty container)
  compassRoseObj = lv_obj_create(compassScr);
  lv_obj_remove_style_all(compassRoseObj);
  lv_obj_set_size(compassRoseObj, 298, 260);
  lv_obj_set_pos(compassRoseObj, 182, 30);
  lv_obj_clear_flag(compassRoseObj, LV_OBJ_FLAG_SCROLLABLE);

  // NavBar at bottom
  compassNavBar = fcNavBarCreate(compassScr, NUM_SCREENS, SCREEN_COMPASS);

  // Start hidden — shown when currentScreen == SCREEN_COMPASS
  lv_obj_add_flag(compassScr, LV_OBJ_FLAG_HIDDEN);

  logPrintln("[LVGL] Compass screen built (#109)");
}

// Update left panel labels from sensor data (called from updateDisplay at 2Hz)
void updateCompassData() {
  char buf[64];

  // Heading + cardinal
  if (imuAvailable && magAvailable) {
    const char* card = getCardinal(imuData.heading);
    lv_label_set_text_fmt(compassLblHdg, "%.0f\xC2\xB0 %s", imuData.heading, card);
    lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_VALUE, 0);
  } else {
    lv_label_set_text(compassLblHdg, "No IMU");
    lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_ERROR, 0);
  }

  // GPS coordinates
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

  // Altitude
  if (gpsData.valid) {
    float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
    lv_label_set_text_fmt(compassLblAlt, "Alt %.0f %s",
      alt, useMetricUnits ? "m" : "ft");
  } else {
    lv_label_set_text(compassLblAlt, "Alt --");
  }

  // Speed
  if (gpsData.valid) {
    float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
    lv_label_set_text_fmt(compassLblSpd, "Spd %.1f %s",
      speed, useMetricUnits ? "km/h" : "mph");
  } else {
    lv_label_set_text_fmt(compassLblSpd, "Spd -- %s",
      useMetricUnits ? "km/h" : "mph");
  }

  // Temperature
  bool hasTempSensor = shtAvailable || bmeAvailable;
  if (hasTempSensor) {
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempDisplay = useFahrenheit ? tempC * 9.0 / 5.0 + 32.0 : tempC;
    lv_label_set_text_fmt(compassLblTemp, "Temp %.1f\xC2\xB0%c",
      tempDisplay, useFahrenheit ? 'F' : 'C');
  } else {
    lv_label_set_text(compassLblTemp, "Temp --");
  }

  // Forecast with color coding
  const char* fc = weatherTrend.forecast;
  lv_color_t fcstColor = FC_COLOR_VALUE;
  if (strstr(fc, "Storm")) fcstColor = FC_COLOR_ERROR;
  else if (strstr(fc, "Rain") || strstr(fc, "Snow") ||
           strstr(fc, "Unsettled") || strstr(fc, "Precip")) fcstColor = FC_COLOR_WARN;
  else if (strcmp(fc, "Init") == 0 || strcmp(fc, "Learning") == 0 ||
           strcmp(fc, "Traveled") == 0) fcstColor = FC_COLOR_DIM;
  lv_obj_set_style_text_color(compassLblFcst, fcstColor, 0);
  lv_label_set_text_fmt(compassLblFcst, "Fcst %s %s", getTrendArrow(), fc);

  // GPS status
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

  // Time
  char timeBuf[16];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    formatTimeStr(timeBuf, timeinfo.tm_hour, timeinfo.tm_min,
                  timeinfo.tm_sec, false);
  } else {
    strcpy(timeBuf, "--:--");
  }
  lv_label_set_text(compassLblTime, timeBuf);

  // Invalidate compass rose if heading changed >2°
  if (imuAvailable && magAvailable) {
    float diff = fabs(imuData.heading - compassLastHeading);
    if (diff > 180) diff = 360 - diff;  // Wrap-around
    if (diff >= 2.0f) {
      compassLastHeading = imuData.heading;
      lv_obj_invalidate(compassRoseObj);
    }
  }
}
```

**Step 3: Call buildCompassScreen() from initLVGL()**

After `initFCTheme()` (line 1908) and before the `#if LVGL_TEST_MODE` block (line 1911), add:

```cpp
  // Build LVGL compass screen (#109)
  if (lvglAvailable) {
    buildCompassScreen();
  }
```

**Step 4: Wire screen switching in updateDisplay()**

At line 5639 in `updateDisplay()`, wrap the TFT rendering block to skip when compass is LVGL-managed:

```cpp
// BEFORE (line 5639-5663):
  if (!tftSleeping) {
    TFT_eSprite* c = &spr;
    // ... screen change clearing ...
    switch (currentScreen) {
      case SCREEN_COMPASS:   drawScreenCompass(c);   break;
      // ... other cases ...
    }
    zonePushDirty();
    // ...
  }

// AFTER:
  if (!tftSleeping) {
    // Show/hide LVGL compass screen based on currentScreen
    if (compassScr) {
      if (currentScreen == SCREEN_COMPASS) {
        lv_obj_clear_flag(compassScr, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(compassScr, LV_OBJ_FLAG_HIDDEN);
      }
    }

    // LVGL-managed compass: update data, skip legacy draw
    if (currentScreen == SCREEN_COMPASS && compassScr) {
      updateCompassData();
      fcNavBarSetActive(compassNavBar, currentScreen);

      // Still need screen-change clear for transitions from legacy screens
      static int lastDrawnScreen_lv = -1;
      if (currentScreen != lastDrawnScreen_lv) {
        if (spriteAvailable) {
          spr.fillSprite(COLOR_BG);
          spr.pushSprite(0, 0);
        }
        lastDrawnScreen_lv = currentScreen;
      }
    } else {
      // Legacy sprite pipeline for all other screens
      TFT_eSprite* c = &spr;
      static int lastDrawnScreen = -1;
      bool wasForced_inner = wasForced;
      if (spriteAvailable && (currentScreen != lastDrawnScreen || wasForced_inner)) {
        spr.fillSprite(COLOR_BG);
        spr.pushSprite(0, 0);
        zonePrevCount = 0;
        lastDrawnScreen = currentScreen;
      }
      switch (currentScreen) {
        case SCREEN_COMPASS:   drawScreenCompass(c);   break;
        case SCREEN_GEOCACHE:  drawScreenGeocache(c);  break;
        case SCREEN_ENV:       drawScreenEnv(c);       break;
        case SCREEN_TELEMETRY: drawScreenTelemetry(c); break;
        case SCREEN_SETTINGS:  drawScreenSettings(c);  break;
      }
      zonePushDirty();
    }

    lastTFTUpdate = millis();
    tftUpdateCount++;
  }
```

**Step 5: Bump version and compile**

Change `FW_VERSION` (line 28) to `"0.40.1"`.

Compile: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 "C:\Dev\Field_Compass\Field_Compass"`

Expected: Clean compile. Flash ~1,785-1,790KB (est. +6-10KB from label code + lv_timer_handler always-on).

**Step 6: Upload and verify**

Upload: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 "C:\Dev\Field_Compass\Field_Compass"`

Verify: Device boots, compass screen shows left-panel labels with sensor data. Rose area is blank (placeholder). Other screens still work via legacy pipeline.

**Step 7: Commit and tag**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.40.1 — LVGL compass left panel + coexistence wiring (#109)"
git tag -a v0.40.1 -m "v0.40.1 — Compass left panel labels + lv_timer_handler production (#109)"
```

---

### Task 2: Compass Rose Custom Draw (v0.40.2)

**Goal:** Implement the rotating compass rose via `LV_EVENT_DRAW_MAIN` custom draw callback with anti-aliased LVGL primitives.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`
  - Replace empty `compassRoseDrawCb` forward declaration with full implementation
  - Wire draw callback in `buildCompassScreen()`
  - Add lubber line as separate static object
  - Bump `FW_VERSION` to `"0.40.2"`

**Step 1: Implement compassRoseDrawCb()**

Replace the forward declaration with the full draw callback. This function is called by LVGL whenever `compassRoseObj` is invalidated:

```cpp
static void compassRoseDrawCb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);

  // Rose geometry — center of the 298x260 container
  // Object position is at (182, 30) relative to screen
  int32_t objW = lv_obj_get_width(obj);
  int32_t objH = lv_obj_get_height(obj);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  int32_t cx = coords.x1 + objW / 2;   // Center X in screen coords
  int32_t cy = coords.y1 + objH / 2;   // Center Y in screen coords
  int32_t radius = 108;

  float heading = compassLastHeading;
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
  arcDsc.color = lv_color_hex(0x808080);  // FC_COLOR_DIM equivalent
  arcDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &arcDsc);

  // --- Tick marks (12 ticks every 30°) ---
  int tickCardLen = radius / 10;     // ~11px
  int tickInterLen = radius / 20;    // ~5px

  for (int i = 0; i < 12; i++) {
    float tickAngle = (i * 30.0f + rotDeg - 90.0f) * M_PI / 180.0f;
    int tickLen = (i % 3 == 0) ? tickCardLen : tickInterLen;

    lv_draw_line_dsc_t lineDsc;
    lv_draw_line_dsc_init(&lineDsc);
    lineDsc.p1.x = cx + cosf(tickAngle) * (radius - tickLen);
    lineDsc.p1.y = cy + sinf(tickAngle) * (radius - tickLen);
    lineDsc.p2.x = cx + cosf(tickAngle) * radius;
    lineDsc.p2.y = cy + sinf(tickAngle) * radius;
    lineDsc.width = (i % 3 == 0) ? 2 : 1;
    lineDsc.color = lv_color_hex(0x808080);
    lineDsc.opa = LV_OPA_COVER;
    lv_draw_line(layer, &lineDsc);
  }

  // --- 8 Diamond needles ---
  struct { float angle; int length; int halfWidth; lv_color_t color; lv_color_t tailColor; } needles[] = {
    {  0, radius*93/100, radius*10/100, lv_color_hex(0x00FFFF), lv_color_hex(0x212121)}, // N cyan
    { 45, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)}, // NE gray
    { 90, radius*93/100, radius*10/100, lv_color_hex(0xFFFFFF), lv_color_hex(0x212121)}, // E white
    {135, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)}, // SE gray
    {180, radius*93/100, radius*10/100, lv_color_hex(0xFF0000), lv_color_hex(0x212121)}, // S red
    {225, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)}, // SW gray
    {270, radius*93/100, radius*10/100, lv_color_hex(0xFFFFFF), lv_color_hex(0x212121)}, // W white
    {315, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)}, // NW gray
  };

  for (int i = 0; i < 8; i++) {
    float tipRad = (needles[i].angle + rotDeg - 90.0f) * M_PI / 180.0f;
    float perpRad = tipRad + M_PI / 2.0f;

    // Tip point
    int32_t tipX = cx + (int32_t)(cosf(tipRad) * needles[i].length);
    int32_t tipY = cy + (int32_t)(sinf(tipRad) * needles[i].length);

    // Side points at center
    int32_t sX1 = cx + (int32_t)(cosf(perpRad) * needles[i].halfWidth);
    int32_t sY1 = cy + (int32_t)(sinf(perpRad) * needles[i].halfWidth);
    int32_t sX2 = cx - (int32_t)(cosf(perpRad) * needles[i].halfWidth);
    int32_t sY2 = cy - (int32_t)(sinf(perpRad) * needles[i].halfWidth);

    // Tail point (opposite, 1/3 length)
    float tailRad = tipRad + M_PI;
    int tailLen = needles[i].length / 3;
    int32_t tailX = cx + (int32_t)(cosf(tailRad) * tailLen);
    int32_t tailY = cy + (int32_t)(sinf(tailRad) * tailLen);

    // Tip triangle
    lv_draw_triangle_dsc_t triDsc;
    lv_draw_triangle_dsc_init(&triDsc);
    triDsc.p[0].x = tipX;  triDsc.p[0].y = tipY;
    triDsc.p[1].x = sX1;   triDsc.p[1].y = sY1;
    triDsc.p[2].x = sX2;   triDsc.p[2].y = sY2;
    triDsc.color = needles[i].color;
    triDsc.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &triDsc);

    // Tail triangle
    triDsc.p[0].x = tailX; triDsc.p[0].y = tailY;
    triDsc.color = needles[i].tailColor;
    lv_draw_triangle(layer, &triDsc);
  }

  // --- Center hub ---
  int hubR = max(5, radius / 15);
  lv_draw_arc_dsc_t hubDsc;
  lv_draw_arc_dsc_init(&hubDsc);
  hubDsc.center.x = cx;
  hubDsc.center.y = cy;
  hubDsc.radius = hubR;
  hubDsc.width = hubR;  // Filled circle = width == radius
  hubDsc.start_angle = 0;
  hubDsc.end_angle = 360;
  hubDsc.color = lv_color_hex(0xFFFFFF);
  hubDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &hubDsc);
}
```

**Step 2: Wire draw callback and lubber line in buildCompassScreen()**

In the compass rose section of `buildCompassScreen()`, replace the empty placeholder with:

```cpp
  // === Right Panel: Compass Rose ===
  compassRoseObj = lv_obj_create(compassScr);
  lv_obj_remove_style_all(compassRoseObj);
  lv_obj_set_size(compassRoseObj, 298, 260);
  lv_obj_set_pos(compassRoseObj, 182, 30);
  lv_obj_clear_flag(compassRoseObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(compassRoseObj, compassRoseDrawCb, LV_EVENT_DRAW_MAIN, NULL);

  // Lubber line (fixed orange triangle at top of rose)
  // Position: above the outer ring, centered on rose center (x=331)
  lv_obj_t* lubber = lv_obj_create(compassScr);
  lv_obj_remove_style_all(lubber);
  lv_obj_set_size(lubber, 20, 12);
  lv_obj_set_pos(lubber, 321, 18);  // Above rose ring
  lv_obj_set_style_bg_color(lubber, FC_COLOR_WARN, 0);
  lv_obj_set_style_bg_opa(lubber, LV_OPA_COVER, 0);
  // Note: rectangle approximation — upgrade to triangle draw in a future polish pass
```

**Step 3: Bump version and compile**

Change `FW_VERSION` to `"0.40.2"`.

Compile: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 "C:\Dev\Field_Compass\Field_Compass"`

Expected: Clean compile. Flash ~1,790-1,795KB.

**Step 4: Upload and verify**

Upload and verify: The compass screen should now show the rotating compass rose with anti-aliased needles and outer ring. The rose should rotate smoothly as heading changes. Left panel labels should continue updating.

**Step 5: Commit and tag**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.40.2 — LVGL compass rose with anti-aliased draw (#109)"
git tag -a v0.40.2 -m "v0.40.2 — Compass rose via LV_EVENT_DRAW_MAIN custom draw (#109)"
```

---

### Task 3: Integration, Polish, and Release (v0.41.0)

**Goal:** Add degree labels around rose, refine lubber line, verify full integration, tag release.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`
  - Add 4 cardinal labels (N/E/S/W) to compass screen
  - Update their positions in `updateCompassData()`
  - Bump `FW_VERSION` to `"0.41.0"`

**Step 1: Add cardinal direction labels around rose**

In `buildCompassScreen()`, add 4 `lv_label` objects for N/E/S/W positioned around the rose:

```cpp
// Globals (add near other compass globals):
static lv_obj_t* compassLblN = NULL;
static lv_obj_t* compassLblE = NULL;
static lv_obj_t* compassLblS = NULL;
static lv_obj_t* compassLblW = NULL;

// In buildCompassScreen(), after compassRoseObj:
  // Cardinal direction labels (rotate with heading)
  const char* cardinals[] = {"N", "E", "S", "W"};
  lv_obj_t** cardLbls[] = {&compassLblN, &compassLblE, &compassLblS, &compassLblW};
  lv_color_t cardColors[] = {
    lv_color_hex(0x00FFFF),  // N cyan
    lv_color_hex(0xFFFFFF),  // E white
    lv_color_hex(0xFF0000),  // S red
    lv_color_hex(0xFFFFFF),  // W white
  };
  for (int i = 0; i < 4; i++) {
    *cardLbls[i] = lv_label_create(compassScr);
    lv_label_set_text(*cardLbls[i], cardinals[i]);
    lv_obj_set_style_text_font(*cardLbls[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(*cardLbls[i], cardColors[i], 0);
  }
```

**Step 2: Position cardinal labels on heading update**

In `updateCompassData()`, after the rose invalidation block, add:

```cpp
  // Reposition cardinal direction labels around rose
  if (compassLblN) {
    int32_t roseCx = 331;  // Center of rose (182 + 298/2)
    int32_t roseCy = 160;  // Center of rose (30 + 260/2)
    int32_t labelR = 120;  // Just outside outer ring
    float cardAngles[] = {0, 90, 180, 270};
    lv_obj_t* cardObjs[] = {compassLblN, compassLblE, compassLblS, compassLblW};
    float rot = -compassLastHeading;

    for (int i = 0; i < 4; i++) {
      float rad = (cardAngles[i] + rot - 90.0f) * M_PI / 180.0f;
      int32_t lx = roseCx + (int32_t)(cosf(rad) * labelR) - 5;
      int32_t ly = roseCy + (int32_t)(sinf(rad) * labelR) - 8;
      lv_obj_set_pos(cardObjs[i], lx, ly);
    }
  }
```

**Step 3: Bump version, compile, upload, verify**

Change `FW_VERSION` to `"0.41.0"`.

Compile and upload. Verify:
- Left panel labels update with sensor data
- Compass rose rotates smoothly (2° threshold)
- Cardinal labels (N/E/S/W) rotate with the rose
- Anti-aliased needle edges visible
- Screen switching to/from compass works cleanly
- Other screens unaffected

**Step 4: Commit, tag, push**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.41.0 — Complete LVGL compass screen with cardinal labels (#109)

Closes #109"
git tag -a v0.41.0 -m "v0.41.0 — LVGL compass screen migration complete (#109)"
git push origin main --tags
```

**Step 5: Close #109 and update MEMORY.md**

- Add summary comment to #109 with flash sizes and feature list
- Update MEMORY.md: current version, new LVGL Compass Screen section
- Update LVGL migration issues line: #109 (done)
