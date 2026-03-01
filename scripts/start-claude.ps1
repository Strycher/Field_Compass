<#
.SYNOPSIS
    One-click startup for Field Compass dev environment.
    Starts local Dolt server, verifies Beads, checks Agent Mail, launches Claude Code.

.DESCRIPTION
    1. Checks/starts local Dolt SQL server on port 3307
    2. Verifies Beads can connect to Dolt
    3. Checks Agent Mail readiness (reminder for MCP check)
    4. Launches Claude Code with --dangerously-skip-permissions

.USAGE
    Right-click > Run with PowerShell
    Or from terminal: .\scripts\start-claude.ps1
#>

$ErrorActionPreference = "Continue"
$ProjectDir = "C:\Dev\Field_Compass"
$DoltDir = "$ProjectDir\.beads\dolt"
$DoltPort = 3307
$DoltDB = "beads_FC"
$AgentName = "fc-firmware-agent"

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Field Compass Dev Environment Startup" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# ── Step 1: Local Dolt Server ─────────────────────
Write-Host "[1/4] Checking local Dolt server on port $DoltPort..." -ForegroundColor Yellow

$doltRunning = $false
try {
    $conn = New-Object System.Net.Sockets.TcpClient
    $conn.Connect("127.0.0.1", $DoltPort)
    $conn.Close()
    $doltRunning = $true
    Write-Host "  -> Dolt already running on port $DoltPort" -ForegroundColor Green
} catch {
    Write-Host "  -> Starting local Dolt SQL server..." -ForegroundColor Gray

    if (-not (Test-Path $DoltDir)) {
        Write-Host "  X Dolt directory not found: $DoltDir" -ForegroundColor Red
        Write-Host "    Run 'bd init' first to set up the Beads database." -ForegroundColor Gray
        exit 1
    }

    Start-Process -FilePath "dolt" `
        -ArgumentList "sql-server","--port=$DoltPort" `
        -WorkingDirectory $DoltDir `
        -WindowStyle Hidden

    # Wait for server to initialize
    Start-Sleep -Seconds 3

    try {
        $conn = New-Object System.Net.Sockets.TcpClient
        $conn.Connect("127.0.0.1", $DoltPort)
        $conn.Close()
        $doltRunning = $true
        Write-Host "  -> Dolt server started successfully" -ForegroundColor Green
    } catch {
        Write-Host "  X Failed to start Dolt server!" -ForegroundColor Red
        Write-Host "    Try manually:" -ForegroundColor Gray
        Write-Host "    cd $DoltDir && dolt sql-server --port=$DoltPort" -ForegroundColor Gray
        exit 1
    }
}

# ── Step 2: Verify Beads ────────────────────────
Write-Host ""
Write-Host "[2/4] Verifying Beads/Dolt connectivity..." -ForegroundColor Yellow

Set-Location $ProjectDir

if ($doltRunning) {
    $bdTest = & bd dolt test 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  -> Beads connected to Dolt ($DoltDB)" -ForegroundColor Green

        # Show quick stats
        $stats = & bd stats 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  $stats" -ForegroundColor Gray
        }
    } else {
        Write-Host "  X Beads cannot reach Dolt" -ForegroundColor Red
        Write-Host "    $bdTest" -ForegroundColor Gray
        Write-Host "    Check that $DoltDB database exists in $DoltDir" -ForegroundColor Gray
        exit 1
    }
} else {
    Write-Host "  ~ Skipped (Dolt not running)" -ForegroundColor DarkYellow
    exit 1
}

# ── Step 3: Agent Mail Readiness ──────────────────
Write-Host ""
Write-Host "[3/4] Checking Agent Mail readiness..." -ForegroundColor Yellow

# Verify beads is functional with a quick command
$bdReady = & bd ready 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "  -> Beads task queue operational" -ForegroundColor Green
} else {
    Write-Host "  ! Beads 'bd ready' returned errors (non-fatal)" -ForegroundColor DarkYellow
    Write-Host "    $bdReady" -ForegroundColor Gray
}

Write-Host "  i Agent Mail MCP check happens inside Claude Code session" -ForegroundColor Gray
Write-Host "  i Agent name: $AgentName" -ForegroundColor Gray

# ── Step 4: Launch Claude Code ──────────────────
Write-Host ""
Write-Host "[4/4] Launching Claude Code..." -ForegroundColor Yellow
Write-Host "  Mode: --dangerously-skip-permissions" -ForegroundColor DarkYellow
Write-Host "  Directory: $ProjectDir" -ForegroundColor Gray
Write-Host "  Dolt: local ($DoltDir) on port $DoltPort" -ForegroundColor Gray
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Launch Claude Code in dangerous mode
claude --dangerously-skip-permissions
