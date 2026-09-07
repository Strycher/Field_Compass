#!/usr/bin/env python3
"""Inventory what src.ino actually defines, according to the linker (#215).

Reads the compiled object file rather than the source, because pattern-matching
over the source got this badly wrong: it reported 111 file-scope variables where
the linker sees 295, having silently skipped every `static` declaration. The
plan built on that number would have been planned against a third of the real
state. See docs/god-object-inventory.md.

The whole-firmware ELF is the wrong input -- it carries ~8,500 functions from
Arduino core, LVGL, TFT_eSPI and the rest. `src.ino.cpp.o` contains exactly what
the god object defines and nothing else.

Usage:
    pio run -e feather_s3          # produce the object file first
    python scripts/symbol-inventory.py
    python scripts/symbol-inventory.py --json
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys

OBJ = pathlib.Path(".pio/build/feather_s3/src/src.ino.cpp.o")
TOOLCHAIN_GLOB = "packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm*"


def find_nm() -> str:
    """Locate the toolchain's nm. The host nm is x86 binutils and is not
    guaranteed to read an Xtensa object -- the same reasoning that keeps the CI
    size step on `pio run --target size` rather than binutils `size`."""
    root = pathlib.Path.home() / ".platformio"
    hits = sorted(root.glob(TOOLCHAIN_GLOB))
    if not hits:
        sys.exit(f"could not find xtensa-esp32s3-elf-nm under {root}")
    return str(hits[0])


def symbols(nm: str, obj: pathlib.Path):
    out = subprocess.run([nm, "--defined-only", "--print-size", str(obj)],
                         capture_output=True, encoding="utf-8", errors="replace")
    if out.returncode != 0:
        sys.exit(f"nm failed: {out.stderr[:300]}")
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        cls, name = parts[-2], parts[-1]
        size = int(parts[1], 16) if len(parts) >= 4 else 0
        yield cls, name, size


def classify(rows):
    """Split by linkage, which is what actually matters for extraction.

    A `static` file-scope variable is TU-private: it moves with whatever unit
    uses it and needs no extern. An external-linkage global is the one that
    couples translation units together. Counting them as one number hides the
    only distinction that changes how hard the work is.
    """
    inv = {
        "functions_external": [],   # T
        "functions_static": [],     # t
        "vars_external": [],        # B/D, plain name
        "vars_static": [],          # B/b/D/d, _ZL... = file-scope static
        "vars_function_local": [],  # _ZZ... = static inside a function
        "other": [],
    }
    for cls, name, size in rows:
        if cls == "T":
            inv["functions_external"].append((name, size))
        elif cls == "t":
            inv["functions_static"].append((name, size))
        elif cls in ("B", "b", "D", "d"):
            if name.startswith("_ZZ"):
                inv["vars_function_local"].append((name, size))
            elif name.startswith("_ZL"):
                inv["vars_static"].append((name, size))
            else:
                inv["vars_external"].append((name, size))
        else:
            inv["other"].append((cls, name))
    return inv


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--obj", default=str(OBJ))
    a = ap.parse_args()

    obj = pathlib.Path(a.obj)
    if not obj.exists():
        sys.exit(f"{obj} not found -- run `pio run -e feather_s3` first")

    inv = classify(symbols(find_nm(), obj))
    fe, fs = len(inv["functions_external"]), len(inv["functions_static"])
    ve, vs, vl = (len(inv["vars_external"]), len(inv["vars_static"]),
                  len(inv["vars_function_local"]))

    if a.json:
        print(json.dumps({k: [n for n, _ in v] if k != "other" else v
                          for k, v in inv.items()}, indent=2))
        return 0

    print(f"src.ino symbol inventory, from {obj}\n")
    print(f"  functions                     {fe + fs}")
    print(f"    external linkage (T)        {fe}")
    print(f"    static (t)                  {fs}")
    print()
    print(f"  file-scope variables          {ve + vs}")
    print(f"    external linkage            {ve}   <-- couples translation units")
    print(f"    static (TU-private)         {vs}   <-- moves with its unit, no extern")
    print()
    print(f"  function-local statics        {vl}   (not file scope)")
    print(f"  other symbol classes          {len(inv['other'])}")
    print()
    print("  The external-linkage variable count is the extraction difficulty.")
    print("  Static ones are already private to this translation unit and follow")
    print("  whichever new unit uses them.")
    biggest = sorted(inv["vars_external"], key=lambda t: -t[1])[:8]
    if biggest:
        print("\n  largest external-linkage variables by size:")
        for n, s in biggest:
            print(f"    {s:>7}  {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
