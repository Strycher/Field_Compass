# LVGL Theme & Anti-Aliased Fonts Implementation Plan (#107)

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable 7 anti-aliased Montserrat font sizes and create a Field Compass LVGL theme with named styles mapping the existing 7-color palette.

**Architecture:** Default theme base (`lv_theme_default_init()`) with 7 `lv_style_t` named styles. Font/color `#defines` for semantic usage. Coexistence with existing RGB565 TFT_eSprite pipeline.

**Tech Stack:** LVGL 9.5.0, Montserrat fonts (built-in), ESP32-S3 4MB Flash

---

### Task 1: Enable Montserrat fonts in lv_conf.h — v0.38.1

**Files:**
- Modify: `C:\Users\stryc\OneDrive\Documents\Arduino\libraries\lv_conf.h` (lines 653-665)

**Step 1: Enable 6 additional font sizes**

In `lv_conf.h`, change lines 657-665 from `0` to `1` for these sizes:

```c
#define LV_FONT_MONTSERRAT_14 1   /* already enabled */
#define LV_FONT_MONTSERRAT_16 1   /* NEW: +101KB */
#define LV_FONT_MONTSERRAT_18 1   /* NEW: +119KB */
#define LV_FONT_MONTSERRAT_20 1   /* NEW: +136KB */
#define LV_FONT_MONTSERRAT_24 1   /* NEW: +177KB */
#define LV_FONT_MONTSERRAT_28 1   /* NEW: +228KB */
#define LV_FONT_MONTSERRAT_32 1   /* NEW: +278KB */
```

All other sizes remain `0`. No other changes to lv_conf.h.

**Step 2: Bump version to 0.38.1**

In `Field_Compass.ino` line 28, change:
```c
#define FW_VERSION "0.38.1"
```

**Step 3: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Flash should be ~2.6-2.7MB (current 1.6MB + ~1MB fonts).
Record exact flash size for commit message.

**Step 4: Commit and tag**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.38.1 — Enable 7 Montserrat font sizes in lv_conf.h (#107)

Enable 14/16/18/20/24/28/32px Montserrat fonts for anti-aliased text.
Flash: <ACTUAL_SIZE> bytes (+<DELTA> from v0.38.0)"
git tag -a v0.38.1 -m "v0.38.1 — Enable 7 Montserrat fonts (#107)"
```

Note: lv_conf.h is outside the repo (Arduino libraries dir) — not tracked in git.
Only Field_Compass.ino (version bump) is committed.

---

### Task 2: Add theme system — FC_COLOR, FC_FONT defines, initFCTheme(), named styles — v0.38.2

**Files:**
- Modify: `C:\Dev\Field_Compass\Field_Compass\Field_Compass.ino`

**Step 1: Add LVGL color defines after existing RGB565 colors**

After line 216 (`#define COLOR_DIM 0x7BEF // Gray`), add:

```c
// LVGL color palette — RGB888 equivalents of RGB565 defines above (#107)
#define FC_COLOR_BG       lv_color_hex(0x000000)   // Black
#define FC_COLOR_TEXT     lv_color_hex(0xFFFFFF)   // White
#define FC_COLOR_HEADER   lv_color_hex(0x00FFFF)   // Cyan
#define FC_COLOR_VALUE    lv_color_hex(0x00FF00)   // Green
#define FC_COLOR_WARN     lv_color_hex(0xFF6400)   // Orange
#define FC_COLOR_ERROR    lv_color_hex(0xFF0000)   // Red
#define FC_COLOR_DIM      lv_color_hex(0x7B7B7B)   // Gray
```

**Step 2: Add font size aliases after LVGL color defines**

Immediately after the FC_COLOR block:

```c
// LVGL font aliases — semantic sizes for Field Compass UI (#107)
#define FC_FONT_XS    &lv_font_montserrat_14   // Fine labels, status text
#define FC_FONT_SM    &lv_font_montserrat_16   // Body text, list items
#define FC_FONT_MD    &lv_font_montserrat_18   // Standard values (theme default)
#define FC_FONT_LG    &lv_font_montserrat_20   // Emphasized values
#define FC_FONT_XL    &lv_font_montserrat_24   // Section headers
#define FC_FONT_XXL   &lv_font_montserrat_28   // Large headers
#define FC_FONT_HERO  &lv_font_montserrat_32   // Hero values (heading, telemetry)
```

**Step 3: Add named style globals after LVGL input device globals**

After line 81 (`static lv_group_t* lvglGroup = NULL;`), add:

```c
// LVGL named styles — Field Compass theme (#107)
static lv_style_t fcStyleHeader;   // Cyan, XL (24) — screen titles
static lv_style_t fcStyleValue;    // Green, LG (20) — sensor values
static lv_style_t fcStyleHero;     // Green, HERO (32) — large numbers
static lv_style_t fcStyleBody;     // White, MD (18) — body text
static lv_style_t fcStyleLabel;    // Gray, SM (16) — secondary labels
static lv_style_t fcStyleWarn;     // Orange, MD (18) — warnings
static lv_style_t fcStyleError;    // Red, MD (18) — errors
```

**Step 4: Add initFCTheme() function**

Add this function immediately before `initLVGL()` (before line 1373):

```c
// ============== Field Compass LVGL Theme (#107) ==============
void initFCTheme() {
  // Initialize default dark theme with cyan primary, green secondary
  lv_theme_t* theme = lv_theme_default_init(
      lvglDisplay,
      FC_COLOR_HEADER,    // primary — focus rings, active elements
      FC_COLOR_VALUE,     // secondary — accents, toggles
      true,               // dark mode
      FC_FONT_MD          // default app font = 18px Montserrat
  );
  lv_display_set_theme(lvglDisplay, theme);

  // Screen background: black
  lv_obj_set_style_bg_color(lv_screen_active(), FC_COLOR_BG, 0);

  // Initialize named styles
  lv_style_init(&fcStyleHeader);
  lv_style_set_text_color(&fcStyleHeader, FC_COLOR_HEADER);
  lv_style_set_text_font(&fcStyleHeader, FC_FONT_XL);

  lv_style_init(&fcStyleValue);
  lv_style_set_text_color(&fcStyleValue, FC_COLOR_VALUE);
  lv_style_set_text_font(&fcStyleValue, FC_FONT_LG);

  lv_style_init(&fcStyleHero);
  lv_style_set_text_color(&fcStyleHero, FC_COLOR_VALUE);
  lv_style_set_text_font(&fcStyleHero, FC_FONT_HERO);

  lv_style_init(&fcStyleBody);
  lv_style_set_text_color(&fcStyleBody, FC_COLOR_TEXT);
  lv_style_set_text_font(&fcStyleBody, FC_FONT_MD);

  lv_style_init(&fcStyleLabel);
  lv_style_set_text_color(&fcStyleLabel, FC_COLOR_DIM);
  lv_style_set_text_font(&fcStyleLabel, FC_FONT_SM);

  lv_style_init(&fcStyleWarn);
  lv_style_set_text_color(&fcStyleWarn, FC_COLOR_WARN);
  lv_style_set_text_font(&fcStyleWarn, FC_FONT_MD);

  lv_style_init(&fcStyleError);
  lv_style_set_text_color(&fcStyleError, FC_COLOR_ERROR);
  lv_style_set_text_font(&fcStyleError, FC_FONT_MD);

  logPrintln("[LVGL] Field Compass theme initialized (7 styles)");
}
```

**Step 5: Call initFCTheme() from initLVGL()**

In `initLVGL()`, after the encoder/group block (after line 1440 `}`), before the `#if LVGL_TEST_MODE` block, add:

```c
  // Initialize Field Compass theme and named styles (#107)
  initFCTheme();
```

**Step 6: Bump version to 0.38.2**

In `Field_Compass.ino` line 28, change:
```c
#define FW_VERSION "0.38.2"
```

**Step 7: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Flash should be slightly larger than v0.38.1 (theme code ~2KB).
Record exact flash size.

**Step 8: Commit and tag**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.38.2 — Add FC theme, color/font defines, 7 named styles (#107)

- FC_COLOR_* defines (RGB888 equivalents of existing RGB565 palette)
- FC_FONT_* aliases (XS/SM/MD/LG/XL/XXL/HERO → Montserrat 14-32)
- initFCTheme(): default dark theme + 7 named lv_style_t globals
- Called from initLVGL() after input device setup
Flash: <ACTUAL_SIZE> bytes"
git tag -a v0.38.2 -m "v0.38.2 — FC theme system (#107)"
```

---

### Task 3: Font/color demo test screen, upload, verify — v0.39.0

**Files:**
- Modify: `C:\Dev\Field_Compass\Field_Compass\Field_Compass.ino`

**Step 1: Set LVGL_TEST_MODE=1**

Line 63: `#define LVGL_TEST_MODE 1`

**Step 2: Replace test UI block with font/color demo**

Replace the entire `#if LVGL_TEST_MODE` block (lines 1443-1466) with:

```c
  // Font & color demo screen (#107): all 7 styles on black background
  #if LVGL_TEST_MODE
  {
    // Row layout: Y positions for 7 lines with spacing
    static const struct { const char* name; lv_style_t* style; const char* sample; } rows[] = {
      { NULL, &fcStyleHeader, "HEADER \xE2\x80\x94 Cyan 24px" },
      { NULL, &fcStyleValue,  "VALUE \xE2\x80\x94 Green 20px" },
      { NULL, &fcStyleHero,   "HERO \xE2\x80\x94 Green 32px" },
      { NULL, &fcStyleBody,   "BODY \xE2\x80\x94 White 18px" },
      { NULL, &fcStyleLabel,  "LABEL \xE2\x80\x94 Gray 16px" },
      { NULL, &fcStyleWarn,   "WARN \xE2\x80\x94 Orange 18px" },
      { NULL, &fcStyleError,  "ERROR \xE2\x80\x94 Red 18px" },
    };
    int16_t y = 10;
    for (int i = 0; i < 7; i++) {
      lv_obj_t* lbl = lv_label_create(lv_screen_active());
      lv_label_set_text(lbl, rows[i].sample);
      lv_obj_add_style(lbl, rows[i].style, 0);
      lv_obj_set_pos(lbl, 10, y);
      // Advance Y based on font size + padding
      const lv_font_t* f = lv_obj_get_style_text_font(lbl, 0);
      y += lv_font_get_line_height(f) + 6;
    }
    lv_timer_handler();  // Render to TFT
    delay(3000);         // Hold for visual confirmation
  }
  logPrintln("[LVGL] Font/color demo rendered (LVGL_TEST_MODE=1)");
  #endif
```

**Step 3: Compile with LVGL_TEST_MODE=1**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Record flash size (test build).

**Step 4: Upload to device and verify**

Upload: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 C:/Dev/Field_Compass/Field_Compass/`

Verify on device:
- All 7 lines visible on black background
- Each line shows different font size (14→32px progression visible)
- Text edges are smooth (anti-aliased), not jagged
- Colors match: Cyan, Green, Green, White, Gray, Orange, Red
- Device runs stable (no watchdog reset)

**Step 5: Set LVGL_TEST_MODE=0 and bump version**

Line 63: `#define LVGL_TEST_MODE 0`
Line 28: `#define FW_VERSION "0.39.0"`

**Step 6: Compile production build**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Flash should be ~2.7MB.
Record exact flash size (production = final number).

**Step 7: Upload production and verify stability**

Upload: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 C:/Dev/Field_Compass/Field_Compass/`

Verify: Device boots normally, all screens work as before (theme is inert in production — LVGL_TEST_MODE=0 means no LVGL rendering in loop).

**Step 8: Commit and tag v0.39.0**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.39.0 — LVGL theme with AA fonts and color palette (#107)

- 7 Montserrat fonts (14/16/18/20/24/28/32px) — anti-aliased 4bpp
- Default dark theme: cyan primary, green secondary
- 7 named styles: fcStyleHeader/Value/Hero/Body/Label/Warn/Error
- Font/color demo test screen (LVGL_TEST_MODE=1)
- Production verified stable (LVGL_TEST_MODE=0)
Flash: <ACTUAL_SIZE> bytes"
git tag -a v0.39.0 -m "v0.39.0 — LVGL theme + AA fonts (#107)"
```

**Step 9: Push and close**

```bash
git push origin main
git push origin --tags
```

Close #107 on GitHub with summary comment. Update project board to Done.

**Step 10: Update MEMORY.md**

Add new section documenting:
- 7 font sizes with aliases
- 7 named styles with color/font assignments
- Flash usage at v0.39.0
- Theme init approach (default + overrides)
