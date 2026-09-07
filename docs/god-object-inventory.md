# God object inventory — what `src.ino` actually contains, and who uses what

**Task:** [#215](https://github.com/Strycher/Field_Compass/issues/215) · **Epic:** [#212](https://github.com/Strycher/Field_Compass/issues/212) · **Feature:** [#151](https://github.com/Strycher/Field_Compass/issues/151)

Measured from the linker's view of the compiled object and its relocations, not from reading the source. Every number here reproduces with:

```bash
pio run -e feather_s3
python scripts/symbol-inventory.py             # what the file defines
python scripts/reference-graph.py              # who uses what, cut set, prototype reliance
python scripts/reference-graph.py --appendix   # the tables in the appendix, verbatim
```

---

## 1. The previous numbers were wrong where it mattered

[#212](https://github.com/Strycher/Field_Compass/issues/212) carries a table tagged `[REGEX]` with an explicit warning that no such number should be planned against until this task cross-validated it. That warning was justified — though not uniformly:

| measure | regex claimed | linker says | verdict |
|---|---|---|---|
| functions authored in `src.ino` | 206 | **209** | within three — the regex counted functions well |
| of which `static` | 44 | **42** | within two |
| `IRAM_ATTR` functions | 0 | **1** — `touchISR`, in `.iram1.0` | wrong |
| file-scope variables | 111 | **295** | **a 2.7× undercount** |
| functions relying on generated prototypes | 70 | **13** | **a 5× overcount** |

The pattern is the finding. The regex was competent at counting *functions* and at counting *references* (its fan-in figures for the variables it did find match the linker's). It was blind to `static` variable *declarations*, and it conflated two different questions when measuring prototype reliance. The two errors that would have misdirected the plan are the two that touch state and the `.ino`→`.cpp` boundary — exactly the things a decomposition plan is about.

The object file carries 215 `T`/`t` symbols; the 209 excludes two library inlines emitted into this translation unit (`lv_obj_set_style_pad_all`, a `WebServer::streamFile` instantiation), the static initialiser, one compiler clone, and two lambda bodies. None of those is a function anyone will extract. Debug info says where each symbol was defined, so the filter is by evidence rather than by name pattern alone.

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

It counted functions *called before their definition* — there are 71 — and reported that as reliance on the Arduino preprocessor. But 58 of those 71 already have an explicit forward declaration in the source. Only **13** have none and would break the moment their code leaves the `.ino`. They are listed in Appendix F.

---

## 2. The number that governs the work is 97, not 295

**A `static` file-scope variable is already private to its translation unit.** When its code moves into `screen_compass.cpp`, it moves with it, needs no `extern`, and the compiler enforces that nothing else can reach it. Those 198 are not an obstacle; they are 198 pieces of state that are *already* correctly scoped and will follow their code for free.

**An external-linkage variable is what couples translation units.** Those 97 are the entire cross-unit surface. Every one either moves into exactly one new unit and stays private there, or becomes a deliberately published interface.

So the extraction problem is **97 variables wide**, not 295 — a materially easier problem than either raw number suggests, and different in kind. Appendix A lists all 97 with their fan-in.

---

## 3. The reference graph — who uses what

Derived from `objdump -r` over the per-function sections the build already emits (`-ffunction-sections` / `-fdata-sections` are on). Only references to symbols `src.ino` itself defines are kept; library calls are excluded. Function-centric tables count authored functions only; variable fan-in counts every referrer, because a reference is a reference.

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
 11  imuData, magAvailable, geocacheSubScreen, fram, oledSleepMs, tftSleepMs
```

The regex found eight of the top nine, with matching counts. It missed the single most-referenced variable in the file: `webServer`, 24 users.

### 3.2 Functions by fan-in — what must be extracted first (Q4)

```
 54  logPrintf
 40  logPrintln
 14  sdOpenSafe
 13  logPrint
 12  recordSDSuccess
  7  gcUpdateFilterLabels
  6  fcDropdownSetValue
  5  fcHeaderCreate, findTimeoutIndex, framWriteHeader, gcApplyFilters, getCardinal
```

The plan's "logging must be early regardless" is confirmed by the linker: the five logging/SD-safety functions sit at the bottom of the call stack with 133 incoming edges between them. They are extracted **first**, before anything that calls them moves. The full list of functions with two or more callers is Appendix D.

### 3.3 Coupling profile

```
authored functions touching NO external-linkage variable : 64 of 209
   ...and calling no other src.ino function               : 41   ← free extractions
```

Forty-one functions are pure leaves — no shared state, no intra-file calls. They can move at any time, in any order, with no `extern` and no risk. They are the warm-up, and they are named in Appendix C.

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

These are consumers, not providers: they read across every domain. `updateSettingsData` alone touches 29 of the 97. They go **last**, once the state they read has an owner to read it from. Everything at eight or more variables is Appendix E.

### 3.5 The file is one connected component — seams must be created, not discovered (Q3)

Join every pair of functions that share an external-linkage variable. The result is **a single component of 145 functions**. Remove the nine hot-core variables and it is *still* one component of 113 functions sharing 82 variables. There is no natural seam by shared state alone.

The cut set measures how much state must be deliberately owned before seams appear. Greedily remove the highest-fan-in variable and re-join, until the largest cluster is under 40 functions:

```
 0 removed: largest = 145
 8 removed: largest = 113
16 removed: largest = 104
24 removed: largest =  71
32 removed: largest =  52
35 removed: largest =  25   19  7  6  5  5  3  2  2  2   ← seams
```

**35 of the 97 external-linkage variables — more than a third — must become owned interfaces before the rest of the file separates into domains on its own.** The full removal order, with the largest cluster after each step, is Appendix B.

Greedy removal is a heuristic, and many variables tie at fan-in 11–15. The first version of the script broke ties by dictionary order and gave 35 on one run and 37 on another; ties now break by name, so the figure is reproducible. The shape does not depend on the tie rule — either way, more than a third of the external state has to be owned, and the first nine removed are the same nine.

That set *is* the ownership backlog. It is not a side effect of decomposition; it is the decomposition.

---

## 4. Answering #215's questions

**Q1 — dead globals.** The question named `bmeAvailable` on the reasoning that the SHT41 replaced the BME in v0.20. **It is not dead: 20 references**, against `shtAvailable`'s 35.

Exactly one external-linkage variable is dead, and the graph script had to be corrected to see it properly: **`touchDetected` is write-only.** `touchISR()` sets it (`src.ino:1039`, *"Flag only — NO I2C in ISR"*) and no function ever reads it. The first version of the script reported it as *unreferenced* because it did not attribute `.iram1.*` sections to the ISR; fixing that shows the write and confirms the absence of any read. So: one dead variable, and it is a leftover ISR flag, not a sensor-availability bool.

**Q2 — domains.** §3.1 and §3.5. The external-linkage set clusters as the fan-in table suggests once the hot core is owned: weather (`weatherHistory` 6,912 B, `weatherTrend`), geocache (`cacheList` 3,200 B, `cacheListCount`, the `gcFilter*` family), sensor handles (`envSensor`, `lsm`, `lis`, `sht4`, `oled`, `tft`, `ctp`, `rtc`), GPS (`gpsData`, `gpsBuffer`), peripheral availability (`sdAvailable`, `framAvailable`, `shtAvailable`, `bmeAvailable`, `imuAvailable`, `magAvailable`, `oledAvailable`, `rtcAvailable`, `touchAvailable`, `batteryAvailable`), UI state (`currentScreen`, `previousScreen`, `settingsSubScreen`, `geocacheSubScreen`), units and timezone (`useMetricUnits`, `useFahrenheit`, `use12Hour`, `posixTZ`, `tz*`), display sleep (`tftSleepMs`, `oledSleepMs`, `tftSleeping`, `oledSleeping`, `lastActivityTime`), magnetometer calibration (the `magCal*` and `magOffset*` families). But §3.5 is the constraint: those domains do not exist as separable regions *yet*.

**Q3 — the real dependency graph.** §3 and the appendix. Computed from relocations, committed as `scripts/reference-graph.py`, dumpable in full with `--json`.

**Q4 — extraction order.** Determined by the graph, in four bands:

1. **Leaves** — the 41 functions with no shared state and no intra-file calls (Appendix C). Zero risk, any order.
2. **Logging and SD safety** — `logPrintf`, `logPrintln`, `logPrint`, `sdOpenSafe`, `recordSDSuccess`. Highest fan-in in the file; nothing above them can move until they have.
3. **The cut set, as ownership units** — the 35 variables of Appendix B, each taken into the unit that owns it, in removal order. `webServer` first.
4. **Consumers** — `updateSettingsData`, `handleWebDiags`, `updateGeocacheData`, `loop`, `gcApplyFilters` (Appendix E). Last, because they read everything.

Every step is verified the way [#186](https://github.com/Strycher/Field_Compass/issues/186) established: symbol-set comparison via `nm`, since identical size totals do not imply identical output.

**Q5 — what breaks on leaving the `.ino`.** **13 functions**, Appendix F. Each needs an explicit prototype written before its callers move. The other 196 authored functions either have one already or are never called early.

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

1. **Every `[REGEX]` figure in [#212](https://github.com/Strycher/Field_Compass/issues/212) is superseded by this document.** The function counts survive within a handful; the state and prototype figures do not.
2. **Stage 2's "hot-core ownership" is scoped against the wrong set and the wrong size.** It names eight variables from a pool it thought was 111. The real pool is 97, the real cut set is 35, and the most-referenced variable in the file was not on its list.
3. **The `.ino`→`.cpp` risk is small and enumerated.** Thirteen named functions need a prototype; the plan's "33%" mitigation row can be replaced by a checklist.
4. **Extraction order is now determined, not guessed** — §4 Q4, four bands.
5. **The per-extraction verification method is unaffected**, and it is what produced this document.

## 7. Proposed, not created

Structure is the owner's grant. Proposed tasks under [#212](https://github.com/Strycher/Field_Compass/issues/212), in order:

- **Name the five anonymous structs** and remove `touchDetected`. Declaration-only; unblocks band 3.
- **Write the 13 missing prototypes** (Appendix F). Mechanical; unblocks every `.cpp` move.
- **Band 1 — move the 41 leaves** (Appendix C). One PR, zero externs, proves the harness.
- **Band 2 — extract logging and SD safety** into their own unit.
- **Band 3 — own the cut set** (Appendix B), one unit per domain, in removal order. This is the bulk of the Epic and where the 35 variables become interfaces.
- **Band 4 — consumers and screens** (Appendix E), last.

---

## Appendix — the full tables

Generated verbatim by `python scripts/reference-graph.py --appendix` against `main` at the commit this document landed in. Regenerate rather than hand-edit.

### A. All 97 external-linkage variables by fan-in

| fan-in | variable | fan-in | variable | fan-in | variable |
|---:|---|---:|---|---:|---|
| 24 | `webServer` | 19 | `gpsData` | 18 | `sdAvailable` |
| 16 | `framAvailable` | 16 | `useMetricUnits` | 15 | `cacheListCount` |
| 15 | `currentScreen` | 15 | `settingsSubScreen` | 15 | `useFahrenheit` |
| 13 | `cacheList` | 13 | `tftBrightness` | 12 | `bmeAvailable` |
| 12 | `envData` | 12 | `shtAvailable` | 12 | `shtData` |
| 11 | `fram` | 11 | `geocacheSubScreen` | 11 | `imuData` |
| 11 | `magAvailable` | 11 | `oledSleepMs` | 11 | `tftSleepMs` |
| 10 | `framHeader` | 10 | `tzDisplayName` | 10 | `use12Hour` |
| 9 | `batteryAvailable` | 9 | `imuAvailable` | 9 | `oled` |
| 9 | `tftSleeping` | 9 | `tzSelectedIndex` | 8 | `battery` |
| 8 | `posixTZ` | 8 | `wifiConnected` | 7 | `lastActivityTime` |
| 7 | `listHighlightIndex` | 7 | `oledAvailable` | 7 | `oledSleeping` |
| 7 | `selectedCacheIndex` | 7 | `weatherTrend` | 6 | `envSensor` |
| 6 | `gcFilteredCount` | 6 | `weatherHistoryCount` | 5 | `gcFilterDMax` |
| 5 | `gcFilterDMin` | 5 | `gcFilterFoundMode` | 5 | `gcFilterMaxDistKm` |
| 5 | `gcFilterTMax` | 5 | `gcFilterTMin` | 5 | `gcSortMode` |
| 5 | `magCalibrating` | 5 | `magOffsetX` | 5 | `magOffsetY` |
| 5 | `oledTimeoutLabels` | 5 | `tft` | 5 | `tftTimeoutLabels` |
| 4 | `gcFilteredIndices` | 4 | `magOffsetZ` | 4 | `touchAvailable` |
| 3 | `gcCachedDist` | 3 | `listScrollOffset` | 3 | `magCalMaxX` |
| 3 | `magCalMaxY` | 3 | `magCalMaxZ` | 3 | `magCalMinX` |
| 3 | `magCalMinY` | 3 | `magCalMinZ` | 3 | `magCalStartTime` |
| 3 | `magCalibrated` | 3 | `rtcAvailable` | 2 | `ctp` |
| 2 | `gcLastSortTime` | 2 | `lastFramFlush` | 2 | `lastWiFiAttempt` |
| 2 | `lis` | 2 | `lsm` | 2 | `ntpSynced` |
| 2 | `previousScreen` | 2 | `rtc` | 2 | `rtcSyncedFromGPS` |
| 2 | `sht4` | 2 | `weatherHistory` | 2 | `weatherHistoryHead` |
| 2 | `webServerStarted` | 1 | `NTP_SERVER` | 1 | `WIFI_PASS_1` |
| 1 | `WIFI_PASS_2` | 1 | `WIFI_PASS_3` | 1 | `WIFI_SSID_1` |
| 1 | `WIFI_SSID_2` | 1 | `WIFI_SSID_3` | 1 | `buttonCLongPressHandled` |
| 1 | `buttonCPressStart` | 1 | `gpsBuffer` | 1 | `gpsBufferIndex` |
| 1 | `lastButtonPress` | 1 | `lastWeatherLog` | 1 | `rtcSyncedFromNTP` |
| 1 | `touchDetected` |  |  |  |  |

### B. The cut set — 35 variables, in removal order

| # | variable | fan-in | largest cluster after removal |
|---:|---|---:|---:|
| 1 | `webServer` | 24 | 138 |
| 2 | `gpsData` | 19 | 134 |
| 3 | `sdAvailable` | 18 | 125 |
| 4 | `framAvailable` | 16 | 125 |
| 5 | `useMetricUnits` | 16 | 125 |
| 6 | `cacheListCount` | 15 | 122 |
| 7 | `currentScreen` | 15 | 121 |
| 8 | `settingsSubScreen` | 15 | 113 |
| 9 | `useFahrenheit` | 15 | 113 |
| 10 | `cacheList` | 13 | 109 |
| 11 | `tftBrightness` | 13 | 108 |
| 12 | `bmeAvailable` | 12 | 108 |
| 13 | `envData` | 12 | 107 |
| 14 | `shtAvailable` | 12 | 107 |
| 15 | `shtData` | 12 | 104 |
| 16 | `fram` | 11 | 104 |
| 17 | `geocacheSubScreen` | 11 | 101 |
| 18 | `imuData` | 11 | 101 |
| 19 | `magAvailable` | 11 | 101 |
| 20 | `oledSleepMs` | 11 | 101 |
| 21 | `tftSleepMs` | 11 | 82 |
| 22 | `framHeader` | 10 | 71 |
| 23 | `tzDisplayName` | 10 | 71 |
| 24 | `use12Hour` | 10 | 71 |
| 25 | `batteryAvailable` | 9 | 71 |
| 26 | `imuAvailable` | 9 | 69 |
| 27 | `oled` | 9 | 65 |
| 28 | `tftSleeping` | 9 | 64 |
| 29 | `tzSelectedIndex` | 9 | 64 |
| 30 | `battery` | 8 | 60 |
| 31 | `posixTZ` | 8 | 60 |
| 32 | `wifiConnected` | 8 | 52 |
| 33 | `lastActivityTime` | 7 | 46 |
| 34 | `listHighlightIndex` | 7 | 45 |
| 35 | `oledAvailable` | 7 | 25 |

Clusters remaining afterwards: 25, 19, 7, 6, 5, 5, 3, 2, 2, 2.

Two steps are worth noticing. Removing `tftSleepMs` (#21) drops the largest cluster from 101 to 82, and removing `oledAvailable` (#35) drops it from 45 to 25 — those two variables are the hinges holding the largest blobs together, which is not visible from their fan-in alone.

### C. The 41 leaf functions — no shared state, no intra-file calls

| | | |
|---|---|---|
| `calcBearing(float, float, float, float)` | `calcDistanceKm(float, float, float, float)` | `compassRoseDrawCb(_lv_event_t*)` |
| `decodeROT13(char*)` | `extractXMLField(String const&, char const*, char const*, char*, unsigned int)` | `fcActionBarCreate(_lv_obj_t*, bool, bool)` |
| `fcDropdownCreate(_lv_obj_t*, short, char const*, char const*)` | `fcDropdownGetIndex(_lv_obj_t*)` | `fcDropdownSetValue(_lv_obj_t*, int, char const*)` |
| `fcHeaderCreate(_lv_obj_t*, char const*)` | `fcHeaderSetSDStatus(_lv_obj_t*, SDIndicatorState)` | `fcHeaderSetTitle(_lv_obj_t*, char const*)` |
| `fcListPickerItemCb(_lv_event_t*)` | `fcNavBarCreate(_lv_obj_t*, unsigned char, unsigned char)` | `fcNavBarSetActive(_lv_obj_t*, unsigned char)` |
| `fcSettingsScrollCreate(_lv_obj_t*, int)` | `fcToggleClickCb(_lv_event_t*)` | `fcToggleCreate(_lv_obj_t*, short, char const*, char const*, char const*, bool)` |
| `fcToggleGetValue(_lv_obj_t*)` | `fcToggleSetValue(_lv_obj_t*, bool)` | `fcToggleUpdateVisuals(_lv_obj_t*, bool)` |
| `findTimeoutIndex(unsigned long const*, int, unsigned long)` | `getCardinal(float)` | `getCurrentTimestamp()` |
| `getIaqAccuracyText(unsigned char)` | `getIaqQualityText(float)` | `getWeatherFilename(char*, int)` |
| `hPaToInHg(float)` | `logTimestamp()` | `lvglEncoderReadCb(_lv_indev_t*, lv_indev_data_t*)` |
| `lvglLogCb(signed char, char const*)` | `lvglTickCb()` | `nmeaParse(char*, char**, int)` |
| `recordSDSuccess()` | `sameLocation(float, float, float, float)` | `serialRingAppend(char const*)` |
| `serialRingPeek()` | `serialRingRead()` | `shouldAttemptReInit()` |
| `telShowGpsDataRows(bool)` | `telShowImuDataRows(bool)` |  |

The `fc*` widget helpers (fourteen of the 41) are the `ui_widgets` unit the plan already anticipated, and they are already leaves. `recordSDSuccess` is a leaf with twelve callers — it touches only `static` state, which moves with it.

### D. Authored functions with two or more callers inside `src.ino` (46)

| callers | function |
|---:|---|
| 54 | `logPrintf(char const*, ...)` |
| 40 | `logPrintln(char const*)` |
| 14 | `sdOpenSafe(char const*, char const*, bool)` |
| 13 | `logPrint(char const*)` |
| 12 | `recordSDSuccess()` |
| 7 | `gcUpdateFilterLabels()` |
| 6 | `fcDropdownSetValue(_lv_obj_t*, int, char const*)` |
| 5 | `fcHeaderCreate(_lv_obj_t*, char const*)` |
| 5 | `findTimeoutIndex(unsigned long const*, int, unsigned long)` |
| 5 | `framWriteHeader()` |
| 5 | `gcApplyFilters()` |
| 5 | `getCardinal(float)` |
| 4 | `applyTimezone()` |
| 4 | `calcDistanceKm(float, float, float, float)` |
| 4 | `fcNavBarCreate(_lv_obj_t*, unsigned char, unsigned char)` |
| 4 | `fcNavBarSetActive(_lv_obj_t*, unsigned char)` |
| 4 | `formatTimeStr(char*, int, int, int, bool)` |
| 4 | `getIaqAccuracyText(unsigned char)` |
| 4 | `getTrendArrow()` |
| 4 | `loadSettings()` |
| 4 | `logTimestamp()` |
| 4 | `serialLogAppend(char const*)` |
| 4 | `serialLogFlush()` |
| 3 | `calcBearing(float, float, float, float)` |
| 3 | `fcListPickerOpen(char const*, char const**, int, int, _lv_obj_t*)` |
| 3 | `framFlushToSD()` |
| 3 | `getIaqQualityText(float)` |
| 3 | `getWeatherFilename(char*, int)` |
| 3 | `isBatteryConnected()` |
| 3 | `recordSDError(SDErrorType)` |
| 3 | `saveSettings()` |
| 3 | `serialRingAppend(char const*)` |
| 3 | `wakeAllDisplays()` |
| 2 | `addToWeatherHistory(WeatherReading)` |
| 2 | `extractXMLField(String const&, char const*, char const*, char*, unsigned int)` |
| 2 | `fcToggleSetValue(_lv_obj_t*, bool)` |
| 2 | `getCurrentTimestamp()` |
| 2 | `initWebServer()` |
| 2 | `loadCacheFoundStatus()` |
| 2 | `mktimeUTC(tm*)` |
| 2 | `navigateScreen(int)` |
| 2 | `parseGPXFromString(String const&, bool)` |
| 2 | `saveCacheFoundStatus()` |
| 2 | `serialLogRotate()` |
| 2 | `syncRTCFromSystemTime(char const*)` |
| 2 | `trySDReInit()` |

### E. Authored functions touching eight or more external-linkage variables (20)

| vars | calls | function |
|---:|---:|---|
| 29 | 5 | `updateSettingsData()` |
| 23 | 1 | `handleWebDiags()` |
| 20 | 4 | `updateGeocacheData()` |
| 19 | 21 | `loop()` |
| 15 | 1 | `gcApplyFilters()` |
| 12 | 5 | `initWiFi()` |
| 12 | 1 | `readIMU()` |
| 11 | 2 | `factoryReset()` |
| 11 | 3 | `handleButtonCShortPress()` |
| 11 | 6 | `handleButtons()` |
| 11 | 2 | `loadSettingsFromFRAM()` |
| 11 | 5 | `resetBtnCb(_lv_event_t*)` |
| 11 | 1 | `saveSettingsToFRAM()` |
| 11 | 3 | `updateCompassData()` |
| 10 | 5 | `buildSettingsScreen()` |
| 9 | 1 | `calStartBtnCb(_lv_event_t*)` |
| 9 | 2 | `handleWebJSON()` |
| 9 | 5 | `loadSettings()` |
| 9 | 4 | `saveSettings()` |
| 8 | 0 | `gcUpdateFilterLabels()` |

### F. The 13 functions that rely on Arduino-generated prototypes

- `calcBearing(float, float, float, float)`
- `calcDistanceKm(float, float, float, float)`
- `calculateForecast()`
- `getCardinal(float)`
- `getIaqQualityText(float)`
- `getTrendArrow()`
- `isBatteryConnected()`
- `loadBsecFromFRAM()`
- `loadBsecState()`
- `mktimeUTC(tm*)`
- `readBME688()`
- `readIMU()`
- `readSHT41()`

Four of these (`calcBearing`, `calcDistanceKm`, `getCardinal`, `getIaqQualityText`) are also in Appendix C — leaves that only need a prototype to move. They are the natural first PR of band 1. `getTrendArrow` is not a leaf: it reads `weatherTrend`, and moves with the weather unit in band 3.
