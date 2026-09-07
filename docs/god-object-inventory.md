# God object inventory — what `src.ino` actually contains

**Task:** [#215](https://github.com/Strycher/Field_Compass/issues/215) · **Epic:** [#212](https://github.com/Strycher/Field_Compass/issues/212) · **Feature:** [#151](https://github.com/Strycher/Field_Compass/issues/151)

Measured from the linker's view of the compiled object, not from reading the source. Reproduce with:

```bash
pio run -e feather_s3
python scripts/symbol-inventory.py
```

---

## 1. The previous numbers were wrong, and one of them was wrong enough to matter

[#212](https://github.com/Strycher/Field_Compass/issues/212) carries a table tagged `[REGEX]` with an explicit warning that no such number should be planned against until this task cross-validated it. That warning was justified:

| measure | regex claimed | linker says | error |
|---|---|---|---|
| functions | 206 | **215** | −9 |
| `static` functions | 44 | **48** | −4 |
| file-scope variables | 111 | **295** | **−184, a 2.7× undercount** |

The function counts were close enough to be merely untidy. **The variable count was not.** A decomposition plan built on 111 pieces of state would have been planned against roughly a third of what is actually there.

### Why the regex missed two thirds of the state

It matched declarations at brace depth zero but never matched `static` ones. Every file-scope `static` variable — 198 of them, largely LVGL widget pointers such as `compassScr`, `gcDetLblDT`, `gcListRows` — was invisible to it.

The linker cannot make that mistake. In the object file they appear as `_ZL…` symbols, and demangling confirms them: `_ZL10compassScr` → `compassScr`.

The arithmetic closes exactly, which is how we know the classification is right rather than merely plausible:

```
 97  external-linkage file-scope variables   (plain names)
198  static file-scope variables             (_ZL…)
 13  function-local statics                  (_ZZ…)
---
308  total B/b/D/d symbols in src.ino.cpp.o
```

---

## 2. The number that actually governs the work is 97, not 295

This is the finding that changes planning, and it is invisible if you count variables as one bucket.

**A `static` file-scope variable is already private to its translation unit.** When its code moves into `screen_compass.cpp`, it moves with it, needs no `extern`, and cannot be referenced from anywhere else — the compiler enforces that. Those 198 are not an obstacle; they are 198 pieces of state that are *already* correctly scoped and will follow their code for free.

**An external-linkage variable is what couples translation units.** Those 97 are the entire cross-unit surface. Every one either has to move into exactly one new unit and stay private, or become a deliberate published interface.

So the extraction problem is **97 variables wide**, not 295. That is a materially easier problem than either raw number suggests, and materially different in kind.

The largest external-linkage variables, by bytes, are where the domains are most obvious:

```
   6912  weatherHistory
   3200  cacheList
    680  envSensor
    420  webServer
    272  tft
    128  gpsBuffer
     88  lsm
     88  oled
```

---

## 3. The anonymous-struct blocker is real — confirmed, with lines

Five state blobs are single-instance anonymous structs:

```
src/src.ino:463  } gpsData;
src/src.ino:490  } imuData;
src/src.ino:573  } shtData;
src/src.ino:585  } envData;
src/src.ino:607  } weatherTrend;
```

`extern` cannot name a variable of anonymous struct type. Putting the anonymous definition in a header gives every translation unit a *distinct* type plus duplicate definitions; `extern decltype(gpsData) gpsData;` is circular.

**Naming them is a prerequisite for any extraction that crosses them**, and it is a declaration-only change — no call site moves.

Worth noting this is not a house style being violated: the file already names six other structs (`FRAMHeader`, `FRAMBatteryEntry`, `FRAMSettings`, `GeocacheEntry`, `TZPreset`, `WeatherReading`). The five state blobs are the exception, not the rule, which makes the fix uncontroversial.

---

## 4. Answering #215's questions

**Q1 — which globals are dead?** The question names `bmeAvailable` as a candidate, on the reasoning that the SHT41 replaced the BME as primary temp/humidity in v0.20.

**It is not dead: 20 references in `src.ino`** against `shtAvailable`'s 35. Both are live. The premise was wrong, and no global was found that the linker reports as defined but unused.

This is worth stating plainly because it removes a hoped-for simplification: there is no dead weight to delete before extracting. All 295 variables are carried forward.

**Q2 — coherent domains.** The external-linkage set clusters as the size table suggests: weather (`weatherHistory`, `weatherTrend`), geocache (`cacheList`, `cacheListCount`), sensor handles (`envSensor`, `lsm`, `oled`, `tft`), GPS (`gpsData`, `gpsBuffer`), peripheral availability (`sdAvailable`, `framAvailable`, `shtAvailable`, `bmeAvailable`), UI state (`currentScreen`, `settingsSubScreen`), units (`useMetricUnits`, `useFahrenheit`).

**Q3 / Q4 — dependency graph and extraction order.** Not answered here. Doing it properly requires per-function symbol references, which needs a relocation-level analysis (`nm --undefined-only` per section, or `objdump -r`) rather than the definition-level inventory this task produced. **This is the remaining gap** and should be its own task before extraction order is fixed.

**Q5 — reliance on generated prototypes.** Not re-measured. The `[REGEX]` figure of 70 of 206 is in the same family of unvalidated numbers as the ones corrected above, and should be treated as unknown until measured the same way.

**Q6 — is a `globals.h` of externs an acceptable Phase 1?**

**No, and the 97/198 split is the reason.** A `globals.h` would publish all 97 external-linkage variables as an explicit, permanent interface — converting an accidental god object into a documented one, and making every later attempt to narrow scope a breaking change against a header other units include.

The alternative is ownership-first: each unit takes its state with it and publishes only what its callers genuinely need. That is harder per step and leaves nothing to undo afterwards. It is also what the owner already ruled in [#212](https://github.com/Strycher/Field_Compass/issues/212) — *"no transitional externs header at any point"* — and this measurement supports that ruling rather than merely restating it.

---

## 5. What this changes about the plan

1. **Every `[REGEX]` figure still in [#212](https://github.com/Strycher/Field_Compass/issues/212) should be treated as unmeasured** until produced the way these were. Two of three checked were wrong; one was wrong by 2.7×.
2. **Stage 2's "hot-core ownership" is scoped against the wrong set.** It names eight hot globals from a pool it thought was 111. That analysis needs redoing against the real 97 external-linkage variables.
3. **The per-extraction verification method is unaffected.** Symbol-set comparison via `nm` is exactly what produced this document, and [#186](https://github.com/Strycher/Field_Compass/issues/186) already proved that identical size totals do not imply identical output.
4. **Question 3 — the dependency graph — is the real remaining unknown**, and extraction order cannot be fixed without it.

## 6. Proposed, not created

Structure is the owner's grant. Proposed next tasks under [#212](https://github.com/Strycher/Field_Compass/issues/212):

- **Name the five anonymous structs.** Declaration-only, no call sites touched, unblocks everything else.
- **Measure the reference graph.** Per-function symbol references, to answer Q3/Q4 with the same rigour as this document.
- **Re-scope hot-core ownership** against the real 97, replacing the figures Stage 2 currently assumes.
