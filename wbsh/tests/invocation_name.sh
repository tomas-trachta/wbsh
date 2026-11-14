#!/bin/bash
# A copy of the shell names itself after argv[0] without .exe, in $0 and in
# its error prefix, as bash does: run as bash.exe it is `bash`.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
mkdir -p _run_tmp/invname
cp "$WBSH" _run_tmp/invname/bash.exe
cp "$WBSH" _run_tmp/invname/sh.exe

_run_tmp/invname/bash.exe -c 'echo "0=$0"; nosuch_command_xyz' 2>&1
_run_tmp/invname/sh.exe -c 'echo "0=$0"; shopt -po posix' 2>&1
_run_tmp/invname/bash.exe -c 'echo "named 0=$0"' given
echo 'echo "stdin 0=$0"' | _run_tmp/invname/bash.exe
_run_tmp/invname/bash.exe _run_tmp/invname/no_such_script.sh 2>&1 | sed 's#: _run_tmp/invname/#: #'

rm -rf _run_tmp/invname
