#!/usr/bin/env python3
"""Reference graph of src.ino, from relocations (#215, Q3/Q4).

symbol-inventory.py says WHAT the god object defines. This says WHO USES WHAT:
for every function, which of the file's own variables it touches and which of
the file's own functions it calls. That is the information extraction order
depends on, and it cannot be read off the source reliably -- the same regex
method that undercounted variables by 2.7x would be guessing here too.

Method. The build uses -ffunction-sections and -fdata-sections, so each function
owns `.text.<fn>` and `.literal.<fn>` sections and each variable owns a
`.bss.<var>` / `.data.<var>` / `.rodata.<var>` section. `objdump -r` lists,
per section, every symbol it references. Attribute the relocations of a
function's sections to that function, keep only references to symbols this
object itself defines, and the intra-file graph falls out.

Usage:
    pio run -e feather_s3
    python scripts/reference-graph.py            # summary tables
    python scripts/reference-graph.py --json     # full adjacency
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys

OBJ = pathlib.Path(".pio/build/feather_s3/src/src.ino.cpp.o")
TOOLS = pathlib.Path.home() / ".platformio/packages/toolchain-xtensa-esp32s3/bin"


def tool(name: str) -> str:
    hits = sorted(TOOLS.glob(f"xtensa-esp32s3-elf-{name}*"))
    if not hits:
        sys.exit(f"missing toolchain binary {name} under {TOOLS}")
    return str(hits[0])


def run(cmd: list[str]) -> str:
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        sys.exit(f"{cmd[0]} failed: {r.stderr[:300]}")
    return r.stdout


def demangle(names: list[str]) -> dict[str, str]:
    out = run([tool("c++filt")] + names) if names else ""
    return dict(zip(names, (l.strip() for l in out.splitlines())))


def defined_symbols(obj: pathlib.Path):
    funcs, var_ext, var_static = set(), set(), set()
    for line in run([tool("nm"), "--defined-only", str(obj)]).splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        cls, name = parts[-2], parts[-1]
        if cls in ("T", "t"):
            funcs.add(name)
        elif cls in ("B", "b", "D", "d"):
            if name.startswith("_ZZ"):
                continue                      # function-local static, not file scope
            (var_static if name.startswith("_ZL") else var_ext).add(name)
    return funcs, var_ext, var_static


SECTION_RE = re.compile(r"^RELOCATION RECORDS FOR \[(.+?)\]:")
# ".bss.gpsData" / ".data._ZL3foo" / ".rodata.bar" -> owning symbol
DATA_SEC_RE = re.compile(r"^\.(?:bss|data|rodata|sbss|sdata)\.(.+)$")
# .iram1.N holds ISRs (IRAM_ATTR). The first version of this script matched
# only .text/.literal and so reported the touch ISR's flag as unreferenced --
# it IS referenced, from a section this regex did not cover. Xtensa puts the
# ISR body in .iram1.<n> and its literals in .iram1.<n>.literal; objdump names
# the owning function only in the symbol table, so ISR sections are attributed
# by looking up which function symbol lives in that section.
FN_SEC_RE = re.compile(r"^\.(?:text|literal)\.(.+)$")
IRAM_SEC_RE = re.compile(r"^\.iram1\.\d+(?:\.literal)?$")


def iram_owners(obj: pathlib.Path) -> dict[str, str]:
    """section name -> function symbol, for .iram1.* sections only.

    `objdump -t` rows look like:  00000000 g  F .iram1.1  0000003c _Z8touchISRv
    """
    owners = {}
    for line in run([tool("objdump"), "-t", str(obj)]).splitlines():
        parts = line.split()
        # Column count varies with the flag field ("g F" vs "l d"), so find the
        # section by content rather than by position. The first version used a
        # fixed index, landed on the flag column, and matched nothing -- which
        # made the touch ISR's flag look unreferenced.
        sec = next((p for p in parts if p.startswith(".iram1.")), None)
        if sec and "F" in parts[1:4]:
            owners[sec] = parts[-1]
    return owners


def references(obj: pathlib.Path):
    """function -> set of raw symbol/section names it references."""
    refs: dict[str, set[str]] = collections.defaultdict(set)
    iram = iram_owners(obj)
    current = None
    for line in run([tool("objdump"), "-r", str(obj)]).splitlines():
        m = SECTION_RE.match(line)
        if m:
            sec = m.group(1)
            fm = FN_SEC_RE.match(sec)
            if fm:
                current = fm.group(1)
            elif IRAM_SEC_RE.match(sec):
                current = iram.get(sec.removesuffix(".literal"))
            else:
                current = None
            continue
        if current is None or not line.strip() or line.startswith("OFFSET"):
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        target = parts[2]
        target = re.sub(r"[+-]0x[0-9a-fA-F]+$", "", target)   # strip addends
        dm = DATA_SEC_RE.match(target)
        refs[current].add(dm.group(1) if dm else target)
    return refs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--obj", default=str(OBJ))
    a = ap.parse_args()
    obj = pathlib.Path(a.obj)
    if not obj.exists():
        sys.exit(f"{obj} not found -- run `pio run -e feather_s3` first")

    funcs, var_ext, var_static = defined_symbols(obj)
    all_vars = var_ext | var_static
    raw = references(obj)

    # keep only edges to symbols THIS object defines; drop self-references
    graph = {}
    for fn in funcs:
        tgt = raw.get(fn, set())
        graph[fn] = {
            "vars_ext":    sorted(t for t in tgt if t in var_ext),
            "vars_static": sorted(t for t in tgt if t in var_static),
            "calls":       sorted(t for t in tgt if t in funcs and t != fn),
        }

    names = demangle(sorted(funcs | all_vars))
    d = lambda s: names.get(s, s)

    if a.json:
        print(json.dumps({d(f): {k: [d(x) for x in v] for k, v in g.items()}
                          for f, g in graph.items()}, indent=2))
        return 0

    # ---- fan-in of external-linkage variables: the real hot core ----------
    fanin = collections.Counter()
    users = collections.defaultdict(list)
    for fn, g in graph.items():
        for v in g["vars_ext"]:
            fanin[v] += 1
            users[v].append(fn)
    print(f"external-linkage variables by fan-in (functions referencing them)")
    print(f"  {len(var_ext)} variables, {sum(1 for v in var_ext if fanin[v]==0)} referenced by no function here\n")
    for v, n in fanin.most_common(20):
        print(f"  {n:>3}  {d(v)}")

    # ---- fan-in of functions: what must be extracted first ----------------
    call_in = collections.Counter()
    for fn, g in graph.items():
        for c in g["calls"]:
            call_in[c] += 1
    print(f"\nfunctions by fan-in (callers within src.ino)")
    for f, n in call_in.most_common(15):
        print(f"  {n:>3}  {d(f)}")

    # ---- coupling profile per function -------------------------------------
    leaf = [f for f, g in graph.items() if not g["vars_ext"] and not g["calls"]]
    pure_ext0 = [f for f, g in graph.items() if not g["vars_ext"]]
    print(f"\ncoupling profile")
    print(f"  functions touching NO external-linkage variable : {len(pure_ext0)} of {len(funcs)}")
    print(f"  ...and calling no other src.ino function        : {len(leaf)}")
    heavy = sorted(graph.items(), key=lambda kv: -len(kv[1]["vars_ext"]))[:12]
    print(f"\nheaviest consumers of shared state (external-linkage vars touched)")
    for f, g in heavy:
        print(f"  {len(g['vars_ext']):>3} vars  {len(g['calls']):>3} calls  {d(f)}")

    # ---- cut set: how much state must be OWNED before seams appear -------
    # Functions sharing an external-linkage variable are joined. Remove the
    # highest-fan-in variable, re-join, repeat, until the largest component
    # falls below `target` functions. The variables removed are the ones that
    # must become deliberate ownership interfaces before the rest of the file
    # separates into domains on its own. On first measurement this was 37 of
    # 97 -- the file is ONE connected component, and seams have to be created,
    # not discovered.
    def largest_component(excluded: set[str]) -> list[int]:
        parent: dict[str, str] = {}

        def find(x):
            parent.setdefault(x, x)
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        for fn, g in graph.items():
            for v in g["vars_ext"]:
                if v not in excluded:
                    parent[find("f:" + fn)] = find("v:" + v)
        sizes = collections.Counter(find(k) for k in parent if k.startswith("f:"))
        return sorted(sizes.values(), reverse=True)

    target = 40
    removed: list[str] = []
    while True:
        sizes = largest_component(set(removed))
        if not sizes or sizes[0] < target or len(removed) >= len(fanin):
            break
        removed.append(max((v for v in fanin if v not in removed), key=lambda v: fanin[v]))
    print(f"\ncut set: {len(removed)} of {len(var_ext)} external-linkage variables must be"
          f" owned before the largest cluster is under {target} functions")
    print(f"  resulting clusters: {sizes[:6]}")
    print(f"  in removal order: {', '.join(d(v) for v in removed)}")

    # ---- Q5: reliance on Arduino-generated prototypes ---------------------
    # A callee defined AFTER its caller needs a prototype. In a .ino the Arduino
    # preprocessor supplies one; in a .cpp nothing does. Count callees that are
    # called before definition AND have no explicit forward declaration in the
    # source -- those are what breaks the moment code leaves the .ino.
    defline: dict[str, int] = {}
    for line in run([tool("nm"), "-l", "--defined-only", str(obj)]).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] in ("T", "t") and ":" in parts[-1]:
            try:
                defline[parts[2]] = int(parts[-1].rsplit(":", 1)[1])
            except ValueError:
                pass
    early = {c for f, g in graph.items() for c in g["calls"]
             if f in defline and c in defline and defline[f] < defline[c]}
    src = pathlib.Path("src/src.ino")
    text = src.read_text(encoding="utf-8", errors="replace") if src.exists() else ""

    def has_forward_decl(sym: str) -> bool:
        name = d(sym).split("(")[0].split("::")[-1].strip()
        pat = r"^[^\S\n]*[\w:\*&<> ,]+\b" + re.escape(name) + r"\s*\([^;{]*\)\s*;"
        return re.search(pat, text, re.M) is not None

    implicit = sorted(d(s) for s in early if not has_forward_decl(s))
    print(f"\ngenerated-prototype reliance (Q5)")
    print(f"  callees invoked before their definition : {len(early)} of {len(funcs)}")
    print(f"  of which have NO forward declaration    : {len(implicit)}  <-- break on leaving the .ino")
    for name in implicit:
        print(f"    {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
