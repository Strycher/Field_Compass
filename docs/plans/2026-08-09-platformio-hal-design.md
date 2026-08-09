# PlatformIO Migration + Display Portability — Design Plan

**Feature:** #151
**Status:** Approved 2026-08-09. Epics created.
**Revision:** 2 — narrowed scope; driver fork dropped; LovyanGFX added.

---

## Goal

Make the build reproducible off one machine, and make the display a
*configuration* rather than a compile-time weld — so components can change
without the firmware fighting back.

Deliberately narrowed. The `.ino` split and a full hardware-abstraction layer
are **out of scope** for now (see *Deferred*).

---

## Evidence base

Gathered 2026-08-09:

| Finding | Evidence |
|---|---|
| Sketchbook is OneDrive-redirected | `arduino-cli config get directories.user` → `C:\Users\stryc\OneDrive\Documents\Arduino` |
| LVGL config outside git | `libraries/lv_conf.h`, 52 KB |
| Display setup outside git | `User_Setup_Select.h:30` → `Setup_Field_Compass_ESP32S3_ST7789.h` (name says ST7789; panel is ST7796) |
| Vendor library source is patched | `TFT_Drivers/ST7796_Rotation.h`, project-authored rotation swap |
| **The patch is unnecessary** | Its `case 1` writes `MX\|MY\|MV\|COLOR_ORDER` — identical to stock `case 3`. Source calls `setRotation(1)` once (line 1353) and never uses rotations 4–7 |
| Display coupling is thin | 12 distinct `tft.*` methods, ~25 call sites. LVGL does the drawing |
| No CI gate | `.github/workflows/` has no compile workflow; `gh pr checks` → "no checks reported" on #145, #148 |
| pioarduino already standard here | 25 `platformio.ini` under `C:\Dev`; meshcore/Crosswire pin release `53.03.13-1` |

### The rotation finding, in full

`ST7796_Rotation.h` was patched because the Hosyond MSP3526 panel is mounted 180°
from TFT_eSPI's assumption. The patch swaps MADCTL values between complementary
rotation pairs — its own comment reads *"case 1: Landscape — uses Bodmer's
original rotation 3 MADCTL."*

Since the firmware only ever calls `setRotation(1)`, **stock TFT_eSPI with
`setRotation(3)` produces the identical MADCTL byte and identical width/height.**
A one-line source change replaces a maintained fork of a third-party library.

This must be verified on hardware before the patched library is discarded.

---

## Sequencing

```
A: stop depending on patched/external config
     │
     ▼
B: PlatformIO + pioarduino  ──────▶  #147 credential provisioning (needs build flags)
     │
     ▼
C: CI compile gate (closes #150)
     │
     ▼
D: TFT_eSPI → LovyanGFX
```

Two rules drive this order:

1. **CI before the display swap.** D changes the performance-critical LVGL flush
   path. It gets a safety net first.
2. **One variable at a time.** B and D are *not* combined. If a parity check
   fails after changing both the toolchain and the display library, the failure
   is unattributable.

---

## Epic A — Stop depending on patched and external config

Still `arduino-cli`. No toolchain change. Removes the two hardest external
dependencies; the `User_Setup*.h` dependency dies in Epic B via `build_flags`.

| # | Task |
|---|---|
| A1 | Replace the rotation patch with `setRotation(3)` + a comment recording *why* (panel mounted 180°). Reinstall stock TFT_eSPI. **Hardware-verify orientation before discarding the patched copy** |
| A2 | Vendor `lv_conf.h` into `include/`; document how the build resolves it |
| A3 | Record exact installed library versions as a manifest — the evidence base for pinning in B3 |
| A4 | **Integration test:** stock TFT_eSPI + in-repo `lv_conf.h` → build, flash, verify orientation and UI |

## Epic B — PlatformIO / pioarduino

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13-1/platform-espressif32.zip
```

Not `platformio/espressif32` — Espressif stopped maintaining PlatformIO support;
the official platform tops out at Arduino core 2.x. FC is on 3.3.8.

| # | Task |
|---|---|
| B1 | `platformio.ini`: platform, board, PSRAM + partition + flash flags. `board_build.arduino.memory_type` must be right or PSRAM silently vanishes and the LVGL draw buffers die |
| B2 | TFT_eSPI configuration via `build_flags` — retires `User_Setup.h` / `User_Setup_Select.h` / `Setup_Field_Compass_*.h` |
| B3 | Pin `lib_deps` to the A3 manifest |
| B4 | `src/` / `include/` / `lib/` layout; `.ino` still compiling as-is, no logic change |
| B5 | Build-time `-DFW_VERSION` from `git describe` — closes the `0.51.1` vs `v0.51.10` drift |
| B6 | **Parity check** vs the arduino-cli binary: flash size, RAM size, boot log |
| B7 | **Integration test:** flash the PIO artifact; verify display, touch, GPS, IMU, SD, FRAM, sensors, web. `arduino-cli` retired only after this passes |

## Epic C — CI compile gate (closes #150)

| # | Task |
|---|---|
| C1 | GH Actions workflow running `pio run`, on firmware-touching paths |
| C2 | Cache platform + libraries — a full S3 LVGL build is slow |
| C3 | **Verify the gate gates:** push a deliberate syntax error, confirm red, revert |
| C4 | Correct the CI-gate row in `CLAUDE.md` (it claims a gate that never existed) |
| C5 | **Integration test:** throwaway PR confirms the check is required and blocking |

## Epic D — TFT_eSPI → LovyanGFX

**Why:** LovyanGFX configures the panel at *runtime*, in a config struct in our
own source. A second panel becomes a config struct rather than a rebuild of
library headers — and it drives SSD1306/SH110x too, bringing the OLEDs under one
driver instead of a separate `Adafruit_SH110X` path.

The migration is small: 12 methods, ~25 sites, all with direct equivalents.

| # | Task |
|---|---|
| D1 | LovyanGFX panel + bus config struct for ST7796 on the existing SPI pins |
| D2 | Port the LVGL flush callback. **Byte-swap semantics differ** — `pushPixels` vs `pushColors(..., true)`; wrong here shows up as wrong colours |
| D3 | Port boot splash, `init`, `setRotation`, and the MIPI DCS sleep in/out commands |
| D4 | **Re-verify 80 MHz SPI + SD bus sharing (#116).** Those workarounds were tuned against TFT_eSPI's DMA behaviour |
| D5 | Remove the TFT_eSPI dependency and its build flags |
| D6 | **Integration test:** full hardware pass — orientation, colour accuracy, redraw performance, SD coexistence under load |

---

## Deferred (explicitly not now)

| Item | Why deferred | Notes for later |
|---|---|---|
| `.ino` → translation units | High risk, no forcing need yet | 8,702 lines, 162 functions. Must be strictly serialized — every extraction touches one file |
| Full hardware abstraction layer | Epic D delivers most of the display-portability benefit far cheaper | Revisit if sensor/input churn actually starts hurting |
| SparkFun uBlox GPS | Not blocking | **Two changes, not one:** dropping the PA1616D also drops its backup-RTC role. The Adalogger PCF8523 becomes the timekeeper |
| Buttons / dial / joystick / MCP23017 | Still being chosen | 3 free GPIOs (D11/D12/D13). More than 3 inputs needs the expander |
| Additional OLEDs | Lands naturally with Epic D | LovyanGFX drives SH110x/SSD1306 |
| LoRa | Whole separate initiative | MeshCore fork and Meshtastic experience already exist under `C:\Dev` |

## Risks

| Risk | Mitigation |
|---|---|
| `setRotation(3)` assumption wrong on real hardware | A1 requires hardware verification *before* the patched library is discarded |
| PIO build differs subtly (PSRAM, partitions, flash mode) | B6 parity check; arduino-cli alive until B7 signs off |
| LovyanGFX colour/byte-order regression | D2 called out explicitly; D6 verifies colour accuracy |
| SD/TFT bus contention returning under LovyanGFX | D4 is a dedicated task, not an afterthought |
| Scope drift back into "rewrite the firmware" | Deferred list above is binding; every epic ends in hardware sign-off |
