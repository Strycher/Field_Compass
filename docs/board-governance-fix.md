# MANDATORY — Board Governance Fix + 6 Orphaned Issues

You have a systemic governance failure. 6 closed issues are sitting in active board columns (Ready, Testing) instead of Done. This produces incorrect board data and inflates the visible workload.

**Root cause:** The closing workflow does not apply `board:done` label. Issues get closed but never moved to Done on the board.

## Part 1: Systemic Fix (BEFORE cleanup)

### A. Update CLAUDE.md closing workflow

Every issue closure MUST include the board label. Change the closing step from:

```
bd close <id> AND gh issue close <N> --comment "Completed."
```

To:

```
bd close <id> AND gh issue close <N> --comment "Completed." AND gh issue edit <N> --add-label "board:done"
```

No exceptions. Every agent session that closes an issue without this is producing bad board data.

### B. Update `.github/workflows/sync-labels-to-board.yml`

Add a safety net. Add `closed` to the issue event triggers and a job that auto-applies `board:done` when an issue is closed without it:

Change:

```yaml
on:
  issues:
    types: [labeled]
```

To:

```yaml
on:
  issues:
    types: [labeled, closed]
```

Add this new job BEFORE the existing `sync` job:

```yaml
  # Safety net: auto-apply board:done when issue is closed without it
  auto-done:
    runs-on: ubuntu-latest
    if: github.event.action == 'closed'
    steps:
      - name: Apply board:done to closed issue
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          labels=$(gh issue view ${{ github.event.issue.number }} --repo ${{ github.repository }} --json labels -q '.labels[].name')
          if ! echo "$labels" | grep -q "board:done"; then
            gh issue edit ${{ github.event.issue.number }} --repo ${{ github.repository }} --add-label "board:done"
          fi
```

Update the existing `sync` job's `if` condition to only fire on `labeled` events (not `closed`):

```yaml
  sync:
    runs-on: ubuntu-latest
    if: >-
      github.event.action == 'labeled' && (
      startsWith(github.event.label.name, 'board:') ||
      startsWith(github.event.label.name, 'priority:'))
```

Commit both CLAUDE.md and workflow changes before proceeding to cleanup.

## Part 2: Cleanup (AFTER systemic fix is committed)

Apply `board:done` to these 6 closed issues stuck in active columns:

```bash
# Was in 'Ready' (1 issue)
gh issue edit 31 --add-label "board:done" --repo Strycher/Field_Compass

# Was in 'Testing' (5 issues)
gh issue edit 123 --add-label "board:done" --repo Strycher/Field_Compass
gh issue edit 124 --add-label "board:done" --repo Strycher/Field_Compass
gh issue edit 125 --add-label "board:done" --repo Strycher/Field_Compass
gh issue edit 126 --add-label "board:done" --repo Strycher/Field_Compass
gh issue edit 127 --add-label "board:done" --repo Strycher/Field_Compass
```

## Verification

After cleanup, confirm the board matches reality:

```bash
# Count open issues in the repo
gh issue list --repo Strycher/Field_Compass --state open --json number -q 'length'

# Compare to non-Done columns on the board — they should match
```
