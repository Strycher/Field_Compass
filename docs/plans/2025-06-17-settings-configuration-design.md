# Settings: Unit & Time Configuration — Design Document

**Issue**: #98
**Date**: 2025-06-17
**Version target**: v0.30.0

## Problem

All time, temperature, and distance preferences are hardcoded in firmware. Timezone is locked to US Eastern (`GMT_OFFSET_SEC = -18000`), DST is always applied, temperature always shows Fahrenheit primary, and `useMetricUnits` has no UI to change it. Users cannot configure any of these without recompiling.

## Solution

Add a "Configuration" sub-screen to the Settings menu with four settings on a single screen: Timezone, Time Format, Temperature Units, and Distance Units. Build a reusable settings UI toolkit with touch-first navigation (Back/OK buttons) and concurrent button support. Persist all settings to SD card. Replace hardcoded timezone constants with POSIX TZ strings for automatic DST handling.

## Settings Menu Reorder

```
SETTINGS (4 items, SETTINGS_MENU_COUNT=4)
  1. Configuration      → Sub-screen 1 (this issue)
  2. Compass Cal        → Sub-screen 2 (placeholder)
  3. Operational        → Sub-screen 3 (placeholder — future: move OPS screen here)
  4. Diagnostics        → Sub-screen 4 (placeholder)
```

## Configuration Screen Layout

```
┌──────────────────────────────────────────────────┐
│ CONFIGURATION                                     │  30px header
├──────────────────────────────────────────────────┤
│                                                   │
│  Time Zone     [  US Eastern (UTC-5)         ▾ ]  │  tap → inline selector
│  Time Format   [ 12 Hour ◉ ]  [ 24 Hour ○ ]      │  tap to toggle
│  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │  separator
│  Temperature   [ °F ◉ ]      [ °C ○ ]            │  tap to toggle
│  Distance      [ Imperial ◉ ] [ Metric ○ ]       │  tap to toggle
│                                                   │
│  Preview:  2:45 PM  ·  72.5°F  ·  0.5 mi         │  live preview
│                                                   │
├──────────────────────────────────────────────────┤
│  [← Back]                                [OK ✓]  │  50px action bar
└──────────────────────────────────────────────────┘
```

### Timezone Selector
Tapping the timezone row opens an inline overlay list of ~15 presets. User taps a timezone to select it. List shows ~5 visible items with scroll support (swipe or A/B buttons).

### Live Preview
Bottom area shows current time, temperature, and a sample distance formatted with the current toggle selections. Updates in real-time as settings are toggled.

## Reusable UI Components

### Widget Functions
| Function | Purpose | Used By |
|---|---|---|
| `drawToggle(c, y, label, optA, optB, value)` | Two-option toggle row | Time/Temp/Distance |
| `drawDropdown(c, y, label, currentValue)` | Dropdown trigger row with ▾ indicator | Timezone |
| `drawSelectorOverlay(c, items[], count, selectedIdx, scrollOffset)` | Scrollable selection list overlay | Timezone picker |
| `drawActionBar(c, showOK, showBack)` | Bottom bar with Back and OK touch buttons | Every sub-screen |
| `handleSettingsTap(x, y)` | Central tap dispatcher for settings screens | Settings touch handler |

### Touch Navigation Standard (All Settings Sub-Screens)
- **Back button**: Bottom-left (x: 0-120, y: 270-320) — discard changes, return to menu
- **OK button**: Bottom-right (x: 360-480, y: 270-320) — save changes, return to menu
- **Toggle tap zones**: Each toggle option is a tap target (~200×36px)
- **Concurrent button support**: A/B = scroll/toggle, C-short = OK, C-long = Back

## Data Model

### New Global Variables
```cpp
bool use12Hour = true;                                    // Default: 12-hour
bool useFahrenheit = true;                                // Default: Fahrenheit
// bool useMetricUnits already exists (line 453, default false)
char posixTZ[48] = "EST5EDT,M3.2.0,M11.1.0";            // Default: US Eastern
char tzDisplayName[24] = "US Eastern";                    // Friendly name
```

### Timezone Preset Table
```cpp
struct TZPreset {
  const char* name;       // "US Eastern"
  const char* posix;      // "EST5EDT,M3.2.0,M11.1.0"
  int8_t stdOffset;       // -5 (for display)
};

static const TZPreset tzPresets[] = {
  {"US Eastern",    "EST5EDT,M3.2.0,M11.1.0",       -5},
  {"US Central",    "CST6CDT,M3.2.0,M11.1.0",       -6},
  {"US Mountain",   "MST7MDT,M3.2.0,M11.1.0",       -7},
  {"US Pacific",    "PST8PDT,M3.2.0,M11.1.0",       -8},
  {"US Alaska",     "AKST9AKDT,M3.2.0,M11.1.0",     -9},
  {"US Hawaii",     "HST10",                         -10},
  {"US Arizona",    "MST7",                           -7},
  {"UTC",           "UTC0",                            0},
  {"UK / Ireland",  "GMT0BST,M3.5.0/1,M10.5.0",      0},
  {"Central Europe","CET-1CEST,M3.5.0,M10.5.0/3",    1},
  {"Eastern Europe","EET-2EEST,M3.5.0/3,M10.5.0/4",  2},
  {"Japan / Korea", "JST-9",                           9},
  {"Australia East","AEST-10AEDT,M10.1.0,M4.1.0/3",  10},
  {"New Zealand",   "NZST-12NZDT,M9.5.0,M4.1.0/3",  12},
};
#define TZ_PRESET_COUNT 14
```

### Settings File: `/config/settings.txt`
```
use12Hour=1
useFahrenheit=1
useMetricUnits=0
posixTZ=EST5EDT,M3.2.0,M11.1.0
tzName=US Eastern
```

## Time Architecture Change

### Remove
- `const long GMT_OFFSET_SEC = -18000;` (line 69)
- `const int DAYLIGHT_OFFSET_SEC = 3600;` (line 70)
- All direct usage of these constants

### Replace With
- `configTime(0, 0, NTP_SERVER);` — NTP provides UTC
- `setenv("TZ", posixTZ, 1); tzset();` — POSIX handles timezone + DST
- All display code uses `localtime()` which auto-applies TZ rules
- RTC continues to store UTC (unchanged)

### GPS Time Fix
Currently (line 4040-4054), GPS time is manually offset:
```cpp
t += GMT_OFFSET_SEC + DAYLIGHT_OFFSET_SEC;
```
Replace with: set system time as UTC, let `localtime()` handle conversion for display.

## Code Changes

### New Functions
| Function | Purpose |
|---|---|
| `loadSettings()` | Read `/config/settings.txt` from SD, parse key=value, set globals |
| `saveSettings()` | Write current globals to `/config/settings.txt` |
| `drawSettingsConfig(c)` | Render the Configuration sub-screen |
| `drawToggle(...)` | Reusable toggle widget |
| `drawDropdown(...)` | Reusable dropdown trigger widget |
| `drawSelectorOverlay(...)` | Timezone selector overlay |
| `drawActionBar(...)` | Reusable Back/OK bar |
| `handleSettingsTap(x, y)` | Route taps to config screen widgets |
| `applyTimezone()` | Call `setenv("TZ", ...) + tzset()` |

### Modified Functions
| Function | Change |
|---|---|
| `setup()` | Call `loadSettings()` after SD init, before WiFi |
| `initWiFi()` | Replace `configTime(GMT_OFFSET_SEC, ...)` with UTC + POSIX TZ |
| `drawScreenEnv()` | Temperature display uses `useFahrenheit` |
| `drawScreenSettings()` | Reorder sub-screens, add Configuration case |
| `handleSettingsCSelect()` | Route to new sub-screen |
| `settingsMenuItems[]` | Reorder: Configuration, Compass Cal, Operational, Diagnostics |
| GPS time display (OPS screen) | Use `localtime()` instead of manual offset |
| GPS time sync | Set system time as UTC, remove manual offset |

### Removed
| Item | Reason |
|---|---|
| `GMT_OFFSET_SEC` | Replaced by POSIX TZ |
| `DAYLIGHT_OFFSET_SEC` | Embedded in POSIX TZ string |

## Acceptance Criteria

- [ ] Configuration screen accessible from Settings menu (sub-screen 1)
- [ ] Timezone selectable from ~14 presets with inline selector
- [ ] 12/24-hour toggle with live preview
- [ ] °F/°C toggle with live preview
- [ ] Imperial/Metric toggle with live preview
- [ ] All settings persist to `/config/settings.txt` on SD
- [ ] Settings loaded from SD on boot (defaults if file missing)
- [ ] POSIX TZ replaces hardcoded GMT_OFFSET_SEC / DAYLIGHT_OFFSET_SEC
- [ ] DST handled automatically by POSIX TZ rules
- [ ] Environment screen respects useFahrenheit setting
- [ ] All time displays use configured format and timezone
- [ ] Touch Back/OK buttons on action bar work
- [ ] Buttons A/B/C still work concurrently
- [ ] Reusable UI widgets (drawToggle, drawActionBar) available for future screens
- [ ] Compiles and uploads cleanly
