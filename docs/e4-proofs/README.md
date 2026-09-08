# E4 proofs

One file per extraction, the verbatim output of `scripts/verify_extraction.py` at the time the band was committed. The header line of each file says which commits were compared. These are the evidence behind the "Measured" blocks of [`../god-object-inventory.md`](../god-object-inventory.md) §9–§15; the decision log is [`../e4-session-2026-09-07.md`](../e4-session-2026-09-07.md).

| file | band | issue / PR | merge |
|---|---|---|---|
| `verify-263-band2-logging.txt` | 2 | #263 / #269 | `4cc1f21` |
| `verify-270-band3a-fram-settings-rtc.txt` | 3a | #270 / #275 | `43e569a` |
| `verify-271-band3b-gps-imu-env-battery.txt` | 3b | #271 / #276 | `e6465bb` |
| `verify-272-band3c-weather-geocache.txt` | 3c | #272 / #277 | `0053f77` |
| `verify-273-band3d-oled-display-uistate-touch.txt` | 3d | #273 / #278 | `7bfefa7` |
| `verify-274-band3e-web.txt` | 3e | #274 / #279 | `ae514ed` |
| `verify-265a-band4-lvgl_port.txt` | 4, unit 1 | #265 / #280 | commit `9ce4ddb`, PR open |
| `verify-265b-band4-oled_screens.txt` | 4, unit 2 | #265 / #280 | commit `6ec9fe6`, PR open |
| `verify-265c-band4-screens-navigation.txt` | 4, unit 3 | #265 / #280 | commit `b6f2261`, PR open |

Bands 1 and E4-1/E4-2 (#260–#262) predate the script; their proofs were done by hand with the same `nm` method and are written up in inventory §8.

**What a PASS means.** The union of `nm --print-size --defined-only` over every object under `src/` has the same symbol set before and after, matched by demangled name (a linkage change or a deliberately changed signature is listed and must have been declared); every function whose size changed is attributed by comparing its reference set — calls, external and static variables — from the relocation graph, with the only tolerated differences being the ones a move produces; from band 3c on, every moved function's reference set is compared as well; and the linked output sections' deltas are printed so growth outside function bodies is visible. Anything else fails the script.

**What is not here.** The `nm` captures, reference-graph JSON, ELF and map files each proof consumed were session artefacts and are not committed; they are reproducible from the commits named in each header (build the "before" commit, capture, build the "after" commit, run the script with the `--moved` / `--published` lists in that band's commit message). The script's docstring is the usage.
