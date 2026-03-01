@echo off
REM One-click launcher: Dolt + Beads + Agent Mail check + Claude Code
REM Double-click this file or run from any terminal.
cd /d C:\Dev\Field_Compass
powershell -ExecutionPolicy Bypass -File "%~dp0start-claude.ps1"
pause
