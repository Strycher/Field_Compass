#!/usr/bin/env bash
# =============================================================================
# Field Compass — Pre-flight Check
# =============================================================================
# Runs on SessionStart. Blocks the session (exit 2) if required coordination
# services are unavailable. Auto-starts the Dolt SSH tunnel if it's down.
#
# Required services:
#   1. Dolt SSH tunnel (localhost → Hetzner:3307) — for Beads task tracking
#   2. MCP Agent Mail (getunfocused.app/mcp/) — for multi-agent coordination
#
# Zombie port detection:
#   If a previous session's SSH tunnel died without closing its socket,
#   the port stays LISTENING with a dead PID. This hook detects that and
#   falls back to alternate ports (3308, 3309).
#
# Exit codes:
#   0 = all checks passed, session may proceed
#   2 = BLOCKING — required service unavailable, session must not start
# =============================================================================

set -euo pipefail

HETZNER_HOST="unfocused@46.224.181.82"
DOLT_PORTS=(3307 3308 3309)  # Cascade: try each until one works
AGENT_MAIL_HEALTH="https://getunfocused.app/health/liveness"
PID_FILE="$HOME/.field-compass-tunnel.pid"

PASS="✓"
FAIL="✗"
WARN="⚠"
errors=0

log()  { echo "$@" >&2; }
pass() { log "$PASS $1"; }
fail() { log "$FAIL $1"; errors=$((errors + 1)); }
warn() { log "$WARN $1"; }

log ""
log "═══ Field Compass Pre-flight Check ═══"
log ""

# ─── Helper: check if a PID is a live process ─────────────────────────────
pid_alive() {
  local pid=$1
  # tasklist on Windows — returns 0 if process exists
  if tasklist //FI "PID eq $pid" 2>/dev/null | grep -q "$pid"; then
    return 0
  fi
  return 1
}

# ─── Helper: check if port is listening AND the PID is alive ──────────────
port_healthy() {
  local port=$1
  local pid
  pid=$(netstat -ano 2>/dev/null | grep "127.0.0.1:${port}" | grep -i "listen" | awk '{print $NF}' | head -1)

  if [ -z "$pid" ]; then
    return 1  # Port not listening at all
  fi

  if pid_alive "$pid"; then
    return 0  # Port listening with a live process — healthy
  else
    warn "Port $port held by zombie PID $pid (process dead, socket orphaned)"
    return 1
  fi
}

# ─── Helper: kill old tunnel from PID file ────────────────────────────────
cleanup_old_tunnel() {
  if [ -f "$PID_FILE" ]; then
    local old_pid
    old_pid=$(cat "$PID_FILE" 2>/dev/null || true)
    if [ -n "$old_pid" ] && pid_alive "$old_pid"; then
      log "  Killing previous tunnel (PID $old_pid)..."
      taskkill //F //PID "$old_pid" 2>/dev/null || kill "$old_pid" 2>/dev/null || true
      sleep 1
    fi
    rm -f "$PID_FILE"
  fi
}

# ─── 1. Dolt SSH Tunnel ─────────────────────────────────────────────────────
# Beads connects to Dolt via localhost. This requires an SSH tunnel
# forwarding to the Hetzner server where Dolt runs.

ACTIVE_PORT=""

# First, try to clean up any previous tunnel
cleanup_old_tunnel

# Try each port in the cascade
for port in "${DOLT_PORTS[@]}"; do
  if port_healthy "$port"; then
    ACTIVE_PORT=$port
    pass "Dolt SSH tunnel (port $port — existing healthy tunnel)"
    break
  fi

  # Port not healthy (either not listening or zombie). Try to start tunnel.
  # -o ExitOnForwardFailure=yes = fail immediately if port is already bound
  if ssh -o ExitOnForwardFailure=yes -fNL "${port}:127.0.0.1:3307" "$HETZNER_HOST" 2>/dev/null; then
    sleep 2
    if port_healthy "$port"; then
      # Save the PID for cleanup by next session
      local_pid=$(netstat -ano 2>/dev/null | grep "127.0.0.1:${port}" | grep -i "listen" | awk '{print $NF}' | head -1)
      echo "$local_pid" > "$PID_FILE"
      ACTIVE_PORT=$port
      pass "Dolt SSH tunnel (auto-started on port $port)"
      break
    fi
  fi

  # This port didn't work (zombie or bind failure), try next
  if [ "$port" != "${DOLT_PORTS[-1]}" ]; then
    warn "Port $port unavailable, trying next..."
  fi
done

if [ -z "$ACTIVE_PORT" ]; then
  fail "Dolt SSH tunnel — all ports (${DOLT_PORTS[*]}) failed"
  log "  Zombie ports may require system reboot to release."
  log "  Manual fix: ssh -fNL 3307:127.0.0.1:3307 ${HETZNER_HOST}"
fi

# ─── 2. Beads Connectivity ──────────────────────────────────────────────────
# Verify Beads can actually talk to Dolt through the tunnel.

if [ -n "$ACTIVE_PORT" ]; then
  export PATH="/c/Dev/flutter/bin:$PATH"
  export BD_DSN="root:@tcp(127.0.0.1:${ACTIVE_PORT})/beads_FC"

  if bd list --quiet 2>/dev/null | head -1 >/dev/null 2>&1; then
    pass "Beads (bd list via port $ACTIVE_PORT)"

    # ─── 2b. Pull latest Beads data from Dolt remote ─────────────
    # Prevents agents from starting with stale task state.
    # Warning only — stale data is better than a blocked session.
    #
    # Auto-commit metadata first: `dolt push` writes tracking data to
    # the metadata table outside SQL transactions, leaving dirty state
    # that blocks all future pulls. Commit it silently before pulling.
    ssh -o ConnectTimeout=5 "$HETZNER_HOST" \
      "docker exec dolt-beads dolt --data-dir=/var/lib/dolt sql -q \
       \"USE beads_FC; CALL dolt_add('metadata'); CALL dolt_commit('-m', 'chore: auto-commit metadata before pull', '--allow-empty');\"" \
      2>/dev/null || true

    if bd dolt pull 2>/dev/null; then
      pass "Beads data synced (bd dolt pull)"
    else
      warn "bd dolt pull failed — session may have stale Beads data"
      log "  This is a warning, not a blocker. Continuing."
    fi
  else
    # Tunnel is up but Beads can't connect — maybe Dolt server isn't running
    fail "Beads cannot reach Dolt (tunnel up but Dolt may not be running)"
    log "  Check Dolt on server: ssh ${HETZNER_HOST} \"docker ps | grep dolt\""
  fi
fi

# ─── 3. MCP Agent Mail ──────────────────────────────────────────────────────
# Agent Mail runs on Hetzner, accessed via HTTPS through Cloudflare/nginx.

if curl -s --connect-timeout 5 "$AGENT_MAIL_HEALTH" 2>/dev/null | grep -q "alive"; then
  pass "Agent Mail (${AGENT_MAIL_HEALTH})"
else
  fail "Agent Mail is unreachable"
  log "  Check: ssh ${HETZNER_HOST} \"cd /opt/mcp_agent_mail && docker compose -f docker-compose.prod.yml logs --tail 20\""
  log "  Restart: ssh ${HETZNER_HOST} \"cd /opt/mcp_agent_mail && docker compose -f docker-compose.prod.yml down && docker compose -f docker-compose.prod.yml up -d\""
fi

# ─── 4. Git Hooks Path ─────────────────────────────────────────────────────
# Ensure core.hookspath points to .claude/hooks/ (tracked, works in worktrees).
# .beads/hooks/ is gitignored and doesn't exist in worktrees → no hooks run.

HOOKS_PATH=$(cd /c/Dev/Field_Compass && git config core.hookspath 2>/dev/null || echo "")
if [ "$HOOKS_PATH" = ".claude/hooks" ]; then
  pass "Git hooks path (.claude/hooks — tracked, worktree-safe)"
else
  warn "Git hooks path is '${HOOKS_PATH:-<unset>}' — fixing to .claude/hooks"
  cd /c/Dev/Field_Compass && git config core.hookspath .claude/hooks
  pass "Git hooks path fixed to .claude/hooks"
fi

# ─── 5. Abandoned Worktree Detection ──────────────────────────────────────
# Scan for worktrees that might be leftovers from crashed agent sessions.
# Warning only — don't auto-delete (might have uncommitted work).

ABANDONED=()
while IFS= read -r wt_line; do
  wt_path=$(echo "$wt_line" | awk '{print $1}')
  wt_branch=$(echo "$wt_line" | awk '{print $2}' | tr -d '[]')

  # Skip bare repo and main worktree
  if [ "$wt_path" = "/c/Dev/Field_Compass" ] || [ "$wt_path" = "/c/Dev/Field_Compass-main" ]; then
    continue
  fi

  # Skip if it looks like a known pattern but check age
  if [ -d "$wt_path" ]; then
    # Check last commit time in the worktree
    LAST_COMMIT=$(cd "$wt_path" 2>/dev/null && git log -1 --format=%ct 2>/dev/null || echo "0")
    NOW=$(date +%s)
    AGE_HOURS=$(( (NOW - LAST_COMMIT) / 3600 ))

    if [ "$AGE_HOURS" -gt 24 ]; then
      ABANDONED+=("$wt_path (branch: $wt_branch, last commit: ${AGE_HOURS}h ago)")
    fi
  fi
done < <(cd /c/Dev/Field_Compass && git worktree list 2>/dev/null | grep -v "^$")

if [ ${#ABANDONED[@]} -gt 0 ]; then
  warn "Found ${#ABANDONED[@]} potentially abandoned worktree(s):"
  for wt in "${ABANDONED[@]}"; do
    log "    $wt"
  done
  log "  To clean up: git worktree remove <path> && git branch -d <branch>"
  log "  These may contain uncommitted work from crashed agent sessions."
fi

# ─── 6. Session State (Compaction Recovery) ──────────────────────────────────
# If an agent session is active, output its state. This output becomes a
# system reminder that survives context compaction, allowing the agent to
# recover its working context (current epic, task, branch, budget).

# Resolve script path relative to this file (works in any worktree)
PREFLIGHT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SESSION_STATE_SCRIPT="$PREFLIGHT_DIR/session-state.sh"
SESSION_STATE_FILE="/c/Dev/Field_Compass/.claude/agent-session.json"

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
    log "  If this is a new interactive session, ignore this — run"
    log "  'bash .claude/hooks/session-state.sh reset' to clear stale state."
    log ""
  fi
fi

# ─── Result ──────────────────────────────────────────────────────────────────

log ""
if [ "$errors" -gt 0 ]; then
  log "═══ BLOCKED: $errors check(s) failed ═══"
  log "Fix the issues above, then retry. Agents must not operate without"
  log "Beads (task coordination) and Agent Mail (file reservation)."
  log ""
  exit 2
fi

if [ -n "$ACTIVE_PORT" ]; then
  log "  Export: BD_DSN=root:@tcp(127.0.0.1:${ACTIVE_PORT})/beads_FC"
fi

log "═══ All checks passed — session ready ═══"
log ""
exit 0
