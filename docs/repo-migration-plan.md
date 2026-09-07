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

- [ ] **No open PRs.** [#244](https://github.com/Strycher/Field_Compass/pull/244) merged or closed.
- [ ] **`docs/ci-diagnosis.md` is on `main`** — [#244](https://github.com/Strycher/Field_Compass/pull/244). *(Owner: land it. In flight.)*
- [ ] **[#200](https://github.com/Strycher/Field_Compass/issues/200) settled** — `fc/200-legacy-match` merged, or the branch explicitly abandoned. *(Owner decision outstanding.)*
- [ ] **Three superseded branches deleted** — evidence in §3.1.
- [ ] **All worktrees removed**, `main` clean.
- [ ] **No Citadel task in progress** except the migration's own.
- [ ] **A verified backup exists** — see 4.1.

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

### 4.1 Back up everything — do this first

A full mirror clone plus a JSON export of every issue, comment, label, milestone and project item, stored **outside** every git working tree at `C:\Dev\.field_compass\migration-backup\`, alongside the device registry that already lives there for the same reason.

Verify by counting: 243 issue records, 243 comments, and a mirror whose `git log --all` reaches every branch. **If the counts do not match, stop.**

### 4.2 Redact history

`git filter-repo --replace-text` over a **copy** of the mirror, mapping each of the six literals to a placeholder. Never run against the working clone.

Verify: `git log -p --all | grep` finds zero occurrences of any of the four needles (`REDACTED_WIFI_PASSWORD`, `REDACTED_SSID_1`, `REDACTED_HOTSPOT_PASSWORD`, `REDACTED_SSID_3`) across all refs. Confirm the tree at `HEAD` still builds — `pio run -e feather_s3` — because redaction touches a live source file.

### 4.3 ⛔ IRREVERSIBLE — rename the old repository

`Strycher/Field_Compass` → `Strycher/Field_Compass-archive`.

Renaming leaves a redirect; creating a new repo at the old name overrides it, which is the intent. Verify the archive responds at its new name before continuing.

### 4.4 Create the new repository and push

New public `Strycher/Field_Compass`. Push the redacted history — all branches, all tags.

Verify: clone it fresh to a scratch directory, run the four-needle grep over `--all`, and build. **A clean clone is the only trustworthy check**; a local repo can hide objects the remote does not have, and vice versa.

### 4.5 ⛔ IRREVERSIBLE — recreate 243 issue slots in order

Strict ascending order, 1 → 243. Nothing else may create an issue **or a pull request** in the new repository during this run, because PRs consume the same number space and a single stray one shifts every subsequent number.

For each original number:

- **was a PR** (38 of them: 128, 129, 130, 132, 136, 138, 140, 141, 143, 145, 148, 152, 165, 167, 170, 171, 174, 176, 178, 180, 182, 189, 190, 192, 193, 197, 199, 202, 204, 206, 207, 220, 235, 237, 238, 240, 241, 243) → create a closed placeholder naming the PR and linking to it in the archive
- **was an issue** → create with sanitized body, original author and date stamped in, then labels, then comments, then close if it was closed

**Seven issues carry credentials** — [#7](https://github.com/Strycher/Field_Compass/issues/7), [#9](https://github.com/Strycher/Field_Compass/issues/9), [#34](https://github.com/Strycher/Field_Compass/issues/34), [#35](https://github.com/Strycher/Field_Compass/issues/35), [#39](https://github.com/Strycher/Field_Compass/issues/39), [#99](https://github.com/Strycher/Field_Compass/issues/99), [#166](https://github.com/Strycher/Field_Compass/issues/166). Their bodies and comments are sanitized before creation. [#34](https://github.com/Strycher/Field_Compass/issues/34) and [#99](https://github.com/Strycher/Field_Compass/issues/99) are **open** and must stay usable, so they are recreated in full minus the literals — not reduced to pointers.

Verify after every 25: the highest issue number equals the count created. Drift means stop immediately; it cannot be corrected afterwards without deleting issues.

### 4.6 Re-link

Native sub-issue parent/child links, then `<!-- depends-on: #N -->` bodies, then the project board items with Status and Priority set on both the field and the label.

Citadel needs no change — `external_issue_number` stays valid because the numbers were preserved. Verify by spot-checking five tasks across the range.

Stale commit SHAs quoted in issue bodies are left as they are; a note in the Epic records that pre-migration SHAs refer to the archive.

### 4.7 Rebuild repository settings

Branch protection (`compile-gate` required, `strict: false`, `enforce_admins: true`, no force pushes, no deletions), the `PROJECT_PAT` secret, and all 31 labels.

Verify branch protection by **repeating the #223 break test**: push a deliberate syntax error, confirm `mergeStateStatus=BLOCKED`, close unmerged, delete the branch. A protection setting that has not been observed blocking is not known to block.

### 4.8 Repoint the local working directory

`C:\Dev\Field_Compass` stays where it is:

```bash
git remote set-url origin https://github.com/Strycher/Field_Compass.git
git fetch origin
git reset --hard origin/main
```

Untracked and ignored files survive this — local settings, un-committed hooks, scratch. **Claude Code sessions and memory are keyed to the local directory path**, `~/.claude/projects/C--Dev-Field-Compass/`, not to the git remote, so both are unaffected.

Verify: `git log` shows redacted history, `pio run -e feather_s3` succeeds, `python scripts/pio-flash.py list` still resolves the device registry at `C:\Dev\.field_compass\`.

### 4.9 ⛔ IRREVERSIBLE — private, then archive

Make `Field_Compass-archive` private, then archive it. Archiving makes it read-only, so it is last. Verify the 38 PRs are still readable by the owner afterwards.

### 4.10 Close out

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

2. **Epic parent** — [#245](https://github.com/Strycher/Field_Compass/issues/245) is currently parentless. Parents are the owner's grant.
