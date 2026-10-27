#!/bin/bash
# Probe: command / type / unset / export / return / local / declare options (bash 5.2 semantics).

mkdir -p _run_tmp
start=$PWD

echo "=== 1. command -v / -V"
f() { :; }
echo "[$(command -v echo)] [$(command -v f)] [$(command -v if)] [$(command -v nosuch_cmd)]"
command -v nosuch_cmd; echo "missing rc=$?"
command -v nosuch_cmd echo >/dev/null; echo "one found rc=$?"
case "$(command -v cmd)" in /*cmd.exe|/*CMD.EXE|/*Cmd.exe) echo "cmd path is posix" ;; *) echo "cmd path: $(command -v cmd)" ;; esac
command -V if
command -V nosuch_cmd 2>&1; echo "rc=$?"
alias zz='echo z'
command -v zz
unalias zz
echo() { builtin echo "function echo"; }
command echo "command skips the function"
unset -f echo

echo "=== 2. type -t / -p / -P / -a / -f"
type -t echo f if '[[' '{' return nosuch_cmd; echo "rc=$?"
alias ll='ls -l'
type ll; type -t ll
unalias ll
type -p echo; echo "-p of a builtin rc=$?"
[ -n "$(type -P cmd)" ] && echo "-P finds cmd on PATH"
type -t -f f; echo "-f hides functions rc=$?"
type -a f | head -1
type -z 2>&1; echo "rc=$?"
builtin echo "builtin runs the builtin"
builtin nosuch_cmd 2>&1; echo "rc=$?"

echo "=== 3. unset -f / -v, export -n"
h() { echo hf; }
hv=1; unset hv; type -t h
unset -v h; type -t h
unset h; type -t h; echo "unset falls back to the function rc=$?"
g() { :; }; unset -f g; type -t g; echo "unset -f rc=$?"
readonly RO=1
unset RO 2>&1; echo "rc=$?"
unset 1x 2>&1; echo "rc=$?"
export XN=1; export -n XN
echo "XN=$XN printenv=[$(printenv XN)]"
export XE='a "b" $c'
export -p | grep '^declare -x XE='
export | grep -c '^declare -x XE='

echo "=== 4. return from a sourced file"
printf 'echo before\nreturn 3\necho NOT_REACHED\n' > _run_tmp/ret.sh
. ./_run_tmp/ret.sh; echo "rc=$?"
printf 'for i in 1 2; do return 4; done\necho NOT_REACHED\n' > _run_tmp/retloop.sh
source _run_tmp/retloop.sh; echo "rc=$?"
sf() { . ./_run_tmp/ret.sh; echo "function continues rc=$?"; }; sf
return 2>&1; echo "top level rc=$?"

echo "=== 5. declare in a function is local, -g is global"
d=out; fd() { declare d=in; typeset t=in; declare -g gd=in; }; t=out; fd
echo "d=$d t=$t gd=$gd"
x=global; fx() { local x; echo "[${x-unset}]"; x=changed; }; fx; echo "x=$x"
fr() { local -r r=1; r=2; echo "r=$r"; }; fr 2>&1; r=5; echo "r after=$r"
fi_() { local -i n=3+4; n+=1; echo "n=$n"; n=010; echo "n=$n"; }; fi_
fl() { local -l lo=ABC; local -u up=abc; lo+=XY; echo "$lo $up"; }; fl
arr=(g1 g2); fa() { local -a arr=(l1 "l 2"); arr+=(l3); echo "${#arr[@]} ${arr[1]}"; }; fa
echo "arr restored: ${arr[*]}"
fm() { local -A m=([k]=v ["a b"]=c); echo "${m[a b]} ${m[k]}"; }; fm; echo "m=[${m[k]}]"
fe() { local HOME=/x; printenv HOME; local Q=1; printenv Q; echo "Q rc=$?"; }; fe
readonly GR=1; fg_() { local GR=2; }; fg_ 2>&1; echo "rc=$? GR=$GR"
fp() { local a=1 b; local; }; fp
local q=1 2>&1; echo "local at top level rc=$?"
declare -z 2>&1; echo "rc=$?"

echo "=== 6. declaration arguments do not word-split"
s="a: b"
fs() { local m=$1; echo "[$m]"; local j="$@"; echo "[$j]"; local k=$*; echo "[$k]"; }
fs "$s" "z  y"
declare q=$s; export w=$s; readonly ro2=$s; typeset tt=$s
echo "[$q] [$w] [$ro2] [$tt]"
pat='*'; declare gl=$pat; echo "[$gl]"
declare -a ar=($s); echo "${#ar[@]}"
v="(c d)"; declare z=$v; declare -a za=$v; echo "[$z] ${#za[@]}"

echo "=== 7. declare -p output re-evaluates"
v='it'"'"'s a "q" $x `y` \z'
declare -p v
saved=$(declare -p v); unset v; eval "$saved"; echo "[$v]"
ia=([2]=x [5]="y z"); declare -p ia
declare -A am=([k]=v ["a b"]="c d"); declare -p am
saved=$(declare -p am); unset am; eval "$saved"; echo "${am[a b]}"
declare -ir nr=5; declare -p nr
declare -xl xl=Q; declare -p xl
declare -p nosuch_var 2>&1; echo "rc=$?"
declare -F f; declare -f f | head -1
pv='a b'; declare | grep '^pv='

echo "=== 8. cd -P / -L, pwd -P"
mkdir -p _run_tmp/cdp
cd -P _run_tmp/cdp && echo "in ${PWD##*/}"
cd -L .. && echo "in ${PWD##*/}"
cd -x 2>&1; echo "rc=$?"
cd -- "$start"
[ "$(pwd -P)" = "$(pwd)" ] && echo "pwd -P agrees"

echo "=== 9. set -x quotes arguments"
(set -x; echo 'a b' c '' "it's" '*' x=y '~' a~ '#a' a#) 2>&1

echo "=== 10. exec of a builtin ends the shell"
(exec echo "exec'd"; echo NOT_REACHED)
