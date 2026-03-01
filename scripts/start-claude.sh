#!/usr/bin/env bash
# One-click launcher: Dolt + Beads + Agent Mail check + Claude Code
# Usage: ./scripts/start-claude.sh

set -e
PROJECT_DIR="/c/Dev/Field_Compass"
DOLT_DIR="$PROJECT_DIR/.beads/dolt"
DOLT_PORT=3307
DOLT_DB="beads_FC"
AGENT_NAME="fc-firmware-agent"

echo ""
echo "========================================"
echo "  Field Compass Dev Environment Startup"
echo "========================================"
echo ""

# ── Step 1: Local Dolt Server ─────────────────────
echo "[1/4] Checking local Dolt server on port $DOLT_PORT..."

DOLT_RUNNING=false
if nc -z 127.0.0.1 $DOLT_PORT 2>/dev/null; then
    DOLT_RUNNING=true
    echo "  -> Dolt already running on port $DOLT_PORT"
else
    echo "  -> Starting local Dolt SQL server..."

    if [ ! -d "$DOLT_DIR" ]; then
        echo "  X Dolt directory not found: $DOLT_DIR"
        echo "    Run 'bd init' first to set up the Beads database."
        exit 1
    fi

    cd "$DOLT_DIR" && dolt sql-server --port=$DOLT_PORT &
    cd "$PROJECT_DIR"

    # Wait for server to initialize
    sleep 3

    if nc -z 127.0.0.1 $DOLT_PORT 2>/dev/null; then
        DOLT_RUNNING=true
        echo "  -> Dolt server started successfully"
    else
        echo "  X Failed to start Dolt server!"
        echo "    Try manually: cd $DOLT_DIR && dolt sql-server --port=$DOLT_PORT"
        exit 1
    fi
fi

# ── Step 2: Verify Beads ────────────────────────
echo ""
echo "[2/4] Verifying Beads/Dolt connectivity..."

cd "$PROJECT_DIR"

if [ "$DOLT_RUNNING" = true ]; then
    if bd dolt test 2>/dev/null; then
        echo "  -> Beads connected to Dolt ($DOLT_DB)"
        bd stats 2>/dev/null || true
    else
        echo "  X Beads cannot reach Dolt"
        echo "    Check that $DOLT_DB database exists in $DOLT_DIR"
        exit 1
    fi
else
    echo "  ~ Skipped (Dolt not running)"
    exit 1
fi

# ── Step 3: Agent Mail Readiness ──────────────────
echo ""
echo "[3/4] Checking Agent Mail readiness..."

if bd ready 2>/dev/null; then
    echo "  -> Beads task queue operational"
else
    echo "  ! Beads 'bd ready' returned errors (non-fatal)"
fi

echo "  i Agent Mail MCP check happens inside Claude Code session"
echo "  i Agent name: $AGENT_NAME"

# ── Step 4: Launch Claude Code ──────────────────
echo ""
echo "[4/4] Launching Claude Code..."
echo "  Mode: --dangerously-skip-permissions"
echo "  Directory: $PROJECT_DIR"
echo "  Dolt: local ($DOLT_DIR) on port $DOLT_PORT"
echo ""
echo "========================================"
echo ""

claude --dangerously-skip-permissions
