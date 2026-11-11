#!/bin/bash
# The Windows environment variables that Git Bash scripts use by their
# upper-case names. Windows stores them as SystemRoot, windir, ComSpec,
# ProgramFiles; Git Bash (MSYS) gives them to scripts upper-case, so scripts
# written for it say $SYSTEMROOT, $WINDIR, $COMSPEC, $PROGRAMFILES.
# The driver starts wbsh from PowerShell, so it sees the native names.
. "$STRESS_ROOT/lib/check.sh"

severity medium
for name in SYSTEMROOT WINDIR COMSPEC PROGRAMFILES PROGRAMDATA APPDATA LOCALAPPDATA USERPROFILE TEMP TMP; do
    value=$("$STRESS_WBSH" -r -c "printf '%s' \"\$$name\"" < /dev/null)
    check "\$$name is set, as in Git Bash" [ -n "$value" ]
done
value=$("$STRESS_WBSH" -r -c 'printf "%s" "$SystemRoot"' < /dev/null)
check "\$SystemRoot (native name) is still set" [ -n "$value" ]
finish
