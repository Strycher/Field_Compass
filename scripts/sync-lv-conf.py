#!/usr/bin/env python3
"""Sync include/lv_conf.h into the Arduino libraries folder — or check for drift.

WHY THIS EXISTS (issue #158)
----------------------------
LVGL locates `lv_conf.h` by looking one directory above its own library folder,
i.e. `<sketchbook>/libraries/lv_conf.h`. Under arduino-cli there is no clean way
to serve that file from the repo: the alternatives need a machine-specific
absolute path baked into a build property.

So this is a BRIDGE, not the destination. The repo copy at `include/lv_conf.h`
is the source of truth; this script pushes it to the libraries folder. Once the
PlatformIO migration lands (epic #154), `build_flags = -I include
-D LV_CONF_INCLUDE_SIMPLE` makes the repo copy authoritative for real and this
script can be deleted.

Until then the two copies can silently diverge — which is exactly the class of
problem #158 set out to kill. `--check` exists so that divergence is detectable
rather than discovered by a mysterious runtime bug.

USAGE
-----
    python scripts/sync-lv-conf.py            # copy repo -> libraries
    python scripts/sync-lv-conf.py --check    # exit 1 if they differ (CI/preflight)
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
from pathlib import Path

REPO_COPY = Path(__file__).resolve().parent.parent / "include" / "lv_conf.h"

# arduino-cli's `directories.user`. Overridable for a non-default sketchbook.
DEFAULT_SKETCHBOOK = Path.home() / "OneDrive" / "Documents" / "Arduino"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="report drift, change nothing, exit 1 if they differ")
    ap.add_argument("--sketchbook", type=Path, default=DEFAULT_SKETCHBOOK,
                    help=f"Arduino sketchbook dir (default: {DEFAULT_SKETCHBOOK})")
    args = ap.parse_args()

    target = args.sketchbook / "libraries" / "lv_conf.h"

    if not REPO_COPY.is_file():
        print(f"FAIL: repo copy missing: {REPO_COPY}", file=sys.stderr)
        return 2

    if not target.parent.is_dir():
        print(f"FAIL: libraries dir not found: {target.parent}", file=sys.stderr)
        print("      Pass --sketchbook if arduino-cli's directories.user differs.", file=sys.stderr)
        return 2

    repo_sha = digest(REPO_COPY)

    if args.check:
        if not target.is_file():
            print(f"DRIFT: {target} does not exist. Run without --check to install it.", file=sys.stderr)
            return 1
        if digest(target) != repo_sha:
            print("DRIFT: lv_conf.h in the libraries folder differs from the repo copy.", file=sys.stderr)
            print(f"       repo:      {REPO_COPY}  ({repo_sha[:16]})", file=sys.stderr)
            print(f"       libraries: {target}  ({digest(target)[:16]})", file=sys.stderr)
            print("       The repo is the source of truth. Reconcile before building —", file=sys.stderr)
            print("       a stale libraries copy means you are not building what is committed.", file=sys.stderr)
            return 1
        print(f"OK: lv_conf.h in sync ({repo_sha[:16]})")
        return 0

    shutil.copy2(REPO_COPY, target)
    if digest(target) != repo_sha:
        print("FAIL: copy completed but hashes still differ.", file=sys.stderr)
        return 2
    print(f"Synced {REPO_COPY} -> {target}  ({repo_sha[:16]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
