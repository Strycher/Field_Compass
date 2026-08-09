# PlatformIO Migration + Hardware Abstraction — Design Plan

**Issue:** #151
**Status:** DRAFT — awaiting human approval. No child epics created yet.
**Date:** 2026-08-09

---

## Initiative

**Field Compass becomes a hardware-flexible firmware platform.**

Two problems, one solution path:

1. **The build is not reproducible.** Four project-critical files live outside
   git, in the Arduino sketchbook. One of them
   (`TFT_Drivers/ST7796_Rotation.h`) is a *patched vendor source file* that any
   library upgrade silently reverts. The firmware builds on exactly one PC.
2. **The firmware is welded to one hardware stack.** Components are about to
   change. Today a component swap means editing an 8,700-line `.ino` and hoping.

This initiative is strategy, not a GitHub issue. The Feature (#151) and its
epics are where the work lives.

## Feature label

`feature:platform-modernization`

---

## Evidence base

Gathered 2026-08-09, this session:

| Finding | Evidence |
|---|---|
| Sketchbook is OneDrive-redirected | `arduino-cli config get directories.user` → `C:\Users\stryc\OneDrive\Documents\Arduino` |
| LVGL config outside git | `libraries/lv_conf.h`, 52 KB |
| Active display setup outside git | `User_Setup_Select.h:30` includes `<User_Setups/Setup_Field_Compass_ESP32S3_ST7789.h>` — filename says ST7789, panel is ST7796 |
| **Vendor library source is patched** | `TFT_Drivers/ST7796_Rotation.h` carries project-authored comments swapping rotation pairs |
| No CI gate exists | `.github/workflows/` has no compile workflow; `gh pr checks` → "no checks reported" on #145 and #148 |
| Scale of the single file | 8,702 lines, 162 top-level functions, ~1,550 `lv_*` call sites, 47 `SD.` and 27 `bme` references |
| pioarduino already standard here | 25 `platformio.ini` files under `C:\Dev`; meshcore/Crosswire pin `pioarduino/platform-espressif32` release `53.03.13-1` |

---

## Sequencing rationale

```
E1 vendor config ──▶ E2 PlatformIO ──▶ E3 CI gate ──▶ E4 translation units ──▶ E5 HAL
                            │
                            └──▶ #147 credential provisioning (needs build flags)
```

One choice in this order matters more than the rest:

> **The CI gate lands BEFORE the refactoring epics, not after.**

E4 and E5 move thousands of lines of working firmware. Doing that without an
automated compile gate means every mistake is found by a human flashing a board.
CI is cheap once the build is reproducible (E2), so it goes third — as a safety
net for the risky work, not a victory lap after it.

`arduino-cli` keeps working in parallel until E2's integration test passes.
There is always a path back.

---

## Epic 1 — Reproducible build: vendor external config into the repo

Independently valuable, low risk, and everything later depends on it. Still
`arduino-cli` at the end of this epic — no toolchain change yet.

| # | Task | Files |
|---|---|---|
| 1.1 | Vendor `lv_conf.h` into `include/`; document how the build finds it | 2 |
| 1.2 | Vendor the active TFT_eSPI setup header; decide + document the `User_Setup_Select.h` strategy | 2 |
| 1.3 | **Capture the `ST7796_Rotation.h` patch** — see open decision below. Must record *why* the rotation pairs are swapped; that rationale exists only as a code comment on one machine | 2–3 |
| 1.4 | Record exact installed library versions as a manifest — the evidence base for pinning in 2.2 | 1 |
| 1.5 | Rename the setup header: says `ST7789`, panel is `ST7796` | 1 |
| 1.6 | **Integration test:** move the OneDrive library config aside, build from a clean checkout, confirm success | — |

**Open decision (1.3).** Vendoring the whole TFT_eSPI driver into `lib/` forks it
from upstream and you own the maintenance. A patch script keeps upstream but is
fragile across versions. Recommendation: **vendor it** — the patch is small, the
library is stable, and a fork you can see beats a patch that silently fails.
Owner's call.

## Epic 2 — PlatformIO / pioarduino build

Platform string, matching the meshcore family already in `C:\Dev`:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13-1/platform-espressif32.zip
```

Not `platformio/espressif32` — Espressif stopped maintaining PlatformIO support
and the official platform tops out at Arduino core 2.x. FC is on 3.3.8.

| # | Task | Notes |
|---|---|---|
| 2.1 | `platformio.ini`: pioarduino platform, board, PSRAM + partition + flash flags | `board_build.arduino.memory_type` must be right or PSRAM silently vanishes and the LVGL draw buffers die |
| 2.2 | Pin `lib_deps` to versions captured in 1.4 | |
| 2.3 | Source layout `src/` / `include/` / `lib/`, `.ino` still compiling as-is | No logic change |
| 2.4 | Build-time `-DFW_VERSION` from `git describe` | Closes the `0.51.1` vs `v0.51.10` drift |
| 2.5 | **Parity check** vs the arduino-cli build: flash size, RAM size, boot log | Evidence, not vibes |
| 2.6 | **Integration test:** flash the PIO artifact; verify display, touch, GPS, IMU, SD, FRAM, sensors, web | Human sign-off. `arduino-cli` retired only after this passes |

## Epic 3 — CI compile gate (closes #150)

| # | Task |
|---|---|
| 3.1 | GH Actions workflow running `pio run` over the env matrix, on firmware-touching paths |
| 3.2 | Cache the platform + libraries — a full S3 LVGL build is slow |
| 3.3 | **Verify the gate actually gates:** push a deliberate syntax error, confirm red, revert |
| 3.4 | Correct the CI-gate row in `CLAUDE.md` (it claims a gate that never existed) |
| 3.5 | **Integration test:** throwaway PR confirms the check is required and blocking |

## Epic 4 — `.ino` → translation units

The risky one. Arduino's preprocessor auto-generates function prototypes today;
that goes away.

> **File-convergence warning.** Every extraction touches `main.cpp`. These tasks
> MUST be strictly serialized via `depends-on`. No parallel agents.

| # | Task |
|---|---|
| 4.1 | `.ino` → `src/main.cpp` with explicit prototypes. Mechanical, zero logic change; becomes the size/behaviour baseline |
| 4.2 | Extract storage (SD + FRAM) → `src/storage/` |
| 4.3 | Extract sensors (SHT41, BME688, IMU, battery) → `src/sensors/` |
| 4.4 | Extract GPS/NMEA → `src/gnss/` |
| 4.5 | Extract web server → `src/web/` |
| 4.6 | Extract UI/LVGL screens → `src/ui/` — largest, likely splits per screen |
| 4.7 | **Integration test:** full hardware pass + flash/RAM comparison against the 4.1 baseline |

## Epic 5 — Hardware Abstraction Layer

The epic that serves *"I intend to change components rapidly."* Two mechanisms,
solving different problems:

- **Compile-time — board profiles.** One header per hardware variant holding
  every pin, I2C address, and a capability list; PlatformIO `[env:]` selects it.
  This makes a different display or MCU a *build target* rather than a branch.
- **Runtime — capability discovery.** An I2C probe at boot builds a
  present/absent map. A sensor that is unplugged, swapped, or readdressed
  degrades the UI gracefully instead of hanging or showing stale values. **This
  is what makes rapid swapping survivable** — you can pull a part and still boot.

| # | Task |
|---|---|
| 5.1 | `include/boards/fc_v1.h` — every pin + I2C address currently scattered through `main` collected into one profile (settles #149) |
| 5.2 | Runtime I2C probe → capability flags; boot log states what was found and what was missing, loudly (SAFELANE §6) |
| 5.3 | UI degradation: an absent sensor renders unavailable, never a stale or zero value |
| 5.4 | Sensor interface + adapters for current parts (SHT41, BME688, LSM6DSOX/LIS3MDL, MAX17048) |
| 5.5 | Input abstraction: touch / encoder / buttons behind one interface — the seam MCP23017 + joystick plug into |
| 5.6 | Display abstraction. **Hardest.** TFT_eSPI binds pins at compile time via its own headers, so a second panel means a second env, not a runtime switch |
| 5.7 | Second `[env:]` proving a variant builds — target TBD |
| 5.8 | **Integration test:** both envs build; primary hardware fully verified; one sensor physically unplugged to prove graceful degradation |

### ⚠ Blocked on input — which components were purchased?

Tasks 5.4–5.7 cannot be specified without this. The BOM lists MCP23017, tactile
buttons, and the 5-way joystick as *evaluating / not ordered*, and the ambient
light sensor as *wired, not yet in firmware*.

- **If input hardware** (buttons / joystick / MCP23017): 5.5 carries the epic,
  5.6 is deferrable, epic is moderate.
- **If the display or MCU changed:** 5.6 becomes the centre of gravity, likely
  needs its own epic, and **LovyanGFX is worth evaluating against TFT_eSPI** —
  Desk Command Center already uses it and it handles multi-panel far better.

---

## Out of scope

- **#147** (WiFi credentials in firmware) — slots after E2, once build flags
  and/or SD-backed config exist. Doing it earlier means doing it twice.
- **#149** (CLAUDE.md pin table) — settled naturally by task 5.1.

## Risks

| Risk | Mitigation |
|---|---|
| PIO build differs subtly from arduino-cli (PSRAM, partitions, flash mode) | Task 2.5 parity check; arduino-cli kept alive until 2.6 signs off |
| Losing the ST7796 rotation-patch rationale | Task 1.3 requires recording *why*, not just *what* |
| Epic 4 breaking working firmware | CI gate (E3) lands first; 4.1 is behaviour-neutral and becomes the baseline |
| Parallelism causing merge hell in one large file | E4 strictly serialized via `depends-on` |
| Scope drift into "rewrite the firmware" | Every epic ends in a hardware integration test with human sign-off |
