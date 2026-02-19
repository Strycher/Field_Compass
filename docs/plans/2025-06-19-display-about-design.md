# Display Settings (#91) & About/System Info (#92) — Design

**Date:** 2025-06-19
**Issues:** #91, #92 (absorbs #93 OPS Screen Disposition)
**Epic:** #85 (Settings Screen System)

## Menu Structure

Expand settings menu from 4 to 5 items. Insert Display at index 1 and About at index 4. Shift existing placeholders.

```
Index  Name            SubScreen  Status
-----  --------------  ---------  --------
0      Configuration   1          Existing (#98)
1      Display         2          NEW (#91)
2      Compass Cal     3          Placeholder (#89)
3      Diagnostics     4          Placeholder (#90)
4      About           5          NEW (#92)
```

- `SETTINGS_MENU_COUNT` changes from 4 to 5
- Menu row height stays at 40px (5 x 40 = 200, fits above action bar at y=270)
- Operational placeholder removed — its content merges into About

## #91 — Display Settings (sub-screen 2)

### Global Variables

```cpp
uint8_t  tftBrightness = 255;      // PWM 25-255, step 25
uint32_t tftSleepMs    = 0;        // 0 = never (default)
uint32_t oledSleepMs   = 300000;   // 5 minutes (default)
int      displayFocusRow = -1;     // 0=Brightness, 1=TFT timeout, 2=OLED timeout, 3=Back, 4=OK
```

### Layout (480x320, landscape)

```
y=0    [HEADER: "DISPLAY"]
y=45   [Brightness]  [===████████████====]  [200]     <- slider bar + numeric
y=95   [TFT Sleep ]  [  Never  ]                      <- cycle selector
y=145  [OLED Sleep]  [ 5 min   ]                      <- cycle selector
y=270  [<- Back]                       [OK  >]        <- action bar
```

### Brightness Slider

- Horizontal bar: x=160, y=50, w=240, h=24
- Fill proportional to value (25-255)
- Numeric readout right of bar (e.g., "255")
- **Touch:** Tap/drag on bar sets brightness proportionally (map touch x to 25-255)
- **Buttons:** A/B when focused on row 0 decrease/increase by 25 (clamp 25-255)
- **Live preview:** `analogWrite(TFT_BL, tftBrightness)` on every change
- Minimum 25 prevents accidental blackout

### Timeout Selectors

Preset arrays (shared between TFT and OLED):

```cpp
// TFT presets (includes Never)
const uint32_t tftTimeoutPresets[]  = {0, 60000, 120000, 300000, 600000, 900000, 1800000};
const char*    tftTimeoutLabels[]   = {"Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
const int      TFT_TIMEOUT_COUNT   = 7;

// OLED presets (no Never — burn-in risk)
const uint32_t oledTimeoutPresets[] = {60000, 120000, 300000, 600000, 900000, 1800000};
const char*    oledTimeoutLabels[]  = {"1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
const int      OLED_TIMEOUT_COUNT  = 6;
```

- **Touch:** Tap value area cycles to next preset
- **Buttons:** C-short when focused cycles to next preset (wraps)
- Display: show current label (e.g., "5 min")

### Navigation

Same pattern as Configuration screen:
- `displayFocusRow`: 0=Brightness, 1=TFT Timeout, 2=OLED Timeout, 3=Back, 4=OK
- A/B cycle focus, C-short activates focused row
- Action bar: Back (discard, reload) + OK (save)

### Persistence

Add to `saveSettings()` / `loadSettings()`:
- `tftBrightness=255`
- `tftSleepMs=0`
- `oledSleepMs=300000`

### Runtime Integration

- `wakeTFT()`: use `tftBrightness` instead of `TFT_BL_PWM` constant
- `setup()`: use `tftBrightness` after `loadSettings()` for initial brightness
- Sleep timeout check: use `tftSleepMs` / `oledSleepMs` instead of compile-time `#define`s

## #92 — About / System Info (sub-screen 5)

### Layout

```
y=0    [HEADER: "ABOUT"]
y=45   Version:   0.30
y=75   Uptime:    2d 04:23:15
y=105  Heap:      142 / 328 KB
y=135  PSRAM:     1.6 / 2.0 MB
y=175  Battery:   87% (3.92V)
y=205  WiFi:      192.168.1.42
y=270  [<- Back]                                      <- action bar (no OK)
```

### Behavior

- Read-only, no editable fields
- Auto-refreshes via zone keys that include displayed values
- Back only (no OK — nothing to save)
- C-short = Back (auto-focused, same as placeholder pattern)
- No A/B navigation needed

### Data Sources

| Field | Source |
|-------|--------|
| Version | `FW_VERSION` macro |
| Uptime | `millis()` formatted as days + HH:MM:SS |
| Heap | `ESP.getFreeHeap()` / `ESP.getHeapSize()` |
| PSRAM | `ESP.getFreePsram()` / `ESP.getPsramSize()` |
| Battery | `batteryPercent` / `batteryVoltage` globals |
| WiFi | `WiFi.localIP()` or "Disconnected" |

## Issue Disposition

- **#91** — Implemented by this work
- **#92** — Implemented by this work
- **#93 (OPS Screen Disposition)** — Absorbed into #92, close as duplicate/merged
