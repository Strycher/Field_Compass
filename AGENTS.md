# Agent Instructions

**Read [`CLAUDE.md`](./CLAUDE.md) first — it is the source of truth for this project.**

This file exists for agent tooling that looks for `AGENTS.md` by convention. It
intentionally contains no rules of its own; everything is defined in the
DifferentWire standards that `CLAUDE.md` points to.

## Governance

| Question | Where it is answered |
|---|---|
| How do I behave (tollgates, debugging, prohibitions) | `C:\Dev\DifferentWire\standards\SAFELANE.md` |
| Universal rules (board, hierarchy, Agent Mail, commits) | `C:\Dev\DifferentWire\standards\CLAUDE-BASE.md` |
| Field Compass specifics (hardware, build, branch strategy) | [`CLAUDE.md`](./CLAUDE.md) |
| Hardware pinout and BOM | [`docs/HARDWARE.md`](./docs/HARDWARE.md) |

## Issue tracking

Task tracking is **Citadel**, via the `dw` CLI. The GitHub Project board is the
source of truth; Citadel is the coordination layer.

```bash
dw --project Field_Compass ready              # find unblocked work
dw --project Field_Compass claim <task-id>    # claim exactly one task
dw --project Field_Compass close <task-id> --reason "verified on device"
```

No work without a GitHub issue **and** a claimed Citadel task — the pre-commit
hook enforces this.

> **Historical note:** this project previously used `bd` (Beads). Beads was
> retired in favour of Citadel and `.beads/` was removed in #144. `bd` commands
> no longer apply; ignore any that survive in old issues or commit messages.

## Session completion

Field Compass is firmware. A change is not done when it compiles — it is done
when a human has flashed it and verified the behaviour on the device. Do not
self-certify hardware behaviour, and do not merge your own PR: see
`CLAUDE.md` § *Branch Strategy* and SAFELANE § 3 *Tollgating*.
