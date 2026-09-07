#!/usr/bin/env python3
"""Reference graph of src.ino, from relocations (#215, Q3/Q4/Q5).

symbol-inventory.py says WHAT the god object defines. This says WHO USES WHAT:
for every function, which of the file's own variables it touches and which of
the file's own functions it calls. That is the information extraction order
depends on, and it cannot be read off the source reliably -- the same regex
method that undercounted variables by 2.7x would be guessing here too.

Method. The build uses -ffunction-sections and -fdata-sections, so each function
owns `.text.<fn>` and `.literal.<fn>` sections (ISRs own `.iram1.<n>`), and each
variable owns a `.bss.<var>` / `.data.<var>` / `.rodata.<var>` section.
`objdump -r` lists, per section, every symbol it references. Attribute the
relocations of a function's sections to that function, keep only references to
symbols this object itself defines, and the intra-file graph falls out.

Two filters keep the numbers honest:

  * AUTHORED functions only, for every function-centric table. The object also
    carries library inlines emitted into this TU (an LVGL setter, a WebServer
    template instantiation), the static initialiser, compiler clones ($isra$),
    and lambda bodies. None of those is a function anyone will extract. Debug
    info says where each symbol was defined; anything not from src.ino, or
    matching a clone pattern, is excluded from function counts and lists.
    Variable fan-in keeps every referrer, because a reference is a reference.

  * DETERMINISTIC cut set. The greedy removal picks the highest-fan-in variable
    each step; many variables tie (fan-in 11-15), and the first version broke
    ties by dict order, giving 35 on one run and 37 on another. Ties now break
    by name so the number is reproducible rather than merely plausible.

Usage:
    pio run -e feather_s3
    python scripts/reference-graph.py             # summary
    python scripts/reference-graph.py --appendix  # markdown tables for the doc
    python scripts/reference-graph.py --json      # full adjacency
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
SRC = pathlib.Path("src/src.ino")
TOOLS = pathlib.Path.home() / ".platformio/packages/toolchain-xtensa-esp32s3/bin"
CUT_TARGET = 40   # stop owning variables once the largest cluster is below this

SECTION_RE = re.compile(r"^RELOCATION RECORDS FOR \[(.+?)\]:")
DATA_SEC_RE = re.compile(r"^\.(?:bss|data|rodata|sbss|sdata)\.(.+)$")
FN_SEC_RE = re.compile(r"^\.(?:text|literal)\.(.+)$")
IRAM_SEC_RE = re.compile(r"^\.iram1\.\d+(?:\.literal)?$")
ARTIFACT_RE = re.compile(r"^_GLOBAL__sub_I|\$isra\$|\$part\$|\$constprop\$")


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


def definitions(obj: pathlib.Path) -> dict[str, tuple[str, int]]:
    """symbol -> (source file, line) from debug info, for T/t symbols."""
    out = {}
    for line in run([tool("nm"), "-l", "--defined-only", str(obj)]).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] in ("T", "t") and ":" in parts[-1]:
            path, _, ln = parts[-1].rpartition(":")
            try:
                out[parts[2]] = (path, int(ln))
            except ValueError:
                pass
    return out


def authored(funcs: set[str], defs: dict, names: dict[str, str]) -> set[str]:
    keep = set()
    for f in funcs:
        if ARTIFACT_RE.search(f):
            continue
        dm = names.get(f, f)
        if "{lambda" in dm or "operator()" in dm:
            continue
        if f not in defs or not defs[f][0].replace("\\", "/").endswith("src.ino"):
            continue
        keep.add(f)
    return keep


def iram_owners(obj: pathlib.Path) -> dict[str, str]:
    """section name -> function symbol, for .iram1.* sections only."""
    owners = {}
    for line in run([tool("objdump"), "-t", str(obj)]).splitlines():
        parts = line.split()
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
        target = re.sub(r"[+-]0x[0-9a-fA-F]+$", "", parts[2])
        # objdump names a relocation against a LOCAL symbol by its SECTION, not
        # the symbol: a call to static fcToggleClickCb appears as
        # `.text._ZL15fcToggleClickCbP11_lv_event_t`, and its address in a
        # literal pool as `.literal._ZL...`. The first version of this script
        # mapped only data sections back to names, so every call to a static
        # function was silently dropped -- twelve "leaves" in the band-1
        # mapping showed zero callers, including LVGL callbacks that are
        # plainly registered. Map function sections the same way.
        dm = DATA_SEC_RE.match(target)
        fm = FN_SEC_RE.match(target)
        if dm:
            refs[current].add(dm.group(1))
        elif fm:
            refs[current].add(fm.group(1))
        elif IRAM_SEC_RE.match(target):
            owner = iram.get(target.removesuffix(".literal"))
            if owner:
                refs[current].add(owner)
        else:
            refs[current].add(target)
    return refs


class Analysis:
    def __init__(self, obj: pathlib.Path):
        self.funcs, self.var_ext, self.var_static = defined_symbols(obj)
        self.defs = definitions(obj)
        self.names = demangle(sorted(self.funcs | self.var_ext | self.var_static))
        self.authored = authored(self.funcs, self.defs, self.names)
        raw = references(obj)
        self.graph = {
            f: {
                "vars_ext": sorted(t for t in raw.get(f, ()) if t in self.var_ext),
                "vars_static": sorted(t for t in raw.get(f, ()) if t in self.var_static),
                "calls": sorted(t for t in raw.get(f, ()) if t in self.funcs and t != f),
            }
            for f in self.funcs
        }
        self.fanin = collections.Counter(v for g in self.graph.values() for v in g["vars_ext"])
        self.call_in = collections.Counter(c for g in self.graph.values() for c in g["calls"])

    def d(self, s: str) -> str:
        return self.names.get(s, s)

    # -- cut set ------------------------------------------------------------
    def clusters(self, excluded: set[str]) -> list[int]:
        parent: dict[str, str] = {}

        def find(x):
            parent.setdefault(x, x)
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        for fn, g in self.graph.items():
            for v in g["vars_ext"]:
                if v not in excluded:
                    parent[find("f:" + fn)] = find("v:" + v)
        sizes = collections.Counter(find(k) for k in parent if k.startswith("f:"))
        return sorted(sizes.values(), reverse=True) or [0]

    def cut_set(self) -> list[tuple[str, int]]:
        """[(variable, largest cluster after removing it), ...] in removal order."""
        removed: list[str] = []
        trace: list[tuple[str, int]] = []
        while self.clusters(set(removed))[0] >= CUT_TARGET and len(removed) < len(self.fanin):
            pick = sorted((v for v in self.fanin if v not in removed),
                          key=lambda v: (-self.fanin[v], self.d(v)))[0]
            removed.append(pick)
            trace.append((pick, self.clusters(set(removed))[0]))
        return trace

    # -- Q5 -------------------------------------------------------------------
    def prototype_reliant(self) -> tuple[set[str], list[str]]:
        line = {f: ln for f, (_, ln) in self.defs.items()}
        early = {c for f, g in self.graph.items() for c in g["calls"]
                 if f in line and c in line and line[f] < line[c] and c in self.authored}
        text = SRC.read_text(encoding="utf-8", errors="replace") if SRC.exists() else ""

        def has_forward_decl(sym: str) -> bool:
            name = self.d(sym).split("(")[0].split("::")[-1].strip()
            pat = r"^[^\S\n]*[\w:\*&<> ,]+\b" + re.escape(name) + r"\s*\([^;{]*\)\s*;"
            return re.search(pat, text, re.M) is not None

        return early, sorted(self.d(s) for s in early if not has_forward_decl(s))

    def leaves(self) -> list[str]:
        return sorted(self.d(f) for f in self.authored
                      if not self.graph[f]["vars_ext"] and not self.graph[f]["calls"])


def summary(a: Analysis) -> None:
    d = a.d
    n_auth = len(a.authored)
    print(f"functions: {len(a.funcs)} T/t symbols, {n_auth} authored in src.ino "
          f"({len(a.funcs) - n_auth} library inlines / initialiser / clones / lambdas excluded)")
    print(f"  authored static: {sum(1 for f in a.authored if f.startswith('_ZL'))}")

    print(f"\nexternal-linkage variables by fan-in (functions referencing them)")
    print(f"  {len(a.var_ext)} variables, "
          f"{sum(1 for v in a.var_ext if a.fanin[v] == 0)} referenced by no function here\n")
    for v, n in a.fanin.most_common(20):
        print(f"  {n:>3}  {d(v)}")

    print(f"\nauthored functions by fan-in (callers within src.ino)")
    for f, n in sorted(a.call_in.items(), key=lambda kv: (-kv[1], d(kv[0]))):
        if f in a.authored and n >= 5:
            print(f"  {n:>3}  {d(f)}")

    leaves = a.leaves()
    no_ext = [f for f in a.authored if not a.graph[f]["vars_ext"]]
    print(f"\ncoupling profile (authored functions)")
    print(f"  touching NO external-linkage variable : {len(no_ext)} of {n_auth}")
    print(f"  ...and calling no other src.ino function : {len(leaves)}")

    heavy = sorted((f for f in a.authored), key=lambda f: -len(a.graph[f]["vars_ext"]))[:12]
    print(f"\nheaviest consumers of shared state (external-linkage vars touched)")
    for f in heavy:
        g = a.graph[f]
        print(f"  {len(g['vars_ext']):>3} vars  {len(g['calls']):>3} calls  {d(f)}")

    trace = a.cut_set()
    print(f"\ncut set: {len(trace)} of {len(a.var_ext)} external-linkage variables must be"
          f" owned before the largest cluster is under {CUT_TARGET} functions")
    print(f"  resulting clusters: {a.clusters({v for v, _ in trace})[:8]}")
    print(f"  in removal order: {', '.join(d(v) for v, _ in trace)}")

    early, implicit = a.prototype_reliant()
    print(f"\ngenerated-prototype reliance (Q5)")
    print(f"  authored callees invoked before their definition : {len(early)} of {n_auth}")
    print(f"  of which have NO forward declaration             : {len(implicit)}  <-- break on leaving the .ino")
    for name in implicit:
        print(f"    {name}")


def appendix(a: Analysis) -> None:
    d = a.d
    rows = sorted(a.var_ext, key=lambda v: (-a.fanin[v], d(v)))
    print(f"### A. All {len(a.var_ext)} external-linkage variables by fan-in\n")
    print("| fan-in | variable | fan-in | variable | fan-in | variable |")
    print("|---:|---|---:|---|---:|---|")
    cells = [f"{a.fanin[v]} | `{d(v)}`" for v in rows]
    while len(cells) % 3:
        cells.append(" | ")
    for i in range(0, len(cells), 3):
        print("| " + " | ".join(cells[i:i + 3]) + " |")

    trace = a.cut_set()
    print(f"\n### B. The cut set — {len(trace)} variables, in removal order\n")
    print("| # | variable | fan-in | largest cluster after removal |")
    print("|---:|---|---:|---:|")
    for i, (v, big) in enumerate(trace, 1):
        print(f"| {i} | `{d(v)}` | {a.fanin[v]} | {big} |")
    print(f"\nClusters remaining afterwards: {a.clusters({v for v, _ in trace})[:10]}")

    leaves = a.leaves()
    print(f"\n### C. The {len(leaves)} leaf functions — no shared state, no intra-file calls\n")
    print("| | | |\n|---|---|---|")
    for i in range(0, len(leaves), 3):
        row = [f"`{x}`" for x in leaves[i:i + 3]] + [""] * (3 - len(leaves[i:i + 3]))
        print("| " + " | ".join(row) + " |")

    multi = [(f, n) for f, n in a.call_in.items() if n >= 2 and f in a.authored]
    print(f"\n### D. Authored functions with two or more callers inside src.ino ({len(multi)})\n")
    print("| callers | function |\n|---:|---|")
    for f, n in sorted(multi, key=lambda kv: (-kv[1], d(kv[0]))):
        print(f"| {n} | `{d(f)}` |")

    heavy = sorted(a.authored, key=lambda f: (-len(a.graph[f]["vars_ext"]), d(f)))
    heavy = [f for f in heavy if len(a.graph[f]["vars_ext"]) >= 8]
    print(f"\n### E. Authored functions touching eight or more external-linkage variables ({len(heavy)})\n")
    print("| vars | calls | function |\n|---:|---:|---|")
    for f in heavy:
        g = a.graph[f]
        print(f"| {len(g['vars_ext'])} | {len(g['calls'])} | `{d(f)}` |")

    _, implicit = a.prototype_reliant()
    print(f"\n### F. The {len(implicit)} functions that rely on Arduino-generated prototypes\n")
    for name in implicit:
        print(f"- `{name}`")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--raw", action="store_true",
                    help="with --json: keep mangled names (the form verify_extraction.py consumes)")
    ap.add_argument("--appendix", action="store_true")
    ap.add_argument("--obj", default=str(OBJ))
    args = ap.parse_args()
    obj = pathlib.Path(args.obj)
    if not obj.exists():
        sys.exit(f"{obj} not found -- run `pio run -e feather_s3` first")

    a = Analysis(obj)
    if args.json and args.raw:
        print(json.dumps(a.graph, indent=2))
    elif args.json:
        print(json.dumps({a.d(f): {k: [a.d(x) for x in v] for k, v in g.items()}
                          for f, g in a.graph.items()}, indent=2))
    elif args.appendix:
        appendix(a)
    else:
        summary(a)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
