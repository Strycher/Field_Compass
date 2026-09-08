# Field Compass — CLAUDE.md

> **Standards:** This project follows DifferentWire standards.
> Read and apply: `C:\Dev\DifferentWire\standards\CLAUDE-BASE.md`, `C:\Dev\DifferentWire\standards\SAFELANE.md`
> Credential inventory: `C:\Dev\.credentials.env`

## ⚠ Diagnosing hardware? Read `docs/DIAGNOSTICS.md` FIRST

The device publishes its own health over WiFi at `http://fieldcompass.local/` —
20 endpoints including per-peripheral OK/N-A status, a JSON API, and downloadable
SD-backed serial logs.

```bash
curl -s http://fieldcompass.local/diags | sed 's/<[^>]*>//g'
```

**Do this before asking a human to observe anything, capture serial, or press a
button.** In #166 one request to `/diags` identified a dead SPI bus that a serial
capture would not have shown as clearly. See [`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md)
for the endpoint reference, the triage order, and the **button-free flash
procedure** (the BOOT button is buried inside the enclosure — never ask for a
physical press).

## Project Overview

| Field | Value |
|-------|-------|
| Project Name | Field Compass |
| Organization | Strycher (personal) |
| Repository | https://github.com/Strycher/Field_Compass |
| Archive | https://github.com/Strycher/Field_Compass-archive — private, read-only (#245) |
| Project Type | Embedded firmware (PlatformIO; an Arduino `.ino` residue plus C++ translation units under `src/`, since E4 #212) |
| GitHub Project | Field Compass Backlog (#3) |

### This repository was rebuilt clean on 2026-09-07 (#245)

The original carried WiFi credentials in plaintext, publicly, from 2026-02. History
cannot be scrubbed in place: GitHub pins every PR's head commit at `refs/pull/N/head`
permanently and a PR cannot be deleted, so 46 refs would have kept the credentials
fetchable regardless of what happened to `main`.

So the history was redacted into a **new** repository, which has no such refs. Two
consequences that matter day to day:

- **Issue numbers were preserved slot for slot**, so every `#NNN` in the source, here,
  and in Citadel's `external_issue_number` still resolves. Numbers that were pull
  requests exist as **closed placeholders** pointing at the archive — the PRs themselves
  could not be recreated. See #245 for the list.
- **`src/web.cpp` holds placeholder credentials** (`REDACTED_SSID_1` and friends; they
  were in `src.ino` until E4 band 3e, #274). A build from `main` compiles and **cannot
  join WiFi**, which also means no `fieldcompass.local` diagnostics. Until #99 moves
  credentials to runtime storage, flashing needs a local uncommitted edit — and nothing
  in `.gitignore` protects such an edit from being committed by accident.

Pre-migration commit SHAs quoted in old issues refer to the archive, not here.

## Project Parameters

| Parameter | Value |
|-----------|-------|
| PROJECT_NAME | Field Compass |
| PROJECT_DIR | `/c/Dev/Field_Compass` |
| BUILD_COMMAND | `pio run -e feather_s3` (Adafruit 5477) · `pio run -e um_feathers3` (UM FeatherS3, the bench board since 2026-09-05 — #283) |
| CITADEL_PROJECT | `Field_Compass` |
| GITHUB_PROJECT_ID | `PVT_kwHODGcOBc4BOJgD` |
| INFRA_PROFILE | Maker |
| Main Source File | `src/src.ino` — `setup`, `loop`, `initLVGL`, the buttons; ~4,400 lines on `main` after band 3 of E4, ~750 once #280 (band 4) merges. Named for the directory, see Build & Flash. The rest of the firmware is one unit per domain under `src/`, see Repository Layout |

## Hardware Specifications

| Component | Value |
|-----------|-------|
| MCU | **Two boards build from this tree (#283).** Bench since 2026-09-05: **UM FeatherS3D** (Adafruit PID 6399, 16 MB flash, 8 MB QSPI PSRAM), env `um_feathers3`, registry `um_feather`. Original: Adafruit ESP32-S3 Feather 4MB Flash 2MB PSRAM (PID 5477), env `feather_s3`, registry `field_compass` |
| Display | Hosyond 3.5" ST7796U IPS TFT 480x320 (MSP3526), TFT_eSPI **rotation 3** — the panel is mounted 180° from TFT_eSPI's assumption, so landscape is the complement of the nominal rotation (#157). Configured via `build_flags`, not `User_Setup.h` (#184). On the ESP32-S3 TFT_eSPI needs `USE_FSPI_PORT` or it writes to address 0 (#284) |
| Touch | FT6336U capacitive on I2C `0x38` (CTP_INT: GPIO 6 on the UM board, GPIO 14 on the Adafruit; CHANGE interrupt) |
| IMU | LSM6DSOX + LIS3MDL (STEMMA QT) — heading reference is X+ |
| GPS | PA1616D (MT3339) — Serial1 @ 9600, CR1220 backup, PMTK101 hot restart |
| SD | Adalogger FeatherWing (SD_CS = GPIO10) |
| FRAM | MB85RS2MTA 256KB (FRAM_CS = GPIO15) — battery + weather write buffers |
| Temp/Humidity | SHT41 on I2C `0x44` (STEMMA QT) |
| Battery | MAX17048 on I2C `0x36` / `0x7E` |
| UI Framework | LVGL 9.5.0 — dual 480x50 PSRAM draw buffers (~96KB) |

### Pin Assignments

**UM FeatherS3D (the bench board) — wire it from [`docs/um-bench-wiring.md`](docs/um-bench-wiring.md)
and nothing else.** That page is silkscreen labels only and was locked by the owner on
2026-09-08 after every peripheral came up. The GPIO view of the same wiring, as the
firmware sees it (`src/fc_config.h` UM block + `platformio.ini` `[env:um_feathers3]`):

- **I2C1:** SDA=8, SCL=9 (header + first STEMMA QT); **I2C2:** SDA=16, SCL=15 (second STEMMA QT, LDO2-powered)
- **SPI:** SCK=36, MOSI=35, MISO=37
- **TFT:** CS=17, DC=18, RST=14, BL=5
- **SD/FRAM/Touch:** SD_CS=3, FRAM_CS=12, CTP_INT=6
- **Buttons:** A=1, B=38, C=33 · **LDO2 enable:** 39 · **GPS UART:** RX=44, TX=43

**Adafruit ESP32-S3 Feather (PID 5477), env `feather_s3`** — these numbers are Adafruit GPIOs
and collide with the UM silkscreen numbers (14, 17, 18 mean different things). Do not wire
the UM board from this list:

- **I2C:** SDA=3, SCL=4 (STEMMA QT)
- **SPI:** SCK=36, MOSI=35, MISO=37
- **TFT:** CS=18, DC=17, RST=16, BL=8 (PWM-dimmable)
- **SD/FRAM/Touch:** SD_CS=10, FRAM_CS=15, CTP_INT=14
- **Buttons:** A=9, B=6, C=5 · **GPS Serial1:** RX=38, TX=39 (variant `RX`/`TX`; an earlier line here said 5/6, which was wrong)

## Repository Layout (#186)

Standard PlatformIO layout. There is no `Field_Compass/` sketch directory any more.

| Path | Contents |
|------|----------|
| `src/` | `src.ino` (`setup`, `loop`, `initLVGL`, buttons) + one unit per domain, each a `.h`/`.cpp` pair: `logging`, `fram`, `settings`, `rtc`, `gps`, `imu`, `env`, `battery`, `weather`, `geocache`, `oled`, `display`, `ui_state`, `touch`, `web`, `geo`, `ui_widgets` (and, once #280 merges, `lvgl_port`, `oled_screens`, `screen_*`, `navigation`); headers `fc_config.h` (pins), `fc_theme.h`, `fc_version.h` (`FW_VERSION`); `lv_psram_alloc.c` (LVGL PSRAM allocator, #164). Headers live in `src/`, not `include/`, because arduino-cli does not read `-I include` (#161). The record of how it got this way: `docs/god-object-inventory.md`, `docs/e4-session-2026-09-07.md` |
| `include/` | `lv_conf.h` — vendored, #158. Reached via `-I include` in `build_flags` |
| `lib/` | Vendored libraries. Empty; everything is pinned in `lib_deps` |
| `partitions/` | Flash layout CSV |

## Build & Flash

Two envs, one per board (#283). The bench has been the UM FeatherS3 (registry name
`um_feather`) since the 2026-09-05 rewire; the Adafruit env still builds and is unchanged.
Pin mapping for both: `docs/HARDWARE.md`.

```bash
pio run -e um_feathers3
```

```bash
pio run -e feather_s3
```

```bash
python scripts/pio-flash.py list
```

**Flashing goes through the wrapper, never raw.** `pio-flash` verifies the board's
identity against the registry before it writes anything, and
`.claude/hooks/block-raw-flash.py` refuses raw `arduino-cli upload`, `pio run -t
upload` and `esptool` invocations. Preview then confirm:

```bash
python scripts/pio-flash.py preview um_feather --env um_feathers3
```

The env must match the board the device name resolves to: `um_feather` takes
`um_feathers3`, `field_compass` (the Adafruit board) takes `feather_s3`. The wrapper
checks identity, not purpose — it will not stop a `feather_s3` image going onto the UM
board, and that image drives the wrong pins there (`docs/HARDWARE.md`).

Never hardcode a COM port — app and bootloader modes enumerate on *different*
ports, so re-detect every time. The board needs a manual RESET press after a
bootloader-mode flash; do not script the bootloader exit. Device state
(registry, flash history) lives at `C:\Dev\.field_compass\`, outside every git
working tree — see the Device State section below.

### Why the sketch is called `src.ino` and not `Field_Compass.ino`

Because `arduino-cli` demands a sketch at `<dir>/<dir>.ino`. It resolves the
parent directory and looks for a file matching its name, so with the firmware
at `src/Field_Compass.ino` it fails — pointed at the directory *or* at the
`.ino` file itself:

```
Can't open sketch: main file missing from sketch: src\src.ino
```

PlatformIO does not care what the `.ino` is called; it compiles any of them in
`src/`. So `src.ino` is the one name that satisfies both toolchains, and both
are verified building from it. The name is ugly and says nothing about the
project — that is the price of keeping the arduino-cli path alive until #161
signs off on PlatformIO, and it is a one-line rename to undo afterwards.

Both build commands:

```bash
pio run -e feather_s3
```

```bash
arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 src/
```

`arduino-cli` lives at `C:\Program Files\Arduino CLI\arduino-cli.exe`. Note it
builds a *different image* from PlatformIO — see the baseline comparison in
`docs/library-manifest.md`, whose deltas B6 (#188) still has to explain. Use
PlatformIO for anything you intend to flash.

## Device State — one fixed location, never in the repo (#198)

Everything `pio-flash` knows about physical hardware lives at `C:\Dev\.field_compass\`
and **nowhere else**:

| File | What it is |
|------|-----------|
| `hardware-devices.yaml` | Device registry — which physical board each name means |
| `flash-history.jsonl` | Append-only flash log |
| `registry-backups/` | Timestamped snapshot before every registry write |
| `flash-backups/` | Flash images from `pio-flash backup` |

This path is **fixed and absolute**, not resolved relative to the repo. That is
deliberate. It used to be a tracked file, which meant every worktree carried its
own copy and `git checkout` could rewrite device identities as a side effect —
so sessions in different worktrees held different views of the hardware and
argued about which was right. `.gitignore` does not fix that: worktrees are
separate directories, so an ignored file in the primary clone is invisible from
a worktree and the guard just refuses there.

**Do not** create `hardware-devices.yaml` in the repo, hand-edit the registry, or
"sync" it anywhere. Registrations go through the wrapper, which reads identity
off the live board:

```bash
python scripts/pio-flash.py bootstrap <name> --port <COMx>
```

`.claude/hooks/block-registry-edit.py` refuses direct edits. Reads are unrestricted.

This is Field Compass's own registry — meshcore-firmware, wadamesh and LoRa each
keep their own. A future central registry has to model devices that move between
projects; see #198 for that sketch.

## FC-Specific Overrides

### Branch Strategy — Per-Issue, Not Per-Epic (override of CLAUDE-BASE)

CLAUDE-BASE mandates *one epic = one branch = one PR*. Field Compass overrides this to **one issue = one branch = one PR** because:

- Single-file source (`Field_Compass.ino`) means long-lived branches guarantee merge conflicts
- Each compile needs hardware verification before shipping — natural serialization point
- Firmware epics span weeks of hardware iteration, making epic branches impractical

| Rule | Value |
|------|-------|
| Branch naming | `fc/<issue>-short-desc` (e.g., `fc/120-sd-indicator`) |
| Scope | One GitHub issue per branch |
| Commits | Every successful compile on the branch = commit (per SAFELANE §6) |
| CI gate | `.github/workflows/compile.yml` — `pio run` (envs **discovered** from `platformio.ini`, never listed) plus `pytest scripts/`, on every PR and on push to `main`. **`compile-gate` is the required status check**: a failing build *or* a failing test blocks the merge (#221, #223, #249) |
| Verification gate | Human flashes locally + verifies on hardware |
| Merge | After human verification: agent runs `gh pr merge <N> --auto --rebase` |
| Worktrees | Optional — parallel-agent rule (below) means usually not needed |
| Post-merge | Tag on `main` if the commit represents a shippable milestone |

**Require `compile-gate`, never `build (feather_s3)`.** That job name is *generated*
from the matrix, which is generated from the discovered envs. Requiring it by hand
breaks discovery both ways: a new env ships unguarded under a name nobody added to
the required list, and a renamed env leaves every PR stuck on a check at `expected`.
`compile-gate` aggregates `discover`, `build` and `tests` under a fixed name, so
branch protection is written once. Adding the test suite in #249 needed no
protection change at all.

The gate fails **closed**, cancellation included — a cancelled run verified nothing,
and letting Cancel produce a green check would make it a one-click bypass. A red
check from a cancel clears on re-run, with no new commit. Docs-only PRs skip the
build job entirely but still report `compile-gate` green; the test suite is
unconditional and never skipped.

### Single-File Source ⇒ Serialize Parallel Agents (MANDATORY)

Field Compass was a single ~8,000-line source file until E4 (#212) split it into units; `src/src.ino` is now the residue and the code lives in one file per domain. The premise of this rule is gone, but **the rule stands until the owner relaxes it** — it was written for the single-file era and the owner decides whether the per-issue branch override (above) and this serialisation survive the split.

At session start, after preflight passes, check `dw --project Field_Compass list --status in_progress`. If ANY task is already claimed:

1. **STOP and ask the user:** "Another agent is working on `<task-id>`. Field Compass is single-file firmware — parallel agents will conflict. Run a second agent anyway?"
2. Do NOT proceed until the user explicitly confirms.
3. If the user declines, report what the other agent is doing and exit gracefully.

### Interactive Mode Still Requires a Citadel Task

CLAUDE-BASE § *Interactive Mode — Citadel Still Applies* applies to FC without exception. Even a one-line fix the user asks for requires `dw claim` first. This is a break from FC's pre-2026-04 rules — the "interactive carve-out" is gone.

### Continuous Work Loop OFF by Default

CLAUDE-BASE describes Worker mode as opt-in via `/work`. FC keeps that default and reinforces **why**: hardware verification must happen between every task. After completing a task, the agent reports "compiled + flashed + observed X" and waits for human verification. The auto-pick-next-task loop only runs when the user explicitly types `/work`.

### Epic Integration Testing (still mandatory)

Per-issue branching does NOT exempt FC from CLAUDE-BASE § *Epic Completion Protocol*. Every FC epic still requires an integration test as its final child task — but FC runs it on hardware, not in CI.

| Rule | Detail |
|------|--------|
| Integration test is a dedicated task | Created when the epic is groomed, not appended after-the-fact. Title: `Epic #N integration test: <scope>` |
| Depends on all other epic children | Use `<!-- depends-on: #A, #B, #C -->` so the task only surfaces in `dw ready` after the epic's work is merged |
| Runs on hardware | Compile, flash, and verify all epic deliverables work together — memory, timing, and cross-feature interactions |
| Human sign-off gates close | Agent closes the integration task only after the human confirms "verified on device — epic complete" |
| Blocks next epic's entry | The next epic's entry task MUST `depends-on` this integration test, per CLAUDE-BASE |

Task-level hardware verification (per FC's per-issue branching rule) proves each piece works in isolation. The epic integration test proves the pieces work together. Both are required.

### Session State Management (Compaction Recovery)

Applies only in Worker mode. See `.claude/hooks/session-state.sh` and `.claude/hooks/preflight.sh` Section 5.

## Versioning

FC follows CLAUDE-BASE's SemVer rules (`vMAJOR.MINOR.PATCH`) with one firmware-specific adaptation:

- Every successful compile on a branch = commit
- After PR merge to `main`, tag the commit that represents a shippable milestone
- `FW_VERSION` constant in `src/fc_version.h` (moved out of `src.ino` by E4 band 3e, #274) must match the tag
- **Do NOT use auto-commit version-bump workflows** — per SAFELANE §7 incident 2026-03-29 (auto-commits orphaned by rebase merges). Compute at build time or update `FW_VERSION` manually before tagging.

## Board & Label Conventions (FC overrides)

- **Column names:** standard 7 (Backlog / Todo / Ready / In Progress / Testing / Deferred / Done) per CLAUDE-BASE. FC's legacy "On Hold" is renamed to "Deferred".
- **Priority tiers:** 4 (P0–P3) per CLAUDE-BASE. FC's legacy `P4 - Deferred` is collapsed into `P3 - Low/Cosmetic`.
- **Dependency tag:** `<!-- depends-on: #NNN -->` in issue body, processed by `.github/workflows/auto-promote-ready.yml`.
- **Board sync:** Labels only. `gh issue edit <N> --add-label "board:in-progress"`. Never run `gh project` commands (GraphQL budget).

## Quick Reference — FC-Specific

```bash
# ── Start a task ───────────────────────────────────────────
dw --project Field_Compass ready                    # Find unblocked work
dw --project Field_Compass claim FC-<id>            # Claim one task
git checkout -b fc/<issue>-short-desc               # Per-issue branch

# ── Compile + flash ────────────────────────────────────────
pio run -e um_feathers3                             # Build for the UM FeatherS3 bench board (#283)
pio run -e feather_s3                               # Build for the Adafruit 5477 board
python scripts/pio-flash.py list                    # Enumerate + match registry
python scripts/pio-flash.py preview um_feather --env um_feathers3   # env must match the board
python scripts/pio-flash.py confirm <device> --token <token-file>
# Raw arduino-cli/esptool/pio upload are refused by block-raw-flash.py

# ── Commit on the branch (pre-commit hook verifies Citadel claim) ──
git add src/src.ino
git commit -m "feat(#<issue>): description"
git push -u origin fc/<issue>-short-desc

# ── Open PR, wait for human verification, enable auto-merge ───
gh pr create --title "feat(#<issue>): description" --body "..."
# After human verifies on hardware:
gh pr merge <pr-number> --auto --rebase

# ── Close task ─────────────────────────────────────────────
dw --project Field_Compass close FC-<id> --reason "verified on device"
gh issue edit <issue> --add-label "board:done"
```

See CLAUDE-BASE § *Task Management (Citadel)* for the full `dw` CLI reference.
See CLAUDE-BASE § *GitHub Board Sync (Label-Based)* for board labels.
See SAFELANE § 3 *Tollgating* for mandatory approval points.
