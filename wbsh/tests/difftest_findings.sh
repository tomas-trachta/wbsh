#!/bin/bash
# The divergences tools/difftest/FINDINGS.md listed, fixed: special keys in
# associative arrays, readonly variables in arithmetic and prefixes,
# GLOBIGNORE and dotglob, [[ arithmetic errors, eval line numbers, kill's
# messages, here-documents in pipelines, $- in a -c pipeline, bash 5.2+
# `unset 'a[@]'`, and background jobs with redirections.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=wbsh
T=_findings_tmp
rm -rf $T
mkdir -p $T

echo "=== associative array keys @ and *"
declare -A m=([a]=1 [b]=2)
m[*]=star; echo "same line"
echo "after m[*]=: $?"
m[@]=at
echo "after m[@]=: $? ${#m[@]}"
k='*'; m[$k]=kstar
k='@'; m[$k]=kat
for k in '*' '@' a; do echo "[$k]=${m[$k]}"; done
echo "${#m[@]} elements"
declare -p m
unset 'm[@]'
k='*'; unset 'm[$k]'
declare -p m
a=(1 2)
a[*]=3
echo "indexed a[*]=: $? ${a[*]}"
unset 'a[@]'
declare -p a
unset 'nothere[1]'; echo "unset of a missing element: $?"

echo "=== readonly in arithmetic"
readonly R=1
(( R = 12 )); echo "(( )): $?"
let R=5; echo "let: $?"
(( R++ )); echo "R++: $? R=$R"
echo $(( R = 3 )); echo "not reached"
echo "after \$(( )): $?"

echo "=== readonly prefix assignment"
R=8 true 2>/dev/null; echo "status $?"
R=9 echo "R is still $R" 2>/dev/null

echo "=== dynamic variables in arithmetic"
echo "LINENO $(( LINENO > 0 ))"
(( EPOCHSECONDS > 1000000000 )) && echo "EPOCHSECONDS"
RANDOM=7; r=$RANDOM; RANDOM=7; (( RANDOM == r )) && echo "RANDOM"

echo "=== GLOBIGNORE and dotglob"
mkdir $T/g; : > $T/g/.hidden; : > $T/g/shown
cd $T/g
GLOBIGNORE=shown; echo *; shopt -p dotglob
GLOBIGNORE=; echo *; shopt -p dotglob
unset GLOBIGNORE; echo *; shopt -p dotglob
cd ../..

echo "=== [[ arithmetic errors"
[[ 1x -eq 1 ]]; echo "status $?"
x=08; [[ $x -lt 9 ]]; echo "status $?"

echo "=== eval line numbers"
eval 'echo $LINENO
echo $LINENO'
eval '
echo $LINENO'
eval 'echo d
)'
echo "status $?"

echo "=== kill messages"
kill -9 nosuch; echo "status $?"
kill -s NOSIG 1; echo "status $?"
kill %99; echo "status $?"

echo "=== here-document in a pipeline stage"
printf 'cat <<END | cat\nabc\n' > $T/hd.sh
"$WBSH" $T/hd.sh 2>&1 | sed "s|$T/||"

echo "=== \$- in a pipeline stage of -c"
"$WBSH" -c 'echo $- | cat'

echo "=== background job with redirections"
start=$SECONDS
out=$("$WBSH" -c 'ping -n 3 127.0.0.1 >/dev/null 2>&1 & echo started')
echo "$out after $(( SECONDS - start < 2 ? 0 : SECONDS - start ))s"

rm -rf $T
