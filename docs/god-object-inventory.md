# God object inventory — what `src.ino` actually contains, and who uses what

**Task:** [#215](https://github.com/Strycher/Field_Compass/issues/215) · **Epic:** [#212](https://github.com/Strycher/Field_Compass/issues/212) · **Feature:** [#151](https://github.com/Strycher/Field_Compass/issues/151)

Measured from the linker's view of the compiled object and its relocations, not from reading the source. Every number here reproduces with:

```bash
pio run -e feather_s3
python scripts/symbol-inventory.py     # what the file defines
python scripts/reference-graph.py      # who uses what, cut set, prototype reliance
```

---

## 1. The previous numbers were wrong, and one of them was wrong enough to matter

[#212](https://github.com/Strycher/Field_Compass/issues/212) carries a table tagged `[REGEX]` with an explicit warning that no such number should be planned against until this task cross-validated it. That warning was justified:

| measure | regex claimed | linker says | error |
|---|---|---|---|
| functions | 206 | **215** | −9 |
| `static` functions | 44 | **48** | −4 |
| `IRAM_ATTR` functions | 0 | **1** (`touchISR`, in `.iram1.0`) | wrong |
| file-scope variables | 111 | **295** | **−184, a 2.7× undercount** |
| functions relying on generated prototypes | 70 | **13** | **+57, a 5× overcount** |

The function counts were merely untidy. The variable count and the prototype count were both wrong enough to have misdirected the plan — one by making the state look a third of its real size, the other by making the `.ino`→`.cpp` move look five times more dangerous than it is.

### Why the regex missed two thirds of the state

It matched declarations at brace depth zero but never matched `static` ones. Every file-scope `static` variable — 198 of them, largely LVGL widget pointers such as `compassScr`, `gcDetLblDT`, `gcListRows` — was invisible to it. In the object file they are `_ZL…` symbols; demangling confirms them.

The arithmetic closes exactly, which is how we know the classification is right rather than plausible:

```
 97  external-linkage file-scope variables   (plain names)
198  static file-scope variables             (_ZL…)
 13  function-local statics                  (_ZZ…)
---
308  total B/b/D/d symbols in src.ino.cpp.o
```

### Why the regex overcounted prototype reliance

It counted functions *called before their definition* — there are 71 — and reported that as reliance on the Arduino preprocessor. But 58 of those 71 already have an explicit forward declaration in the source. Only **13** have none and would break the moment their code leaves the `.ino`.

---

## 2. The number that governs the work is 97, not 295

**A `static` file-scope variable is already private to its translation unit.** When its code moves into `screen_compass.cpp`, it moves with it, needs no `extern`, and the compiler enforces that nothing else can reach it. Those 198 are not an obstacle; they are 198 pieces of state that are *already* correctly scoped and will follow their code for free.

**An external-linkage variable is what couples translation units.** Those 97 are the entire cross-unit surface. Every one either moves into exactly one new unit and stays private there, or becomes a deliberately published interface.

So the extraction problem is **97 variables wide**, not 295 — a materially easier problem than either raw number suggests, and different in kind.

---

## 3. The reference graph — who uses what

Derived from `objdump -r` over the per-function sections the build already emits (`-ffunction-sections` / `-fdata-sections` are on). Only references to symbols `src.ino` itself defines are kept; library calls are excluded.

### 3.1 The real hot core (Q2)

External-linkage variables by fan-in — the number of `src.ino` functions that reference them:

```
 24  webServer          ← missed entirely by the regex hot-core list
 19  gpsData
 18  sdAvailable
 16  useMetricUnits
 16  framAvailable
 15  currentScreen
 15  cacheListCount
 15  useFahrenheit
 15  settingsSubScreen
 13  cacheList
 13  tftBrightness
 12  bmeAvailable, envData, shtAvailable, shtData
 11  magAvailable, oledSleepMs, tftSleepMs, imuData, geocacheSubScreen
```

The regex found eight of the top nine, with matching counts — its *reference* counting was sound; its *declaration* finding was not. It missed the single most-referenced variable in the file: `webServer`, 24 users.

### 3.2 Functions by fan-in — what must be extracted first (Q4)

```
 54  logPrintf
 40  logPrintln
 14  sdOpenSafe
 13  logPrint
 12  recordSDSuccess
  7  gcUpdateFilterLabels
  6  fcDropdownSetValue
  5  fcHeaderCreate, findTimeoutIndex, gcApplyFilters, getCardinal, framWriteHeader
```

The plan's "logging must be early regardless" is confirmed by the linker: the four logging/SD-safety functions sit at the bottom of the call stack with 121 incoming edges between them. They are extracted **first**, before anything that calls them moves.

### 3.3 Coupling profile

```
functions touching NO external-linkage variable : 70 of 215
   ...and calling no other src.ino function     : 47   ← free extractions
```

Forty-seven functions are pure leaves — no shared state, no intra-file calls. They can move at any time, in any order, with no `extern` and no risk. They are the warm-up.

### 3.4 Heaviest consumers — what must be extracted last

```
 29 vars   5 calls  updateSettingsData
 23 vars   1 calls  handleWebDiags
 20 vars   4 calls  updateGeocacheData
 19 vars  21 calls  loop
 15 vars   1 calls  gcApplyFilters
 12 vars   5 calls  initWiFi
 12 vars   1 calls  readIMU
```

These are consumers, not providers: they read across every domain. `updateSettingsData` alone touches 29 of the 97. They go **last**, once the state they read has an owner to read it from.

### 3.5 The file is one connected component — seams must be created, not discovered (Q3)

Join every pair of functions that share an external-linkage variable. The result is **a single component of 145 functions**. Remove the nine hot-core variables and it is *still* one component of 113 functions sharing 82 variables. There is no natural seam by shared state alone.

The cut set measures how much state must be deliberately owned before seams appear. Greedily remove the highest-fan-in variable and re-join, until the largest cluster is under 40 functions:

```
 0 removed: largest = 145
 8 removed: largest = 116
16 removed: largest = 104
24 removed: largest =  71   17  6  3  2
32 removed: largest =  52    7  6  5  3
35 removed: largest =  26   14  7  6  5  5   ← seams
```

**Roughly 35 of the 97 external-linkage variables — more than a third — must become owned interfaces before the rest of the file separates into domains on its own.** The greedy walk is tie-order sensitive (many variables sit at fan-in 11–15), so the exact integer moves between 35 and 37 across runs; the first ten in removal order do not: `webServer, gpsData, sdAvailable, framAvailable, useMetricUnits, settingsSubScreen, useFahrenheit, cacheListCount, currentScreen, tftBrightness`.

That set *is* the ownership backlog. It is not a side effect of decomposition; it is the decomposition.

---

## 4. Answering #215's questions

**Q1 — dead globals.** The question named `bmeAvailable` on the reasoning that the SHT41 replaced the BME in v0.20. **It is not dead: 20 references**, against `shtAvailable`'s 35.

Exactly one external-linkage variable is dead, and the linker had to be corrected to find it: **`touchDetected` is write-only.** `touchISR()` sets it (`src.ino:1039`, *"Flag only — NO I2C in ISR"*) and no function ever reads it. The first version of the graph script reported it as *unreferenced* because it did not attribute `.iram1.*` sections to the ISR; fixing that shows the write and confirms the absence of any read. So: one dead variable, and it is a leftover ISR flag, not a sensor-availability bool.

**Q2 — domains.** §3.1 and §3.5. The external-linkage set clusters as the fan-in table suggests once the hot core is owned: weather (`weatherHistory` 6,912 B, `weatherTrend`), geocache (`cacheList` 3,200 B, `cacheListCount`), sensor handles (`envSensor`, `lsm`, `oled`, `tft`), GPS (`gpsData`, `gpsBuffer`), peripheral availability (`sdAvailable`, `framAvailable`, `shtAvailable`, `bmeAvailable`), UI state (`currentScreen`, `settingsSubScreen`), units (`useMetricUnits`, `useFahrenheit`). But §3.5 is the constraint: those domains do not exist as separable regions *yet*.

**Q3 — the real dependency graph.** §3. Computed from relocations, committed as `scripts/reference-graph.py`, dumpable in full with `--json`.

**Q4 — extraction order.** Determined by the graph, in four bands:

1. **Leaves** — the 47 functions with no shared state and no intra-file calls. Zero risk, any order.
2. **Logging and SD safety** — `logPrintf`, `logPrintln`, `logPrint`, `sdOpenSafe`, `recordSDSuccess`. Highest fan-in in the file; nothing above them can move until they have.
3. **The cut set, as ownership units** — the ~35 variables of §3.5, each taken into the unit that owns it, in removal order. `webServer` first.
4. **Consumers** — `updateSettingsData`, `handleWebDiags`, `updateGeocacheData`, `loop`, `gcApplyFilters`. Last, because they read everything.

Every step is verified the way [#186](https://github.com/Strycher/Field_Compass/issues/186) established: symbol-set comparison via `nm`, since identical size totals do not imply identical output.

**Q5 — what breaks on leaving the `.ino`.** **13 functions**, listed by the script: `calcBearing`, `calcDistanceKm`, `calculateForecast`, `getCardinal`, `getIaqQualityText`, `getTrendArrow`, `isBatteryConnected`, `loadBsecFromFRAM`, `loadBsecState`, `mktimeUTC`, `readBME688`, `readIMU`, `readSHT41`. Each needs an explicit prototype written before its callers move. The other 202 either have one already or are never called early.

**Q6 — a `globals.h` of externs as Phase 1?** **No, and §2 and §3.5 together are the reason.** A `globals.h` would publish all 97 external-linkage variables as a permanent interface — converting an accidental god object into a documented one, and making every later narrowing a breaking change against a header other units include. Worse, §3.5 shows the file has no natural seams: a `globals.h` would freeze exactly the coupling that has to be dissolved. This supports the owner's existing ruling in [#212](https://github.com/Strycher/Field_Compass/issues/212) — *"no transitional externs header at any point"* — with measurement rather than assertion.

---

## 5. The anonymous-struct blocker — confirmed, with lines

```
src/src.ino:463  } gpsData;
src/src.ino:490  } imuData;
src/src.ino:573  } shtData;
src/src.ino:585  } envData;
src/src.ino:607  } weatherTrend;
```

`extern` cannot name a variable of anonymous struct type; a header copy gives each unit a distinct type plus duplicate definitions; `extern decltype(gpsData) gpsData;` is circular. **Naming them is a prerequisite**, and it is declaration-only. Four of the five are in the cut set (`gpsData` #2, `envData` #13, `shtData` #15, `imuData` #18), so this blocks band 3 almost immediately.

The file already names six other structs (`FRAMHeader`, `FRAMBatteryEntry`, `FRAMSettings`, `GeocacheEntry`, `TZPreset`, `WeatherReading`); the five state blobs are the exception, which makes the fix uncontroversial.

---

## 6. What this changes about the plan

1. **Every `[REGEX]` figure in [#212](https://github.com/Strycher/Field_Compass/issues/212) is superseded by this document.** Five of six checked were wrong; two were wrong by multiples.
2. **Stage 2's "hot-core ownership" is scoped against the wrong set and the wrong size.** It names eight variables from a pool it thought was 111. The real pool is 97, the real cut set is ~35, and the most-referenced variable in the file was not on its list.
3. **The `.ino`→`.cpp` risk is small and enumerated.** Thirteen named functions need a prototype; the plan's "33%" mitigation row can be replaced by a checklist.
4. **Extraction order is now determined, not guessed** — §4 Q4, four bands.
5. **The per-extraction verification method is unaffected**, and it is what produced this document.

## 7. Proposed, not created

Structure is the owner's grant. Proposed tasks under [#212](https://github.com/Strycher/Field_Compass/issues/212), in order:

- **Name the five anonymous structs** and remove `touchDetected`. Declaration-only; unblocks band 3.
- **Write the 13 missing prototypes.** Mechanical; unblocks every `.cpp` move.
- **Band 1 — move the 47 leaves.** One PR, zero externs, proves the harness.
- **Band 2 — extract logging and SD safety** into their own unit.
- **Band 3 — own the cut set**, one unit per domain, in removal order. This is the bulk of the Epic and where the ~35 variables become interfaces.
- **Band 4 — consumers and screens**, last.
