# Design: LVGL Environment + Telemetry Screens (#111)

**Date:** 2026-02-24
**Issue:** #111
**Depends on:** #108 (widget library), #110 (geocache — established coexistence pattern)

## Summary

Migrate `drawScreenEnv()` and `drawScreenTelemetry()` from the legacy TFT_eSprite zone-tracking pipeline to LVGL `lv_label` objects with the established show/hide coexistence pattern.

## Architecture

- **Two new root containers**: `envScr` and `telemetryScr` (full-screen, hidden by default)
- **Manual absolute positioning** via `lv_obj_set_pos()` — consistent with compass and geocache screens
- **No sub-screens** — both are single-view data displays
- **Same coexistence pattern** as compass/geocache: show/hide in `updateDisplay()`, route to update functions, skip legacy sprite draw

## Environment Screen (~14 LVGL objects)

### Layout
- Header ("ENVIRONMENT") at y=0 via `fcHeaderCreate()`
- 6 label-value rows starting at y=40, row height 28px
- Labels at x=10 (dim gray, FC_FONT_SM/16px)
- Values at x=80 (colored, FC_FONT_MD/18px)
- NavBar at bottom via `fcNavBarCreate()`

### Data Rows
| Row | Label | Value Source | Color Logic |
|-----|-------|-------------|-------------|
| 1 | Temp: | SHT41 or BME688, respects `useFahrenheit` | Green (default) |
| 2 | Humid: | SHT41 or BME688 | Green (default) |
| 3 | IAQ: | BME688 `envData.iaq` + accuracy text | >200 red, >100 yellow, else green |
| 4 | CO2: | BME688 `envData.co2Equivalent` | >2000 red, >1000 yellow, else green |
| 5 | Press: | BME688 `envData.pressure` (hPa + inHg) | Green (default) |
| 6 | Fcst: | `getTrendArrow()` + `weatherTrend.forecast` | "Storm"=red, "Rain"/"Snow"=yellow |

### Conditional States
- **No BME688**: IAQ/CO2/Pressure/Forecast rows show "N/A (no BME688)" in dim gray
- **No sensors at all**: Single error label "No env sensors" in red, centered

## Telemetry Screen (~28 LVGL objects)

### Layout
- Header ("TELEMETRY") at y=0 via `fcHeaderCreate()`
- GPS section label "GPS" at y=38 (cyan, centered, FC_FONT_SM)
- GPS data rows: 4 rows × 2 columns, starting y=56, row height 28px
- Divider line at y=172 (1px gray)
- IMU section label "IMU" at y=178 (cyan, centered, FC_FONT_SM)
- IMU data rows: 2 rows × 2 columns, starting y=196, row height 28px
- NavBar at bottom

### Column Geometry
- Left label x=20, left value x=110
- Right label x=250, right value x=350

### GPS Data Rows (when valid)
| Row | Left Label/Value | Right Label/Value |
|-----|-----------------|-------------------|
| 1 | Lat: / 39.3525N | Lon: / 84.3825W |
| 2 | Alt: / 250.3 ft | Spd: / 0.0 mph |
| 3 | Sat: / 8 | HDOP: / 1.2 |
| 4 | Status: / Fix OK (TTFF 32s) | — |

### GPS States (show/hide groups)
- **Valid**: Show all 4 data rows
- **Acquiring**: Hide data rows, show "Acquiring fix..." (yellow) + elapsed time + sat count
- **No GPS**: Hide data rows, show "No GPS data" (red) + "Check connection" (dim)

### IMU Data Rows
| Row | Left Label/Value | Right Label/Value |
|-----|-----------------|-------------------|
| 1 | Hdg: / 203 SW | Roll: / -2 deg |
| 2 | Pitch: / 1 deg | Accel: / 9.81 m/s2 |

### IMU Unavailable State
- Hide data rows, show "IMU not available" (red, centered)

## Data Update Functions

### `updateEnvData()`
- Format all 6 value labels using `lv_label_set_text_fmt()`
- Set value colors via `lv_obj_set_style_text_color()` based on thresholds
- Handle BME/SHT availability by showing/hiding relevant labels

### `updateTelemetryData()`
- Format GPS values respecting `useMetricUnits`
- Handle 3 GPS states by showing/hiding label groups
- Format IMU values, handle IMU unavailable state

## Object Count Impact

| Screen | New Objects | Running Total |
|--------|-----------|---------------|
| Compass | ~40 | ~40 |
| Geocache | ~178 | ~218 |
| **Environment** | **~14** | **~232** |
| **Telemetry** | **~28** | **~260** |

Well within 96KB LV_MEM_SIZE (which handles ~178 geocache objects + arc drawing).

## Coexistence Wiring

Expand the existing LVGL routing block in `updateDisplay()`:

```cpp
// Show/hide all LVGL screens
if (envScr) {
  if (currentScreen == SCREEN_ENV)
    lv_obj_clear_flag(envScr, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(envScr, LV_OBJ_FLAG_HIDDEN);
}
if (telemetryScr) {
  // same pattern
}

// Route to LVGL update functions
if ((currentScreen == SCREEN_COMPASS && compassScr) ||
    (currentScreen == SCREEN_GEOCACHE && geocacheScr) ||
    (currentScreen == SCREEN_ENV && envScr) ||
    (currentScreen == SCREEN_TELEMETRY && telemetryScr)) {
  // ... call appropriate update function
}
```

## Risk Assessment

- **Low risk**: No custom draw callbacks, animations, or complex interactions — just labels
- **No PSRAM impact**: Labels use LVGL's internal heap only
- **Heap safe**: ~42 new objects is modest vs geocache's 178
- **No new dependencies**: All data sources already exist and are read by legacy code
