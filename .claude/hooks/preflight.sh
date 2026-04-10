#!/usr/bin/env bash
# =============================================================================
# Field Compass — Pre-flight Check
# =============================================================================
# Runs on SessionStart. Blocks the session (exit 2) if required coordination
# services are unavailable. ALL checks are hard blocks.
#
# Required services:
#   1. Citadel API — task management (HARD BLOCK)
#   2. Agent Mail  — multi-agent coordination (HARD BLOCK)
#
# Exit codes:
#   0 = all checks passed
#   2 = BLOCKING — required service unavailable
#
# Derived from C:\Dev\DifferentWire\standards\hooks\preflight.sh
# =============================================================================

set -euo pipefail

# ─── Project-specific ─────────────────────────────────────────────────────
PROJECT_DIR="/c/Dev/Field_Compass"
# ──────────────────────────────────────────────────────────────────────────

CITADEL_HEALTH="https://getunfocused.app/citadel/health"
AGENT_MAIL_HEALTH="https://getunfocused.app/health/liveness"
HETZNER_HOST="unfocused@46.224.181.82"

PASS="✓"
FAIL="✗"
WARN="⚠"
errors=0

log()  { echo "$@" >&2; }
pass() { log "$PASS $1"; }
fail() { log "$FAIL $1"; errors=$((errors + 1)); }
warn() { log "$WARN $1"; }

log ""
log "═══ Field Compass Preflight Check ═══"
log ""

# ─── 1. Citadel API (HARD BLOCK) ────────────────────────────────────────
# Validate response body contains "citadel", not just HTTP 200.
CITADEL_BODY=$(curl -s --max-time 5 "$CITADEL_HEALTH" 2>/dev/null || echo "")
if echo "$CITADEL_BODY" | grep -q '"citadel"'; then
  pass "Citadel API"
else
  fail "Citadel API unreachable or returning wrong content"
  log "  Response: ${CITADEL_BODY:0:80}"
  log "  No task management = no work. Fix before proceeding."
  log "  Check: ssh ${HETZNER_HOST} \"docker ps | grep api\""
fi

# ─── 2. Agent Mail (HARD BLOCK) ─────────────────────────────────────────
if curl -s --connect-timeout 5 "$AGENT_MAIL_HEALTH" 2>/dev/null | grep -q "alive"; then
  pass "Agent Mail"
else
  fail "Agent Mail unreachable"
  log "  No coordination layer = no multi-agent safety. Fix before proceeding."
  log "  Check: ssh ${HETZNER_HOST} \"cd /opt/mcp_agent_mail && docker compose -f docker-compose.prod.yml logs --tail 20\""
fi

# ─── 3. Git Hooks Path ──────────────────────────────────────────────────
HOOKS_PATH=$(cd "$PROJECT_DIR" && git config core.hookspath 2>/dev/null || echo "")
if [ "$HOOKS_PATH" = ".claude/hooks" ]; then
  pass "Git hooks path (.claude/hooks)"
else
  warn "Git hooks path is '${HOOKS_PATH:-<unset>}' — fixing to .claude/hooks"
  cd "$PROJECT_DIR" && git config core.hookspath .claude/hooks
  pass "Git hooks path fixed"
fi

# ─── 4. Abandoned Worktree Detection ────────────────────────────────────
ABANDONED=()
while IFS= read -r wt_line; do
  wt_path=$(echo "$wt_line" | awk '{print $1}')
  wt_branch=$(echo "$wt_line" | awk '{print $2}' | tr -d '[]')
  if [ "$wt_path" = "$PROJECT_DIR" ]; then continue; fi
  if [ -d "$wt_path" ]; then
    LAST_COMMIT=$(cd "$wt_path" 2>/dev/null && git log -1 --format=%ct 2>/dev/null || echo "0")
    NOW=$(date +%s)
    AGE_HOURS=$(( (NOW - LAST_COMMIT) / 3600 ))
    if [ "$AGE_HOURS" -gt 24 ]; then
      ABANDONED+=("$wt_path (branch: $wt_branch, ${AGE_HOURS}h ago)")
    fi
  fi
done < <(cd "$PROJECT_DIR" && git worktree list 2>/dev/null | grep -v "^$")

if [ ${#ABANDONED[@]} -gt 0 ]; then
  warn "Found ${#ABANDONED[@]} potentially abandoned worktree(s):"
  for wt in "${ABANDONED[@]}"; do
    log "    $wt"
  done
  log "  To clean up: git worktree remove <path> && git branch -d <branch>"
fi

# ─── 5. Session State Recovery (Worker mode only) ───────────────────────
PREFLIGHT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SESSION_STATE_SCRIPT="$PREFLIGHT_DIR/session-state.sh"
SESSION_STATE_FILE="$PROJECT_DIR/.claude/agent-session.json"

if [ -f "$SESSION_STATE_FILE" ] && [ -f "$SESSION_STATE_SCRIPT" ]; then
  SESSION_OUTPUT=$(bash "$SESSION_STATE_SCRIPT" read 2>/dev/null || true)
  if [ -n "$SESSION_OUTPUT" ]; then
    log ""
    log "═══ Active Agent Session ═══"
    log ""
    while IFS= read -r line; do
      log "  $line"
    done <<< "$SESSION_OUTPUT"
    log ""
    log "  This session state was recovered from disk."
    log "  If /work was invoked, resume your current task."
    log "  Otherwise run: bash .claude/hooks/session-state.sh reset"
    log ""
  fi
fi

# ─── Result ──────────────────────────────────────────────────────────────
log ""
if [ "$errors" -gt 0 ]; then
  log "═══ BLOCKED: $errors check(s) failed ═══"
  log "Agents must not operate without Citadel AND Agent Mail."
  log ""
  exit 2
fi

log "═══ All checks passed — session ready ═══"
log ""
exit 0
