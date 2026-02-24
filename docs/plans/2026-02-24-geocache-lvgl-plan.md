# #110 LVGL Geocache Screen Migration — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate the 3-sub-screen geocache screen (Nav, List, Details) from legacy TFT_eSprite to LVGL widgets.

**Architecture:** 3 static pre-built containers inside a root `geocacheScr`, shown/hidden based on `geocacheSubScreen`. Custom draw callback for nav triangle + search zone circle. Manual label rows for cache list. Coexistence wiring in `updateDisplay()` matching the compass screen (#109) pattern.

**Tech Stack:** LVGL 9.5.0, ESP32-S3, TFT_eSPI, FC widget library (#108)

---

### Task 1: Static Widget Handles + buildGeocacheScreen() Scaffold

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`
  - After compass static handles (~line 495): add geocache widget handles
  - In `initLVGL()` (line 2325): add `buildGeocacheScreen()` call after `buildCompassScreen()`
  - Before `initLVGL()` (~line 2250): add `buildGeocacheScreen()` function

**Step 1: Add static widget handle declarations**

After the compass widget handles (around line 495, before `GeocacheEntry` struct), add:

```cpp
// LVGL Geocache screen widget handles (#110)
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
```

**Step 2: Write buildGeocacheScreen() — root + 3 empty sub-containers**

Place before `initLVGL()` (~line 2250). Start with just the scaffold (no children yet):

```cpp
void buildGeocacheScreen() {
  // Root container — full screen, hidden by default
  geocacheScr = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(geocacheScr);
  lv_obj_set_size(geocacheScr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheScr, 0, 0);
  lv_obj_set_style_bg_color(geocacheScr, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(geocacheScr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(geocacheScr, LV_OBJ_FLAG_SCROLLABLE);

  // Nav sub-screen container (sub 0)
  geocacheNavCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheNavCtr);
  lv_obj_set_size(geocacheNavCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheNavCtr, 0, 0);
  lv_obj_clear_flag(geocacheNavCtr, LV_OBJ_FLAG_SCROLLABLE);

  // List sub-screen container (sub 1)
  geocacheListCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheListCtr);
  lv_obj_set_size(geocacheListCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheListCtr, 0, 0);
  lv_obj_clear_flag(geocacheListCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);

  // Details sub-screen container (sub 2)
  geocacheDetailsCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheDetailsCtr);
  lv_obj_set_size(geocacheDetailsCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheDetailsCtr, 0, 0);
  lv_obj_clear_flag(geocacheDetailsCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);

  // Start entire geocache screen hidden
  lv_obj_add_flag(geocacheScr, LV_OBJ_FLAG_HIDDEN);

  logPrintln("[LVGL] Geocache screen built (#110)");
}
```

**Step 3: Call from initLVGL()**

At line 2325, after `buildCompassScreen();`, add:

```cpp
  // Build LVGL geocache screen (#110)
  buildGeocacheScreen();
```

**Step 4: Compile to verify scaffold compiles**

```bash
arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/
```

Expected: compiles with ~same flash as v0.41.1 (small increase from empty containers).

**Step 5: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat(#110): Geocache screen scaffold — root + 3 sub-containers"
```

---

### Task 2: Nav Sub-screen Labels

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — `buildGeocacheScreen()`

**Step 1: Add Nav sub-screen widgets inside geocacheNavCtr**

Inside `buildGeocacheScreen()`, after the Nav container creation, add:

```cpp
  // === Nav Sub-screen (sub 0) ===
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
  // Draw callback added in Task 3

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
```

**Step 2: Compile**

Expected: compiles, flash increases ~1-2KB.

**Step 3: Commit**

```bash
git commit -am "feat(#110): Nav sub-screen labels and layout"
```

---

### Task 3: Nav Custom Draw Callback (Triangle + Search Zone)

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`
  - Add `geocacheNavDrawCb()` static function before `buildGeocacheScreen()`
  - Add `lv_obj_add_event_cb()` in `buildGeocacheScreen()` for `gcNavGraphicObj`

**Step 1: Add state variables for nav graphic**

Near the geocache static handles:

```cpp
static float gcNavLastBearing = -999;
static float gcNavLastHeading = -999;
static bool  gcNavLastInZone = false;
static int32_t gcNavPulseRadius = 0;  // For search zone animation
```

**Step 2: Write geocacheNavDrawCb()**

This replicates `drawNavTriangle()` (lines 6942-6966) and `drawSearchZoneCircle()` (lines 6969-6987) using LVGL draw primitives:

```cpp
static void geocacheNavDrawCb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  int32_t cx = (coords.x1 + coords.x2) / 2;
  int32_t cy = (coords.y1 + coords.y2) / 2;

  // Get current cache data
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
    // === Direction Arrow (same math as drawNavTriangle) ===
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

    lv_color_t arrowColor = lv_color_hex(0x00BFFF);  // FC_COLOR_HEADER cyan

    // Triangle 1: tip → rear1 → rearCenter
    lv_draw_triangle_dsc_t tri1;
    lv_draw_triangle_dsc_init(&tri1);
    tri1.p[0].x = tipX; tri1.p[0].y = tipY;
    tri1.p[1].x = rear1X; tri1.p[1].y = rear1Y;
    tri1.p[2].x = rearCX; tri1.p[2].y = rearCY;
    tri1.bg_color = arrowColor;
    tri1.bg_opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &tri1);

    // Triangle 2: tip → rear2 → rearCenter
    lv_draw_triangle_dsc_t tri2;
    lv_draw_triangle_dsc_init(&tri2);
    tri2.p[0].x = tipX; tri2.p[0].y = tipY;
    tri2.p[1].x = rear2X; tri2.p[1].y = rear2Y;
    tri2.p[2].x = rearCX; tri2.p[2].y = rearCY;
    tri2.bg_color = arrowColor;
    tri2.bg_opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &tri2);
  }
}
```

**Step 3: Register callback in buildGeocacheScreen()**

After `gcNavGraphicObj` creation:

```cpp
  lv_obj_add_event_cb(gcNavGraphicObj, geocacheNavDrawCb, LV_EVENT_DRAW_MAIN, NULL);
```

**Step 4: Compile**

**Step 5: Commit**

```bash
git commit -am "feat(#110): Nav custom draw — direction arrow + search zone circle"
```

---

### Task 4: List Sub-screen Widgets

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — `buildGeocacheScreen()`

**Step 1: Add List sub-screen widgets inside geocacheListCtr**

```cpp
  // === List Sub-screen (sub 1) ===
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
```

**Step 2: Compile**

**Step 3: Commit**

```bash
git commit -am "feat(#110): List sub-screen with 20 pre-created rows"
```

---

### Task 5: Details Sub-screen Widgets

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — `buildGeocacheScreen()`

**Step 1: Add Details sub-screen widgets inside geocacheDetailsCtr**

```cpp
  // === Details Sub-screen (sub 2) ===
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
```

**Step 2: Compile**

**Step 3: Commit**

```bash
git commit -am "feat(#110): Details sub-screen labels and layout"
```

---

### Task 6: updateGeocacheData() — Data Update Function

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`
  - Add `updateGeocacheData()` function after `buildGeocacheScreen()`

**Step 1: Write updateGeocacheData()**

This is the LVGL equivalent of the 3 `drawCache*Screen()` functions. It reads `geocacheSubScreen` and updates only the visible sub-screen's labels:

```cpp
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

  // === NAV sub-screen data ===
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

      // Accuracy
      if (useMetricUnits) {
        lv_label_set_text_fmt(gcNavLblAccuracy, "+/-%dm", (int)accuracyM);
      } else {
        lv_label_set_text_fmt(gcNavLblAccuracy, "+/-%dft", (int)(accuracyM * 3.28084f));
      }
      lv_color_t accColor = getAccuracyColor(accuracyM);
      lv_obj_set_style_text_color(gcNavLblAccuracy, accColor, 0);

      // Hint
      if (inSearchZone) {
        lv_label_set_text(gcNavLblHint, cache.hint);
      } else {
        char hintPreview[32];
        strncpy(hintPreview, cache.hint, 30);
        hintPreview[30] = '\0';
        if (strlen(cache.hint) > 30) strcat(hintPreview, "..");
        lv_label_set_text(gcNavLblHint, hintPreview);
      }

      // Invalidate nav graphic on bearing/heading change
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

  // === LIST sub-screen data ===
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

      // Name (truncated)
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

  // === DETAILS sub-screen data ===
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

    // Found status
    if (c.found) {
      lv_label_set_text(gcDetLblFound, "[\u2605 FOUND]");
      lv_obj_set_style_text_color(gcDetLblFound, FC_COLOR_VALUE, 0);
    } else {
      lv_label_set_text(gcDetLblFound, "[ NOT FOUND ]");
      lv_obj_set_style_text_color(gcDetLblFound, FC_COLOR_DIM, 0);
    }
    fcNavBarSetActive(gcDetNavBar, currentScreen);
  }
}
```

**Step 2: Compile**

**Step 3: Commit**

```bash
git commit -am "feat(#110): updateGeocacheData() — data update for all 3 sub-screens"
```

---

### Task 7: Coexistence Wiring in updateDisplay()

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — `updateDisplay()` (line 6044)

**Step 1: Add geocache show/hide after compass show/hide (line 6065)**

After the compass show/hide block, add:

```cpp
    // Show/hide LVGL geocache screen based on currentScreen (#110)
    if (geocacheScr) {
      if (currentScreen == SCREEN_GEOCACHE)
        lv_obj_clear_flag(geocacheScr, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(geocacheScr, LV_OBJ_FLAG_HIDDEN);
    }
```

**Step 2: Add LVGL geocache update block**

Change the LVGL compass block (line 6068) to handle both LVGL screens:

Replace:
```cpp
    if (currentScreen == SCREEN_COMPASS && compassScr) {
```

With a combined block handling both LVGL screens:

```cpp
    // LVGL-managed screens: update data, skip legacy sprite draw
    if ((currentScreen == SCREEN_COMPASS && compassScr) ||
        (currentScreen == SCREEN_GEOCACHE && geocacheScr)) {

      if (currentScreen == SCREEN_COMPASS) {
        updateCompassData();
        fcNavBarSetActive(compassNavBar, currentScreen);
      } else if (currentScreen == SCREEN_GEOCACHE) {
        updateGeocacheData();
      }

      // Screen-change clear
      static int lastLvglScreen = -1;
      if (currentScreen != lastLvglScreen) {
        if (spriteAvailable) {
          spr.fillSprite(COLOR_BG);
          spr.pushSprite(0, 0);
        }
        lastLvglScreen = currentScreen;
      }
    } else {
```

**Step 3: Compile**

**Step 4: Upload and verify**

Upload to device, navigate to geocache screen. Verify:
- Nav sub-screen shows "No cache loaded" (no GPX loaded)
- Screen transitions (swipe/buttons) work between compass and geocache
- No reboots

**Step 5: Commit**

```bash
git commit -am "feat(#110): Coexistence wiring — geocache LVGL in updateDisplay()"
```

---

### Task 8: Search Zone Pulse Animation

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Add LVGL animation for pulsing**

In `buildGeocacheScreen()`, after the nav graphic callback registration, add a looping animation that drives `gcNavPulseRadius` from 0→8→0:

```cpp
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
```

**Step 2: Compile and verify**

**Step 3: Commit**

```bash
git commit -am "feat(#110): Search zone pulse animation"
```

---

### Task 9: getAccuracyColor LVGL Helper

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Add LVGL color version of getAccuracyColor**

The legacy `getAccuracyColor()` returns `uint16_t` (RGB565). Add an LVGL version near the geocache functions:

```cpp
static lv_color_t getAccuracyColor(float accuracyM) {
  if (accuracyM < 10.0f) return FC_COLOR_VALUE;   // Green
  if (accuracyM < 25.0f) return FC_COLOR_WARN;    // Orange
  return FC_COLOR_ERROR;                           // Red
}
```

Note: If the legacy `getAccuracyColor()` returns RGB565, this new function shadows it in the LVGL context. Check for conflicts — may need to rename one or the other, or make this an overload.

**Step 2: Compile and verify no conflicts**

**Step 3: Commit if needed**

---

### Task 10: Full Integration Test + Version Tag

**Step 1: Upload to device**

**Step 2: Test all 3 sub-screens:**
- Nav: verify "No cache loaded" message, then upload a GPX via web to test with data
- List: A/B scrolling, C select, C+ details
- Details: A/B prev/next, C toggle found, C+ back
- Screen transitions between compass ↔ geocache via swipe/buttons

**Step 3: Capture serial for 30+ seconds on geocache screen — verify zero reboots**

**Step 4: Update FW_VERSION to "0.42.0"**

**Step 5: Commit and tag**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.42.0 — LVGL geocache screen migration (#110)"
git tag -a v0.42.0 -m "v0.42.0 — LVGL geocache screen with Nav, List, Details sub-screens (#110)"
git push origin main --tags
```

**Step 6: Close #110 on GitHub**

```bash
gh issue close 110 --comment "Completed in v0.42.0. All 3 sub-screens migrated to LVGL."
```

**Step 7: Update project board**

Set #110 Status to Done.
