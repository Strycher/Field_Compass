# LVGL Settings Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate the Settings menu and all 6 sub-screens from legacy TFT_eSprite to LVGL, completing the full UI migration.

**Architecture:** Single root `settingsScr` with 7 child containers (menu + 6 sub-screens), visibility toggled by `settingsSubScreen`. Same proven pattern as geocache (#110). Incremental: one sub-screen per commit, coexisting with legacy until complete.

**Tech Stack:** LVGL 9.5.0, ESP32-S3 Arduino, existing FC widget library (#108)

**Design doc:** `docs/plans/2026-02-25-settings-lvgl-design.md`

---

### Task 1: Root Screen + Menu Container

Build the settings root screen, 6 menu buttons, and coexistence wiring. After this task, entering Settings shows the LVGL menu instead of the legacy sprite menu.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Declare settings LVGL globals**

After the existing screen pointer block (~line 1956, near `telemetryScr`), add:

```cpp
// LVGL Settings screen (#112)
static lv_obj_t* settingsScr = NULL;        // Root container
static lv_obj_t* settingsMenuCtr = NULL;    // Sub-screen 0: menu
static lv_obj_t* settingsConfigCtr = NULL;  // Sub-screen 1: configuration
static lv_obj_t* settingsDisplayCtr = NULL; // Sub-screen 2: display
static lv_obj_t* settingsCalCtr = NULL;     // Sub-screen 3: compass cal
static lv_obj_t* settingsDiagsCtr = NULL;   // Sub-screen 4: diagnostics
static lv_obj_t* settingsAboutCtr = NULL;   // Sub-screen 5: about
static lv_obj_t* settingsResetCtr = NULL;   // Sub-screen 6: factory reset

// Settings menu button array for focus/highlight
static lv_obj_t* settingsMenuBtns[SETTINGS_MENU_COUNT];
```

**Step 2: Write `buildSettingsScreen()` — root + menu only**

Place after the existing `buildTelemetryScreen()` function. Build:
- Root container `settingsScr` (480x320, black, hidden)
- Menu container `settingsMenuCtr` (480x320)
  - `fcHeaderCreate(settingsMenuCtr, "SETTINGS")`
  - Flex container (y=35, height=230) holding 6 `lv_btn` objects, one per `settingsMenuItems[]`
  - Each button: 440px wide, 34px tall, dark gray bg, white label text, rounded corners
  - Click callback: sets `settingsSubScreen` to (button index + 1), hides menu, shows target container
  - `fcActionBarCreate(settingsMenuCtr, true, false)` — Back button returns to previous screen (`currentScreen = prevScreen`)

Reference existing patterns:
- `buildGeocacheScreen()` at line 2513 for root container setup
- `settingsMenuItems[]` at line 6396 for menu item labels
- `SETTINGS_MENU_COUNT` for item count

**Step 3: Wire coexistence in `updateDisplay()`**

In `updateDisplay()` (~line 7374), add settings show/hide logic alongside existing LVGL screens:

```cpp
// Show/hide LVGL settings screen (#112)
if (settingsScr) {
  if (currentScreen == SCREEN_SETTINGS)
    lv_obj_clear_flag(settingsScr, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(settingsScr, LV_OBJ_FLAG_HIDDEN);
}
```

Update the LVGL skip condition (~line 7420) to include settings:

```cpp
if ((currentScreen == SCREEN_COMPASS && compassScr) ||
    (currentScreen == SCREEN_GEOCACHE && geocacheScr) ||
    (currentScreen == SCREEN_ENV && envScr) ||
    (currentScreen == SCREEN_TELEMETRY && telemetryScr) ||
    (currentScreen == SCREEN_SETTINGS && settingsScr)) {
```

Inside the LVGL branch, add `updateSettingsData()` call (stub for now).

**Step 4: Write `updateSettingsData()` stub**

```cpp
void updateSettingsData() {
  if (!settingsScr) return;
  // Show/hide sub-screen containers based on settingsSubScreen
  if (settingsMenuCtr) lv_obj_add_flag(settingsMenuCtr, LV_OBJ_FLAG_HIDDEN);
  // (other containers hidden when they exist)

  switch (settingsSubScreen) {
    case 0: if (settingsMenuCtr) lv_obj_clear_flag(settingsMenuCtr, LV_OBJ_FLAG_HIDDEN); break;
    // Cases 1-6 added in subsequent tasks
  }
}
```

**Step 5: Call `buildSettingsScreen()` from `initLVGL()`**

After the existing `buildTelemetryScreen()` call (~line 3545):

```cpp
  // Build LVGL settings screen (#112)
  buildSettingsScreen();
```

**Step 6: Compile and verify**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Clean compile. Flash increase ~2-4KB.

**Step 7: Upload and verify menu renders**

Run: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 Field_Compass/`
Verify: Navigate to Settings — should show LVGL menu with 6 items. Back button returns to previous screen. Tapping a menu item does nothing visible yet (sub-screen containers not built).

**Step 8: Commit**

```
feat(#112): Add LVGL Settings root screen + menu container
```

---

### Task 2: Configuration Sub-Screen

Build the Configuration container with timezone dropdown, 3 toggles, live preview, and save/load via action bar.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Build configuration container in `buildSettingsScreen()`**

Add after menu container construction:

```cpp
// --- Sub-screen 1: Configuration ---
settingsConfigCtr = lv_obj_create(settingsScr);
// (full-screen container, black bg, no border/padding)
```

Contents:
- `fcHeaderCreate(settingsConfigCtr, "CONFIGURATION")`
- `fcDropdownCreate(settingsConfigCtr, 45, "Time Zone", tzDisplayName)` — store pointer as `cfgTzDropdown`
  - Click handler → `fcListPickerOpen()` with `tzPresets[].name` (14 items), selected = `tzSelectedIndex`
  - ListPicker `LV_EVENT_VALUE_CHANGED` → update `tzSelectedIndex`, `posixTZ`, `tzDisplayName`, refresh dropdown label
- `fcToggleCreate(settingsConfigCtr, 90, "Time", "12 Hour", "24 Hour", use12Hour ? 0 : 1)` — store as `cfgTimeToggle`
  - `LV_EVENT_VALUE_CHANGED` → `use12Hour = !fcToggleGetValue(cfgTimeToggle)`
- `fcToggleCreate(settingsConfigCtr, 135, "Temp", "\xC2\xB0F", "\xC2\xB0C", useFahrenheit ? 0 : 1)` — store as `cfgTempToggle`
  - `LV_EVENT_VALUE_CHANGED` → `useFahrenheit = !fcToggleGetValue(cfgTempToggle)`
- `fcToggleCreate(settingsConfigCtr, 180, "Distance", "Imperial", "Metric", useMetricUnits ? 1 : 0)` — store as `cfgDistToggle`
  - `LV_EVENT_VALUE_CHANGED` → `useMetricUnits = (bool)fcToggleGetValue(cfgDistToggle)`
- `lv_label` for live preview at y=225 — store as `cfgPreviewLabel`
- `fcActionBarCreate(settingsConfigCtr, true, true)`:
  - Back click → `loadSettings()`, reset toggle/dropdown visuals to loaded values, `settingsSubScreen = 0`
  - OK click → `saveSettings()`, `settingsSubScreen = 0`

Reference:
- `fcToggleCreate()` at line 1636 for toggle pattern
- `fcDropdownCreate()` at line 1695 for dropdown pattern
- `fcListPickerOpen()` at line 1774 for modal picker pattern
- `tzPresets[]` at line 551 for timezone data
- `use12Hour` at line 526, `useFahrenheit` at line 527, `useMetricUnits` at line 525

**Step 2: Add live preview update in `updateSettingsData()`**

In the `case 1:` branch, format and set `cfgPreviewLabel` text showing current time (formatted per `use12Hour`), temperature (per `useFahrenheit`), and distance example (per `useMetricUnits`).

Reference: The legacy live preview logic in `drawSettingsConfig()` at line 8497.

**Step 3: Wire sub-screen visibility**

In `updateSettingsData()`, add `case 1:` to show `settingsConfigCtr` and call preview update.

**Step 4: Compile and verify**

Expected: Clean compile. Flash increase ~3-5KB.

**Step 5: Upload and verify Configuration works**

Verify: Tap "Configuration" in menu → shows header, timezone dropdown, 3 toggles, live preview, Back/OK.
- Toggle changes update preview immediately
- Timezone picker opens as modal overlay with 14 items
- OK saves, Back discards changes
- Encoder navigates between widgets

**Step 6: Commit**

```
feat(#112): Add LVGL Configuration sub-screen (timezone, toggles, preview)
```

---

### Task 3: Display Sub-Screen

Build the Display container with brightness slider, two timeout dropdowns, and save/load.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Build display container in `buildSettingsScreen()`**

Contents:
- `fcHeaderCreate(settingsDisplayCtr, "DISPLAY")`
- `lv_label` "Brightness" at y=50
- `lv_slider` at y=50, x=160, width=200: range 25-255, initial value = `tftBrightness`
  - Style: green indicator, dark gray background track, white knob
  - `LV_EVENT_VALUE_CHANGED` → `tftBrightness = lv_slider_get_value(slider); analogWrite(TFT_BL, tftBrightness);`
  - Store as `dispBrightnessSlider`
- `lv_label` for numeric readout to the right of slider — store as `dispBrightnessLabel`
  - Updated in slider callback: `lv_label_set_text_fmt(dispBrightnessLabel, "%d", val)`
- `fcDropdownCreate(settingsDisplayCtr, 105, "TFT Sleep", tftTimeoutLabels[findTimeoutIndex(tftSleepMs, tftTimeoutPresets, TFT_TIMEOUT_COUNT)])`
  - Click → `fcListPickerOpen()` with `tftTimeoutLabels[]` (7 items)
  - Selection → `tftSleepMs = tftTimeoutPresets[selectedIdx]`, update label
- `fcDropdownCreate(settingsDisplayCtr, 155, "OLED Sleep", oledTimeoutLabels[findTimeoutIndex(oledSleepMs, oledTimeoutPresets, OLED_TIMEOUT_COUNT)])`
  - Click → `fcListPickerOpen()` with `oledTimeoutLabels[]` (6 items)
  - Selection → `oledSleepMs = oledTimeoutPresets[selectedIdx]`, update label
- `fcActionBarCreate(settingsDisplayCtr, true, true)`:
  - Back click → restore `tftBrightness` from saved value (`loadSettings()`), apply PWM, `settingsSubScreen = 0`
  - OK click → `saveSettings()`, `settingsSubScreen = 0`

Reference:
- `tftTimeoutPresets[]` / `tftTimeoutLabels[]` at line 6406
- `oledTimeoutPresets[]` / `oledTimeoutLabels[]` at line 6410
- `findTimeoutIndex()` helper (search for it near timeout presets)
- `analogWrite(TFT_BL, tftBrightness)` at line 1092

**Step 2: Wire sub-screen visibility**

In `updateSettingsData()`, add `case 2:` to show `settingsDisplayCtr`. Update brightness label if slider value changed externally.

**Step 3: Compile, upload, verify**

Verify: Slider adjusts brightness live. Timeout dropdowns open pickers. Back restores brightness. OK saves.

**Step 4: Commit**

```
feat(#112): Add LVGL Display sub-screen (brightness slider, timeout dropdowns)
```

---

### Task 4: Compass Calibration Sub-Screen

Build the Compass Cal container with idle/active/complete states.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Build compass cal container in `buildSettingsScreen()`**

**Idle state widgets** (visible when `!magCalibrating`):
- `fcHeaderCreate(settingsCalCtr, "COMPASS CAL")`
- `lv_label` "Status:" at y=50, value label showing "Calibrated" (green) or "Not calibrated" (dim)
- `lv_label` "Offsets:" at y=80, value label with X/Y/Z floats or "---"
- `lv_btn` "Start Calibration" at y=130, centered, green bg (or gray if `!magAvailable`)
  - Click → set `magCalibrating = true`, init min/max, hide idle widgets, show cal widgets
- `fcActionBarCreate(settingsCalCtr, true, false)` — Back only

**Active calibration widgets** (visible when `magCalibrating`, initially hidden):
- `lv_arc` progress ring: centered at (240, 170), outer radius 70, range 0-100
  - Style: green indicator, dark gray background
- `lv_label` countdown centered inside arc — "15", "14", ... "0"
- `lv_label` instruction below arc: "Rotate device slowly 360\xC2\xB0"
- `lv_label` live min/max X/Y/Z readings below instruction

Store pointers for idle group and cal group to batch show/hide.

Reference:
- `magCalibrating`, `magCalibrated`, `magOffsetX/Y/Z` at line 374
- `MAG_CAL_DURATION_MS` (15000) at line 380
- Legacy compass cal rendering at line 8836 for layout details

**Step 2: Add calibration progress update in `updateSettingsData()`**

In `case 3:`:
- If `magCalibrating`: calculate elapsed %, update arc value, update countdown label, update min/max label
- On completion (`elapsed >= MAG_CAL_DURATION_MS`): compute offsets, save, show "CAL COMPLETE" for 3s, return to idle
- If idle: update status and offsets labels from current `magCalibrated`/offset values

**Step 3: Compile, upload, verify**

Verify: Status shows correctly. Start button works. Progress ring animates during 15s calibration. Completion shows offsets. Back returns to menu.

**Step 4: Commit**

```
feat(#112): Add LVGL Compass Cal sub-screen (progress arc, calibration states)
```

---

### Task 5: Diagnostics Sub-Screen

Build the Diagnostics container with 8 label-value pairs updated each frame.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Build diagnostics container in `buildSettingsScreen()`**

- `fcHeaderCreate(settingsDiagsCtr, "DIAGNOSTICS")`
- 8 rows, each with a cyan label and a white/green value label:
  1. BSEC — load/save/accuracy status
  2. Weather — memory/files/total
  3. Heap — free/total KB
  4. PSRAM — free/total KB + sprite status
  5. Sensors — BME/SHT/IMU/Bat/FRAM/CTP availability
  6. Temps — SHT vs BME readings + delta
  7. GPS — fix time or acquiring status
  8. MagCal — offset values or "None"
- `fcActionBarCreate(settingsDiagsCtr, true, false)` — Back only
- Store value label pointers in an array `diagValueLabels[8]`

Reference: `drawSettingsDiags()` at line 9138 for exact data formatting.

**Step 2: Add diagnostics update in `updateSettingsData()`**

In `case 4:`: Format and set all 8 value labels using the same data sources as the legacy function. Update every frame.

**Step 3: Compile, upload, verify**

Verify: All 8 rows display correct live data. Values update in real-time.

**Step 4: Commit**

```
feat(#112): Add LVGL Diagnostics sub-screen (8 live data rows)
```

---

### Task 6: About Sub-Screen

Build the About container with 6 info rows.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Build about container in `buildSettingsScreen()`**

- `fcHeaderCreate(settingsAboutCtr, "ABOUT")`
- 6 rows (label + value):
  1. Version — `FW_VERSION` (static)
  2. Uptime — formatted from `millis()` (live)
  3. Heap — free/total KB (live)
  4. PSRAM — free/total KB (live)
  5. Battery — percentage + voltage or "USB Only" or "N/A" (live)
  6. WiFi — SSID + IP or "Disconnected" (live)
- `fcActionBarCreate(settingsAboutCtr, true, false)` — Back only
- Store value label pointers in array `aboutValueLabels[6]`

Reference: `drawSettingsAbout()` at line 9027 for exact formatting.

**Step 2: Add about update in `updateSettingsData()`**

In `case 5:`: Update uptime, heap, PSRAM, battery, WiFi labels. Version is static (set once in build).

**Step 3: Compile, upload, verify**

Verify: All 6 rows show correct data. Uptime ticks. Heap/battery update live.

**Step 4: Commit**

```
feat(#112): Add LVGL About sub-screen (version, uptime, system info)
```

---

### Task 7: Factory Reset Sub-Screen

Build the Factory Reset container with warning text and reset button.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Build factory reset container in `buildSettingsScreen()`**

- `fcHeaderCreate(settingsResetCtr, "FACTORY RESET")`
- `lv_label` warning text at y=90: "Reset all settings to\nfactory defaults?" — orange (`fcStyleWarn`)
- `lv_label` info text at y=155: "Compass calibration will\nbe preserved." — dim (`fcStyleLabel`)
- `fcActionBarCreate(settingsResetCtr, true, false)` — Back button
- Custom red `lv_btn` "Reset" at (360, 275), 100x40 — replaces OK button position
  - Click → call `factoryReset()`, then reset all LVGL widget values to defaults, return to menu

Reference: `drawSettingsFactoryReset()` at line 8580, `factoryReset()` at line 4841.

**Step 2: Wire sub-screen visibility**

In `updateSettingsData()`, add `case 6:` to show `settingsResetCtr`.

**Step 3: Compile, upload, verify**

Verify: Warning text renders in orange. Reset button calls factoryReset. Settings revert to defaults. Back returns to menu.

**Step 4: Commit**

```
feat(#112): Add LVGL Factory Reset sub-screen (warning + reset button)
```

---

### Task 8: Legacy Cleanup + Version Bump

Remove all legacy settings rendering and tap handling code. Bump version.

**Files:**
- Modify: `Field_Compass/Field_Compass.ino`

**Step 1: Remove legacy drawing functions**

Delete these functions entirely:
- `drawSettingsMenu()` (~line 8631)
- `drawSettingsConfig()` (~line 8497)
- `drawSettingsDisplay()` (~line 8701)
- `drawSettingsCompassCal()` (~line 8836)
- `drawSettingsDiags()` (~line 9138)
- `drawSettingsAbout()` (~line 9027)
- `drawSettingsFactoryReset()` (~line 8580)
- `drawScreenSettings()` (~line 9332)

**Step 2: Remove legacy tap handlers**

Delete these functions:
- `handleConfigTap()` (~line 6729)
- `handleDisplayTap()` (~line 8780)
- `handleCompassCalTap()` (~line 6811)

Remove settings tap routing from `handleTap()` (~lines 6879-6927).

**Step 3: Remove legacy helper widgets**

Delete these functions (only used by settings):
- `drawToggle()` (~line 8384)
- `drawDropdown()` (~line 8422)
- `drawActionBar()` (~line 8363)
- `drawSelectorOverlay()` (~line 8446)

Verify no other code references these helpers before deleting.

**Step 4: Remove stale state variables**

Remove if no longer needed:
- `configFocusRow`, `displayFocusRow`, `compassCalFocusRow`, `resetFocusRow` (focus now via LVGL group)
- `tzSelectorOpen` (replaced by `fcListPickerOpen` modal)
- Zone-based dirty tracking for settings (if any)

**Step 5: Version bump**

Change `FW_VERSION` from "0.44.1" to "0.45.0" (new feature = minor version bump).

**Step 6: Compile and verify**

Expected: Clean compile. Flash should **decrease** from removing legacy code, partially offset by LVGL additions. Net change TBD.

**Step 7: Upload and full regression test**

Verify ALL settings sub-screens work:
- [ ] Menu renders, all 6 items clickable
- [ ] Configuration: timezone, toggles, preview, save/load
- [ ] Display: brightness slider, timeout dropdowns, save/load
- [ ] Compass Cal: status, start, progress, completion
- [ ] Diagnostics: all 8 rows with live data
- [ ] About: version, uptime, heap, battery, WiFi
- [ ] Factory Reset: warning, reset button, settings revert
- [ ] Encoder navigation through all screens
- [ ] Touch navigation through all screens
- [ ] Back/OK buttons work correctly on all screens

**Step 8: Commit and tag**

```
feat(#112): Remove legacy Settings code, bump to v0.45.0

Complete LVGL migration of Settings menu + all 6 sub-screens.
Legacy TFT_eSprite drawing functions, tap handlers, and helper
widgets removed. All UI now rendered via LVGL.
```

Tag: `v0.45.0`

---

## Notes

- **Object budget**: ~90 new objects, running total ~350. 96KB LV_MEM_SIZE should suffice. If OOM occurs during arc/slider rendering, increase to 128KB in `lv_conf.h`.
- **Encoder focus**: All interactive widgets auto-join default focus group. Menu buttons, toggles, dropdowns, slider, action bar buttons all navigable via encoder.
- **Compass calibration timing**: The existing `magCalibrating` state machine in `loop()` handles min/max tracking. The LVGL screen only needs to read those values and update the arc/labels — no calibration logic changes needed.
- **Settings persistence**: `loadSettings()` and `saveSettings()` are unchanged. LVGL widgets read/write the same global variables (`use12Hour`, `tftBrightness`, etc.).
- **GPS debug toggle (#115)**: Currently accessible via web endpoint `/gps/debug`. Future: add to Configuration sub-screen after this migration is stable. Tracked separately.
