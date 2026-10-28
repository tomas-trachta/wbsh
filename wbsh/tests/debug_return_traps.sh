#!/bin/bash
# The DEBUG and RETURN traps, and BASH_COMMAND.

echo "=== DEBUG before each command"
trap 'echo "dbg [$BASH_COMMAND] line $LINENO"' DEBUG
:
echo hi
x=1
for i in 1 2; do echo "i=$i"; done
case a in a) echo case;; esac
[[ 1 == 1 ]]
(( x++ ))
if true; then echo yes; fi
f() { echo "in f"; }
f
echo a | cat
y=$(echo sub)
( echo subshell )
trap - DEBUG

echo "=== DEBUG keeps \$? and PIPESTATUS"
trap 'echo "D \$?=$? [$BASH_COMMAND]"' DEBUG
true
false
true | false
echo "after: ${PIPESTATUS[*]}"
trap - DEBUG

echo "=== arithmetic for and while"
trap 'echo "F [$BASH_COMMAND]"' DEBUG
for ((j = 0; j < 2; j++)); do :; done
while false; do :; done
trap - DEBUG

echo "=== functrace"
set -T
trap 'echo "T [$BASH_COMMAND]"' DEBUG
k() { echo k; }
k
v=$(echo inner); echo "v=$v"
trap - DEBUG
set +T

echo "=== RETURN"
g() { trap 'echo "ret from ${FUNCNAME[0]} \$?=$?"' RETURN; return 3; }
g; echo "g status $?"
h() { echo "h runs"; }
h
trap - RETURN
a() { trap 'echo "a ret \$?=$? cmd=$BASH_COMMAND"' RETURN; false; }
a; echo "a status $?"
trap - RETURN

echo "=== RETURN after source"
mkdir -p _run_tmp
src=_run_tmp/debug_return_traps_src.sh
printf 'echo sourced\n' > "$src"
trap 'echo "ret source"' RETURN
. "$src"
h
set -T
h
set +T
trap - RETURN
rm -f "$src"

echo "=== BASH_COMMAND in the ERR trap"
trap 'echo "err in [$BASH_COMMAND]"' ERR
test -d /nonexistent_debug_return_traps
trap - ERR
echo "$BASH_COMMAND"
