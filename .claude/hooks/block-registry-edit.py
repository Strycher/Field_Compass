#!/usr/bin/env python3
r"""block-registry-edit.py - PreToolUse hook for Claude Code.

Makes `pio-flash bootstrap` the ONLY sanctioned writer of the Field Compass
device registry.

Why a dedicated hook (#198)
---------------------------
#198 moved the registry out of the repo to C:\Dev\.field_compass so that
branching, worktrees, rebases and merges cannot reach it. That fixed the
copy-drift problem and simultaneously opened a smaller one: the existing
guards are repo-shaped.

  block-primary-clone-edit.py  classifies the target's git working tree and
                               PASSES anything that is not in a repo. The
                               registry is now, by design, not in a repo.
  block-bash-file-mutation.py  keys on the governed dev roots, which do cover
                               C:\Dev\.field_compass - so the SHELL path is
                               already guarded. Edit/Write/MultiEdit are not.

So the tool path is unguarded, and the shell path is guarded by something that
covers it incidentally rather than deliberately. This hook closes the first,
backstops the second, and states the intent in one place.

What is protected, and why refusal is the right level
-----------------------------------------------------
The owner's rule for this guard family (#198, 2026-09-05) is: REFUSE on
identity, WARN on purpose, never add approval ceremony.

  "The guard prevents flashing against the wrong identity. It doesn't prevent
   a change in role or a change in human timing."

A hand edit of the registry is squarely an IDENTITY concern - it is the exact
act of making a device entry disagree with the hardware it names, which is the
one failure the whole wrapper exists to prevent. So this refuses.

It does NOT refuse anything about purpose: which firmware a board runs, what
role it plays, or how long since it was last flashed are the human's call and
are untouched here.

Reads always pass. Inspecting the registry is normal and frequent.

pio-flash.py itself
-------------------
Deliberately NOT re-guarded here. It is tracked in scripts/, so it is already
covered end to end:
    Edit/Write in the primary clone -> block-primary-clone-edit.py refuses
    shell mutation anywhere          -> block-bash-file-mutation.py refuses
    edit inside a worktree           -> allowed, and that IS the approved
                                        pathway: branch -> PR -> human merge
Duplicating that here would add a second thing to keep in sync for no coverage.

Escape hatch
------------
FC_REGISTRY_EDIT_OK=1 - HUMAN ONLY, logged to stderr. For repairing a corrupt
registry by hand, which bootstrap cannot do. An agent must not set this itself.
Mirrors DW_ALLOW_BASH_EDIT in block-bash-file-mutation.py.

Tracks: Strycher/Field_Compass#198

Exit codes
----------
0  pass through
2  block AND surface stderr to the model (Claude Code semantics)
"""
from __future__ import annotations

import json
import os
import re
import sys

# Guarded by BASENAME, not by full path. A stray copy is exactly the failure
# #198 exists to prevent, so a hand edit of "some other" hardware-devices.yaml
# is still refused - if one has appeared somewhere it should not exist, editing
# it is not the fix.
GUARDED_BASENAMES = {
    "hardware-devices.yaml",
    "flash-history.jsonl",
}

# Anything under the state dir, including registry-backups/ and flash-backups/.
STATE_DIR = os.environ.get("FIELD_COMPASS_STATE_DIR", r"C:\Dev\.field_compass")

EDIT_TOOLS = {"Edit", "Write", "MultiEdit", "NotebookEdit"}
SHELL_TOOLS = {"Bash", "PowerShell"}

BOOTSTRAP_HINT = (
    "Registrations go through the wrapper, which captures the DeviceID and\n"
    "usb serial from the live board instead of trusting typed-in values:\n"
    "  python scripts/pio-flash.py bootstrap <name> --port <COMx>\n"
    "Inspect without writing:\n"
    "  python scripts/pio-flash.py list"
)


def _norm(p: str) -> str:
    return p.strip().strip('"').strip("'").replace("\\", "/").lower()


def _is_guarded(path: str) -> bool:
    if not path:
        return False
    n = _norm(path)
    if n.rsplit("/", 1)[-1] in GUARDED_BASENAMES:
        return True
    return n.startswith(_norm(STATE_DIR).rstrip("/") + "/")


# Shell mutation verbs. This layer is a BACKSTOP only - block-bash-file-mutation
# already parses shell properly. Here we only need "this command plausibly
# writes a guarded file", so a keyword scan beside a guarded name is enough, and
# a second tokenizer to keep in sync is not worth the maintenance.
_SHELL_MUTATION = re.compile(
    r"(?:\bsed\b[^|;&]*?\s-i|\bperl\b[^|;&]*?\s-[a-z]*i|\btee\b|\bcp\b|\bmv\b|"
    r"\brm\b|\bdd\b|\btruncate\b|\bSet-Content\b|\bAdd-Content\b|\bOut-File\b|"
    r"\bRemove-Item\b|\bCopy-Item\b|\bMove-Item\b)",
    re.IGNORECASE,
)


def _names_guarded(text: str) -> bool:
    low = text.replace("\\", "/").lower()
    return any(b in low for b in GUARDED_BASENAMES) or _norm(STATE_DIR) in low


def _shell_touches_guarded(cmd: str) -> bool:
    if not _names_guarded(cmd):
        return False

    # Redirection is directional, and treating it coarsely costs a false
    # positive that matters: `cat <registry> > /tmp/x` is a READ. Only a
    # guarded name on the RIGHT of a redirect operator is a write to it.
    for tail in re.split(r">>?", cmd)[1:]:
        if _names_guarded(tail.split("|")[0].split(";")[0]):
            return True

    return bool(_SHELL_MUTATION.search(cmd))


def _refuse(what: str, detail: str) -> int:
    print(
        f"BLOCK: direct mutation of the Field Compass device registry - {what}\n"
        f"\n"
        f"{detail}\n"
        f"\n"
        f"The registry records which physical board each name refers to. Editing\n"
        f"it by hand is how a name comes to point at the wrong hardware, which is\n"
        f"the single failure the flash wrapper exists to prevent. It is also no\n"
        f"longer in git (#198), so a bad hand edit has no `git checkout --` undo.\n"
        f"\n"
        f"{BOOTSTRAP_HINT}\n"
        f"\n"
        f"Human override (NOT for agents): set FC_REGISTRY_EDIT_OK=1. Use it only\n"
        f"to repair a corrupt file, which bootstrap cannot do. Prior versions are\n"
        f"in {STATE_DIR}\\registry-backups\\.",
        file=sys.stderr,
    )
    return 2


def main() -> int:
    raw = sys.stdin.read()
    try:
        event = json.loads(raw)
    except json.JSONDecodeError as e:
        # Fail closed, consistent with the other hooks in this directory.
        print(
            f"BLOCK: block-registry-edit could not parse PreToolUse JSON: {e}\n"
            f"Raw input (first 200 chars): {raw[:200]!r}",
            file=sys.stderr,
        )
        return 2

    tool = event.get("tool_name", "")
    tin = event.get("tool_input") or {}

    if os.environ.get("FC_REGISTRY_EDIT_OK") == "1":
        print(
            "block-registry-edit: FC_REGISTRY_EDIT_OK=1 - registry write allowed.\n"
            "This is a human-only override. If an agent set it, that is a defect.",
            file=sys.stderr,
        )
        return 0

    if tool in EDIT_TOOLS:
        target = tin.get("file_path") or tin.get("notebook_path") or ""
        if _is_guarded(target):
            return _refuse(f"{tool} on {target}", "Refusing a direct editor write.")
        return 0

    if tool in SHELL_TOOLS:
        cmd = tin.get("command", "")
        if cmd and _shell_touches_guarded(cmd):
            return _refuse(
                "shell write",
                "Refusing a shell command that writes a guarded path.\n"
                "Original command was:\n  " + cmd,
            )
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
