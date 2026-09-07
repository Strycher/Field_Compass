#!/usr/bin/env python3
"""Move functions, variables, declarations and macros out of src/src.ino into a
new translation unit (#262, #263, E4).

The per-extraction harness the E4 plan called for. Hand-editing thousands of
lines of moves across four bands is exactly the error-prone path the plan was
written to avoid, so the move is a deterministic tool whose every run is
checkpointed and whose output IS the pull request diff. Nothing here is
invisible: the manifest it prints goes into the commit message, and --dry-run
prints it without writing.

What it does, precisely:

  * Functions (--functions): found by their signature line at column 0, taken
    through the matching closing brace plus the contiguous `//` comment lines
    directly above (section banners excluded). Static functions stay
    file-private in the .cpp; every other function gets a prototype in the
    header. If src.ino already carried a standalone prototype, THAT text is
    used -- it is the declaration callers compile against and it carries the
    default arguments (`sdOpenSafe(..., bool silent = false)`) that a
    regenerated prototype would drop.
  * Variables (--move-vars stay private; --publish-vars become the unit's
    interface): single-line definitions, moved to the .cpp before the
    functions. Published ones lose `static` and get `extern <decl>;` in the
    header, initialiser stripped.
  * Declarations (--move-decls): `enum`/`struct` blocks, single- or
    multi-line, into the header ahead of the externs and prototypes.
  * Macros (--move-defines): single `#define` lines into the header. A macro
    that is defined conditionally (an #if/#else pair) cannot be moved by name;
    move its whole block with --header-lines A-B, a verbatim range appended
    to the header after the prototypes.
  * src.ino loses everything moved plus any now-redundant standalone
    prototypes, and gains `#include "<unit>.h"` after the last top-of-file
    #include.

Every location is resolved before anything is mutated; any ambiguity (0 or >1
matches, overlapping ranges) refuses the whole run. Headers go in src/, not
include/: arduino-cli does not read platformio.ini's `-I include`.

Verification is NOT this tool's job. After every extraction: build under both
toolchains, then compare `nm --print-size --defined-only` over the UNION of
src/*.o before and after -- the symbol set must be identical apart from
deliberate linkage changes on published variables, and every size delta must
be attributed by comparing each changed function's reference set.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

SRC = pathlib.Path("src/src.ino")
BANNER_RE = re.compile(r"^// =+ .+ =+$")


def read_lines(p: pathlib.Path) -> list[str]:
    return p.read_text(encoding="utf-8", newline="").splitlines(keepends=True)


def eol(lines: list[str]) -> str:
    for l in lines:
        if l.endswith("\r\n"):
            return "\r\n"
        if l.endswith("\n"):
            return "\n"
    return "\n"


def one(hits: list[int], what: str) -> int:
    if len(hits) != 1:
        sys.exit(f"refusing: {what} has {len(hits)} matches at lines {[h + 1 for h in hits]} (need exactly 1)")
    return hits[0]


def find_definition(lines: list[str], name: str) -> int:
    pat = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*&:<>,]*\s\**" + re.escape(name)
                     + r"\s*\([^;{]*\)\s*(?:\{\s*)?$")
    hits = [i for i, l in enumerate(lines) if pat.match(l.rstrip("\r\n"))]
    if not hits:
        pat_open = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*&:<>,]*\s\**" + re.escape(name) + r"\s*\([^;{)]*$")
        hits = [i for i, l in enumerate(lines) if pat_open.match(l.rstrip("\r\n"))]
    return one(hits, f"function {name}")


def signature_end(lines: list[str], start: int) -> int:
    for i in range(start, min(start + 8, len(lines))):
        if "{" in lines[i]:
            return i
    sys.exit(f"refusing: no opening brace within 8 lines of line {start + 1}")


def block_end(lines: list[str], start: int) -> int:
    """index of the line holding the closing brace that balances the first `{`
    at or after `start`."""
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
    return re.sub(r"\s+", " ", sig) + ";"


def is_static(lines: list[str], start: int) -> bool:
    return lines[start].lstrip().startswith("static ")


# Type may be several words (`unsigned long`, `const char*`); each word may end
# in `*`. The name follows, optionally an array suffix, then `=` or `;`.
VAR_RE_TMPL = r"^\s*(?:static\s+)?(?:volatile\s+)?(?:const\s+)?(?:[A-Za-z_][\w:<>]*\*?\s+)+\**{name}\s*(?:\[[^\]]*\])*\s*(?:=|;)"


def find_var(lines: list[str], name: str) -> int:
    pat = re.compile(VAR_RE_TMPL.format(name=re.escape(name)))
    return one([i for i, l in enumerate(lines)
                if pat.match(l) and not l.lstrip().startswith("extern ")], f"variable {name}")


def extern_decl(line: str) -> str:
    """`static volatile uint16_t x = 0;  // c` -> `extern volatile uint16_t x;`"""
    body = line.rstrip("\r\n")
    body = re.sub(r"//.*$", "", body).strip()
    body = re.sub(r"^static\s+", "", body)
    body = body.split("=", 1)[0].rstrip().rstrip(";").rstrip()
    return "extern " + body + ";"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--unit", required=True)
    ap.add_argument("--functions", default="")
    ap.add_argument("--move-vars", default="", help="variables moved into the .cpp, kept private")
    ap.add_argument("--publish-vars", default="", help="variables moved into the .cpp and declared extern in the header")
    ap.add_argument("--move-defines", default="")
    ap.add_argument("--move-decls", default="", help="enum/struct names to move into the header")
    ap.add_argument("--header-lines", default="", help="A-B verbatim line range moved to the header (1-based, inclusive)")
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

    split = lambda s: [n for n in s.split(",") if n]
    names, mvars, pvars = split(a.functions), split(a.move_vars), split(a.publish_vars)
    defines, decls = split(a.move_defines), split(a.move_decls)

    # ---- locate everything first; only then mutate --------------------------
    moves = []   # (comment_start, def_start, brace, end, name, static)
    for n in names:
        d = find_definition(lines, n)
        b = signature_end(lines, d)
        e = block_end(lines, b)
        moves.append((comment_start(lines, d), d, b, e, n, is_static(lines, d)))
    moves.sort()
    for x, y in zip(moves, moves[1:]):
        if y[0] <= x[3]:
            sys.exit(f"refusing: {x[4]} and {y[4]} overlap")

    var_lines = {n: find_var(lines, n) for n in mvars + pvars}
    # a table initialiser (`static const TZPreset tzPresets[] = {` ... `};`) spans
    # lines: carry the whole block, not just its first line (band 3a)
    var_ends = {n: (block_end(lines, i) if "{" in lines[i] and "}" not in lines[i] else i)
                for n, i in var_lines.items()}
    def_lines = {n: one([i for i, l in enumerate(lines)
                        if re.match(r"^\s*#define\s+" + re.escape(n) + r"\b", l)], f"#define {n}")
                 for n in defines}
    decl_ranges = {}
    for n in decls:
        s = one([i for i, l in enumerate(lines)
                 if re.match(r"^\s*(?:typedef\s+)?(?:enum|struct)\s+" + re.escape(n) + r"\b", l)], f"declaration {n}")
        e = block_end(lines, s) if "{" in lines[s] else s
        while ";" not in lines[e]:
            e += 1
        decl_ranges[n] = (s, e)
    hdr_range = None
    if a.header_lines:
        lo, hi = (int(x) for x in a.header_lines.split("-"))
        hdr_range = (lo - 1, hi - 1)

    # existing standalone prototypes of moved functions: reuse text, drop line
    protos = {}   # line index -> (name, text)
    for c, d, b, e, n, st in moves:
        if st:
            continue
        pat = re.compile(r"^\s*(?:static\s+)?[A-Za-z_][\w\s\*&:<>,]*\s\**" + re.escape(n) + r"\s*\([^;{]*\)\s*;")
        for i, l in enumerate(lines):
            if i != d and pat.match(l):
                protos[i] = (n, re.sub(r"\s*//.*$", "", l.rstrip("\r\n")).strip())

    # forward `extern` declarations of published variables are redundant once
    # the header declares them (band 3a: the timeout tables had six)
    fwd = {}   # line index -> text
    for n in pvars:
        fpat = re.compile(r"^\s*extern\s+[^;=]*\b" + re.escape(n) + r"\s*(?:\[[^\]]*\])*\s*;")
        for i, l in enumerate(lines):
            if fpat.match(l):
                fwd[i] = l.strip()

    # ---- build outputs ------------------------------------------------------
    def inc_line(inc: str) -> str:
        """A bare name becomes "name" if it is one of our files in src/, else <name>."""
        if inc[0] in "<\"":
            return f"#include {inc}" + nl
        return (f'#include "{inc}"' if (SRC.parent / inc).exists() else f"#include <{inc}>") + nl

    h = ["#pragma once" + nl, f"// {unit}.h -- extracted from src.ino by scripts/extract_unit.py (E4)." + nl]
    for inc in split(a.header_includes):
        h.append(inc_line(inc))
    if defines:
        h.append(nl)
        h.extend(lines[def_lines[n]] for n in defines)
    if decls:
        for n in decls:
            s, e = decl_ranges[n]
            h.append(nl)
            h.extend(lines[s:e + 1])
    if hdr_range:
        # verbatim block goes BEFORE the externs and prototypes: it is types and
        # macros, and an extern of a struct type needs the struct first (band 3a)
        h.append(nl)
        h.extend(lines[hdr_range[0]:hdr_range[1] + 1])
    if pvars:
        h.append(nl)
        h.extend(extern_decl(lines[var_lines[n]]) + nl for n in pvars)
    if not a.header_only and moves:
        h.append(nl)
        existing = {n: t for _, (n, t) in protos.items()}
        for c, d, b, e, n, st in moves:
            if not st:
                h.append(existing.get(n, prototype(lines, d, b)) + nl)

    cpp = []
    if not a.header_only:
        cpp.append(f"// {unit}.cpp -- extracted from src.ino by scripts/extract_unit.py (E4)." + nl)
        cpp.append(f'#include "{unit}.h"' + nl)
        for inc in split(a.cpp_includes):
            cpp.append(inc_line(inc))
        if mvars or pvars:
            cpp.append(nl)
            for n in sorted(mvars + pvars, key=lambda x: var_lines[x]):
                l = lines[var_lines[n]]
                cpp.append(re.sub(r"^(\s*)static\s+", r"\1", l) if n in pvars else l)
                cpp.extend(lines[var_lines[n] + 1:var_ends[n] + 1])
        for c, d, b, e, n, st in moves:
            cpp.append(nl)
            cpp.extend(lines[c:e + 1])

    # ---- manifest -------------------------------------------------------------
    print(f"unit {unit}: {'header only' if a.header_only else f'{len(moves)} functions'}")
    for c, d, b, e, n, st in moves:
        print(f"  move  {n:<28} lines {c + 1}-{e + 1}  ({'static, .cpp only' if st else 'prototype in header'})")
    for n in mvars:
        print(f"  var   {n:<28} line {var_lines[n] + 1} -> .cpp (private)")
    for n in pvars:
        print(f"  var   {n:<28} line {var_lines[n] + 1} -> .cpp, published: {extern_decl(lines[var_lines[n]])}")
    for n in defines:
        print(f"  define {n:<27} line {def_lines[n] + 1} -> header")
    for n in decls:
        s, e = decl_ranges[n]
        print(f"  decl  {n:<28} lines {s + 1}-{e + 1} -> header")
    if hdr_range:
        print(f"  lines {hdr_range[0] + 1}-{hdr_range[1] + 1} -> header verbatim")
    for i in sorted(protos):
        print(f"  reuse and drop prototype of {protos[i][0]} at line {i + 1}: {protos[i][1]}")
    for i in sorted(fwd):
        print(f"  drop forward extern at line {i + 1}: {fwd[i]}")
    inc_idx = max(i for i, l in enumerate(lines[:200]) if l.startswith("#include"))
    print(f"  insert #include \"{unit}.h\" after line {inc_idx + 1}")
    if a.dry_run:
        print("  dry run: nothing written")
        return 0

    # ---- mutate src.ino -----------------------------------------------------
    remove = set()
    for c, d, b, e, n, st in moves:
        remove.update(range(c, e + 1))
        if e + 1 < len(lines) and lines[e + 1].strip() == "":
            remove.add(e + 1)
    for n, i in var_lines.items():
        remove.update(range(i, var_ends[n] + 1))
    remove.update(def_lines.values())
    for s, e in decl_ranges.values():
        remove.update(range(s, e + 1))
    if hdr_range:
        remove.update(range(hdr_range[0], hdr_range[1] + 1))
    remove.update(protos.keys())
    remove.update(fwd.keys())

    # a `// ...` line that introduced a removed declaration is orphaned when the
    # next kept line is blank (or the file ends): `// User settings (#70)` over
    # six variables that all left. Sweep it, and any comment lines stacked
    # above it. Scoped to removal sites; free-standing comments are untouched.
    def is_comment(i: int) -> bool:
        t = lines[i].rstrip("\r\n")
        return t.lstrip().startswith("//") and not BANNER_RE.match(t)

    for i in sorted(remove):
        if i == 0 or (i - 1) in remove or not is_comment(i - 1):
            continue
        j = i
        while j < len(lines) and j in remove:
            j += 1
        if j >= len(lines) or lines[j].strip() == "":
            k = i - 1
            while k >= 0 and k not in remove and is_comment(k):
                remove.add(k)
                k -= 1
    out = [l for i, l in enumerate(lines) if i not in remove]
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
