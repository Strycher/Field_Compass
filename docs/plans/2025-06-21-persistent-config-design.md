# #104 Design: Persistent Configuration with Factory Reset

**Date:** 2025-06-21
**Issue:** #104
**Status:** Approved

## Context

Settings persistence via `/config/settings.txt` on SD is already functional —
`loadSettings()` runs at boot and `saveSettings()` writes on every toggle change.
The key=value parser falls back to compiled defaults for any missing key, making
the format inherently forward-compatible.

This issue formalizes the lifecycle by adding a Factory Reset mechanism.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Factory Reset scope | Preferences only | Mag calibration is hardware-specific; re-running cal is trivial via long-press-C |
| Atomic writes | No (keep current) | File is ~200 bytes, writes in <1ms, `loadSettings()` handles partial files gracefully |
| Settings versioning | No | Key=value parser is inherently forward-compatible; missing keys get defaults |
| Reset UI location | 6th top-level Settings menu item | Visible, independent, room for confirmation flow |
| Storage medium | SD only | Already working; FRAM reserved for high-frequency ring buffers |

## Implementation

### 1. Factory Reset function

```
void factoryReset():
  - SD.remove("/config/settings.txt")
  - Reset globals to compiled defaults:
      useFahrenheit = true, use12Hour = true, useMetricUnits = false
      posixTZ = "EST5EDT,M3.2.0,M11.1.0", tzDisplayName = "US Eastern"
      tzSelectedIndex = 0, tftBrightness = 200
      tftSleepMs = 300000, oledSleepMs = 60000
  - applyTimezone()
  - Apply tftBrightness via analogWrite
  - Log "[SETTINGS] Factory reset complete"
```

### 2. Settings menu update

- `SETTINGS_MENU_COUNT` 5 -> 6
- Add "Factory Reset" to `settingsMenuItems[]`
- New sub-screen index for factory reset confirmation

### 3. Confirmation screen

`drawSettingsFactoryReset(TFT_eSprite* c)`:
- Header: "FACTORY RESET"
- Warning text in COLOR_WARN: "Reset all settings to defaults?"
- Info text in COLOR_DIM: "Calibration will be preserved."
- Action bar: Back / Reset

### 4. Button handling

New case in settings button handler for factory reset sub-screen:
- Button A / Back tap: return to menu
- Button C / OK tap: call `factoryReset()`, return to menu

### 5. No changes to loadSettings/saveSettings

Already correct. No version tracking, no atomic writes.

## Not Included (YAGNI)

- Settings version number
- Atomic write-rename pattern
- Mag calibration reset
- FRAM-based settings storage
