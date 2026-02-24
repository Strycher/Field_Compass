# LVGL Theme & Anti-Aliased Fonts Design (#107)

**Date:** 2026-02-24
**Issue:** #107 — LVGL: Create theme with anti-aliased fonts and Field Compass color palette
**Depends on:** #105 (done), #106 (done)

## Overview

Primary motivation for the LVGL migration: replace jagged bitmap fonts (TFT_eSPI `setTextSize()`) with smooth anti-aliased Montserrat fonts. Define an LVGL theme mapping the existing 7-color palette to reusable styles.

## Approach

**Approach A: Default Theme + Style Overrides.** Use LVGL's built-in `lv_theme_default_init()` as the base (dark mode, cyan primary, green secondary), then layer named `lv_style_t` constants for Field Compass-specific color roles. The default theme provides free button borders, focus rings, and scrollbar styling. Named styles are decoupled from the base theme — shifting to a fully custom `apply_cb` later requires only replacing the init call.

## Font System

### 7 Montserrat sizes enabled in lv_conf.h

| Size | Alias | Purpose | Flash Cost |
|------|-------|---------|------------|
| 14px | `FC_FONT_XS` | Fine labels, status | 0 (already linked) |
| 16px | `FC_FONT_SM` | Body text, list items | +101 KB |
| 18px | `FC_FONT_MD` | Standard values (theme default) | +119 KB |
| 20px | `FC_FONT_LG` | Emphasized values | +136 KB |
| 24px | `FC_FONT_XL` | Section headers | +177 KB |
| 28px | `FC_FONT_XXL` | Large headers | +228 KB |
| 32px | `FC_FONT_HERO` | Hero values (heading, main telemetry) | +278 KB |

**Total additional flash: ~1,039 KB (~1 MB)**
All uncompressed (4bpp, 16 shading levels) for consistent rendering.

### TFT_eSPI to LVGL mapping

| TFT setTextSize | Approx px height | LVGL equivalent |
|-----------------|-------------------|-----------------|
| 1 | ~8px | FC_FONT_XS (14) |
| 2 | ~16px | FC_FONT_SM/MD (16-18) |
| 3 | ~24px | FC_FONT_XL (24) |
| 4 | ~32px | FC_FONT_HERO (32) |

## Color System

Convert existing RGB565 palette to LVGL `lv_color_hex()` (RGB888):

| Name | RGB565 | RGB888 | LVGL Define |
|------|--------|--------|-------------|
| BG | 0x0000 | 0x000000 | `FC_COLOR_BG` |
| TEXT | 0xFFFF | 0xFFFFFF | `FC_COLOR_TEXT` |
| HEADER | 0x07FF | 0x00FFFF | `FC_COLOR_HEADER` |
| VALUE | 0x07E0 | 0x00FF00 | `FC_COLOR_VALUE` |
| WARN | 0xFD20 | 0xFF6400 | `FC_COLOR_WARN` |
| ERROR | 0xF800 | 0xFF0000 | `FC_COLOR_ERROR` |
| DIM | 0x7BEF | 0x7B7B7B | `FC_COLOR_DIM` |

Existing RGB565 `#defines` remain for legacy TFT_eSprite pipeline during migration.

## Named Styles

7 reusable `static lv_style_t` globals initialized in `initFCTheme()`:

| Style | Text Color | Font | Use Case |
|-------|-----------|------|----------|
| `fcStyleHeader` | Cyan | XL (24) | Screen titles, section headers |
| `fcStyleValue` | Green | LG (20) | Sensor readouts, data values |
| `fcStyleHero` | Green | HERO (32) | Large compass heading, main number |
| `fcStyleBody` | White | MD (18) | Default body text |
| `fcStyleLabel` | Gray | SM (16) | Secondary labels, units |
| `fcStyleWarn` | Orange | MD (18) | Warning messages |
| `fcStyleError` | Red | MD (18) | Error messages |

## Theme Initialization

```c
void initFCTheme() {
    // 1. Init default theme (dark, cyan primary, green secondary)
    lv_theme_t* theme = lv_theme_default_init(
        lvglDisplay, FC_COLOR_HEADER, FC_COLOR_VALUE,
        true, FC_FONT_MD);
    lv_display_set_theme(lvglDisplay, theme);

    // 2. Init each named style with color + font
    lv_style_init(&fcStyleHeader);
    lv_style_set_text_color(&fcStyleHeader, FC_COLOR_HEADER);
    lv_style_set_text_font(&fcStyleHeader, FC_FONT_XL);
    // ... repeat for all 7 styles
}
```

Called from `initLVGL()` after display/indev setup, before test UI.

## Test Screen

When `LVGL_TEST_MODE=1`: renders 7 rows demonstrating each style on black background. Each row shows the style name and a sample string at its designated font size and color. Held for 3 seconds at boot.

## Version Strategy

| Step | Tag | Content |
|------|-----|---------|
| Enable 6 fonts in lv_conf.h, compile | v0.38.1 | Font enablement |
| Add FC_COLOR/FC_FONT defines + initFCTheme() + named styles | v0.38.2 | Theme system |
| Replace test UI with font/color demo, upload, verify | v0.39.0 | Complete #107 |

## Flash Budget

- Current (v0.38.0): 1,646,810 bytes (40% of 4MB)
- Projected (v0.39.0): ~2,700,000 bytes (66% of 4MB, 77% of 3.5MB budget)
- Remaining headroom: ~800 KB for future screens (#108-#114)

## Migration Path to Custom Theme

If the default theme needs replacing:
1. Create `lv_theme_create()` + custom `apply_cb`
2. Move style assignments into the callback
3. All `FC_FONT_*`, `FC_COLOR_*`, and `fcStyle*` constants unchanged
4. Zero impact on screens already using named styles
