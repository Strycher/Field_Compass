# Repository migration runbook

**Epic:** [#245](https://github.com/Strycher/Field_Compass/issues/245) · **This task:** [#246](https://github.com/Strycher/Field_Compass/issues/246)

Rebuild Field Compass in a clean repository that has never contained credentials, and archive the existing one privately with its record intact.

> **Nothing in section 4 runs until the owner approves this document.** Steps marked **⛔ IRREVERSIBLE** cannot be undone by any command in this runbook.

---

## 1. Why

`src/src.ino` has carried six plaintext WiFi credentials since 2026-02, in a repository that has been **public** since 2026-02-03. [#146](https://github.com/Strycher/Field_Compass/issues/146) was filed to remove them, moved the copies out of `docs/HARDWARE.md`, and closed — while the originals stayed in the source. Its own acceptance criterion was *"No SSID or password string in any tracked file"*, and `src/src.ino` is a tracked file.

The owner's decision, recorded 2026-08-08 and unchanged: **the credentials will not be rotated**, and the exposure in history is accepted. This migration does not revisit that.

### Why an in-place history rewrite does not work

`git filter-repo` plus a force push would leave the original commits reachable **by SHA**. Worse, this repository has **38 pull requests**, and GitHub pins every PR's head commit at `refs/pull/N/head` permanently. A PR cannot be deleted. So 38 refs would keep the credentials fetchable no matter what is done to `main`.

A new repository has no such refs. That is the whole argument.

### Why recreation, not GitHub's transfer

GitHub's issue transfer **moves** issues — it would empty the archive that is being kept as the record. It also carries each issue's **edit history**, which the REST API does not expose and therefore cannot be audited. A credential edited out of a body still lives in that body's history.

Recreation writes only content we have inspected. No scan has to be trusted.

---

## 2. Scope

**In:** a new `Strycher/Field_Compass` with redacted full history; 243 issue slots recreated with numbering preserved; labels, board, branch protection, secrets rebuilt; the local working directory repointed; the old repository renamed, made private, and archived.

**Out:** rotating credentials (declined, on the record). Runtime credential storage — that is [#99](https://github.com/Strycher/Field_Compass/issues/99), scheduled **after** this migration by owner ruling. Any firmware behaviour change.

### Accepted losses

| lost | why | mitigation |
|---|---|---|
| 38 PRs | cannot be recreated with review threads or diffs | readable in the private archive; a closed placeholder issue at each PR's number points there |
| comment authorship and timestamps | recreated comments are authored by the migrating account | each recreated body and comment is stamped with its original author and date |
| reactions, timeline cross-references | not reproducible via API | none; noted here so it is not discovered later |
| commit SHAs | redaction rewrites every commit after the first affected one | SHAs quoted in issue bodies go stale; see step 4.6 |

---

## 3. Preconditions

Every one of these must hold before step 4.1.

- [x] **No open PRs** — verified before 4.1 ran.
- [x] **`docs/ci-diagnosis.md` is on `main`** — [#244](https://github.com/Strycher/Field_Compass/pull/244), merged `e1890d0`.
- [x] **[#200](https://github.com/Strycher/Field_Compass/issues/200) settled** — merged `9de4a9f`; `scripts/` green at 54 tests.
- [x] **Three superseded branches deleted** — evidence in §3.1.
- [x] **All worktrees removed**, `main` clean.
- [x] **No Citadel task in progress** except the migration's own.
- [x] **A verified backup exists** — 4.1, reconciled against the live API.

Two things landed while preparing, neither required but both wanted before a one-way step: [#249](https://github.com/Strycher/Field_Compass/issues/249) put `scripts/` under CI so the new repository starts guarded, and [#208](https://github.com/Strycher/Field_Compass/issues/208) fixed the registry guard that 4.1 tripped over.

### 3.1 Branch dispositions, with evidence

| branch | issue | evidence | disposition |
|---|---|---|---|
| `chore/142-untrack-canonical` | [#142](https://github.com/Strycher/Field_Compass/issues/142) closed | its one commit landed on `main` as `daa72a4`, identical subject | delete |
| `feat/135-hook-sync` | [#135](https://github.com/Strycher/Field_Compass/issues/135) closed | landed as `de09d22`; remote branch already gone | delete |
| `archive/fc-131-stale-remote` | [#131](https://github.com/Strycher/Field_Compass/issues/131) closed | 1 of 5 commits landed as `bd9d163`. The other four touch **only** `.claude/commands/refresh-context.md`, `.claude/hooks/preflight.sh`, `pre-commit`, `pre-push` — all now gitignored by policy under #142 / standards#249. Merging would re-track canonical artifacts the project decided must not be tracked. | delete |
| `fc/218-ci-assess` | [#219](https://github.com/Strycher/Field_Compass/issues/219) closed | content being landed by [#244](https://github.com/Strycher/Field_Compass/pull/244) | delete after #244 merges |
| `fc/200-legacy-match` | [#200](https://github.com/Strycher/Field_Compass/issues/200) **open** | one commit, 2026-09-05, touches `scripts/pio-flash.py` + two test files; 8 commits behind `main`, no path overlap so a rebase should be clean | **owner decision** |

---

## 4. The runbook

### 4.1 Back up everything — do this first ✅ DONE 2026-09-07

A full mirror clone plus a JSON export of every issue, comment, label and milestone, at **`C:\Dev\.backups\Field_Compass-migration\`**.

**Not** under `C:\Dev\.field_compass\`, which an earlier draft of this document specified. That directory is device state, and `block-registry-edit.py` guards everything beneath it — correctly, since #198 exists to stop the registry being hand-edited. A migration backup does not belong inside it, and putting it there refused every export command. The guard was right; the placement was wrong.

The mirror must include **`refs/pull/*`**. Those refs are the only surviving copy of the pull requests, which cannot be recreated (§2). `git clone --mirror` from GitHub brings them.

Verify by reconciling against the **live API**, not against numbers written here — they move every time a PR is opened:

```
mirror         126 tags · 45 PR refs · refs/heads/main
issues.json    253 entries = 208 issues + 45 PRs, no gaps in 1..253
comments.json  252
open count     matches repos/<repo> .open_issues_count
```

**If a count does not reconcile, stop.**

### 4.2 Redact history ✅ DONE 2026-09-07

Over a **copy** of the mirror — never the working clone, never the backup:

```bash
git filter-repo --replace-text <map> --replace-message <map> --force
```

**Both flags are required, and `--replace-message` was missing from the first draft.** `--replace-text` rewrites **blob contents only**. On the first pass every blob came out clean while `REDACTED_SSID_3` survived in a *commit message* — *"Mobile hotspot (REDACTED_SSID_3) added as third network"*. Following this document as originally written would have pushed a credential-bearing commit message into the new public repository.

**Order the replacement map longest-literal-first.** `REDACTED_SSID_1` is a substring of `REDACTED_SSID_2`; with the short one first, SSID 2 rewrites to `REDACTED_SSID_1_5G`.

Placeholders are self-explaining and non-functional — `REDACTED_SSID_1`, `REDACTED_WIFI_PASSWORD`, `REDACTED_HOTSPOT_PASSWORD` — so a reader knows the value was scrubbed, and a device flashed from redacted history fails to join WiFi loudly rather than silently.

Verify on a **fresh clone of the result**, not on the rewritten repo: a rewritten repository retains unreachable objects, so a local grep can both miss real hits and invent phantom ones. Check patches *and* messages:

```
git log -p --all           | grep -c <needle>   ->  0
git log --all --format=%B  | grep -c <needle>   ->  0
```

Measured: 16 / 17 / 11 / 11 before, 0 / 0 / 0 / 0 after, with 328 commits, 126 tags and 45 PR refs preserved.

Then confirm the redacted tree still builds — `pio run -e feather_s3`, **from PowerShell**, since PlatformIO refuses to run under MSYS/Git Bash. Redaction edits a live source file, and a placeholder that broke a string literal would compile-fail. Measured: SUCCESS in 140.32 s.

### 4.3 ⛔ IRREVERSIBLE — rename the old repository ✅ DONE 2026-09-07

`Strycher/Field_Compass` → `Strycher/Field_Compass-archive`.

Renaming leaves a redirect; creating a new repo at the old name overrides it, which is the intent. Verify the archive responds at its new name before continuing.

### 4.4 Create the new repository and push ✅ DONE 2026-09-07

New public `Strycher/Field_Compass`. Push the redacted history — all branches, all tags.

Verify: clone it fresh to a scratch directory, run the four-needle grep over `--all`, and build. **A clean clone is the only trustworthy check**; a local repo can hide objects the remote does not have, and vice versa.

### 4.5 ⛔ IRREVERSIBLE — recreate every issue slot in order ✅ DONE 2026-09-07

Strict ascending order, 1 → N. Nothing else may create an issue **or a pull request** in the new repository during this run: PRs consume the same number space, and one stray creation shifts every subsequent number.

**The PR set is COMPUTED at run time, never written down.** An earlier draft listed 38 specific numbers. That list was stale within the hour — it was 44 by the time 4.1 ran and 45 by the time 4.2 finished, because every PR opened while preparing the migration consumed another slot. A hardcoded list guarantees numbering drift on the one step that cannot be repaired.

Same lesson as #221's env matrix: **discovered, not listed.** Read the split from `pulls.json` in the backup, taken at the moment the run starts:

```python
prs   = {p["number"] for p in json.load(open("pulls.json"))}
items = json.load(open("issues.json"))          # issues AND PRs, one entry each
N     = max(i["number"] for i in items)
```

Then for each `n` in `1..N`:

- **`n in prs`** → create a closed placeholder naming the PR and linking to it in the archive. PRs cannot be recreated (§2); the placeholder is the index entry that says where the real one lives.
- **otherwise** → create with sanitized body, original author and date stamped in, then labels, then comments, then close if it was closed.

Confirm before starting that `issues.json` has **no gaps** in `1..N`. It had none when 4.1 ran, and a gap would mean a deleted issue, which changes how slots must be padded.

**Seven issues carry credentials** — [#7](https://github.com/Strycher/Field_Compass/issues/7), [#9](https://github.com/Strycher/Field_Compass/issues/9), [#34](https://github.com/Strycher/Field_Compass/issues/34), [#35](https://github.com/Strycher/Field_Compass/issues/35), [#39](https://github.com/Strycher/Field_Compass/issues/39), [#99](https://github.com/Strycher/Field_Compass/issues/99), [#166](https://github.com/Strycher/Field_Compass/issues/166). Their bodies and comments are sanitized before creation. [#34](https://github.com/Strycher/Field_Compass/issues/34) and [#99](https://github.com/Strycher/Field_Compass/issues/99) are **open** and must stay usable, so they are recreated in full minus the literals — not reduced to pointers.

Verify after every 25: the highest issue number equals the count created. Drift means stop immediately; it cannot be corrected afterwards without deleting issues.

### 4.6 Re-link ✅ DONE 2026-09-07

Native sub-issue parent/child links, then `<!-- depends-on: #N -->` bodies, then the project board items with Status and Priority set on both the field and the label.

Citadel needs no change — `external_issue_number` stays valid because the numbers were preserved. Verify by spot-checking five tasks across the range.

Stale commit SHAs quoted in issue bodies are left as they are; a note in the Epic records that pre-migration SHAs refer to the archive.

### 4.7 Rebuild repository settings ✅ DONE 2026-09-07

Branch protection (`compile-gate` required, `strict: false`, `enforce_admins: true`, no force pushes, no deletions), the `PROJECT_PAT` secret, and all 31 labels.

Verify branch protection by **repeating the #223 break test**: push a deliberate syntax error, confirm `mergeStateStatus=BLOCKED`, close unmerged, delete the branch. A protection setting that has not been observed blocking is not known to block.

### 4.8 Repoint the local working directory ✅ DONE 2026-09-07

`C:\Dev\Field_Compass` stays where it is:

```bash
git remote set-url origin https://github.com/Strycher/Field_Compass.git
git fetch origin
git reset --hard origin/main
```

Untracked and ignored files survive this — local settings, un-committed hooks, scratch. **Claude Code sessions and memory are keyed to the local directory path**, `~/.claude/projects/C--Dev-Field-Compass/`, not to the git remote, so both are unaffected.

Verify: `git log` shows redacted history, `pio run -e feather_s3` succeeds, `python scripts/pio-flash.py list` still resolves the device registry at `C:\Dev\.field_compass\`.

### 4.9 ⛔ IRREVERSIBLE — private, then archive ✅ DONE 2026-09-07

Make `Field_Compass-archive` private, then archive it. Archiving makes it read-only, so it is last. Verify the 38 PRs are still readable by the owner afterwards.

### 4.10 Close out ✅ DONE 2026-09-07

Update `CLAUDE.md`'s repository row. Record in the Epic which numbers are placeholders. Delete the migration backup only once the new repository has been used for a full task cycle — **not on the day of the migration**.

---

## 5. Failure and recovery

| step | if it fails |
|---|---|
| 4.1–4.2 | nothing has changed; fix and re-run |
| 4.3 | rename back; the new name is not yet occupied |
| 4.4 | delete the new repo and recreate; nothing else depends on it yet |
| **4.5** | **no clean recovery.** Numbering drift cannot be repaired without deleting issues. This is why verification runs every 25. |
| 4.6–4.8 | re-runnable; all idempotent |
| 4.9 | unarchive is possible; re-publicising is possible but re-exposes the archive |

The single genuinely dangerous step is **4.5**. Everything before it is reversible, and everything after it is repeatable.

---

## 6. Decisions

### Settled

**All 243 comments are recreated** (owner, 2026-09-07). Not only those on open issues. This preserves the CI verification records on [#221](https://github.com/Strycher/Field_Compass/issues/221) and [#223](https://github.com/Strycher/Field_Compass/issues/223) — the `UNSTABLE` → `BLOCKED` measurement, the rejected cancellation fix and its reasoning, and the docs-only skip proof — which exist nowhere else. Each recreated comment is stamped with its original author and date.

**The three superseded branches are deleted** (owner, 2026-09-07): `archive/fc-131-stale-remote`, `chore/142-untrack-canonical`, `feat/135-hook-sync`. Evidence in §3.1. Done.

### Still open

1. **[#200](https://github.com/Strycher/Field_Compass/issues/200)** — merge `fc/200-legacy-match` before migrating, or abandon it?

   Measured, since "there is a failing test" argues the opposite way to how it first reads:

   ```
   main    1 failed, 52 passed, 1 SyntaxWarning
   branch  54 passed, no warnings
   ```

   The failing test is **self-contradictory**: `test_legacy_hash_still_matches_when_no_serial_recorded` passes `"E8F60ACA4E54"` as the port serial, so it never exercised the no-serial path its name describes. `pio-flash.py:420` documents the contract it violates — *"#503 OWNER RULING: serial-first, GLOBAL, no VID:PID precondition"* — because VID:PID is a device class, not an identity. The test arrived red with the #172 port and is still red upstream at meshcore-firmware#1051.

   Merging is therefore what makes the suite green, and it adds coverage of the genuine Tier 2 path that was never tested. **Recommendation: merge before migrating**, so the new repository starts green rather than inheriting a known-false failure that the next reader has to re-litigate.

   **Settled 2026-09-07: merged** as `9de4a9f`. `scripts/` is green for the first time since the pio-flash port — 54 passed — and is now CI-gated by [#249](https://github.com/Strycher/Field_Compass/issues/249).

2. **Epic parent** — **settled 2026-09-07: [#245](https://github.com/Strycher/Field_Compass/issues/245) is deliberately parentless, by owner ruling.** Not an oversight and not pending. Do not propose a parent for it.

### Nothing is open

Every decision this document raised has been made. The remaining gate is the owner's explicit go at **4.3**, which approving this runbook does not grant.
