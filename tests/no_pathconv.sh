#!/bin/bash
# Verify MSYS_NO_PATHCONV / WBSH_NO_PATHCONV suppress argv translation.
#
# This is the de-facto MSYS convention that cross-shell scripts rely on
# (e.g. `MSYS_NO_PATHCONV=1 docker run -v /c/foo:/bar ...`, where wbsh
# rewriting the volume spec to `C:\foo:\bar` would silently break the
# container mount). Without this, scripts that work in Git Bash fail
# only when run under wbsh.
#
# `cmd.exe` is a native Win32 binary, so wbsh translates POSIX-looking
# args before invoking it. We round-trip a single arg through `echo`
# (a cmd.exe builtin) to see what cmd actually received.

echo "=== default: /c/Windows is translated"
cmd /c echo /c/Windows

echo "=== prefix MSYS_NO_PATHCONV=1: arg passes through"
MSYS_NO_PATHCONV=1 cmd /c echo /c/Windows

echo "=== prefix WBSH_NO_PATHCONV=1: arg passes through"
WBSH_NO_PATHCONV=1 cmd /c echo /c/Windows

echo "=== exported MSYS_NO_PATHCONV=1 then unset for one cmd: translated again"
export MSYS_NO_PATHCONV=1
MSYS_NO_PATHCONV= cmd /c echo /c/Windows
unset MSYS_NO_PATHCONV

echo "=== done"
