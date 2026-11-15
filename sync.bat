@echo off
rem CT1 Skill auto-sync: commit all non-ignored changes and push to origin/main.
rem Runs via the scheduled task "CT1Skill AutoSync" (hourly) and manually on double-click.
cd /d "%~dp0"
git add -A
git diff --cached --quiet
if %errorlevel%==0 goto push
git commit -m "Auto-sync %date% %time%"
:push
git push origin main
if %errorlevel%==0 (
    echo Sync OK.
) else (
    echo Push failed - will retry on the next scheduled run.
    exit /b 1
)
