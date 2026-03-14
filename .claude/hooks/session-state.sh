#!/usr/bin/env bash
# =============================================================================
# Field Compass — Agent Session State Manager
# =============================================================================
# Manages a persistent session state file that survives context compaction.
# The preflight hook reads this file and outputs it as a system reminder,
# allowing agents to recover their working context after compaction.
#
# Usage:
#   session-state.sh <subcommand> [options]
#
# Subcommands:
#   init            Create a new session (fails if one already exists)
#   read            Output session state as formatted text
#   update-task     Set the current task being worked on
#   clear-task      Clear current task (between tasks)
#   record-create   Record a Beads task creation
#   record-close    Record a task completion (increments counter)
#   record-pr       Record a PR creation
#   update-pr-state Update a PR's merge state
#   check-duplicate Check if a task title was already created this session
#   advance-epic    Move to the next epic in the queue
#   check-budget    Check if task budget is exhausted
#   reset           Delete the session file (cleanup)
#
# Exit codes:
#   0 = success / check passed
#   1 = error or check failed (duplicate found, session exists, etc.)
#   2 = HARD STOP — agent must cease all work (budget exhausted, queue empty)
# =============================================================================

set -euo pipefail

# ─── Constants ────────────────────────────────────────────────────────────────

SESSION_FILE="/c/Dev/Field_Compass/.claude/agent-session.json"
SESSION_TMP="${SESSION_FILE}.tmp"

# ─── Helpers ──────────────────────────────────────────────────────────────────

die()  { echo "ERROR: $1" >&2; exit "${2:-1}"; }
info() { echo "$@" >&2; }

# Atomic write: write to .tmp then move into place
atomic_write() {
  local content="$1"
  echo "$content" > "$SESSION_TMP"
  mv -f "$SESSION_TMP" "$SESSION_FILE"
}

# Read the session file, or die if it doesn't exist
read_session() {
  if [ ! -f "$SESSION_FILE" ]; then
    die "No active session (${SESSION_FILE} not found)" 1
  fi
  cat "$SESSION_FILE"
}

# Python helper: takes a Python script on stdin, passes session JSON as arg
py_session() {
  local py_script="$1"
  shift
  python3 -c "$py_script" "$@"
}

# ─── Subcommand: init ────────────────────────────────────────────────────────
# Creates a new session. Refuses to overwrite an existing session.
#
# Options:
#   --agent <name>       Agent Mail name (required)
#   --epics <id,...>     Comma-separated epic IDs (optional)
#   --max-tasks <N>      Task budget, 0=unlimited (optional, default 0)
#
# Exit: 0=created, 1=session already exists (use 'read' to recover)

cmd_init() {
  local agent="" epics="" max_tasks=0

  while [ $# -gt 0 ]; do
    case "$1" in
      --agent)    agent="$2"; shift 2 ;;
      --epics)    epics="$2"; shift 2 ;;
      --max-tasks) max_tasks="$2"; shift 2 ;;
      *) die "init: unknown option '$1'" ;;
    esac
  done

  [ -z "$agent" ] && die "init: --agent is required"

  # Block re-init if session already exists
  if [ -f "$SESSION_FILE" ]; then
    info "Session already exists. Use 'read' to recover state."
    exit 1
  fi

  local now
  now=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

  py_session '
import json, sys

agent = sys.argv[1]
epics_raw = sys.argv[2]
max_tasks = int(sys.argv[3])
started = sys.argv[4]

epic_list = [e.strip() for e in epics_raw.split(",") if e.strip()] if epics_raw else []

session = {
    "session_id": f"{agent}-{started}",
    "agent_name": agent,
    "started_at": started,
    "epic_queue": epic_list,
    "current_epic": epic_list[0] if epic_list else None,
    "max_tasks": max_tasks,
    "current_task": None,
    "tasks_completed": 0,
    "tasks_created_this_session": [],
    "prs_created": []
}

print(json.dumps(session, indent=2))
' "$agent" "$epics" "$max_tasks" "$now" > "$SESSION_TMP"

  mv -f "$SESSION_TMP" "$SESSION_FILE"
  info "Session initialized: agent=$agent, epics=${epics:-none}, max_tasks=${max_tasks}"
}

# ─── Subcommand: read ────────────────────────────────────────────────────────
# Outputs the session state as formatted text suitable for a system reminder.
# Exit: 0=ok, 1=no session

cmd_read() {
  local session
  session=$(read_session) || exit 1

  py_session '
import json, sys

try:
    s = json.loads(sys.stdin.read())
except (json.JSONDecodeError, KeyError):
    print("Session file is corrupted", file=sys.stderr)
    sys.exit(1)

lines = []
agent = s.get("agent_name", "unknown")
lines.append("Agent: " + agent)

# Epic queue
eq = s.get("epic_queue", [])
ce = s.get("current_epic")
if eq:
    parts = []
    for e in eq:
        if e == ce:
            parts.append(e + " (current)")
        else:
            parts.append(e)
    lines.append("Epic Queue: " + ", ".join(parts))
elif ce:
    lines.append("Current Epic: " + ce)
else:
    lines.append("Epic Queue: unrestricted (all ready tasks)")

# Current task
ct = s.get("current_task")
if ct and isinstance(ct, dict):
    tid = ct.get("id", "?")
    ttl = ct.get("title", "?")
    lines.append("Current Task: " + tid + " - " + repr(ttl))
    branch = ct.get("branch")
    if branch:
        lines.append("  Branch: " + branch)
    wt = ct.get("worktree")
    if wt:
        lines.append("  Worktree: " + wt)
else:
    lines.append("Current Task: none (between tasks)")

# Budget
completed = s.get("tasks_completed", 0)
max_t = s.get("max_tasks", 0)
if max_t > 0:
    lines.append("Tasks Completed: " + str(completed) + " of " + str(max_t) + " max")
else:
    lines.append("Tasks Completed: " + str(completed) + " (no limit)")

# PRs
prs = s.get("prs_created", [])
if prs:
    pr_strs = []
    for p in prs:
        num = str(p.get("number", "?"))
        state = p.get("state", "open")
        pr_strs.append("#" + num + " (" + state + ")")
    lines.append("PRs: " + ", ".join(pr_strs))

# Tasks created
tc = s.get("tasks_created_this_session", [])
if tc:
    ids = [t.get("id", "?") for t in tc]
    lines.append("Tasks Created This Session: " + ", ".join(ids))

for line in lines:
    print(line)
' <<< "$session"
}

# ─── Subcommand: update-task ─────────────────────────────────────────────────
# Options: --id, --title, --branch, --worktree

cmd_update_task() {
  local id="" title="" branch="" worktree=""

  while [ $# -gt 0 ]; do
    case "$1" in
      --id)       id="$2"; shift 2 ;;
      --title)    title="$2"; shift 2 ;;
      --branch)   branch="$2"; shift 2 ;;
      --worktree) worktree="$2"; shift 2 ;;
      *) die "update-task: unknown option '$1'" ;;
    esac
  done

  local session
  session=$(read_session) || exit 1

  local result
  result=$(py_session '
import json, sys
s = json.loads(sys.stdin.read())
now_str = sys.argv[1]
tid = sys.argv[2]
title = sys.argv[3]
branch = sys.argv[4]
wt = sys.argv[5]

s["current_task"] = {
    "id": tid,
    "title": title,
    "branch": branch,
    "worktree": wt,
    "claimed_at": now_str
}
print(json.dumps(s, indent=2))
' "$(date -u +"%Y-%m-%dT%H:%M:%SZ")" "$id" "$title" "$branch" "$worktree" <<< "$session")

  atomic_write "$result"
  info "Current task set: $id"
}

# ─── Subcommand: clear-task ──────────────────────────────────────────────────

cmd_clear_task() {
  local session
  session=$(read_session) || exit 1

  local result
  result=$(py_session '
import json, sys
s = json.loads(sys.stdin.read())
s["current_task"] = None
print(json.dumps(s, indent=2))
' <<< "$session")

  atomic_write "$result"
  info "Current task cleared"
}

# ─── Subcommand: record-create ───────────────────────────────────────────────
# Options: --id, --title

cmd_record_create() {
  local id="" title=""

  while [ $# -gt 0 ]; do
    case "$1" in
      --id)    id="$2"; shift 2 ;;
      --title) title="$2"; shift 2 ;;
      *) die "record-create: unknown option '$1'" ;;
    esac
  done

  local session
  session=$(read_session) || exit 1

  local result
  result=$(py_session '
import json, sys
s = json.loads(sys.stdin.read())
tid = sys.argv[1]
title = sys.argv[2]
s.setdefault("tasks_created_this_session", []).append({"id": tid, "title": title})
print(json.dumps(s, indent=2))
' "$id" "$title" <<< "$session")

  atomic_write "$result"
  info "Recorded task creation: $id"
}

# ─── Subcommand: record-close ────────────────────────────────────────────────

cmd_record_close() {
  local session
  session=$(read_session) || exit 1

  local result
  result=$(py_session '
import json, sys
s = json.loads(sys.stdin.read())
s["tasks_completed"] = s.get("tasks_completed", 0) + 1
s["current_task"] = None
print(json.dumps(s, indent=2))
' <<< "$session")

  atomic_write "$result"
  info "Task closed. Total completed: $(echo "$result" | python3 -c 'import json,sys; print(json.load(sys.stdin)["tasks_completed"])')"
}

# ─── Subcommand: record-pr ───────────────────────────────────────────────────
# Options: --number, --branch

cmd_record_pr() {
  local number="" branch=""

  while [ $# -gt 0 ]; do
    case "$1" in
      --number) number="$2"; shift 2 ;;
      --branch) branch="$2"; shift 2 ;;
      *) die "record-pr: unknown option '$1'" ;;
    esac
  done

  local session
  session=$(read_session) || exit 1

  local result
  result=$(py_session '
import json, sys
s = json.loads(sys.stdin.read())
num = int(sys.argv[1])
branch = sys.argv[2]
s.setdefault("prs_created", []).append({"number": num, "branch": branch, "state": "open"})
print(json.dumps(s, indent=2))
' "$number" "$branch" <<< "$session")

  atomic_write "$result"
  info "Recorded PR #$number"
}

# ─── Subcommand: update-pr-state ─────────────────────────────────────────────
# Options: --number, --state (open|merged|closed)

cmd_update_pr_state() {
  local number="" state=""

  while [ $# -gt 0 ]; do
    case "$1" in
      --number) number="$2"; shift 2 ;;
      --state)  state="$2"; shift 2 ;;
      *) die "update-pr-state: unknown option '$1'" ;;
    esac
  done

  local session
  session=$(read_session) || exit 1

  local result
  result=$(py_session '
import json, sys
s = json.loads(sys.stdin.read())
num = int(sys.argv[1])
new_state = sys.argv[2]
for pr in s.get("prs_created", []):
    if pr.get("number") == num:
        pr["state"] = new_state
        break
print(json.dumps(s, indent=2))
' "$number" "$state" <<< "$session")

  atomic_write "$result"
  info "PR #$number → $state"
}

# ─── Subcommand: check-duplicate ─────────────────────────────────────────────
# Options: --title <fragment>
# Exit: 0=no duplicate, 1=duplicate found

cmd_check_duplicate() {
  local title=""

  while [ $# -gt 0 ]; do
    case "$1" in
      --title) title="$2"; shift 2 ;;
      *) die "check-duplicate: unknown option '$1'" ;;
    esac
  done

  [ -z "$title" ] && die "check-duplicate: --title is required"

  local session
  session=$(read_session) || exit 1

  py_session '
import json, sys
s = json.loads(sys.stdin.read())
fragment = sys.argv[1].lower()
for t in s.get("tasks_created_this_session", []):
    if fragment in t.get("title", "").lower():
        ttl = t.get("title", "?")
        tid = t.get("id", "?")
        print("DUPLICATE: " + repr(ttl) + " (id: " + tid + ")", file=sys.stderr)
        sys.exit(1)
sys.exit(0)
' "$title" <<< "$session"
}

# ─── Subcommand: advance-epic ────────────────────────────────────────────────
# Moves current_epic to the next in the queue.
# Exit: 0=advanced, 2=queue empty (HARD STOP)

cmd_advance_epic() {
  local session
  session=$(read_session) || exit 1

  local exit_code
  exit_code=0

  local result
  result=$(py_session '
import json, sys
s = json.loads(sys.stdin.read())
queue = s.get("epic_queue", [])
current = s.get("current_epic")

if not queue:
    # No queue — unrestricted mode, nothing to advance
    print(json.dumps(s, indent=2))
    sys.exit(2)

try:
    idx = queue.index(current)
except ValueError:
    idx = -1

next_idx = idx + 1
if next_idx < len(queue):
    s["current_epic"] = queue[next_idx]
    print(json.dumps(s, indent=2))
    print(f"Advanced to epic: {queue[next_idx]}", file=sys.stderr)
    sys.exit(0)
else:
    print(json.dumps(s, indent=2))
    print("Epic queue exhausted — all epics processed.", file=sys.stderr)
    sys.exit(2)
' <<< "$session") || exit_code=$?

  if [ $exit_code -eq 0 ]; then
    atomic_write "$result"
  elif [ $exit_code -eq 2 ]; then
    info "HARD STOP: Epic queue exhausted."
    exit 2
  fi
}

# ─── Subcommand: check-budget ────────────────────────────────────────────────
# Exit: 0=under budget, 2=budget exhausted (HARD STOP)

cmd_check_budget() {
  local session
  session=$(read_session) || exit 1

  py_session '
import json, sys
s = json.loads(sys.stdin.read())
max_t = s.get("max_tasks", 0)
completed = s.get("tasks_completed", 0)

if max_t <= 0:
    # No budget limit
    sys.exit(0)

if completed >= max_t:
    print(f"BUDGET EXHAUSTED: {completed}/{max_t} tasks completed.", file=sys.stderr)
    sys.exit(2)
else:
    remaining = max_t - completed
    print(f"Budget OK: {completed}/{max_t} completed, {remaining} remaining.", file=sys.stderr)
    sys.exit(0)
' <<< "$session"
}

# ─── Subcommand: reset ───────────────────────────────────────────────────────

cmd_reset() {
  if [ -f "$SESSION_FILE" ]; then
    rm -f "$SESSION_FILE" "$SESSION_TMP"
    info "Session state cleared"
  else
    info "No session to clear"
  fi
}

# ─── Dispatch ─────────────────────────────────────────────────────────────────

SUBCOMMAND="${1:-}"
shift 2>/dev/null || true

case "$SUBCOMMAND" in
  init)             cmd_init "$@" ;;
  read)             cmd_read ;;
  update-task)      cmd_update_task "$@" ;;
  clear-task)       cmd_clear_task ;;
  record-create)    cmd_record_create "$@" ;;
  record-close)     cmd_record_close ;;
  record-pr)        cmd_record_pr "$@" ;;
  update-pr-state)  cmd_update_pr_state "$@" ;;
  check-duplicate)  cmd_check_duplicate "$@" ;;
  advance-epic)     cmd_advance_epic ;;
  check-budget)     cmd_check_budget ;;
  reset)            cmd_reset ;;
  "")               die "Usage: session-state.sh <subcommand> [options]" ;;
  *)                die "Unknown subcommand: $SUBCOMMAND" ;;
esac
