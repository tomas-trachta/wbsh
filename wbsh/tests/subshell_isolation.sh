#!/bin/bash
# Probe: ( ... ) runs as a subshell, so nothing it changes reaches the parent (bash 5.2 semantics).

mkdir -p _run_tmp
start=$PWD

echo "=== 1. functions, positionals, arrays"
(f() { :; }); type f >/dev/null 2>&1 && echo "f leaked" || echo "f stays inside"
g() { echo outer; }; (unset -f g; g() { echo inner; }); g
set -- a b; (set -- p q; shift); echo "pos=$# $1 $2"
(a=(1 2 3)); echo "a=[${a[@]}]"
b=(x y); (b+=(z); unset 'b[0]'); echo "b=${b[*]}"
(declare -A m; m[k]=v); echo "m=[${m[k]}]"

echo "=== 2. variables, attributes, options, cwd"
v=1; (v=2; unset v; w=3); echo "v=$v w=[$w]"
(readonly ro=1); ro=2; echo "ro=$ro"
(export EE=1); echo "EE=[$EE] exported=$(env | grep -c '^EE=')"
(declare -i n); n=1+1; echo "n=$n"
(set -u -o pipefail; shopt -s nullglob); echo "flags=[$(set -o | grep -E '^(nounset|pipefail)' | tr -s ' \t' ' ')] nullglob=$(shopt -q nullglob && echo on || echo off)"
(cd ..); echo "same-dir=$([ "$PWD" = "$start" ] && echo yes)"

echo "=== 3. aliases, locals, the directory stack, getopts"
alias zz='echo hi'
(unalias zz; alias qq=true); alias zz; alias qq >/dev/null 2>&1 && echo "qq leaked" || echo "qq stays inside"
f() { local l=1; (l=2; local m=3; echo "in m=$m"); echo "l=$l m=[$m]"; }; f
m=global; h() { (local m=inner); echo "h m=$m"; }; h; echo "after h m=$m"
pushd .. >/dev/null; (popd >/dev/null; pushd / >/dev/null); d=$(dirs); set -- $d; echo "dirs=$#"; popd >/dev/null
OPTIND=1; (getopts ab: o -a; echo "o=$o OPTIND=$OPTIND"); echo "OPTIND=$OPTIND"

echo "=== 4. EXIT traps"
trap 'echo bye' EXIT; (true); echo after
(trap 'echo sub-exit' EXIT; echo body); listed=$(trap); echo "listed inside \$(...): $listed"
(trap; trap 'echo own' EXIT; trap)
(trap - INT; trap; echo "nothing listed after a change")
(trap 'echo into-file' EXIT; echo body) > _run_tmp/subshell_trap.txt; cat _run_tmp/subshell_trap.txt

echo "=== 5. return, break and continue end only the subshell"
r() { (return 3; echo no); echo "after return rc=$?"; }; r
s() { x=$(echo out; return 4; echo no); echo "x=$x rc=$?"; }; s
for i in 1 2; do (false; break; echo no); echo "loop $i rc=$?"; done
for i in 1 2; do (continue; echo no); echo "cloop $i rc=$?"; done
for i in 1 2; do for j in a b; do (break 2); echo "$i$j"; done; done
for i in 1 2; do x=$(break); echo "cs-loop $i rc=$?"; done
t() { (trap 'echo sub-exit' EXIT; return 5); echo "t rc=$?"; }; t
(exit 6); echo "exit rc=$?"
