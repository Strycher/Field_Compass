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
| Project Type | Embedded firmware (single-file Arduino `.ino`) |
| GitHub Project | Field Compass Backlog (#3) |

## Project Parameters

| Parameter | Value |
|-----------|-------|
| PROJECT_NAME | Field Compass |
| PROJECT_DIR | `/c/Dev/Field_Compass` |
| BUILD_COMMAND | `pio run -e feather_s3` |
| CITADEL_PROJECT | `Field_Compass` |
| GITHUB_PROJECT_ID | `PVT_kwHODGcOBc4BOJgD` |
| INFRA_PROFILE | Maker |
| Main Source File | `src/Field_Compass.ino` (~8,700 lines) |

## Hardware Specifications

| Component | Value |
|-----------|-------|
| MCU | Adafruit ESP32-S3 Feather 4MB Flash 2MB PSRAM (PID 5477) |
| Display | Hosyond 3.5" ST7796U IPS TFT 480x320 (MSP3526), TFT_eSPI rotation 1 |
| Touch | FT6336U capacitive on I2C `0x38` (CTP_INT = GPIO14, FALLING edge) |
| IMU | LSM6DSOX + LIS3MDL (STEMMA QT) — heading reference is X+ |
| GPS | PA1616D (MT3339) — Serial1 @ 9600, CR1220 backup, PMTK101 hot restart |
| SD | Adalogger FeatherWing (SD_CS = GPIO10) |
| FRAM | MB85RS2MTA 256KB (FRAM_CS = GPIO15) — battery + weather write buffers |
| Temp/Humidity | SHT41 on I2C `0x44` (STEMMA QT) |
| Battery | MAX17048 on I2C `0x36` / `0x7E` |
| UI Framework | LVGL 9.5.0 — dual 480x50 PSRAM draw buffers (~96KB) |

### Pin Assignments

- **I2C:** SDA=GPIO3, SCL=GPIO4 (STEMMA QT)
- **SPI:** SCK=36, MOSI=35, MISO=37
- **TFT:** CS=18, DC=17, RST=16, BL=8 (PWM-dimmable)
- **SD/FRAM/Touch:** SD_CS=10, FRAM_CS=15, CTP_INT=14
- **GPS Serial1:** RX=GPIO5, TX=GPIO6

## Repository Layout (#186)

Standard PlatformIO layout. There is no `Field_Compass/` sketch directory any more.

| Path | Contents |
|------|----------|
| `src/` | `Field_Compass.ino` (the firmware) + `lv_psram_alloc.c` (LVGL PSRAM allocator, #164) |
| `include/` | `lv_conf.h` — vendored, #158. Reached via `-I include` in `build_flags` |
| `lib/` | Vendored libraries. Empty; everything is pinned in `lib_deps` |
| `partitions/` | Flash layout CSV |

## Build & Flash

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
python scripts/pio-flash.py preview <device> --env feather_s3
```

Never hardcode a COM port — app and bootloader modes enumerate on *different*
ports, so re-detect every time. The board needs a manual RESET press after a
bootloader-mode flash; do not script the bootloader exit. Device state
(registry, flash history) lives at `C:\Dev\.field_compass\`, outside every git
working tree — see the Device State section below.

### arduino-cli is no longer a working fallback

`arduino-cli` requires a sketch at `<dir>/<dir>.ino`. With the firmware at
`src/Field_Compass.ino` it fails with `main file missing from sketch:
src\src.ino`, whether pointed at the directory or the file. Verified, not
assumed.

Renaming the sketch to `src/src.ino` would restore it, at the cost of a
meaningless filename referenced across every doc, issue and memory. That
tradeoff was declined. The last live arduino-cli baseline is recorded in
`docs/library-manifest.md` for B6's parity check (#188); it cannot be
regenerated from this tree.

The `arduino-cli` binary still lives at `C:\Program Files\Arduino CLI\arduino-cli.exe`
if you need it for an out-of-tree sketch.

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
| CI gate | `pio run -e feather_s3` on push — **not yet implemented**, tracked as #150 |
| Verification gate | Human flashes locally + verifies on hardware |
| Merge | After human verification: agent runs `gh pr merge <N> --auto --rebase` |
| Worktrees | Optional — parallel-agent rule (below) means usually not needed |
| Post-merge | Tag on `main` if the commit represents a shippable milestone |

### Single-File Source ⇒ Serialize Parallel Agents (MANDATORY)

Field Compass has a single ~8,000-line source file. **Parallel agents virtually guarantee merge conflicts, even with worktrees.**

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
- `FW_VERSION` constant in `src/Field_Compass.ino` must match the tag
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
pio run -e feather_s3                               # Build (#186 layout)
python scripts/pio-flash.py list                    # Enumerate + match registry
python scripts/pio-flash.py preview <device> --env feather_s3
python scripts/pio-flash.py confirm <device> --token <token-file>
# Raw arduino-cli/esptool/pio upload are refused by block-raw-flash.py

# ── Commit on the branch (pre-commit hook verifies Citadel claim) ──
git add src/Field_Compass.ino
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
