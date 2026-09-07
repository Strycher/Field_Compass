#!/usr/bin/env python3
"""Prove that an extraction moved code without changing it (E4, #262/#263).

The proof step of the per-extraction harness. extract_unit.py moves code;
this decides whether the move was behaviour-neutral, by the #186 method,
extended with per-function attribution:

  1. SYMBOL SET. `nm --print-size --defined-only` over the union of all our
     objects, before and after, compared by DEMANGLED name. A variable whose
     linkage changed (static `_ZL8sdHealth` -> published `sdHealth`) demangles
     to the same name, so it reads as one symbol with a linkage change rather
     than one removed and one added. A function whose signature deliberately
     changed is paired by its base name and listed as re-signed. Anything else
     removed or added is unexpected -- except a new TU's static initialiser,
     which every unit with a non-trivial global gains.

  2. FUNCTION SIZES. Every function whose size changed is attributed by
     comparing its reference set -- calls, external-linkage variables, static
     variables -- before and after, from the relocation graph. The tolerated
     differences are exactly the ones a move produces:
       * calls to functions that moved out (they are external now, so the
         intra-TU graph no longer lists them);
       * references to variables that moved out or were published (same);
       * a caller of a re-signed function gaining or losing the variable it
         now passes as an argument.
     A function whose reference set differs in any OTHER way is a semantic
     change and the script exits non-zero: that band stops.

  3. OUTPUT SECTIONS. `size -A` on the linked ELFs, so growth outside function
     bodies (literal pools, string sections, alignment) is measured per
     section rather than waved at.

Usage:
    python scripts/verify_extraction.py \
        --before-nm nm-before.txt --before-graph graph-before.json \
        --after-objs .pio/build/feather_s3/src/src.ino.cpp.o,.pio/build/feather_s3/src/logging.cpp.o,... \
        --before-elf <pre-move firmware.elf> --after-elf .pio/build/feather_s3/firmware.elf \
        --moved logPrintf,... --published sdHealth,... [--resigned initSerialLog]

Capture the before-side artefacts BEFORE running extract_unit.py, from a
build of the pre-move tree:
    for o in .pio/build/feather_s3/src/*.o; do
      nm --print-size --defined-only $o | sed "s/$/ $(basename $o .cpp.o)/"
    done > nm-before.txt
    python scripts/reference-graph.py --json --raw > graph-before.json
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import pathlib
import subprocess
import sys

TOOLS = pathlib.Path.home() / ".platformio/packages/toolchain-xtensa-esp32s3/bin"


def tool(name: str) -> str:
    hits = sorted(TOOLS.glob(f"xtensa-esp32s3-elf-{name}*"))
    if not hits:
        sys.exit(f"missing toolchain binary {name}")
    return str(hits[0])


def run(cmd: list[str]) -> str:
    return subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace").stdout


def demangle(names: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for i in range(0, len(names), 150):          # Windows command lines cap at 32K chars
        chunk = names[i:i + 150]
        lines = run([tool("c++filt")] + chunk).splitlines()
        out.update(zip(chunk, (l.strip() for l in lines)))
    return out


def base(dm: str) -> str:
    return dm.split("(")[0].strip()


def load_nm_tagged(p: pathlib.Path):
    out = {}
    for l in p.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = l.split()
        if not parts:
            continue
        obj, parts = parts[-1].split(".")[0], parts[:-1]   # "src.ino" and "src" are the same object
        if len(parts) == 4:
            out[parts[3]] = (parts[2], int(parts[1], 16), obj)
        elif len(parts) == 3:
            out[parts[2]] = (parts[1], 0, obj)
    return out


def nm_union(objs: list[pathlib.Path]):
    out = {}
    for o in objs:
        tag = o.name.split(".")[0]
        for l in run([tool("nm"), "--print-size", "--defined-only", str(o)]).splitlines():
            parts = l.split()
            if len(parts) == 4:
                out[parts[3]] = (parts[2], int(parts[1], 16), tag)
            elif len(parts) == 3:
                out[parts[2]] = (parts[1], 0, tag)
    return out


def sections(elf: pathlib.Path) -> dict[str, int]:
    out = {}
    for l in run([tool("size"), "-A", str(elf)]).splitlines():
        parts = l.split()
        if len(parts) >= 2 and parts[0].startswith("."):
            try:
                out[parts[0]] = int(parts[1])
            except ValueError:
                pass
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--before-nm", required=True)
    ap.add_argument("--before-graph", required=True)
    ap.add_argument("--after-objs", required=True)
    ap.add_argument("--before-elf")
    ap.add_argument("--after-elf")
    ap.add_argument("--moved", default="", help="functions moved out of src.ino (base names)")
    ap.add_argument("--published", default="", help="variables moved out and/or published (base names)")
    ap.add_argument("--resigned", default="", help="functions whose signature deliberately changed (base names)")
    a = ap.parse_args()

    moved = set(filter(None, a.moved.split(",")))
    published = set(filter(None, a.published.split(",")))
    resigned = set(filter(None, a.resigned.split(",")))

    before = load_nm_tagged(pathlib.Path(a.before_nm))
    objs = [pathlib.Path(p) for p in a.after_objs.split(",")]
    after = nm_union(objs)
    names = demangle(sorted(set(before) | set(after)))

    def dm(s: str) -> str:
        return names.get(s, s)

    def norm(s: str) -> str:
        """Demangled name, collapsed to the base name for re-signed functions."""
        d = dm(s)
        b = base(d)
        return b if b in resigned else d

    ok = True

    # ---- 1. symbol set -------------------------------------------------------
    bkeys = {dm(s): s for s in before}
    akeys = {dm(s): s for s in after}
    removed = sorted(k for k in bkeys if k not in akeys)
    added = sorted(k for k in akeys if k not in bkeys)
    resign_pairs = [(r, ad) for r in removed for ad in added
                    if base(r) == base(ad) and before[bkeys[r]][0] in "Tt" and after[akeys[ad]][0] in "Tt"]
    paired = {r for r, _ in resign_pairs} | {ad for _, ad in resign_pairs}
    removed = [k for k in removed if k not in paired]
    added = [k for k in added if k not in paired]
    linkage, relocated = [], []
    for k in bkeys.keys() & akeys.keys():
        bs, as_ = bkeys[k], akeys[k]
        if before[bs][0].upper() == after[as_][0].upper() and before[bs][0] != after[as_][0]:
            linkage.append((k, before[bs][0], after[as_][0]))
        if before[bs][2] != after[as_][2]:
            relocated.append((k, before[bs][2], after[as_][2]))

    print(f"1. symbols: {len(before)} before, {len(after)} after (union of {len(objs)} objects)")
    print(f"   relocated: {len(relocated)}")
    for k, fr, to in sorted(relocated, key=lambda t: (t[2], t[0])):
        print(f"      {fr} -> {to:<10} {k}")
    print(f"   linkage changes: {len(linkage)}")
    for k, fr, to in sorted(linkage):
        flag = "" if base(k) in published else "   <-- NOT declared with --published"
        print(f"      {k:<30} {fr} -> {to}{flag}")
        ok = ok and base(k) in published
    print(f"   re-signed: {len(resign_pairs)}")
    for r, ad in resign_pairs:
        flag = "" if base(r) in resigned else "   <-- NOT declared with --resigned"
        print(f"      {r} -> {ad}{flag}")
        ok = ok and base(r) in resigned
    print(f"   removed: {removed or 'none'}")
    for k in removed:
        print(f"      UNEXPECTED removal: {k}")
        ok = False
    print(f"   added:   {added or 'none'}")
    for k in added:
        if k.startswith("_GLOBAL__sub_I"):
            print(f"      tolerated: {k} (static initialiser of the new TU)")
        else:
            print(f"      UNEXPECTED addition: {k}")
            ok = False

    # ---- 2. function sizes, attributed --------------------------------------
    bgraph = json.loads(pathlib.Path(a.before_graph).read_text(encoding="utf-8"))
    spec = importlib.util.spec_from_file_location("rg", pathlib.Path(__file__).with_name("reference-graph.py"))
    rg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rg)
    agraph = {}
    for o in objs:
        agraph.update(rg.Analysis(o).graph)

    def refset(g):
        return ({norm(c) for c in g["calls"]},
                {norm(v) for v in g["vars_ext"] + g["vars_static"]})

    deltas = [(s, before[s], after[s]) for s in set(before) & set(after) if before[s][1] != after[s][1]]
    print(f"\n2. size deltas: {len(deltas)} symbols, net {sum(y[1] - x[1] for _, x, y in deltas):+d} bytes")
    attributed = collections.Counter()
    for s, x, y in sorted(deltas, key=lambda t: (t[2][1] - t[1][1], dm(t[0]))):
        k = dm(s)
        d = y[1] - x[1]
        if base(k) in moved or y[2] != "src":
            attributed["moved function itself"] += 1
            print(f"   {d:+5d}  {k[:46]:<46} moved function; relaxation in the new TU")
            continue
        bg, ag = bgraph.get(s), agraph.get(s)
        if bg is None or ag is None:
            attributed["library instantiation"] += 1
            print(f"   {d:+5d}  {k[:46]:<46} not an authored function; layout")
            continue
        bc, bv = refset(bg)
        ac, av = refset(ag)
        lost_calls, new_calls = bc - ac, ac - bc
        lost_vars, new_vars = bv - av, av - bv
        expl_calls = {c for c in lost_calls if base(c) in moved}
        expl_vars = {v for v in lost_vars if base(v) in published}
        calls_resigned = {c for c in bc | ac if c in resigned}
        residue_calls = (lost_calls - expl_calls) | new_calls
        residue_vars = (lost_vars - expl_vars) | new_vars
        if residue_calls or (residue_vars and not calls_resigned):
            print(f"   {d:+5d}  {k[:46]:<46} UNEXPLAINED: {sorted(residue_calls | residue_vars)}")
            ok = False
            continue
        if residue_vars:
            attributed["caller of a re-signed function"] += 1
            why = f"calls re-signed {sorted(calls_resigned)}; argument now {sorted(residue_vars)}"
        elif expl_calls:
            attributed["calls a moved function"] += 1
            why = f"calls moved: {sorted(expl_calls)[:4]}"
        elif expl_vars:
            attributed["touches published state"] += 1
            why = f"touches published: {sorted(expl_vars)[:4]}"
        else:
            attributed["layout only"] += 1
            why = "identical references; layout/relaxation"
        print(f"   {d:+5d}  {k[:46]:<46} {why}")
    print("   attribution:", dict(attributed))

    # ---- 3. output sections -------------------------------------------------
    if a.before_elf and a.after_elf:
        sb, sa = sections(pathlib.Path(a.before_elf)), sections(pathlib.Path(a.after_elf))
        print("\n3. linked output sections (non-debug) that changed:")
        for k in sorted(set(sb) | set(sa)):
            if k.startswith(".debug") or k.startswith(".xt."):
                continue
            d = sa.get(k, 0) - sb.get(k, 0)
            if d:
                print(f"   {k:<22} {sb.get(k, 0):>9} -> {sa.get(k, 0):>9} ({d:+d})")

    print("\nRESULT:", "PASS - move is behaviour-neutral by symbol set and reference sets" if ok
          else "FAIL - see UNEXPECTED / UNEXPLAINED above")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
