# CLAUDE.md - Field Compass Project Guidelines

## Project Overview

**Hardware Platform:** Adafruit ESP32-S3 Feather with 4MB Flash 2MB PSRAM - STEMMA QT / Qwiic
**Project Type:** Embedded firmware development with FeatherWing and I2C peripherals

## Hardware Specifications

- **MCU:** ESP32-S3 (dual-core Xtensa LX7)
- **Flash:** 4MB
- **PSRAM:** 2MB
- **Connectivity:** STEMMA QT / Qwiic (I2C)
- **Form Factor:** Adafruit Feather

## Development Workflow Requirements

### Backlog Management (GitHub Issues)

All backlog items MUST be created as GitHub Issues with the following **mandatory fields**:

| Field | Description | Required |
|-------|-------------|----------|
| **Title** | Clear, concise description of the item | Yes |
| **Body** | Detailed description including context | Yes |
| **Status** | Current state (see Status Labels below) | Yes |
| **Priority** | Importance level (see Priority Labels below) | Yes |

#### Status (GitHub Project Field — Board Columns)

| Status | Meaning | Who Moves Here |
|--------|---------|----------------|
| **Backlog** | Ungroomed idea — no spec, no acceptance criteria yet. Parking lot for future work. | Anyone — this is the "idea dump" |
| **Todo** | Groomed — has description, acceptance criteria, and design (if needed). Ready for sprint consideration. | Ben (PM) moves from Backlog after grooming |
| **Ready** | Approved for development, fully specified, all blockers resolved. Agents pick up work from here. | Ben, or auto-promote Action when dependencies close |
| **In Progress** | Actively being worked on by an agent or Ben. | Agent/Ben when starting work |
| **Testing** | PR created or code ready for review/QA. | Agent when work is ready for verification |
| **On Hold** | Deliberately paused by human decision — deprioritized, waiting on external input, scope changed. | Ben — manual decision to freeze work |
| **Done** | Acceptance criteria met, issue closed. | Auto on issue close, or manual |

```
Backlog → Todo → Ready → In Progress → Testing → Done
 (idea)  (groomed) (approved) (coding)   (review)  (shipped)
                                  ↕
                               On Hold
                          (frozen by decision)
```

#### Priority (GitHub Project Field)
- `P0 - Critical` - Must be addressed immediately
- `P1 - High` - Important, address soon
- `P2 - Medium` - Normal priority
- `P3 - Low/Cosmetic` - Nice to have, cosmetic issues
- `P4 - Deferred` - Postponed for future consideration

#### Issue Types (Labels)
- `type:epic` — Parent issue with sub-issues
- `enhancement` — New feature or improvement
- `bug` — Defect or unexpected behavior
- `documentation` — Documentation updates
- `refactor` — Code improvement without behavior change

### Issue Lifecycle

1. **Create** issues when identifying work — enhancements, bugs, docs gaps
2. **Backlog** — new ideas and ungroomed work start here
3. **Groom** — Ben adds description, acceptance criteria, design reference. Moves to Todo.
4. **Ready** — Ben moves groomed issues to Ready, OR the auto-promote Action moves them when dependencies resolve
5. **In Progress** — agent or Ben picks up a Ready issue and begins work
6. **Update** with comments for significant progress
7. **Reference** in commit messages: `feat(#12): add compass widget`
8. **Testing** — code ready for Ben to review/verify on device
9. **On Hold** — if work must be paused (deprioritized, waiting on input, scope change), move here with a comment explaining why
10. **Close** when acceptance criteria met: `gh issue close <number>`
11. All issues must exist in GitHub Project — no local-only issues

### Dependency Automation
- Issues with `<!-- depends-on: #X, #Y -->` tags in their body are tracked by the **auto-promote GitHub Action** (`.github/workflows/auto-promote-ready.yml`).
- When a dependency closes, the Action checks if all deps are resolved. If yes, it auto-promotes the issue from **Todo → Ready**.
- The Action only promotes from Todo. It never touches Backlog, On Hold, or other statuses.
- When creating issues with dependencies, always include the tag: `<!-- depends-on: #123, #456 -->`

### Version Control

#### Commit Strategy
- Commit each **successfully compiled** version
- Write clear, descriptive commit messages
- Reference related Issue numbers in commits (e.g., `Fixes #12`)

#### Version Tagging (Semantic Versioning)

Format: `vMAJOR.MINOR.PATCH`

| Component | When to Increment |
|-----------|------------------|
| **MAJOR** | Breaking changes, incompatible API changes |
| **MINOR** | New features, backward-compatible additions |
| **PATCH** | Bug fixes, backward-compatible fixes |

**Examples:**
- `v0.1.0` - Initial development release
- `v1.0.0` - First stable release
- `v1.1.0` - Added new feature
- `v1.1.1` - Bug fix

## Build Environment

### Prerequisites
- Arduino IDE or PlatformIO
- ESP32 Board Support Package
- Adafruit ESP32-S3 Feather board definition

### Board Configuration
```
Board: Adafruit Feather ESP32-S3 4MB Flash 2MB PSRAM
USB Mode: USB-OTG (TinyUSB)
Upload Speed: 921600
```

## I2C Configuration

Default I2C pins for Adafruit ESP32-S3 Feather:
- **SDA:** GPIO 3
- **SCL:** GPIO 4

STEMMA QT connector provides dedicated I2C connection.

## Code Style Guidelines

- Use descriptive variable and function names
- Comment complex logic
- Keep functions focused and single-purpose
- Use `#define` for hardware pin assignments
- Group related functionality into separate files

## File Structure

```
Field_Compass/
├── CLAUDE.md           # This file - project guidelines
├── README.md           # Project documentation
├── src/                # Source code
│   ├── main.cpp        # Main application entry
│   ├── config.h        # Configuration and pin definitions
│   └── ...
├── lib/                # Project-specific libraries
├── include/            # Header files
├── test/               # Test files
└── docs/               # Additional documentation
```

## Claude Assistant Responsibilities

### Automatic Actions
1. **Create Issues** when identifying:
   - New enhancement opportunities
   - Bugs or defects
   - Documentation gaps

2. **Maintain Issues** by:
   - Adding comments for each update
   - Updating Status labels as work progresses
   - Documenting Specifications and Acceptance Criteria

3. **Version Control** by:
   - Committing successfully compiled code
   - Creating version tags following semantic versioning
   - Writing clear commit messages referencing Issues

### Before Each Commit
- [ ] Code compiles without errors
- [ ] Related Issue(s) updated with progress
- [ ] Commit message references Issue number(s)
- [ ] Version tag created if milestone reached

## Agent Workflow

### Task Management (Beads)
- Use `bd` (Beads) for structured task tracking during development sessions.
- Before starting work, run `bd ready` to see what tasks are unblocked.
- When picking up a task, run `bd update <task-id> --status=in_progress`.
- When completing a task, run `bd close <task-id> --reason "description"`.
- For complex features, decompose into subtasks with dependencies:
  `bd create --title="Subtask name" -t task -p 2 --parent <epic-id>`
- Issue prefix is `FC` — IDs look like `FC-abc`.

### GitHub Board Sync
- The GitHub Project board is the source of truth for project status.
- When creating a Beads task from a GitHub Issue, include the issue number in the title: `"Feature name (#22)"`.
- Include the Beads ID (e.g., FC-abc) in a GitHub Issue comment for cross-reference.
- When moving a Beads task to in_progress, comment on the GitHub Issue with the status change.
- When closing a Beads task, close or update the corresponding GitHub Issue.
- Never let Beads and GitHub get out of sync — update both in the same workflow step.

### Multi-Agent Coordination (Agent Mail)
- If agent-mail MCP is available, register with a descriptive agent name (e.g., "fc-firmware-agent").
- Before editing files, check for file reservations via agent-mail.
- Reserve files you're working on to prevent conflicts with other agents.
- When done with a file, release the reservation.

### Infrastructure Health Check (MANDATORY)

At the start of every session, verify all infrastructure is operational before doing any work:

1. **Dolt SSH Tunnel**: Verify port 3307 is listening (SSH tunnel to Hetzner `unfocused@46.224.181.82`).
   If not running, start it:
   ```bash
   ssh -fNL 3307:127.0.0.1:3307 unfocused@46.224.181.82
   ```

2. **Beads/Dolt**: Run `bd dolt test`. If it fails, check the Dolt Docker container on Hetzner:
   ```bash
   ssh unfocused@46.224.181.82 "docker ps | grep dolt"
   ```
   If still failing, STOP and alert the user.

3. **Agent Mail**: Verify health at `https://getunfocused.app/health/liveness`. If unreachable or returning errors, STOP and alert the user.

4. **Beads readiness**: Run `bd ready` to load current task state.

**CRITICAL**: If Beads or Agent Mail are unreachable, **STOP and tell the user immediately.** Do NOT silently fall back to GitHub-only workflows. Use the startup scripts in `scripts/` or the preflight hook in `.claude/hooks/preflight.sh` to ensure proper initialization.

### Autonomous Operation Rules
- **Only pick up issues from the "Ready" column on the GitHub Project board.** Never pull from Backlog, Todo, or On Hold — Ben decides what is ready for development.
- You MUST operate autonomously without human approval for:
  - Implementing features from GitHub Issues in the "Ready" column
  - Writing and running tests (compile checks)
  - Updating Beads tasks and GitHub Issues
  - Committing code that compiles successfully (per commit convention above)
  - Picking up the next task after completing one (see Continuous Work Loop below)
- If unsure about a design decision, create a GitHub Issue tagged "question" rather than guessing.
- If a task is blocked by unclear requirements, move it to On Hold and comment on the GitHub Issue explaining what's needed.
- When creating sub-issues with dependencies, always include `<!-- depends-on: #NNN -->` in the issue body so the auto-promote Action can chain them.

> **CRITICAL RULE (repeated for emphasis):** If Beads or Agent Mail are unreachable, **STOP and tell the user immediately.** Do NOT silently fall back to GitHub-only workflows. The user has invested in this infrastructure specifically so agents coordinate — ignoring failures defeats the entire purpose.

### Continuous Work Loop (MANDATORY)

**After completing a task, you MUST immediately check for and pick up the next available task. Do NOT stop and ask the user if they want you to continue. Do NOT report "nothing to do" without checking. The standing directive is: work until `bd ready` returns nothing.**

```
Complete task → Reset (below) → bd ready → claim next → work → repeat
```

**Rules:**
1. After every task completion, run the Between-Task Reset Protocol below, then run `bd ready`.
2. If `bd ready` returns tasks, pick the highest-priority one and begin work immediately. Do not ask for permission.
3. If `bd ready` returns nothing, check `bd list --status=open` for blocked tasks and report what they're waiting on. Only THEN tell the user there's no available work.
4. If a task you just completed unblocks downstream tasks, those will appear in `bd ready` — pick them up.
5. The only reasons to stop working are:
   - `bd ready` returns no tasks AND no open tasks exist
   - You encounter a design decision that requires human input (create an issue tagged "question")
   - Infrastructure failure (Beads/Agent Mail unreachable)
   - You've hit a hard blocker you cannot resolve

**Anti-pattern (NEVER do this):**
> "I've completed the task. Would you like me to check for more work?"

**Correct behavior:**
> "Task complete, pushed to main. Checking for next available work..."
> *[runs bd ready, claims task, begins work]*

### Between-Task Reset Protocol

When finishing one task and picking up another **within the same session**, you MUST reset before starting the new task. Do NOT carry over file reservations or working state from the previous task.

```bash
# 1. Finalize the completed task
bd close <old-task-id> --reason "description"
git add <files> && git commit -m "type(#issue): description"
git push origin main

# 2. Release ALL file reservations from the old task
#    release_file_reservations(project_key="...", agent_name="YourName")

# 3. Check inbox for new coordination messages
#    fetch_inbox(project_key="...", agent_name="YourName")

# 4. Find and claim new work
bd ready
bd list --status=in_progress          # Verify no one else claimed it
bd update <new-task-id> --status=in_progress

# 5. Reserve files for the new task
#    file_reservation_paths(project_key="...", agent_name="YourName", paths=[...])

# 6. Begin work on new task IMMEDIATELY — do not ask for permission
```

**Key rule:** Never hold file reservations across tasks. Each task gets a clean slate.

### Session Size Limits (MANDATORY)

Working sessions MUST be limited to **15–30 minutes of effort per issue**, touching **no more than 1–3 files**. This applies to all agents and all sessions without exception.

| Constraint | Target |
|------------|--------|
| Files touched | 1–3 files |
| Completion time | 15–30 minutes |
| Commit scope | Single commit |
| Design decisions | Zero — all decisions resolved before Ready |

- If an issue requires more than 30 minutes of work or touches more than 3 files, it MUST be decomposed into smaller sub-issues before starting.
- Each sub-issue should be independently implementable, compilable, and testable within one session.
- Use `<!-- depends-on: #NNN -->` to chain sub-issues so the auto-promote Action sequences them correctly.
- Never start a large implementation in a single session — plan first, break it down, then work the pieces.
- If you discover mid-session that an issue is larger than estimated, STOP, create sub-issues for the remaining work, and close out what you've completed.

**Red flags that a task is too big:**
- Touches 4+ files or sections across multiple subsystems
- Requires both UI and backend/data changes
- Needs design decisions the agent would have to make
- Description says "implement the entire..." or "add full support for..."

### File-Convergence Rule (MANDATORY for Planning Agent)

**If two tasks touch the same file, one MUST depend on the other. No exceptions.**

Parallel work that modifies the same file guarantees merge conflicts, even if the tasks are logically independent. When breaking epics into sub-issues:

1. For each task, list every file it will create or modify.
2. If two tasks share ANY file, add a `<!-- depends-on: #NNN -->` dependency so they execute sequentially.
3. The task that makes foundational/structural changes goes first; the task that builds on top depends on it.
4. When in doubt, serialize. A false dependency costs minutes; a conflict costs an entire agent session.

**Planning checklist before moving tasks to Ready:**
- [ ] File-overlap matrix generated for all tasks in the batch
- [ ] Every shared-file pair has a dependency link
- [ ] No two Ready tasks touch the same file without a dependency chain
- [ ] Critical shared files (e.g., `Field_Compass.ino`) identified and serialized first

### GitHub API Rate Limit Conservation (MANDATORY)

GitHub's GraphQL API has a **5,000 point/hour limit per user** — shared across ALL agents and workflows. Exhausting it blocks every agent and CI pipeline.

**Rules for all agents:**
1. **Use `gh` CLI for issues** — these use the REST API (separate 5,000/hr budget, rarely exhausted).
2. **Minimize `gh project` commands** — these use GraphQL. Query the board ONCE at session start, then work from Beads locally.
3. **Never poll or loop** — no `watch`, no retries-in-a-loop on GitHub commands.
4. **Batch board updates** — if you need to update multiple issues on the board, consider whether all are truly needed mid-session or can wait until session end.
5. **Beads is your working state, GitHub is the sync target.** Use `bd` commands (zero API cost) for task tracking. Only touch GitHub for issue creation and final status sync.
6. **Check rate limit before batch operations:**
   ```bash
   gh api rate_limit --jq '.resources.graphql.remaining'
   # If < 500, defer non-essential board operations
   ```

**What costs GraphQL points (AVOID unless necessary):**
- `gh project item-list` / `gh project item-edit` / `gh project item-add`
- Any `gh api graphql` calls

**What uses REST (safe, separate budget):**
- `gh issue create/list/close/comment`
- `gh pr create/list/view/merge`
- `gh label create/list`

### Before Each Push Checklist

- [ ] `arduino-cli compile` passes locally (don't push broken code)
- [ ] `git fetch origin main` — check if main has moved ahead
- [ ] If behind main: `git pull --rebase origin main` and resolve conflicts locally
- [ ] Re-compile after rebase (rebase can introduce breakage)

### Dev Environment Startup

Startup scripts establish SSH tunnel, verify Beads, and launch Claude Code:
- **PowerShell:** `.\scripts\start-claude.ps1`

Scripts launch Claude Code in **interactive mode** by default — infrastructure is verified, but no autonomous work begins. This allows ad-hoc queries or manual exploration.

Even if launched without a startup script, the **SessionStart hook** (`.claude/hooks/preflight.sh`) auto-starts the SSH tunnel and blocks the session if Beads or Agent Mail are unreachable. No agent can skip the safety net.

> **CRITICAL RULE (repeated for emphasis):** If Beads or Agent Mail are unreachable at any point during a session, **STOP and tell the user immediately.** Do NOT silently fall back to GitHub-only workflows.

## Quick Reference Commands

```bash
# Issues
gh issue create --title "Title" --body "Description" --label "enhancement"
gh issue comment <number> --body "Progress update"
gh issue close <number>
gh issue list

# Project
gh project item-add 3 --owner Strycher --url <issue-url>

# Rate limit check
gh api rate_limit --jq '.resources.graphql.remaining'

# Versioning
git tag -a v0.48.0 -m "Description"
git push origin main --tags

# Arduino CLI
arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/
arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 Field_Compass/

# Beads
bd ready                              # Show unblocked tasks
bd list                               # Show all tasks
bd create --title="Name" -t task -p 2 # Create task
bd update <id> --status=in_progress   # Claim task
bd close <id> --reason "done"         # Complete task
bd dep tree <epic-id>                 # View dependency tree
```

**Note:** Status and Priority are managed through the GitHub Project board fields, not labels.
