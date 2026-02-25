# LVGL Settings Migration Design — Issue #112

**Date:** 2026-02-25
**Issue:** #112 — LVGL: Migrate Settings menu and all 6 sub-screens
**Depends on:** #108 (widget library), #109-#111 (coexistence pattern)

## Overview

Migrate `drawScreenSettings()`, `drawSettingsMenu()`, and all 6 sub-screens from the legacy TFT_eSprite pipeline to LVGL. This is the final screen migration — after completion, the entire UI runs on LVGL and the legacy sprite pipeline can be removed.

## Strategy

**Incremental sub-screen migration** — same proven approach as compass (#109), geocache (#110), environment (#111), and telemetry (#111). One sub-screen per commit, testable at each step, coexistence with legacy until complete.

## Root Architecture

Single root screen with 7 child containers, visibility toggled by `settingsSubScreen`:

```
settingsScr (lv_obj_t*, 480x320, hidden by default)
├── settingsMenuCtr     (sub-screen 0: main menu)
├── settingsConfigCtr   (sub-screen 1: configuration)
├── settingsDisplayCtr  (sub-screen 2: display settings)
├── settingsCalCtr      (sub-screen 3: compass calibration)
├── settingsDiagsCtr    (sub-screen 4: diagnostics)
├── settingsAboutCtr    (sub-screen 5: about)
└── settingsResetCtr    (sub-screen 6: factory reset)
```

- **Switching**: `settingsSubScreen` controls container visibility (same as `geocacheSubScreen` pattern)
- **Coexistence**: `updateDisplay()` routes `SCREEN_SETTINGS` through LVGL when `settingsScr` exists
- **Build**: `buildSettingsScreen()` called from `initLVGL()`
- **Update**: `updateSettingsData()` refreshes visible sub-screen labels each frame

## Widget Mapping

### Menu (sub-screen 0)
- `fcHeaderCreate("SETTINGS")` — cyan header bar
- 6 `lv_btn` objects in a vertical flex container — one per menu item
- Tap/click → sets `settingsSubScreen`, shows target container
- `fcActionBarCreate(true, false)` — Back returns to previous screen
- Encoder: focus group cycles through 6 buttons + Back

### Configuration (sub-screen 1)
- `fcHeaderCreate("CONFIGURATION")`
- `fcDropdownCreate()` for timezone → `fcListPickerOpen()` with 14 TZ presets
- `fcToggleCreate()` × 3: Time (12H/24H), Temp (°F/°C), Distance (Imperial/Metric)
- `lv_label` for live preview (time | temp | distance), updated in `updateSettingsData()`
- `fcActionBarCreate(true, true)` — Back discards changes (reloads from SD), OK saves

### Display (sub-screen 2)
- `fcHeaderCreate("DISPLAY")`
- `lv_slider` for brightness: range 25-255, `LV_EVENT_VALUE_CHANGED` → `analogWrite(TFT_BL, val)`
- `lv_label` for brightness numeric readout beside slider
- `fcDropdownCreate()` × 2: TFT Sleep (7 presets: Never→30min), OLED Sleep (6 presets: 1min→30min)
- Each dropdown opens `fcListPickerOpen()` with timeout presets
- `fcActionBarCreate(true, true)` — Back restores saved brightness, OK saves

### Compass Cal (sub-screen 3)
**Idle state:**
- `fcHeaderCreate("COMPASS CAL")`
- `lv_label` for status: "Calibrated" (green) / "Not calibrated" (dim)
- `lv_label` for offsets: X/Y/Z values or "---"
- `lv_btn` "Start Calibration" — green if IMU available, disabled (gray) if not
- `fcActionBarCreate(true, false)` — Back only

**Active calibration state:**
- `lv_arc` progress ring: 0-100% range, updated from `MAG_CAL_DURATION_MS` timer
- `lv_label` countdown centered inside arc (15→0)
- `lv_label` instruction: "Rotate device slowly 360°"
- `lv_label` live min/max X/Y/Z readings
- No interactive elements during calibration (idle widgets hidden, cal widgets shown)

**Completion state:**
- "CAL COMPLETE" label with computed offsets, shown for 3 seconds, then return to idle

### Diagnostics (sub-screen 4)
- `fcHeaderCreate("DIAGNOSTICS")`
- 8 label-value `lv_label` pairs: BSEC, Weather, Heap, PSRAM, Sensors, Temps, GPS, MagCal
- Labels in cyan (`fcStyleHeader`), values in white/green (`fcStyleBody`/`fcStyleValue`)
- Updated every frame via `updateSettingsData()`
- `fcActionBarCreate(true, false)` — Back only

### About (sub-screen 5)
- `fcHeaderCreate("ABOUT")`
- 6 label-value `lv_label` pairs: Version, Uptime, Heap, PSRAM, Battery, WiFi
- Live update for uptime, heap, battery via `updateSettingsData()`
- `fcActionBarCreate(true, false)` — Back only

### Factory Reset (sub-screen 6)
- `fcHeaderCreate("FACTORY RESET")`
- `lv_label` warning text (orange, `fcStyleWarn`): "Reset all settings to factory defaults?"
- `lv_label` info text (dim, `fcStyleLabel`): "Compass calibration will be preserved."
- `fcActionBarCreate(true, false)` — Back button
- Custom red "Reset" `lv_btn` replacing the OK button position — calls `factoryReset()`

## Object Budget

~90 new LVGL objects. Running total with existing screens: ~350 objects, well within 96KB LV_MEM_SIZE.

## Event Handling

- **Toggle changes**: `LV_EVENT_VALUE_CHANGED` callbacks update `use12Hour`, `useFahrenheit`, `useMetricUnits`
- **Slider changes**: `LV_EVENT_VALUE_CHANGED` → `analogWrite(TFT_BL, lv_slider_get_value(slider))`
- **Dropdown/ListPicker selection**: `LV_EVENT_VALUE_CHANGED` on caller → update timezone/timeout index
- **Menu item clicks**: `LV_EVENT_CLICKED` → set `settingsSubScreen`, show/hide containers
- **Back button**: `LV_EVENT_CLICKED` → if sub-screen, return to menu; if menu, return to previous screen
- **OK button**: `LV_EVENT_CLICKED` → `saveSettings()` then return to menu
- **Calibration start**: `LV_EVENT_CLICKED` → set `magCalibrating = true`, swap idle/active widget visibility
- **Factory Reset**: `LV_EVENT_CLICKED` → `factoryReset()`

All interactive widgets auto-join the default encoder focus group via existing `lv_group_set_default()`.

## Legacy Code Removal

After all 7 containers are working:
- Remove `drawSettingsMenu()`, `drawSettingsConfig()`, `drawSettingsDisplay()`, `drawSettingsCompassCal()`, `drawSettingsDiags()`, `drawSettingsAbout()`, `drawSettingsFactoryReset()`
- Remove `handleConfigTap()`, `handleDisplayTap()`, `handleCompassCalTap()` and settings tap routing in `handleTap()`
- Remove legacy helpers: `drawToggle()`, `drawDropdown()`, `drawActionBar()`, `drawSelectorOverlay()`
- Remove zone-based dirty tracking for settings (`lastSettingsSubScreen`, zone arrays)
- This completes the full LVGL migration — legacy sprite pipeline only needed for non-screen uses

## Migration Order

1. Root screen + Menu container (buildSettingsScreen, coexistence wiring, 6 menu buttons)
2. Configuration sub-screen (timezone dropdown, 3 toggles, live preview, save/load)
3. Display sub-screen (brightness slider, 2 timeout dropdowns, save/load)
4. Compass Cal sub-screen (idle/active/complete states, arc progress, calibration logic)
5. Diagnostics sub-screen (8 label pairs, frame-rate updates)
6. About sub-screen (6 label pairs, live updates)
7. Factory Reset sub-screen (warning, reset button)
8. Legacy cleanup (remove all drawSettings*, handleXxxTap, legacy helpers)
