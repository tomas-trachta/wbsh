#!/bin/bash
# Probe: `read` field splitting, EOF handling and the -n -N -d -t -s -u options (bash 5.2 semantics).

echo "=== 1. IFS= keeps the line, the last name keeps the raw rest"
printf '  a  b  \n' | { IFS= read -r l; echo "[$l]"; }
printf 'a b c\n' | { IFS= read x y; echo "[$x][$y]"; }
printf 'a:b:c\n' | { IFS=: read x y; echo "[$y]"; }
printf 'a:b:\n' | { IFS=: read x y; echo "[$y]"; }
printf 'a:b::\n' | { IFS=: read x y; echo "[$y]"; }
printf 'a:b: \n' | { IFS=': ' read x y; echo "[$y]"; }
printf ' a  b  c  \n' | { read x y; echo "[$x][$y]"; }
printf 'k=v=w\n' | { IFS='=' read -r k v; echo "[$k][$v]"; }
printf 'a::b:\n' | { IFS=: read -a arr; echo "${#arr[@]} [${arr[1]}] [${arr[2]}]"; }

echo "=== 2. REPLY and backslashes"
printf ' a  b  c  \n' | { read; echo "[$REPLY]"; }
printf 'x\\ y z\n' | { read a b; echo "[$a][$b]"; }
printf 'x\\\ny z\n' | { read a b; echo "[$a][$b]"; }
printf '\\a\\:b:c\n' | { IFS=: read x y; echo "[$x][$y]"; }
printf 'x\\ y z\n' | { read -r a b; echo "[$a][$b]"; }

echo "=== 3. EOF clears the variables and fails"
printf 'last' | { read -r l; echo "rc=$? [$l]"; }
printf 'one\n' | { read -r l; read -r l; echo "rc=$? [$l]"; }
printf 'line1\nline2' | { n=0; while read -r l || [[ -n $l ]]; do n=$((n+1)); done; echo "n=$n"; }

echo "=== 4. -n / -N / -d"
read -n 1 c <<< abc; echo "[$c]"
read -n 2 a b <<< "x yz"; echo "[$a][$b]"
read -N 4 c <<< "ab
cd"; echo "[$c]"
read -N 3 a b <<< "x y z"; echo "[$a][$b]"
read -d : f <<< "one:two"; echo "rc=$? [$f]"
printf 'a\0b' | { read -r -d '' z; echo "rc=$? [$z]"; }
read -n x c <<< abc 2>&1; echo "rc=$?"

echo "=== 5. -u fd, -p, -s"
printf 'p q\n' | { read -u 0 m n; echo "[$m][$n]"; }
exec 3<<< "from three"; read -u 3 t; echo "[$t]"; exec 3<&-
read -u 9 x 2>&1; echo "rc=$?"
printf 'a b\n' | { read -p 'prompt> ' x 2>&1; echo "[$x]"; }
printf 'secret\n' | { read -s pw; echo "[$pw]"; }

echo "=== 6. -t"
read -t 0 <<< "data"; echo "t0 rc=$?"
(sleep 1; echo late) | { read -t 0.2 x; echo "timeout rc=$? [$x]"; }
(sleep 1; echo late) | { read -t 0 x; echo "t0 pipe rc=$?"; }
printf 'ab' | { read -t 1 x; echo "rc=$? [$x]"; }
read -t abc x <<< a 2>&1; echo "rc=$?"

echo "=== 7. bad options and names"
read -z 2>&1; echo "rc=$?"
read 1x <<< a 2>&1; echo "rc=$?"
read -a 2>&1; echo "rc=$?"
