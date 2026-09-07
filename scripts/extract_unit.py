#!/usr/bin/env python3
"""Move functions out of src/src.ino into a new translation unit (#262, E4).

The per-extraction harness the E4 plan called for. Hand-editing two thousand
lines of moves across four bands is exactly the error-prone path the plan was
written to avoid, so the move is a deterministic tool whose every run is
checkpointed and whose output IS the pull request diff. Nothing here is
invisible: the manifest it prints goes into the commit message.

What it does, precisely:

  * Finds each named function's definition in src.ino by its signature line at
    column 0 (return type, name, parameters, opening brace). Exactly one match
    or it refuses.
  * Takes the definition through its matching closing brace, plus the
    contiguous `//` comment lines directly above it (section banners excluded).
  * Writes src/<unit>.cpp with those bodies in their original order, and
    src/<unit>.h with `#pragma once`, the requested includes, any #defines and
    enum/struct declarations moved along, and a prototype for every function
    that is not `static`. Static functions stay file-private in the .cpp.
  * Removes the moved ranges, the moved #defines, the moved declarations and
    any now-redundant standalone prototypes from src.ino, and inserts
    `#include "<unit>.h"` after the last top-of-file #include.

Headers go in src/, not include/: arduino-cli does not read platformio.ini's
`-I include`, and the tree must keep building under both toolchains while #161
is open.

Verification is NOT this tool's job. After every extraction: build under both
toolchains, then compare `nm --print-size --defined-only` over the UNION of
src/*.o before and after -- the symbol set must be identical and every size
delta must be attributed (cross-TU inlining is the expected cause).

Usage:
    python scripts/extract_unit.py --unit geo \
        --functions calcBearing,calcDistanceKm,sameLocation,getCardinal \
        --move-defines LOCATION_THRESHOLD --cpp-includes "<Arduino.h>,<math.h>" \
        [--move-decls SDIndicatorState] [--header-includes "<lvgl.h>"] \
        [--header-only] [--dry-run]
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

SRC = pathlib.Path("src/src.ino")
BANNER_RE = re.compile(r"^// =+ .+ =+$")


def read_lines(p: pathlib.Path) -> list[str]:
    # newline="" preserves whatever line endings the file has, exactly
    return p.read_text(encoding="utf-8", newline="").splitlines(keepends=True)


def eol(lines: list[str]) -> str:
    for l in lines:
        if l.endswith("\r\n"):
            return "\r\n"
        if l.endswith("\n"):
            return "\n"
    return "\n"


def find_definition(lines: list[str], name: str) -> int:
    """0-based index of the line where `name`'s definition starts, or exit."""
    pat = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*&:<>,]*\s\**" + re.escape(name)
                     + r"\s*\([^;{]*\)\s*(?:\{\s*)?$")
    hits = [i for i, l in enumerate(lines) if pat.match(l.rstrip("\r\n"))]
    # a signature may wrap: `ret name(` on one line, params continue -- handle
    # by also matching an unterminated parameter list and scanning forward
    if not hits:
        pat_open = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*&:<>,]*\s\**" + re.escape(name) + r"\s*\([^;{)]*$")
        hits = [i for i, l in enumerate(lines) if pat_open.match(l.rstrip("\r\n"))]
    if len(hits) != 1:
        sys.exit(f"refusing: {name} has {len(hits)} definition matches at lines "
                 f"{[h + 1 for h in hits]} (need exactly 1)")
    return hits[0]


def signature_end(lines: list[str], start: int) -> int:
    """index of the line containing the opening brace of the body."""
    for i in range(start, min(start + 8, len(lines))):
        if "{" in lines[i]:
            return i
    sys.exit(f"refusing: no opening brace within 8 lines of line {start + 1}")


def body_end(lines: list[str], start: int) -> int:
    depth = 0
    seen = False
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if "{" in lines[i]:
            seen = True
        if seen and depth == 0:
            return i
    sys.exit(f"refusing: unbalanced braces from line {start + 1}")


def comment_start(lines: list[str], start: int) -> int:
    i = start
    while i > 0:
        prev = lines[i - 1].rstrip("\r\n")
        if prev.startswith("//") and not BANNER_RE.match(prev):
            i -= 1
        else:
            break
    return i


def prototype(lines: list[str], start: int, brace: int) -> str:
    sig = " ".join(l.strip() for l in lines[start:brace + 1])
    sig = sig.split("{", 1)[0].strip()
    sig = re.sub(r"\s+", " ", sig)
    return sig + ";"


def is_static(lines: list[str], start: int) -> bool:
    return lines[start].lstrip().startswith("static ")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--unit", required=True)
    ap.add_argument("--functions", default="")
    ap.add_argument("--move-defines", default="")
    ap.add_argument("--move-decls", default="", help="enum/struct names to move into the header")
    ap.add_argument("--cpp-includes", default="")
    ap.add_argument("--header-includes", default="")
    ap.add_argument("--header-only", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    lines = read_lines(SRC)
    nl = eol(lines)
    unit = a.unit
    cpp_path = pathlib.Path(f"src/{unit}.cpp")
    h_path = pathlib.Path(f"src/{unit}.h")
    for p in ((h_path,) if a.header_only else (cpp_path, h_path)):
        if p.exists():
            sys.exit(f"refusing: {p} already exists")

    names = [n for n in a.functions.split(",") if n]
    defines = [n for n in a.move_defines.split(",") if n]
    decls = [n for n in a.move_decls.split(",") if n]

    # ---- locate everything first; only then mutate --------------------------
    moves = []   # (comment_start, def_start, brace, end, name, static)
    for n in names:
        d = find_definition(lines, n)
        b = signature_end(lines, d)
        e = body_end(lines, b)
        c = comment_start(lines, d)
        moves.append((c, d, b, e, n, is_static(lines, d)))
    moves.sort()
    for x, y in zip(moves, moves[1:]):
        if y[0] <= x[3]:
            sys.exit(f"refusing: {x[4]} and {y[4]} overlap")

    def_lines = {}
    for dn in defines:
        hits = [i for i, l in enumerate(lines) if re.match(r"^\s*#define\s+" + re.escape(dn) + r"\b", l)]
        if len(hits) != 1:
            sys.exit(f"refusing: #define {dn} has {len(hits)} matches")
        def_lines[dn] = hits[0]

    decl_ranges = {}
    for dn in decls:
        hits = [i for i, l in enumerate(lines) if re.match(r"^\s*(?:typedef\s+)?(?:enum|struct)\s+" + re.escape(dn) + r"\b", l)]
        if len(hits) != 1:
            sys.exit(f"refusing: declaration {dn} has {len(hits)} matches")
        s = hits[0]
        e = s
        while ";" not in lines[e]:
            e += 1
        decl_ranges[dn] = (s, e)

    # standalone prototypes of moved functions (now redundant)
    protos = {}
    for c, d, b, e, n, st in moves:
        if st:
            continue
        sig_name = re.escape(n)
        for i, l in enumerate(lines):
            if i == d:
                continue
            if re.match(r"^\s*(?:static\s+)?[A-Za-z_][\w\s\*&:<>,]*\s\**" + sig_name + r"\s*\([^;{]*\)\s*;", l):
                protos[i] = n

    inc_idx = max(i for i, l in enumerate(lines[:200]) if l.startswith("#include"))

    # ---- build outputs ------------------------------------------------------
    h = ["#pragma once" + nl]
    h.append(f"// {unit}.h -- extracted from src.ino by scripts/extract_unit.py (#262, E4)." + nl)
    for inc in [x for x in a.header_includes.split(",") if x]:
        h.append(f"#include {inc}" + nl)
    if def_lines:
        h.append(nl)
        for dn in defines:
            h.append(lines[def_lines[dn]])
    if decl_ranges:
        h.append(nl)
        for dn in decls:
            s, e = decl_ranges[dn]
            h.extend(lines[s:e + 1])
    if not a.header_only:
        h.append(nl)
        for c, d, b, e, n, st in moves:
            if not st:
                h.append(prototype(lines, d, b) + nl)

    cpp = []
    if not a.header_only:
        cpp.append(f"// {unit}.cpp -- extracted from src.ino by scripts/extract_unit.py (#262, E4)." + nl)
        cpp.append(f'#include "{unit}.h"' + nl)
        for inc in [x for x in a.cpp_includes.split(",") if x]:
            cpp.append(f"#include {inc}" + nl)
        for c, d, b, e, n, st in moves:
            cpp.append(nl)
            cpp.extend(lines[c:e + 1])

    # ---- manifest -------------------------------------------------------------
    print(f"unit {unit}: {'header only' if a.header_only else f'{len(moves)} functions'}")
    for c, d, b, e, n, st in moves:
        print(f"  move  {n:<28} lines {c + 1}-{e + 1}  ({'static, .cpp only' if st else 'prototype in header'})")
    for dn in defines:
        print(f"  define {dn:<27} line {def_lines[dn] + 1} -> header")
    for dn in decls:
        s, e = decl_ranges[dn]
        print(f"  decl  {dn:<28} lines {s + 1}-{e + 1} -> header")
    for i in sorted(protos):
        print(f"  drop redundant prototype of {protos[i]} at line {i + 1}")
    print(f"  insert #include \"{unit}.h\" after line {inc_idx + 1}")
    if a.dry_run:
        print("  dry run: nothing written")
        return 0

    # ---- mutate src.ino bottom-up so indices stay valid ---------------------
    remove = set()
    for c, d, b, e, n, st in moves:
        remove.update(range(c, e + 1))
        # also swallow ONE trailing blank line so we do not leave double blanks
        if e + 1 < len(lines) and lines[e + 1].strip() == "":
            remove.add(e + 1)
    remove.update(def_lines.values())
    for s, e in decl_ranges.values():
        remove.update(range(s, e + 1))
    remove.update(protos.keys())
    out = [l for i, l in enumerate(lines) if i not in remove]
    # recompute include insertion point on the filtered list
    inc_idx2 = max(i for i, l in enumerate(out[:200]) if l.startswith("#include"))
    out.insert(inc_idx2 + 1, f'#include "{unit}.h"' + nl)

    SRC.write_text("".join(out), encoding="utf-8", newline="")
    h_path.write_text("".join(h), encoding="utf-8", newline="")
    if not a.header_only:
        cpp_path.write_text("".join(cpp), encoding="utf-8", newline="")
    print(f"  wrote {h_path}" + ("" if a.header_only else f", {cpp_path}") + f"; src.ino {len(lines)} -> {len(out)} lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
