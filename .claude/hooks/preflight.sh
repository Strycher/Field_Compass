#!/usr/bin/env bash
# =============================================================================
# Field Compass — CLI Agent Pre-flight Check
# =============================================================================
# Runs on SessionStart. Blocks the session (exit 2) if required coordination
# services are unavailable. Auto-starts the Dolt SSH tunnel if it's down.
#
# Required services:
#   1. Dolt SSH tunnel (localhost:3307 → Hetzner:3307) — for Beads task tracking
#   2. MCP Agent Mail (getunfocused.app/mcp/) — for multi-agent coordination
#
# Exit codes:
#   0 = all checks passed, session may proceed
#   2 = BLOCKING — required service unavailable, session must not start
# =============================================================================

set -euo pipefail

HETZNER_HOST="unfocused@46.224.181.82"
DOLT_LOCAL_PORT=3307
AGENT_MAIL_HEALTH="https://getunfocused.app/health/liveness"

PASS="✓"
FAIL="✗"
errors=0

log()  { echo "$@" >&2; }
pass() { log "$PASS $1"; }
fail() { log "$FAIL $1"; errors=$((errors + 1)); }

log ""
log "═══ Field Compass Pre-flight Check ═══"
log ""

# ─── 1. Dolt SSH Tunnel ─────────────────────────────────────────────────────
# Beads connects to Dolt via localhost:3307. This requires an SSH tunnel
# forwarding to the Hetzner server where Dolt runs.

tunnel_running() {
  netstat -an 2>/dev/null | grep "$DOLT_LOCAL_PORT" | grep -qi "listen"
}

if tunnel_running; then
  pass "Dolt SSH tunnel (port $DOLT_LOCAL_PORT)"
else
  log "  Dolt SSH tunnel not running. Starting..."
  # -f = background after auth, -N = no remote command, -L = local forward
  if ssh -fNL ${DOLT_LOCAL_PORT}:127.0.0.1:${DOLT_LOCAL_PORT} "$HETZNER_HOST" 2>/dev/null; then
    sleep 2  # Give tunnel time to establish
    if tunnel_running; then
      pass "Dolt SSH tunnel (auto-started on port $DOLT_LOCAL_PORT)"
    else
      fail "Dolt SSH tunnel — auto-start succeeded but port not listening"
      log "  Manual fix: ssh -fNL ${DOLT_LOCAL_PORT}:127.0.0.1:${DOLT_LOCAL_PORT} ${HETZNER_HOST}"
    fi
  else
    fail "Dolt SSH tunnel — could not auto-start"
    log "  Manual fix: ssh -fNL ${DOLT_LOCAL_PORT}:127.0.0.1:${DOLT_LOCAL_PORT} ${HETZNER_HOST}"
  fi
fi

# ─── 2. Beads Connectivity ──────────────────────────────────────────────────
# Verify Beads can actually talk to Dolt through the tunnel.

if tunnel_running; then
  if bd list --quiet 2>/dev/null | head -1 >/dev/null 2>&1; then
    pass "Beads (bd list)"
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

# ─── Result ──────────────────────────────────────────────────────────────────

log ""
if [ "$errors" -gt 0 ]; then
  log "═══ BLOCKED: $errors check(s) failed ═══"
  log "Fix the issues above, then retry. Agents must not operate without"
  log "Beads (task coordination) and Agent Mail (file reservation)."
  log ""
  exit 2
fi

log "═══ All checks passed — session ready ═══"
log ""
exit 0
