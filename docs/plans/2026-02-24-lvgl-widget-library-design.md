# LVGL Reusable Widget Library Design (#108)

**Date:** 2026-02-24
**Issue:** #108 — LVGL: Build reusable widget library (header, nav bar, action bar, toggle, dropdown)
**Depends on:** #107 (done)

## Overview

Replace the 6 TFT_eSprite draw functions (`drawHeader`, `drawNavBar`, `drawActionBar`, `drawToggle`, `drawDropdown`, `drawSelectorOverlay`) with LVGL-native widget factory functions. Each widget uses the FC theme styles from #107 and works with both touch pointer and encoder input devices from #106.

## Approach

**Factory Functions + LVGL-Native Interaction.** Each widget is an `fc*Create()` function returning an `lv_obj_t*` container with styled children. LVGL handles touch hit-testing, encoder focus navigation, and scroll automatically — no manual coordinate math or `configFocusRow` integers needed. Internal children accessed by index (`lv_obj_get_child()`), no heap-allocated state structs.

## Widget API

| Widget | Create | Update | Returns |
|--------|--------|--------|---------|
| Header | `fcHeaderCreate(parent, title)` | `fcHeaderSetTitle(h, text)` | Container: [0]=title label, [1]=gear button |
| NavBar | `fcNavBarCreate(parent, count, active)` | `fcNavBarSetActive(n, idx)` | Container: [0]=hint_L, [1..N]=dots, [N+1]=hint_R |
| ActionBar | `fcActionBarCreate(parent, back, ok)` | — (static) | Container: [0]=back btn, [1]=ok btn |
| Toggle | `fcToggleCreate(parent, label, optA, optB, val)` | `fcToggleSet/GetValue()` | Container: [0]=label, [1]=btnA, [2]=btnB |
| Dropdown | `fcDropdownCreate(parent, label, items[], count, idx)` | `fcDropdownSet/GetIndex()` | Container: [0]=label, [1]=value button |
| ListPicker | `fcListPickerOpen(title, items[], count, sel, caller)` | — (modal, self-closing) | Overlay + scrollable list |

## Visual Design

Exact visual match to existing TFT_eSprite widgets:

| Widget | Background | Text Style | Dimensions |
|--------|-----------|------------|------------|
| Header | Cyan (FC_COLOR_HEADER) | Black, FC_FONT_SM (16px) | 480×30px, top |
| NavBar | Dark gray (0x18C3) | Active=black on cyan, inactive=dim | 480×25px, bottom |
| ActionBar | Dark gray (0x18C3) | White on buttons | 480×50px, y=270 |
| Toggle | Transparent | Label=gray FC_FONT_SM | 130×30px option buttons, green=selected |
| Dropdown | Transparent | Label=gray, value=green | Value box dark gray + ▼ arrow |
| ListPicker | Very dark blue (0x0841) | Cyan title, white items, green=selected | 420×220px modal, centered |

### Header Detail

- 30px cyan bar spanning full width
- Title: black text, left-aligned at x=10
- Gear icon: `LV_SYMBOL_SETTINGS` in black, right-aligned, tappable button
- Gear only visible on non-Settings screens (controlled by caller)

### NavBar Detail

- 25px dark gray bar at screen bottom
- Numbered dots (1-N), active = cyan filled rect + black text, inactive = dim text
- "A<" hint left, ">B" hint right, both in dim XS font

### ActionBar Detail

- 50px dark gray bar at y=270
- Back button: 110×34px, dark gray (0x4208), radius 6, "← Back" text
- OK button: 110×34px, dark green (0x03E0), radius 6, "OK →" text
- Both buttons join default encoder focus group

### Toggle Detail

- Row: gray label (140px) + two option buttons (130×30px each, gap=10)
- Selected option: green background (0x03E0), white text
- Unselected option: dark gray background (0x2104), dim text
- Tap/encoder toggles between options, fires `LV_EVENT_VALUE_CHANGED`

### Dropdown Detail

- Row: gray label (140px) + value button (remaining width - 20px margin)
- Value button: dark gray (0x2104) background, green value text + dim `LV_SYMBOL_DOWN`
- Tap/encoder opens ListPicker modal

### ListPicker Detail

- Full-screen semi-transparent overlay + centered 420×220 container
- Very dark blue (0x0841) background, gray border, radius 8
- Cyan title at top
- Scrollable list: 36px items, green=selected, touch scroll + encoder scroll
- Selecting item: closes picker, sends `LV_EVENT_VALUE_CHANGED` to caller dropdown

## Interaction Model

LVGL manages all input routing via pointer (touch) and encoder (A/B/C) from #106:

- **Touch:** Tap any widget fires `LV_EVENT_CLICKED`. No coordinate math.
- **Encoder:** A=prev focus, B=next focus, C=enter. Focus ring via default theme.
- **Toggle:** Tap option or encoder enter to switch. Fires `LV_EVENT_VALUE_CHANGED`.
- **Dropdown → ListPicker:** Tap/enter opens picker. Select closes and updates.
- **ListPicker:** Touch drag or encoder to scroll. Enter confirms selection.

## ListPicker Modal Flow

```
fcDropdownCreate() → user taps/presses → fcListPickerOpen()
                                            │
                                   Creates full-screen overlay
                                   + centered list container
                                            │
                               User selects item (touch/encoder)
                                            │
                               Sends LV_EVENT_VALUE_CHANGED to caller
                               Deletes overlay + list container
```

## Version Strategy

| Step | Tag | Content |
|------|-----|---------|
| Header + NavBar | v0.39.1 | Non-interactive display widgets |
| ActionBar + Toggle | v0.39.2 | Interactive widgets with encoder focus |
| Dropdown + ListPicker + demo screen | v0.40.0 | Complete #108 |

## Flash Budget

- Current (v0.39.0): 1,778,994 bytes (61% of 4MB)
- Widget code: ~5-8 KB
- Projected (v0.40.0): ~1,790,000 bytes (~62%)
- Remaining headroom: ~1.7 MB for future screens (#109-#114)

## Migration Path

These widgets are created and tested in `LVGL_TEST_MODE`. During screen migration (#109-#114), each screen replaces its TFT_eSprite draw calls with the LVGL widget equivalents. Legacy draw functions remain until all screens are migrated, then removed.
