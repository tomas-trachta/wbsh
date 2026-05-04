#!/bin/bash
# Verify path translation when invoking native Win32 executables.

# cmd.exe is a native Win32 binary. Args containing POSIX paths should be
# translated for it.
cmd /c "type \"$(echo /c/Windows/win.ini)\"" 2>&1 | head -3
echo "---"
cmd /c "echo cwd=%CD%"
echo "---"
# where.exe is also native; should resolve correctly.
where notepad 2>&1 | head -1
