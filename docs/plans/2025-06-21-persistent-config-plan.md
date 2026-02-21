# Persistent Configuration with Factory Reset — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a Factory Reset menu item to Settings that deletes `/config/settings.txt` and restores all user preferences to compiled defaults.

**Architecture:** New `factoryReset()` function resets globals + deletes SD file. New sub-screen 6 with confirmation UI. Menu grows from 5→6 items. Existing `loadSettings()`/`saveSettings()` unchanged.

**Tech Stack:** Arduino/ESP32-S3, TFT_eSPI sprite rendering, zone-based dirty tracking

---

## Task 1: Add `factoryReset()` function and update menu — v0.35.1

### 1a. Add `factoryReset()` after `saveSettings()` (~line 2506)

```cpp
// Factory reset — delete settings file and restore compiled defaults (#104)
void factoryReset() {
  // Delete stored settings
  if (sdHealth.available && SD.exists("/config/settings.txt")) {
    SD.remove("/config/settings.txt");
  }

  // Restore compiled defaults
  useFahrenheit    = true;
  use12Hour        = true;
  useMetricUnits   = false;
  strncpy(posixTZ, "EST5EDT,M3.2.0,M11.1.0", sizeof(posixTZ) - 1);
  strncpy(tzDisplayName, "US Eastern", sizeof(tzDisplayName) - 1);
  tzSelectedIndex  = 0;
  tftBrightness    = 255;
  tftSleepMs       = 0;
  oledSleepMs      = 300000;

  // Apply
  applyTimezone();
  analogWrite(TFT_BL, tftBrightness);

  logPrintln("[SETTINGS] Factory reset — all settings restored to defaults");
}
```

### 1b. Update `SETTINGS_MENU_COUNT` (line 245)

Change:
```cpp
#define SETTINGS_MENU_COUNT 5  // Configuration, Display, Compass Cal, Diagnostics, About (#91/#92)
```
To:
```cpp
#define SETTINGS_MENU_COUNT 6  // Configuration, Display, Compass Cal, Diagnostics, About, Factory Reset (#104)
```

### 1c. Add "Factory Reset" to `settingsMenuItems[]` (line 3995, after "About")

```cpp
static const char* settingsMenuItems[] = {
  "Configuration",
  "Display",
  "Compass Cal",
  "Diagnostics",
  "About",
  "Factory Reset"       // #104
};
```

### 1d. Add sub-screen case 6 to `drawScreenSettings()` dispatch (~line 6718)

```cpp
  switch (settingsSubScreen) {
    case 1:  drawSettingsConfig(c);                        break;
    case 2:  drawSettingsDisplay(c);                       break;
    case 3:  drawSettingsCompassCal(c);                    break;
    case 4:  drawSettingsDiags(c);                         break;
    case 5:  drawSettingsAbout(c);                         break;
    case 6:  drawSettingsFactoryReset(c);                  break;  // #104
    default: drawSettingsMenu(c);                          break;
  }
```

### 1e. Create `drawSettingsFactoryReset()` (before `drawSettingsMenu()`)

```cpp
// Factory Reset confirmation screen (#104)
void drawSettingsFactoryReset(TFT_eSprite* c) {
  char buf[64];

  // Header
  if (zoneMark(0, 0, SCREEN_W, 30, "FRESET_HDR"))
    drawHeader(c, "FACTORY RESET");

  // Warning message
  if (zoneMark(20, 80, SCREEN_W - 40, 30, "FRESET_WARN")) {
    c->setTextColor(COLOR_WARN);
    c->setTextSize(2);
    c->setCursor(20, 80);
    c->print("Reset all settings to");
    c->setCursor(20, 104);
    c->print("factory defaults?");
  }

  // Info: calibration preserved
  if (zoneMark(20, 145, SCREEN_W - 40, 20, "FRESET_INFO")) {
    c->setTextColor(COLOR_DIM);
    c->setTextSize(2);
    c->setCursor(20, 145);
    c->print("Compass calibration will");
    c->setCursor(20, 169);
    c->print("be preserved.");
  }

  // Action bar: Back / Reset
  if (zoneMark(0, 270, SCREEN_W, 50, "FRESET_BAR")) {
    c->fillRect(0, 270, SCREEN_W, 50, 0x18C3);
    c->setTextSize(2);

    // Back button (left)
    c->fillRoundRect(10, 278, 110, 34, 6, 0x4208);
    c->setTextColor(COLOR_TEXT);
    c->setCursor(22, 286);
    c->print("<- Back");

    // Reset button (right) — red to indicate destructive action
    c->fillRoundRect(360, 278, 110, 34, 6, 0x8000);  // Dark red
    c->setTextColor(COLOR_TEXT);
    c->setCursor(378, 286);
    c->print("RESET");
  }
}
```

### Compile, verify menu shows 6 items and Factory Reset screen renders. Commit.

---

## Task 2: Wire up button and tap handling — v0.35.2

### 2a. Add button C-short handler for sub-screen 6 in `handleSettingsCSelect()` (after the About case, ~line 4241)

After:
```cpp
  // About: C-short = Back (read-only, no save) (#92)
  if (settingsSubScreen == 5) {
    settingsSubScreen = 0;
    ...
  }
```
Add:
```cpp
  // Factory Reset (6): C-short = Back (no action, just confirmation screen) (#104)
  if (settingsSubScreen == 6) {
    settingsSubScreen = 0;
    logPrintln("[SETTINGS] Back (Factory Reset) via C");
    if (spriteAvailable) forceDisplayUpdate = true;
    return;
  }
```

### 2b. Add C-long-press handler for sub-screen 6 in `handleSettingsCLongPress()` (~line 4274)

In the `else` branch at the bottom (currently just `settingsSubScreen = 0`), change it to handle sub-screen 6 specifically:

Replace:
```cpp
    } else {
      settingsSubScreen = 0;
    }
```
With:
```cpp
    } else {
      settingsSubScreen = 0;
    }
```

Note: C-long-press = back navigation (existing behavior). The `else` branch already covers sub-screens 4, 5, and now 6.

### 2c. Add tap handling for sub-screen 6 in `handleTap()` (after About tap handler, ~line 4474)

After the About sub-screen taps block, add:
```cpp
  // Factory Reset sub-screen taps (#104)
  if (currentScreen == SCREEN_SETTINGS && settingsSubScreen == 6) {
    // Back button tap
    if (x >= 10 && x <= 120 && y >= 278 && y <= 312) {
      settingsSubScreen = 0;
      logPrintln("[SETTINGS] Back (Factory Reset)");
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    // Reset button tap
    if (x >= 360 && x <= 470 && y >= 278 && y <= 312) {
      factoryReset();
      settingsSubScreen = 0;
      logPrintln("[SETTINGS] Factory reset confirmed via tap");
      if (spriteAvailable) forceDisplayUpdate = true;
    }
    return;
  }
```

### 2d. Add button A/B handling for sub-screen 6

Sub-screen 6 is read-only (no focus rows to navigate). A/B buttons do nothing. No changes needed — the existing `handleSettingsButtons()` only handles sub-screens 0-3, and sub-screen 6 falls through to no action (same as 4 and 5).

### Compile, verify all navigation paths work:
- Menu select → confirmation screen
- Back (tap/C-short/C-long) → returns to menu
- Reset tap → calls factoryReset(), returns to menu

### Commit.

---

## Task 3: v0.36.0 release — upload, test, close

### 3a. Bump `FW_VERSION` to `"0.36.0"` (line 28)

### 3b. Compile, upload

### 3c. Test matrix:
- [ ] Settings menu shows 6 items: Config, Display, Compass Cal, Diagnostics, About, Factory Reset
- [ ] Menu scrolls with A/B buttons through all 6 items + Back row
- [ ] Factory Reset screen shows warning text and Back/Reset buttons
- [ ] Back button (tap) returns to menu without resetting
- [ ] C-short returns to menu without resetting
- [ ] C-long returns to menu without resetting
- [ ] Reset button (tap) resets all settings and returns to menu
- [ ] After reset: time format = 12h, temp = F, distance = imperial, TZ = US Eastern
- [ ] After reset: brightness = 255, TFT sleep = never, OLED sleep = 5 min
- [ ] After reset: `/config/settings.txt` deleted from SD
- [ ] After reset: compass calibration still intact
- [ ] Reboot after reset: device starts with compiled defaults (no settings file)
- [ ] Change a setting, save, reboot: setting persists (confirms save still works)

### 3d. Commit, tag `v0.36.0`, push

### 3e. Close #104, update project board to Done

---

## Versioning Plan

| Version | Description |
|---------|-------------|
| v0.35.1 | Add factoryReset(), menu item, confirmation screen |
| v0.35.2 | Wire up button/tap handlers |
| v0.36.0 | Feature complete — minor bump |
