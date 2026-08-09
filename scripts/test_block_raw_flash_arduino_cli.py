#!/usr/bin/env python3
"""Tests for the arduino-cli Tier A patterns in block-raw-flash.py (#179).

Field Compass flashes with arduino-cli, not pio. Upstream's hook covers esptool
and `pio run -t upload` only, so without these patterns the hook would block
BOOTLOADER ENTRY (esptool) while leaving the command that actually writes
firmware unguarded.

The false-positive cases are not hypothetical. The session that wrote this hook
repeatedly put the literal text "arduino-cli upload" inside `gh issue comment
--body "..."` arguments. A pattern that fires on those makes the hook unusable
and invites someone to disable it — which is worse than the gap it closes.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

HOOK = Path(__file__).resolve().parent.parent / ".claude" / "hooks" / "block-raw-flash.py"


def run_hook(command: str) -> bool:
    """Feed a Bash tool call to the hook. True if it blocked (non-zero exit)."""
    payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": command}})
    r = subprocess.run([sys.executable, str(HOOK)], input=payload,
                       capture_output=True, text=True, timeout=30)
    return r.returncode != 0


BLOCKED = [
    # The form actually used in this repo: quoted absolute path.
    ('"C:/Program Files/Arduino CLI/arduino-cli.exe" upload '
     '--fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM26 Field_Compass/'),
    "arduino-cli upload --port COM26 Field_Compass/",
    "arduino-cli burn-bootloader --fqbn esp32:esp32:adafruit_feather_esp32s3",
    "arduino-cli compile -u --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/",
    "arduino-cli compile --upload --fqbn esp32:esp32:adafruit_feather_esp32s3",
]

ALLOWED = [
    # Tier 0 — compile and read-only queries must keep working.
    '"C:/Program Files/Arduino CLI/arduino-cli.exe" compile '
    "--fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/",
    "arduino-cli board list",
    "arduino-cli lib list",
    "arduino-cli config get directories.user",
    # The wrapper is the sanctioned route and must pass through.
    "python scripts/pio-flash.py list",
    # FALSE POSITIVES: trigger words inside a quoted argument. The opening quote
    # is preceded by a space, which is not a CMD_BOUNDARY, so these must not fire.
    'gh issue comment 1 --body "run arduino-cli upload to flash it"',
    'gh pr create --body "then arduino-cli burn-bootloader on the device"',
    'git commit -m "docs: explain arduino-cli upload workflow"',
]


@pytest.mark.parametrize("cmd", BLOCKED)
def test_write_paths_are_blocked(cmd):
    assert run_hook(cmd), f"should have been blocked: {cmd}"


@pytest.mark.parametrize("cmd", ALLOWED)
def test_read_paths_and_quoted_text_pass(cmd):
    assert not run_hook(cmd), f"should NOT have been blocked: {cmd}"


def test_esptool_still_blocked():
    """Regression guard: the upstream patterns must survive our additions."""
    assert run_hook("esptool --chip esp32s3 chip-id")
