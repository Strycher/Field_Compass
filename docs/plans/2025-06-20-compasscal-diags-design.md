# Compass Calibration (#89) & Diagnostics (#90) — Design

**Date:** 2025-06-20
**Issues:** #89, #90
**Epic:** #85 (Settings Screen System)

## Overview

Migrate compass calibration and diagnostics into Settings sub-screens. Remove SCREEN_DIAGS from the main screen rotation. Remove C long-press calibration trigger from compass screen.

## #89 — Compass Calibration (sub-screen 3)

### Idle Layout (480x320, landscape)

```
y=0    [HEADER: "COMPASS CAL"]
y=45   Status:    Calibrated          (green) or "Not calibrated" (dim)
y=75   Offsets:   X: 1.25  Y: -0.88  Z: 0.42      (or "—" if none)
y=140  [ Start Calibration ]          <- focus row 0 (cyan highlight)
y=270  [<- Back]                      <- focus row 1
```

### Navigation (Idle)

- `compassCalFocusRow`: 0 = Start Calibration, 1 = Back
- A/B cycle focus between rows 0 and 1
- C-short activates focused row:
  - Row 0: starts calibration (sets `magCalibrating = true`)
  - Row 1: returns to settings menu
- Touch: tap "Start Calibration" button or Back button
- No OK button (nothing to save from idle screen)
- Guard: if `!magAvailable`, show "IMU not detected" and disable Start button

### Calibration Active Layout

When calibration is running, the screen takes over (same as current behavior — full sprite push, bypasses zones):

```
y=0    [HEADER: "CALIBRATING"]
       ┌─────────────────────────┐
       │                         │
       │     ╭───── arc ─────╮   │
       │    ╱   progress      ╲  │
       │   │                   │ │
       │   │      12           │ │    <- countdown seconds, centered
       │   │                   │ │
       │    ╲                 ╱  │
       │     ╰───────────────╯   │
       │                         │
       └─────────────────────────┘
y=210  "Rotate device slowly 360°"
y=240  X: -12.3 to 14.1             <- live min/max
y=255  Y: -8.2 to 11.5
y=270  Z: -15.0 to 9.8
```

### Progress Ring

- **Center:** x=240, y=130 (horizontally centered, vertically offset to leave room for header and text below)
- **Outer radius:** 70px, **Inner radius:** 58px (12px thick ring)
- **Background ring:** Dark gray (0x2104), full 360°
- **Progress arc:** Green (0x03E0), sweeps from 0° (12 o'clock) clockwise
- **Angle calculation:**
  - TFT_eSPI `drawArc()` uses 0° = 6 o'clock, clockwise
  - To start at 12 o'clock: `arcStart = 180`
  - Progress: `arcEnd = (180 + progressDegrees) % 360`
  - Where `progressDegrees = (elapsed * 360) / MAG_CAL_DURATION_MS`
  - Handle wrap-around when arcEnd < arcStart (arc crosses 0°)
- **Countdown number:** Large text (size 4) centered inside the ring
- **Updated every frame** — full sprite push

### Calibration Complete

After 15 seconds:
1. Compute offsets: `(max + min) / 2.0` for each axis
2. Save to `/config/mag_cal.txt`
3. Show "CAL COMPLETE" screen with offsets for 3 seconds
4. Return to idle Compass Cal screen (which now shows updated offsets)

### Removal from Compass Screen

- Remove the `if (currentScreen == SCREEN_COMPASS)` calibration trigger from `handleButtonCLongPress()`
- Remove the calibration overlay code from `drawScreenCompass()`
- Remove the `wasCalibrating` cleanup block from `drawScreenCompass()`
- The `readIMU()` min/max tracking stays untouched (it's driven by `magCalibrating` flag, location-independent)

### Data Sources (unchanged)

| Field | Source |
|-------|--------|
| Calibration status | `magCalibrated` global |
| Offsets | `magOffsetX`, `magOffsetY`, `magOffsetZ` |
| IMU available | `magAvailable` global |
| Min/max during cal | `magCalMinX/Y/Z`, `magCalMaxX/Y/Z` |
| Save path | `/config/mag_cal.txt` |

## #90 — Diagnostics (sub-screen 4)

### Approach

Move the existing `drawScreenDiags()` content into `drawSettingsDiags(c)`. The diagnostics screen is read-only with 12 data zones — same content, just hosted inside the settings framework.

### Layout

Identical to current `drawScreenDiags()` but:
- Replace nav bar (zone 12) with settings-style Back button at y=270
- All 11 data zones remain in their current positions (y=38 through y=278, 24px line height)
- Zone-based rendering with auto-refresh (same zone keys)

### Navigation

- Read-only — no editable fields
- C-short = Back (returns to settings menu)
- Touch: Back button tap (x: 10-120, y: 278-312)
- No A/B navigation needed (no focusable rows)

### Changes to Screen Cycling

1. Remove `SCREEN_DIAGS` from the swipe/button cycling:
   - `NUM_SCREENS` decreases from 7 to 6
   - `SCREEN_DIAGS` define removed
   - `SCREEN_SETTINGS` changes from 7 to 6 (modal overlay, stays outside NUM_SCREENS)
   - Remove `case SCREEN_DIAGS:` from `drawScreen()` switch
   - Remove `case SCREEN_DIAGS:` from OLED screen draw switch
2. Update `drawOLEDScreenDiags()` → no longer called from OLED rotation
3. The `/diags` web endpoint stays untouched (it's independent)

### MagCal Text Update

The diagnostics screen currently says "None (hold C on compass)" for uncalibrated state. Update to "None (Settings > Compass Cal)" to reflect the new location.

## Implementation Notes

- Both screens follow existing settings sub-screen patterns
- `compassCalFocusRow` is a new global (like `configFocusRow`, `displayFocusRow`)
- Calibration drawing in `drawSettingsCompassCal()` uses same `magCalibrating` flag — no change to calibration state machine or `readIMU()` tracking
- The 3-second "CAL COMPLETE" display uses `delay(3000)` same as current (blocking but acceptable for a one-time calibration result)
