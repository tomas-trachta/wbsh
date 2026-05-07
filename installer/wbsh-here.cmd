@echo off
rem Wrapper used by the "Open wbsh here" Explorer context menu.
rem Explorer passes the target folder as %1; we chdir into it and then
rem launch wbsh, preferring Windows Terminal when it's available so the
rem user lands in the bundled `wbsh` profile (tabs, GPU rendering, the
rem custom colour scheme). Falls back to a plain in-place launch on
rem stock Windows installs without WT.
if not "%~1"=="" cd /d "%~1"

where wt.exe >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    start "" wt.exe -p wbsh -d "%CD%"
) else (
    "%~dp0wbsh.exe"
)
