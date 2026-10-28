#!/bin/bash
# Probe: commands sharing one stdin take only what they consume (bash 5.2 semantics).

mkdir -p _run_tmp
printf 'l1\nl2\nl3\n' > _run_tmp/three.txt
printf 'o1\no2\n' > _run_tmp/other.txt

echo "=== 1. read then an external command"
{ read a; findstr '^' | tr -d '\r'; } < _run_tmp/three.txt
printf 'p1\np2\np3\n' | { read a; cat; }

echo "=== 2. a redirection does not see an old read-ahead"
{ read a; read b; } < _run_tmp/three.txt; read x < _run_tmp/three.txt; echo "$x"
{ read a; read b < _run_tmp/other.txt; read c; echo "$a $b $c"; } < _run_tmp/three.txt
{ head -c 1 >/dev/null; cat < _run_tmp/other.txt; cat; } < _run_tmp/three.txt

echo "=== 3. buffered builtins hand back what they did not use"
{ head -n 1; read y; echo "y=$y"; } < _run_tmp/three.txt
{ head -n 1 >/dev/null; findstr '^' | tr -d '\r'; } < _run_tmp/three.txt
{ mapfile -n 1 -t m; read z; echo "m=${m[0]} z=$z"; } < _run_tmp/three.txt
{ mapfile -n 1 -t m; findstr '^' | tr -d '\r'; } < _run_tmp/three.txt
{ cat > /dev/null; read q; echo "rc=$? [$q]"; } < _run_tmp/three.txt

echo "=== 4. many reads from one file"
for i in $(seq 1 2000); do echo "line number $i"; done > _run_tmp/big.txt
n=0; while read -r l; do n=$((n+1)); done < _run_tmp/big.txt; echo "n=$n"
{ read -r first; tail -n 1; } < _run_tmp/big.txt
