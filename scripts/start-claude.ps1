<#
.SYNOPSIS
    One-click startup for Field Compass CLI agent sessions.
    You run this script. Nothing else. It handles everything.

.DESCRIPTION
    What this script does (in order):
      1. Git: checkout main, pull latest, clean up old branches
      2. SSH tunnel to Hetzner Dolt server (port 3307)
      3. Beads/Dolt connectivity
      4. Agent Mail health
      5. Launches Claude Code with startup protocol

    If any infrastructure check fails, launch is BLOCKED.

.USAGE
    Double-click start-claude.bat
    Or from terminal: .\scripts\start-claude.ps1
#>

$ErrorActionPreference = "Continue"
$ProjectDir = "C:\Dev\Field_Compass"
$HetznerHost = "unfocused@46.224.181.82"
$LocalPort = 3307
$RemotePort = 3307
$AgentMailHealth = "https://getunfocused.app/health/liveness"
$Errors = 0

function Write-Pass { param([string]$msg) Write-Host "  $([char]0x2713) $msg" -ForegroundColor Green }
function Write-Fail { param([string]$msg) $script:Errors++; Write-Host "  $([char]0x2717) $msg" -ForegroundColor Red }
function Write-Info { param([string]$msg) Write-Host "  $msg" -ForegroundColor Gray }
function Write-Warn { param([string]$msg) Write-Host "  ! $msg" -ForegroundColor DarkYellow }

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Field Compass CLI Agent Startup" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# ── Always start in the project directory ─────────────────────────
Set-Location $ProjectDir

# ── Step 1: Git ───────────────────────────────────────────────────
Write-Host "[1/5] Git: checkout main, pull latest..." -ForegroundColor Yellow

try {
    # Stash uncommitted work
    $dirty = git status --porcelain 2>$null
    if ($dirty) {
        Write-Warn "Uncommitted changes found, stashing"
        git stash push -m "auto-stash $(Get-Date -Format 'yyyy-MM-dd-HHmmss')" 2>$null | Out-Null
    }

    # Switch to main
    $currentBranch = git branch --show-current 2>$null
    if ($currentBranch -ne "main") {
        git checkout main 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Pass "Switched to main (was on $currentBranch)"
        } else {
            Write-Warn "Could not checkout main"
        }
    } else {
        Write-Pass "Already on main"
    }

    # Pull latest
    $pullOut = git pull origin main 2>&1 | Out-String
    if ($pullOut -match "Already up to date") {
        Write-Pass "Already up to date"
    } elseif ($LASTEXITCODE -eq 0) {
        Write-Pass "Pulled latest changes"
    } else {
        Write-Warn "Git pull had issues, continuing"
    }

    # Clean up merged branches
    git fetch --prune 2>$null | Out-Null
    $merged = git branch --merged main 2>$null | Where-Object { $_ -notmatch "main" -and $_ -notmatch "^\*" }
    if ($merged) {
        $count = @($merged).Count
        foreach ($branch in $merged) {
            $b = $branch.Trim()
            if ($b) { git branch -d $b 2>$null | Out-Null }
        }
        Write-Info "Cleaned up $count merged branch(es)"
    }
} catch {
    Write-Warn "Git setup had issues, continuing"
}

# ── Step 2: SSH Tunnel ────────────────────────────────────────────
Write-Host ""
Write-Host "[2/5] Dolt SSH tunnel (port $LocalPort)..." -ForegroundColor Yellow

$tunnelActive = $false
try {
    $conn = New-Object System.Net.Sockets.TcpClient
    $conn.Connect("127.0.0.1", $LocalPort)
    $conn.Close()
    $tunnelActive = $true
    Write-Pass "Tunnel already active"
} catch {
    Write-Info "Tunnel not running. Starting..."

    try {
        $gitSsh = "C:\Program Files\Git\usr\bin\ssh.exe"
        if (Test-Path $gitSsh) {
            $sshArgs = "-fNL ${LocalPort}:127.0.0.1:${RemotePort} $HetznerHost"
            Start-Process -FilePath $gitSsh -ArgumentList $sshArgs -WindowStyle Hidden
        } else {
            $sshArgs = "-NL ${LocalPort}:127.0.0.1:${RemotePort} $HetznerHost"
            Start-Process -FilePath "ssh" -ArgumentList $sshArgs -WindowStyle Hidden
        }

        Start-Sleep -Seconds 5

        try {
            $conn = New-Object System.Net.Sockets.TcpClient
            $conn.Connect("127.0.0.1", $LocalPort)
            $conn.Close()
            $tunnelActive = $true
            Write-Pass "Tunnel auto-started"
        } catch {
            Write-Fail "Could not establish tunnel"
            Write-Info "Run manually: ssh -fNL 3307:127.0.0.1:3307 $HetznerHost"
        }
    } catch {
        Write-Fail "Could not start SSH process"
        Write-Info "Run manually: ssh -fNL 3307:127.0.0.1:3307 $HetznerHost"
    }
}

# ── Step 3: Beads / Dolt ──────────────────────────────────────────
Write-Host ""
Write-Host "[3/5] Beads/Dolt connectivity..." -ForegroundColor Yellow

if ($tunnelActive) {
    $bdOut = bd dolt test 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
        Write-Pass "Beads connected to Dolt (beads_FC)"
        $stats = bd stats 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0) { Write-Info $stats.Trim() }
    } else {
        Write-Fail "Beads cannot reach Dolt"
    }
} else {
    Write-Fail "Skipped (no tunnel)"
}

# ── Step 4: Agent Mail ────────────────────────────────────────────
Write-Host ""
Write-Host "[4/5] Agent Mail..." -ForegroundColor Yellow

try {
    $response = Invoke-WebRequest -Uri $AgentMailHealth -TimeoutSec 5 -UseBasicParsing -ErrorAction Stop
    if ($response.Content -match "alive") {
        Write-Pass "Agent Mail is healthy"
    } else {
        Write-Fail "Agent Mail responded but not healthy"
    }
} catch {
    Write-Fail "Agent Mail is unreachable"
}

# ── Step 5: Launch or Block ───────────────────────────────────────
Write-Host ""

if ($Errors -gt 0) {
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "  BLOCKED: $Errors check(s) failed" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    Write-Host ""
    Write-Host "Fix the issues above, then re-run this script." -ForegroundColor Yellow
    Write-Host ""
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "[5/5] Launching Claude Code..." -ForegroundColor Yellow
Write-Host "  Mode: --dangerously-skip-permissions" -ForegroundColor DarkYellow
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$StartupMessage = @"
You are starting a new CLI agent session for the Field Compass project. Complete this startup protocol NOW, in order, before doing anything else:

1. **Register with Agent Mail:**
   - ensure_project(human_key=<working directory path>)
   - register_agent(project_key=<working directory path>, program="claude-code", model=<your model id>)

2. **Check inbox** for coordination messages from other agents:
   - fetch_inbox(project_key=<working directory path>, agent_name=<your-name>, include_bodies=true)

3. **Sync Beads:** Run ``bd dolt pull`` then ``bd ready`` to see available work.

4. **Check for in-progress claims:** Run ``bd list --status=in_progress`` — if another agent already claimed a task, pick a different one.

5. **Claim exactly ONE task:** Run ``bd update <id> --status=in_progress`` on your chosen task.

6. **Reserve files** via Agent Mail before editing anything: file_reservation_paths with specific paths/globs.

7. **Report ready:** Tell me which task you claimed, which files you reserved. Then begin work.

8. **Continuous Work Loop:** After completing each task, run the Between-Task Reset Protocol (see CLAUDE.md), then immediately check ``bd ready`` and pick the next task. Do NOT stop and ask — keep working until no tasks remain.
"@

claude --dangerously-skip-permissions $StartupMessage
