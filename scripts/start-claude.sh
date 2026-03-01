#!/usr/bin/env bash
# =============================================================================
# Field Compass — One-click CLI Agent Startup
# =============================================================================
# Usage: ./scripts/start-claude.sh
#
# Steps:
#   1. Git: checkout main, pull latest, clean up old branches
#   2. SSH tunnel to Hetzner Dolt server (port 3307)
#   3. Beads/Dolt connectivity
#   4. Agent Mail health
#   5. Launch Claude Code with startup protocol
#
# If any infrastructure check fails, launch is BLOCKED.
# =============================================================================

set -eo pipefail

PROJECT_DIR="/c/Dev/Field_Compass"
HETZNER_HOST="unfocused@46.224.181.82"
DOLT_PORT=3307
AGENT_MAIL_HEALTH="https://getunfocused.app/health/liveness"
ERRORS=0

pass() { echo "  ✓ $1"; }
fail() { echo "  ✗ $1"; ERRORS=$((ERRORS + 1)); }
info() { echo "  $1"; }
warn() { echo "  ! $1"; }

echo ""
echo "========================================"
echo "  Field Compass CLI Agent Startup"
echo "========================================"
echo ""

cd "$PROJECT_DIR"

# ── Step 1: Git ───────────────────────────────────────────────────
echo "[1/5] Git: checkout main, pull latest..."

dirty=$(git status --porcelain 2>/dev/null || true)
if [ -n "$dirty" ]; then
    warn "Uncommitted changes found, stashing"
    git stash push -m "auto-stash $(date +%Y-%m-%d-%H%M%S)" 2>/dev/null || true
fi

current=$(git branch --show-current 2>/dev/null || echo "unknown")
if [ "$current" != "main" ]; then
    if git checkout main 2>/dev/null; then
        pass "Switched to main (was on $current)"
    else
        warn "Could not checkout main"
    fi
else
    pass "Already on main"
fi

pull_out=$(git pull origin main 2>&1 || true)
if echo "$pull_out" | grep -q "Already up to date"; then
    pass "Already up to date"
else
    pass "Pulled latest changes"
fi

git fetch --prune 2>/dev/null || true
merged=$(git branch --merged main 2>/dev/null | grep -v "main" | grep -v "^\*" || true)
if [ -n "$merged" ]; then
    count=$(echo "$merged" | wc -l | tr -d ' ')
    echo "$merged" | while read -r b; do
        b=$(echo "$b" | xargs)
        [ -n "$b" ] && git branch -d "$b" 2>/dev/null || true
    done
    info "Cleaned up $count merged branch(es)"
fi

# ── Step 2: SSH Tunnel ────────────────────────────────────────────
echo ""
echo "[2/5] Dolt SSH tunnel (port $DOLT_PORT)..."

tunnel_running() {
    netstat -an 2>/dev/null | grep "$DOLT_PORT" | grep -qi "listen"
}

if tunnel_running; then
    pass "Tunnel already active"
else
    info "Tunnel not running. Starting..."
    if ssh -fNL ${DOLT_PORT}:127.0.0.1:${DOLT_PORT} "$HETZNER_HOST" 2>/dev/null; then
        sleep 3
        if tunnel_running; then
            pass "Tunnel auto-started"
        else
            fail "Could not establish tunnel"
            info "Run manually: ssh -fNL $DOLT_PORT:127.0.0.1:$DOLT_PORT $HETZNER_HOST"
        fi
    else
        fail "Could not start SSH process"
        info "Run manually: ssh -fNL $DOLT_PORT:127.0.0.1:$DOLT_PORT $HETZNER_HOST"
    fi
fi

# ── Step 3: Beads / Dolt ──────────────────────────────────────────
echo ""
echo "[3/5] Beads/Dolt connectivity..."

if tunnel_running; then
    if bd dolt test 2>/dev/null; then
        pass "Beads connected to Dolt (beads_FC)"
        bd stats 2>/dev/null || true
    else
        fail "Beads cannot reach Dolt"
    fi
else
    fail "Skipped (no tunnel)"
fi

# ── Step 4: Agent Mail ────────────────────────────────────────────
echo ""
echo "[4/5] Agent Mail..."

if curl -s --connect-timeout 5 "$AGENT_MAIL_HEALTH" 2>/dev/null | grep -q "alive"; then
    pass "Agent Mail is healthy"
else
    fail "Agent Mail is unreachable"
fi

# ── Step 5: Launch or Block ───────────────────────────────────────
echo ""

if [ "$ERRORS" -gt 0 ]; then
    echo "========================================"
    echo "  BLOCKED: $ERRORS check(s) failed"
    echo "========================================"
    echo ""
    echo "Fix the issues above, then re-run this script."
    exit 1
fi

echo "[5/5] Launching Claude Code..."
echo "  Mode: --dangerously-skip-permissions"
echo ""
echo "========================================"
echo ""

claude --dangerously-skip-permissions "You are starting a new CLI agent session for the Field Compass project. Complete this startup protocol NOW, in order, before doing anything else:

1. **Register with Agent Mail:**
   - ensure_project(human_key=\"C:\\dev\\Field_Compass\")
   - register_agent(project_key=\"C:\\dev\\Field_Compass\", program=\"claude-code\", model=\"claude-sonnet-4-20250514\")

2. **Check inbox** for coordination messages from other agents:
   - fetch_inbox(project_key=\"C:\\dev\\Field_Compass\", agent_name=\"<your-name>\", include_bodies=true)

3. **Sync Beads:** Run \`bd dolt pull\` then \`bd ready\` to see available work.

4. **Check for in-progress claims:** Run \`bd list --status=in_progress\` — if another agent already claimed a task, pick a different one.

5. **Claim exactly ONE task:** Run \`bd update <id> --status=in_progress\` on your chosen task.

6. **Reserve files** via Agent Mail before editing anything: file_reservation_paths with specific paths/globs.

7. **Report ready:** Tell me which task you claimed, which files you reserved. Then begin work."
