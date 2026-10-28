#!/bin/bash
# BASH_REMATCH, PIPESTATUS, FUNCNAME / BASH_SOURCE / BASH_LINENO and the ERR trap.

echo "=== BASH_REMATCH"
re='([a-z]+)([0-9]+)'
[[ abc123 =~ $re ]]; echo "${BASH_REMATCH[0]} ${BASH_REMATCH[1]} ${BASH_REMATCH[2]} n=${#BASH_REMATCH[@]}"
[[ xyz =~ $re ]]; echo "no match: n=${#BASH_REMATCH[@]}"

echo "=== PIPESTATUS"
true | false | true; echo "${PIPESTATUS[@]}"
true | false | true | (exit 7); echo "${PIPESTATUS[@]} n=${#PIPESTATUS[@]}"
false; echo "single: ${PIPESTATUS[@]}"
! false; echo "bang: ${PIPESTATUS[@]}"
if false; then :; fi; echo "if: ${PIPESTATUS[@]}"
{ true | false; true; }; echo "brace: ${PIPESTATUS[@]}"
{ true; false | true; } | false; echo "group stage: ${PIPESTATUS[@]}"
x=$(exit 4); echo "assignment: ${PIPESTATUS[@]}"
f() { return 3; }; f; echo "function: ${PIPESTATUS[@]}"
[[ a == b ]]; echo "dbracket: ${PIPESTATUS[@]}"
set -o pipefail; true | false | true; echo "pipefail: ${PIPESTATUS[@]} status $?"; set +o pipefail

echo "=== FUNCNAME, BASH_SOURCE, BASH_LINENO"
here=${BASH_SOURCE[0]##*/}
echo "top: source=$here lineno=${BASH_LINENO[*]}"
[[ "${BASH_SOURCE[0]}" == "$0" ]] && echo "main guard ok"
show() { echo "$1: ${FUNCNAME[*]} / ${BASH_SOURCE[*]##*/} / ${BASH_LINENO[*]}"; }
outer() { show nested; }
show direct
outer
v=$(show substituted); echo "$v"
show piped | cat
lib=_run_tmp/bash_runtime_vars_lib.sh
mkdir -p _run_tmp
printf '%s\n' 'echo "lib top: ${FUNCNAME[*]} / ${BASH_SOURCE[*]##*/}"' 'libf() { show libf; }' 'show in-lib' > "$lib"
. "$lib"
libf
wrap() { . "$lib"; }
wrap
rm -f "$lib"
echo "after: ${FUNCNAME[*]}"

echo "=== ERR trap"
trap 'echo "ERR status=$? line=$LINENO"' ERR
false
true
if false; then :; fi
false || true
false && true
! true
{ false; echo "brace goes on"; }
(false); echo "subshell done"
(false; echo "inside the subshell no trap")
true | false
[[ a == b ]]
case x in x) false;; esac
g() { false; return 2; }
g
echo "--- set -E"
set -E
g
y=$(false; echo out); echo "y=$y"
set +E
eval false
trap 'true' ERR; false; echo "status kept $?"
h() { trap 'echo "own ERR in h"' ERR; false; }
trap - ERR
h
false
trap - ERR
false
echo "=== ERR trap and set -e"
(trap 'echo "trap before exit"' ERR; set -e; { false && true; }; echo "brace with && survives"; ! true; echo "bang survives"; false; echo "not reached")
echo "subshell exit $?"
