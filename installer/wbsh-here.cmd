@echo off
rem Wrapper used by the "Open wbsh here" Explorer context menu.
rem Explorer passes the target folder as %1; we chdir to it and launch wbsh
rem in the same console (so closing wbsh closes the cmd host -- same UX as
rem the built-in "Open in Terminal" verb).
if not "%~1"=="" cd /d "%~1"
"%~dp0wbsh.exe"
