@echo off
rem Wrapper used by the "Open wbshterm here" Explorer context menu.
rem Explorer passes the target folder as %1; we chdir into it so the
rem terminal's shell starts there, then launch the window detached so
rem this console host closes immediately.
if not "%~1"=="" cd /d "%~1"

start "" "%~dp0wbshterm.exe"
