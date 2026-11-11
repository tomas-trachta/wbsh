#!/bin/bash
# `wbsh -c` runs the command without -r, as `$SHELL -c` callers expect; a
# dump flag without -r still only dumps. A file still needs -r (#291).

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

echo "=== -c runs"
"$WBSH" -c 'echo hi'
"$WBSH" -c 'echo "0=$0 1=$1 2=$2"' name one two
"$WBSH" -c 'exit 3'; echo "status $?"
"$WBSH" -r -c 'echo with -r'

echo "=== a dump flag without -r only dumps"
"$WBSH" --ast -c 'echo not run'
"$WBSH" -t -c 'echo not run'
"$WBSH" -e -c 'echo not run'

echo "=== -r with a dump flag dumps and runs"
"$WBSH" -r --ast -c 'echo ran'

echo "=== a file without -r is dumped, with -r it runs"
mkdir -p _run_tmp
echo 'echo from the file' > _run_tmp/cli_file.sh
"$WBSH" _run_tmp/cli_file.sh
"$WBSH" -r _run_tmp/cli_file.sh
rm -f _run_tmp/cli_file.sh
