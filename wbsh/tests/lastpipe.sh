#!/bin/bash
# Probe: shopt -s lastpipe runs the last pipeline element in this shell.

echo "=== 1. off: the last element is a subshell"
echo value | read -r v; echo "[$v]"

echo "=== 2. on: read, a loop and a group set variables here"
shopt -s lastpipe
echo value | read -r v; echo "[$v]"
printf '%s\n' a b c | while read -r line; do n=$((n + 1)); done; echo "n=$n"
printf 'x\ny\n' | { read -r a; read -r b; }; echo "a=$a b=$b"

echo "=== 3. statuses"
echo q | false; echo "rc=$? pipestatus=${PIPESTATUS[*]}"
false | true; echo "pipestatus=${PIPESTATUS[*]}"
echo q | true | read -r q; echo "q=[$q] pipestatus=${PIPESTATUS[*]}"

echo "=== 4. a reader that stops early, and stdin afterwards"
printf '%s\n' 1 2 3 | read -r first; echo "first=$first"
read -r after <<< restored; echo "after=$after"

echo "=== 5. a subshell as the last element is still one"
echo z | ( read -r z; echo "inner z=$z" ); echo "outer z=[$z]"

echo "=== 6. inside a function"
f() { echo fv | read -r fv; echo "fv=$fv"; }; f
