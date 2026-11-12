#!/bin/bash
# After an expansion error bash drops the rest of the input line: the
# other commands on it, and a loop or function call it is in. Then the
# next line runs. eval and source texts drop their own line; a subshell,
# $(...) or pipeline element ends with status 1. Arithmetic that a command
# evaluates itself ((( )), let, [[ -eq ]]) only fails that command, and so
# does a redirection of a command bash would fork (an external program or
# a wbsh utility such as cat).

n='p 2'
echo 1 $(( n )); echo 1 not reached
echo 2 next line
for i in 1 2; do echo "3 $i"; x=$(( n )); echo 3 not reached; done
echo 4
(( n )); echo "5 (( )) status $?"
let n; echo "6 let status $?"
[[ n -eq 1 ]]; echo "7 [[ ]] status $?"
f() { echo 8 in f; x=$(( n )); echo 8 not reached; }; f; echo 8 not reached
echo 9
eval 'x=$(( n )); echo 10 not reached
echo 10 next eval line'; echo "10 after eval $?"
( x=$(( n )); echo 11 not reached
echo 11 not reached ); echo "11 subshell $?"
y=$(x=$(( n )); echo 12 not reached
echo 12 not reached); echo "12 substitution [$y] $?"
echo "13 $(( n ))" | cat; echo 13 after pipe
D=$(mktemp -d) || exit 1
printf 'x=$(( n )); echo 14 not reached\necho 14 next sourced line\n' > "$D/src.sh"
. "$D/src.sh"; echo "14 after source $?"
if x=$(( n )); then echo 15 not reached; fi; echo 15 not reached
x=$(( n )) || echo 16 not reached
echo 16 next
echo ${x!}; echo 17 not reached
echo 17 next
declare -i d; d=n; echo 18 not reached
echo 18 next
local_f() { local v=$(( n )); echo 19 not reached; }; local_f; echo 19 not reached
echo 19 next
cat <<< $(( n )); echo "20 utility here-string: line goes on"
cat < "$(( n ))"; echo "21 utility redirection: line goes on"
echo hi > "$(( n ))"; echo 22 not reached
echo 22 next
for i in 1 2
do
    echo "23 $i $(( n ))"
    echo 23 not reached
done
echo end
rm -rf "$D"

# A dropped line neither fires the ERR trap nor ends the shell under set -e.
( set -e; trap "echo ERR trap" ERR
echo 24 $(( n )); echo 24 not reached
echo 24 next ); echo "24 status $?"
