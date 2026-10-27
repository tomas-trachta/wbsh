#!/bin/bash
# Probe: $(...) and <(...) run as subshells, so nothing they change reaches the parent (bash 5.2 semantics).

start=$PWD

echo "=== 1. variables and cwd"
x=$(y=5; cd ..; echo hi); echo "x=$x y=[$y] same-dir=$([ "$PWD" = "$start" ] && echo yes)"
v=keep; x=$(v=changed; unset v); echo "v=$v"
x=$(OLDPWD=gone; cd /); echo "back in start: $([ "$PWD" = "$start" ] && echo yes)"

echo "=== 2. functions"
x=$(f() { :; }); type f >/dev/null 2>&1 && echo "f leaked" || echo "f stays inside"
g() { echo outer; }; x=$(g() { echo inner; }; g); echo "x=$x now=$(g)"; g
x=$(unset -f g); g

echo "=== 3. set options and shopt"
x=$(set -f); case $- in *f*) echo "noglob leaked";; *) echo "noglob off";; esac
x=$(set -o pipefail); set -o | grep pipefail
x=$(shopt -s nullglob); shopt -q nullglob && echo "nullglob leaked" || echo "nullglob off"

echo "=== 4. arrays and attributes"
x=$(arr=(1 2); declare -A as; as[k]=v); echo "arr=[${arr[*]}] as=[${as[k]}]"
b=(1 2 3); x=$(b[1]=x; unset 'b[2]'); echo "b=${b[*]}"
x=$(export ZZZ=1); echo "ZZZ=[$ZZZ] exported=$(env | grep -c '^ZZZ=')"
x=$(readonly ro=1); ro=2; echo "ro=$ro"
x=$(declare -i n); n=1+1; echo "n=$n"

echo "=== 5. positional parameters"
set -- a b; x=$(set -- p q r; echo $#); echo "inner=$x outer=$# $1 $2"
x=$(shift); echo "after shift: $1"

echo "=== 6. process substitution"
cat <(z=3; cd ..; echo procsub); echo "z=[$z] same-dir=$([ "$PWD" = "$start" ] && echo yes)"
while read -r line; do echo "read $line"; done < <(w=4; echo one; echo two); echo "w=[$w]"

echo "=== 7. \$RANDOM in a substitution does not replay the parent's sequence"
RANDOM=7; a=$RANDOM; RANDOM=7; b=$(echo $RANDOM); c=$RANDOM
[ "$a" = "$c" ] && echo "parent sequence kept" || echo "parent sequence moved"
[ "$a" = "$b" ] && echo "child replayed it" || echo "child reseeded"

echo "=== 8. the value and status still come back"
x=$(echo out; exit 4); echo "x=$x rc=$?"
x=$(trap 'echo from-trap' EXIT; echo body); echo "x=[$x]"
