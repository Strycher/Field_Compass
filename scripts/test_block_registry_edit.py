#!/usr/bin/env python3
r"""Tests for .claude/hooks/block-registry-edit.py (#198).

The hook makes `pio-flash bootstrap` the only sanctioned writer of the device
registry, now that the registry lives outside git and therefore outside the
reach of block-primary-clone-edit.

Two things these tests exist to hold, learned the hard way on #179/#195:

1. A guard that blocks legitimate work gets switched off. So the ALLOWED list
   is as load-bearing as the BLOCKED one - reads, inspection, and the wrapper's
   own invocation must all pass untouched.
2. Test the FALSE POSITIVE classes explicitly, not just the happy path. #195
   shipped a hole precisely because its tests covered quoted arguments but
   never markdown code spans.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

HOOK = Path(__file__).resolve().parent.parent / ".claude" / "hooks" / "block-registry-edit.py"

REGISTRY = r"C:\Dev\.field_compass\hardware-devices.yaml"
HISTORY = r"C:\Dev\.field_compass\flash-history.jsonl"


def run_hook(payload: dict, env_extra: "dict | None" = None) -> bool:
    """Feed a PreToolUse event to the hook. True if it blocked (non-zero exit)."""
    env = dict(os.environ)
    env.pop("FC_REGISTRY_EDIT_OK", None)
    if env_extra:
        env.update(env_extra)
    r = subprocess.run(
        [sys.executable, str(HOOK)],
        input=json.dumps(payload),
        capture_output=True,
        text=True,
        timeout=30,
        env=env,
    )
    return r.returncode != 0


def edit(path: str, tool: str = "Edit") -> dict:
    return {"tool_name": tool, "tool_input": {"file_path": path}}


def sh(cmd: str, tool: str = "Bash") -> dict:
    return {"tool_name": tool, "tool_input": {"command": cmd}}


# ---------------------------------------------------------------------------
# Must be refused
# ---------------------------------------------------------------------------
BLOCKED = [
    pytest.param(edit(REGISTRY), id="edit-registry"),
    pytest.param(edit(REGISTRY, "Write"), id="write-registry"),
    pytest.param(edit(REGISTRY, "MultiEdit"), id="multiedit-registry"),
    pytest.param(edit(HISTORY), id="edit-flash-history"),
    # Forward slashes and a lowercase drive letter must not evade the check.
    pytest.param(edit("c:/dev/.field_compass/hardware-devices.yaml"), id="edit-slash-form"),
    # Anything under the state dir, including the backups the hook advertises.
    pytest.param(
        edit(r"C:\Dev\.field_compass\registry-backups\hardware-devices-20260905-120000.yaml"),
        id="edit-a-backup",
    ),
    # A STRAY in-repo copy is still refused. If one has reappeared somewhere it
    # should not exist, hand-editing it is not the fix - it IS the #198 bug.
    pytest.param(edit(r"C:\Dev\Field_Compass\hardware-devices.yaml"), id="edit-stray-repo-copy"),
    # Shell paths - backstop over block-bash-file-mutation.
    pytest.param(sh("sed -i 's/COM26/COM31/' " + REGISTRY), id="sh-sed-i"),
    pytest.param(sh("echo broken > " + REGISTRY), id="sh-redirect-onto"),
    pytest.param(sh("cp /tmp/mine.yaml " + REGISTRY), id="sh-cp-onto"),
    pytest.param(sh("rm " + HISTORY), id="sh-rm-history"),
    pytest.param(
        sh('Set-Content -Path "' + REGISTRY + '" -Value ""', "PowerShell"),
        id="ps-set-content",
    ),
]


@pytest.mark.parametrize("payload", BLOCKED)
def test_direct_registry_mutation_is_refused(payload):
    assert run_hook(payload), "should have been blocked: %r" % (payload,)


# ---------------------------------------------------------------------------
# Must pass - a guard that obstructs normal work is a guard that gets disabled
# ---------------------------------------------------------------------------
ALLOWED = [
    # Reads. Inspecting the registry is normal and frequent.
    pytest.param(sh("cat " + REGISTRY), id="sh-cat"),
    pytest.param(sh("grep -n usb_serial " + REGISTRY), id="sh-grep"),
    # DIRECTIONAL REDIRECT: a read whose OUTPUT goes elsewhere. A coarse ">"
    # check would wrongly fire on this.
    pytest.param(sh("cat " + REGISTRY + " > /tmp/inspect.yaml"), id="sh-redirect-away"),
    # The sanctioned writer must not be blocked by the hook guarding what it writes.
    pytest.param(
        sh("python scripts/pio-flash.py bootstrap field_compass --port COM26"),
        id="wrapper-bootstrap",
    ),
    pytest.param(sh("python scripts/pio-flash.py list"), id="wrapper-list"),
    # pio-flash.py is deliberately NOT guarded here: block-primary-clone-edit
    # and block-bash-file-mutation already cover it, and an edit inside a
    # worktree IS the approved pathway (branch -> PR -> human merge).
    pytest.param(
        edit(r"C:\Dev\.worktrees\Field_Compass-198-registry\scripts\pio-flash.py"),
        id="edit-pio-flash-in-worktree",
    ),
    # Unrelated files.
    pytest.param(
        edit(r"C:\Dev\Field_Compass\Field_Compass\Field_Compass.ino"), id="edit-firmware"
    ),
    pytest.param(sh("git status"), id="sh-git"),
    # Prose naming the file is not a write. Documentation about the registry is
    # exactly the text most likely to name it - the #195 lesson.
    pytest.param(
        sh('gh issue comment 198 --body "hardware-devices.yaml now lives outside git"'),
        id="prose-mentioning-registry",
    ),
    # Tools this hook does not police at all.
    pytest.param({"tool_name": "Read", "tool_input": {"file_path": REGISTRY}}, id="read-tool"),
    # ---------------------------------------------------------------------
    # #208 regressions. Each of these was refused, and none writes anything
    # guarded. The defect was scope: a redirect matched the whole remainder of
    # the line, and mutation verbs were scanned across the entire command, so
    # a later mention of the registry contaminated an unrelated statement.
    # ---------------------------------------------------------------------
    # NOT a regression: verified to pass on the pre-fix hook too. #208's
    # written reproduction does not actually reproduce in this form, because
    # `echo` is not a mutation verb and /dev/null is not a guarded target.
    # Kept as coverage, and recorded here so the issue's repro is not taken
    # at face value by the next reader.
    pytest.param(
        sh('echo hi > /dev/null; echo "see ' + REGISTRY + ' for details"'),
        id="208-devnull-then-prose-passed-before-too",
    ),
    # The exact shape that blocked the #245 migration backup: a checkpoint call
    # redirecting to /dev/null, then a script mentioning the state dir.
    pytest.param(
        sh(
            "python .claude/hooks/checkpoint_write.py --task X --action Y >/dev/null 2>&1\n"
            'python -c "print(1)"  # writes C:/Dev/.field_compass/migration-backup/x.json'
        ),
        id="208-checkpoint-then-statedir-mention",
    ),
    # A mutation verb in one statement, a registry mention in another.
    pytest.param(
        sh("cp a.txt b.txt && grep usb_serial " + REGISTRY),
        id="208-cp-then-unrelated-read",
    ),
    # Reading the registry and piping onward is still a read.
    pytest.param(
        sh("cat " + REGISTRY + " | tee /tmp/copy.yaml"),
        id="208-read-piped-to-tee-elsewhere",
    ),
]


@pytest.mark.parametrize("payload", ALLOWED)
def test_reads_and_sanctioned_paths_pass(payload):
    assert not run_hook(payload), "should NOT have been blocked: %r" % (payload,)


# ---------------------------------------------------------------------------
# Escape hatch and failure mode
# ---------------------------------------------------------------------------
def test_human_override_allows_the_write():
    assert not run_hook(edit(REGISTRY), {"FC_REGISTRY_EDIT_OK": "1"})


def test_override_must_be_exactly_one():
    """A truthy-looking value is not the override. Only "1" opens the gate."""
    assert run_hook(edit(REGISTRY), {"FC_REGISTRY_EDIT_OK": "true"})


def test_unparsable_event_fails_closed():
    r = subprocess.run(
        [sys.executable, str(HOOK)], input="{not json",
        capture_output=True, text=True, timeout=30,
    )
    assert r.returncode == 2
