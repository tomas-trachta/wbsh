#!/bin/bash
# Ending a long pipeline early: once `head` has what it needs, every stage
# before it stops in turn, however many builtin, function, subshell or
# external stages sit in between. Each case runs under timeout, so a
# regression shows as status 124 rather than a hang.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

run() {
	timeout 20 "$WBSH" -r -c "$1"
	echo "status $?"
}

echo "=== chains of builtin stages"
run 'yes | cat | cat | head -2'
run 'yes | cat | cat | cat | cat | head -1'
run 'yes | tr y n | cat | head -2'
run 'yes | sed s/y/z/ | grep z | cat | head -2'

echo "=== functions, groups and subshells in the chain"
run 'f() { cat; }; yes | f | cat | head -2'
run 'yes | { cat; } | cat | head -2'
run 'yes | (cat) | cat | head -2'
run 'yes | while read -r l; do echo "$l"; done | cat | head -2'

echo "=== the pipeline's status and PIPESTATUS"
run 'yes | cat | cat | head -1; echo "last ${PIPESTATUS[3]}"'
run 'set -o pipefail; yes | cat | head -1; echo "pipefail $?"'
