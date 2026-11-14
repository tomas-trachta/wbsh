#!/bin/bash
# `. /dev/stdin` reads the shell's standard input, not the console.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

echo 'echo three' | "$WBSH" -c '. /dev/stdin'
printf 'echo "args: $*"\n' | "$WBSH" -c 'source /dev/stdin a b'
