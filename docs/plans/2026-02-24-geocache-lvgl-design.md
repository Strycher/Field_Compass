# #110 LVGL Geocache Screen Migration — Design

**Date:** 2026-02-24
**Issue:** #110 — LVGL: Migrate Geocache screen (Nav, List, Details sub-screens)
**Depends on:** #108 (LVGL Widget Library), #109 (LVGL Compass Screen)

## Overview

Migrate `drawScreenGeocache()` and its 3 sub-screens from the legacy TFT_eSprite pipeline to LVGL widgets. This is the first LVGL migration with sub-screen switching.

## Architecture

**3 static containers** — pre-built at init, shown/hidden based on `geocacheSubScreen`:

```
geocacheScr (480×320, root, hidden when not SCREEN_GEOCACHE)
├── geocacheNavCtr     (480×320) — Nav sub-screen (sub 0)
├── geocacheListCtr    (480×320) — List sub-screen (sub 1)
└── geocacheDetailsCtr (480×320) — Details sub-screen (sub 2)
```

Sub-screen switching: `updateGeocacheData()` shows/hides containers based on `geocacheSubScreen`. Existing button handlers continue to set `geocacheSubScreen` — no LVGL events needed for transitions.

## Nav Sub-screen (geocacheNavCtr)

```
┌──────────────────────────────────────────────────┐
│ fcHeader: "GEOCACHE"                         [⚙] │ 0-29
├──────────────────────────────────────────────────┤
│ lblCacheName (centered, white, FC_FONT_LG)       │ 33
│ lblDistance  lblDT "D:2.5 T:3.0"  lblBearing     │ 57
│                                                  │
│         ┌─────────────────┐                      │
│         │  navGraphicObj  │ custom draw callback  │ 80-190
│         │  triangle OR    │                      │
│         │  search zone    │                      │
│         └─────────────────┘                      │
│                                                  │
│ lblAccuracy "+/-15ft" (color-coded)              │ 195
│ lblHint "Look behind the..." (dim)               │ 215
├──────────────────────────────────────────────────┤
│ fcNavBar (5 dots)                                │ 295-319
└──────────────────────────────────────────────────┘
```

### Custom Draw (navGraphicObj)

Single `LV_EVENT_DRAW_MAIN` callback:
- **Not in search zone**: Two `lv_draw_triangle()` calls for direction arrow (same math as legacy `drawNavTriangle` — tip, two rear corners, rear center notch)
- **In search zone**: `lv_draw_arc()` for pulsing circle. LVGL animation (`lv_anim_t`) drives radius variable, triggers `lv_obj_invalidate()` on each step
- **Invalidation**: When bearing/heading changes ≥2° or search zone state toggles

## List Sub-screen (geocacheListCtr)

```
┌──────────────────────────────────────────────────┐
│ fcHeader: "CACHE LIST"              [N/Total][⚙] │ 0-29
├──────────────────────────────────────────────────┤
│ listScrollCtr (scrollable, 480×228)              │ 33-261
│  ├── row[0]: [>] 1.2mi CacheName     * D:2 T:3  │
│  ├── row[1]: [ ] 3.4mi AnotherCache    D:1 T:2  │
│  └── row[N]: ... (up to MAX_CACHES=20)           │
├──────────────────────────────────────────────────┤
│ lblHints "[A]Up [B]Down [C]Select [C+]Details"   │ 265
├──────────────────────────────────────────────────┤
│ fcNavBar                                         │ 295-319
└──────────────────────────────────────────────────┘
```

### Row Structure

Each row is a 460×28 container with child labels:
- Selector ">" (cyan when highlighted, else hidden)
- Distance (green, FC_FONT_SM)
- Name (white, FC_FONT_SM, truncated to ~16 chars)
- Found badge "*" (green, hidden if not found)
- D/T (dim, FC_FONT_XS)

Pre-create all 20 rows; hide unused ones (when `cacheListCount < MAX_CACHES`). Scrolling via `lv_obj_scroll_to_view(row[highlightIndex])`.

## Details Sub-screen (geocacheDetailsCtr)

```
┌──────────────────────────────────────────────────┐
│ fcHeader: "CACHE DETAILS"           [N/Total][⚙] │ 0-29
├──────────────────────────────────────────────────┤
│ lblDetailName (white, FC_FONT_LG)                │ 35
│ lblDetailGC "GC12345" (cyan, FC_FONT_MD)         │ 57
│ lblDetailCoords "39.3525N 84.3825W" (green)      │ 79
│ lblDetailDT "Difficulty: 2.5  Terrain: 3.0"      │ 97
│ lblDetailDist "1.2 mi  Bearing: 204°"            │ 119
│ lblDetailHintLabel "Hint:" (dim)                 │ 141
│ lblDetailHint (wrapped, dim, FC_FONT_SM)         │ 159
│ lblDetailFound "[* FOUND]" / "[ NOT FOUND ]"     │ 200
├──────────────────────────────────────────────────┤
│ lblHints "[A]Prev [B]Next [C]Toggle [C+]Back"    │ 265
├──────────────────────────────────────────────────┤
│ fcNavBar                                         │ 295-319
└──────────────────────────────────────────────────┘
```

## Coexistence Wiring

Same pattern as compass screen in `updateDisplay()`:

```cpp
if (geocacheScr) {
    if (currentScreen == SCREEN_GEOCACHE)
        lv_obj_clear_flag(geocacheScr, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(geocacheScr, LV_OBJ_FLAG_HIDDEN);
}

if (currentScreen == SCREEN_GEOCACHE && geocacheScr) {
    updateGeocacheData();
    // skip legacy drawScreenGeocache()
}
```

Legacy functions remain as fallback when `geocacheScr == NULL`.

## Data Update Flow

`updateGeocacheData()` checks `geocacheSubScreen` and updates only visible labels:
- **Sub 0 (Nav)**: cache name, distance, bearing, accuracy, hint, D/T, invalidate nav graphic
- **Sub 1 (List)**: distance values in all visible rows, highlight/scroll state, found badges
- **Sub 2 (Details)**: all detail labels for `cacheList[listHighlightIndex]`

## Button/Touch Handling

No changes to existing button handlers. They set `geocacheSubScreen`, `listHighlightIndex`, `selectedCacheIndex`, etc. The LVGL update function reads these state variables.

No geocache-specific touch handling exists (only global gear icon + swipe). No changes needed.

## Memory Estimate

- ~80 LVGL objects (40 labels + 3 containers + 20 row containers + headers/navbars)
- ~12-16KB from LVGL's 64KB internal heap
- Zero additional PSRAM (custom draw uses existing render buffers)

## Implementation Steps

1. **Build geocache screen** — `buildGeocacheScreen()` in `initLVGL()`, creates all 3 sub-screen containers with their labels/widgets
2. **Nav custom draw** — `geocacheNavDrawCb()` for triangle + search zone circle
3. **Data update function** — `updateGeocacheData()` populates labels from sensor/GPS data
4. **Coexistence wiring** — show/hide in `updateDisplay()`, skip legacy when LVGL active
5. **Verify** — all 3 sub-screens render, button navigation works, found toggle persists
