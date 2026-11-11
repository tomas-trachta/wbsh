#!/bin/bash
# Deep recursion stops with bash's FUNCNEST error before the stack runs
# out, instead of the shell crashing without a word. A script exits with
# status 1 as bash does; FUNCNEST sets a lower limit.

WBSH_EXE=../../x64/Release/wbsh.exe
[ -x "$WBSH_EXE" ] || WBSH_EXE=../../x64/Debug/wbsh.exe

echo "=== FUNCNEST"
"$WBSH_EXE" -r -c 'f() { f; }; FUNCNEST=50; f; echo not reached' 2>&1
echo "status $?"
"$WBSH_EXE" -r -c 'g() { g; g; }; FUNCNEST=20; g; echo not reached' 2>&1
echo "status $?"

echo "=== endless recursion stops cleanly"
"$WBSH_EXE" -r -c 'h() { h; }; h; echo not reached' 2>&1
echo "status $?"

echo "=== a bounded recursion within the limit still works"
"$WBSH_EXE" -r -c 'f() { (( $1 > 0 )) && f $(( $1 - 1 )) || echo "bottom at $1"; }; f 1000'
echo "status $?"
