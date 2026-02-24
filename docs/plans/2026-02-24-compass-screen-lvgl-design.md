# #109 — LVGL Compass Screen Migration Design

**Date:** 2026-02-24
**Issue:** #109 — Migrate Compass screen with anti-aliased compass rose
**Depends on:** #108 (widget library)

## Overview

Migrate `drawScreenCompass()` and `drawCompassRose()` from the legacy TFT_eSprite + zone-tracking pipeline to LVGL-native rendering. The compass screen has two panels: a left text data column (heading, GPS, weather) and a right rotating compass rose.

## Approach: Custom Draw Event (LV_EVENT_DRAW_MAIN)

Selected over `lv_canvas` for zero additional PSRAM and idiomatic LVGL integration.

- Create a plain `lv_obj_t` sized for the rose area
- Attach `LV_EVENT_DRAW_MAIN` callback that draws directly to the layer
- Use `lv_draw_arc()`, `lv_draw_triangle()`, `lv_draw_line()` on the event's layer
- Invalidate the object when heading changes (2° threshold)
- LVGL's SW renderer provides anti-aliasing on all primitives automatically

## Screen Layout (480×320, landscape)

```
┌──────────────────────────────────────────────────────┐
│  COMPASS (fcHeader — 30px cyan bar)                  │
├──────────┬───────────────────────────────────────────┤
│ Left     │ Right Panel: Compass Rose                 │
│ 178px    │ 298px wide, custom draw event             │
│          │                                           │
│ HDG+CARD │      [lubber triangle — fixed]            │
│ Lat/Lon  │         ╱ outer ring ╲                    │
│ Alt      │        │  8 diamond   │                   │
│ Speed    │        │   needles    │                   │
│ Temp     │        │  rotate w/   │                   │
│ Forecast │         ╲  heading  ╱                     │
│ GPS Stat │          ─────────                        │
│ Time     │                                           │
├──────────┴───────────────────────────────────────────┤
│  NavBar (fcNavBar — 25px dot indicators)             │
└──────────────────────────────────────────────────────┘
```

## Left Panel — Label Widgets

| Row | Content | Font | Color | Y pos |
|-----|---------|------|-------|-------|
| Heading + Cardinal | "204° SW" | FC_FONT_XXL (28) | FC_COLOR_VALUE | 34 |
| Latitude | "Lat 39.3525N" | FC_FONT_SM (16) | dim label + green value | 72 |
| Longitude | "Lon 84.3825W" | FC_FONT_SM (16) | dim label + green value | 90 |
| Altitude | "Alt 820 ft" | FC_FONT_SM (16) | dim + green | 112 |
| Speed | "Spd 2.3 mph" | FC_FONT_SM (16) | dim + green | 132 |
| Temperature | "Temp 72.5°F" | FC_FONT_SM (16) | dim + green | 152 |
| Forecast | "↑ Fair" | FC_FONT_SM (16) | color-coded | 172 |
| GPS Status | "GPS OK Sat:8 HDOP:1.2" | FC_FONT_XS (14) | color-coded | 200 |
| Time | "3:42:15 PM" | FC_FONT_LG (20) | FC_COLOR_TEXT | 268 |

All labels use `lv_label_set_text_fmt()`. Text changes auto-invalidate only that label.

## Compass Rose — Custom Draw

Drawing primitives (all anti-aliased by LVGL's SW renderer):

1. **Outer ring**: `lv_draw_arc()` — full 360° arc, 2px width, FC_COLOR_DIM
2. **12 tick marks**: `lv_draw_line()` — cardinals longer, intercardinals shorter
3. **Degree labels**: Not drawn via layer (labels are tricky in draw callbacks). Use 4 cardinal `lv_label` objects (N/E/S/W) positioned programmatically on heading change.
4. **8 diamond needles**: `lv_draw_triangle()` × 2 per needle (tip half + tail half)
   - N=cyan, S=red, E/W=white, intercardinals=gray
   - Tail: cardinals get dark fill (0x2104), intercardinals gray
5. **Center hub**: `lv_draw_arc()` filled circle
6. **Lubber line**: `lv_draw_triangle()` — orange triangle at top, fixed position (NOT inside the rotating draw callback — separate static object)

### Rotation

- Rose rotation = `-heading` (so N points to magnetic north)
- All coordinates computed with trig from center point
- Same formula as legacy: `tipRad = radians(needleAngle + rotDeg - 90)`

### Invalidation

- Rose `lv_obj_t` is invalidated when `abs(newHeading - lastDrawnHeading) >= 2.0`
- Cardinal labels (N/E/S/W) repositioned on same threshold
- Left panel labels updated individually on sensor data change

## Coexistence Wiring

1. **`lv_timer_handler()`** — Remove `#if LVGL_TEST_MODE` guard, call every loop iteration when `lvglAvailable && !tftSleeping`
2. **`updateDisplay()`** — When `currentScreen == SCREEN_COMPASS`, skip legacy `drawScreenCompass()` call. Instead call `updateCompassData()` which sets label texts and invalidates the rose.
3. **Screen switching** — On switch TO compass: show LVGL compass objects (`lv_obj_clear_flag(LV_OBJ_FLAG_HIDDEN)`). On switch AWAY: hide them (`lv_obj_add_flag(LV_OBJ_FLAG_HIDDEN)`).
4. **Legacy screens** — All other screens continue using TFT_eSprite pipeline unchanged.
5. **Conflict prevention** — When LVGL compass is active, the sprite-based `zonePushDirty()` must not push to the same screen region. The screen-change clearing already handles this via `spr.fillSprite() + pushSprite()`.

## Version Strategy

- **v0.40.1**: Left panel labels + coexistence wiring (no rose yet)
- **v0.40.2**: Compass rose via custom draw event + cardinal labels
- **v0.41.0**: Full integration — 2Hz update loop, lubber line, cleanup, verify on device

## PSRAM Impact

- Zero additional PSRAM allocation (custom draw uses LVGL's existing render buffers)
- Left panel labels use LVGL's internal 64KB heap for widget metadata (~200 bytes per label)
- Total new LVGL heap usage: ~2KB (10 labels × ~200B)

## Flash Estimate

- Left panel labels + update logic: ~2-3KB
- Compass rose draw callback: ~3-4KB (trig math + draw descriptors)
- Coexistence wiring: ~500B
- Estimated total: +6-8KB over v0.40.0 baseline
