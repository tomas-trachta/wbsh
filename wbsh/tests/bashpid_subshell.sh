#!/bin/bash
# $BASHPID differs from $$ in every subshell, in-process ones included, and
# each subshell gets a value of its own (#270).

echo "=== the top level"
[ "$BASHPID" = "$$" ] && echo "top: BASHPID is \$\$"

echo "=== ( ) and \$( )"
(  [ "$BASHPID" != "$$" ] && echo "paren: own BASHPID" )
x=$(echo $BASHPID); [ "$x" != "$$" ] && echo "cmdsub: own BASHPID"
y=$( (echo $BASHPID) ); [ "$y" != "$$" ] && [ "$y" != "$x" ] && echo "nested: own BASHPID"
{ echo $BASHPID; } | { read -r p; [ "$p" != "$$" ] && echo "pipe: own BASHPID"; }

echo "=== each subshell has a different value"
a=$(echo $BASHPID); b=$(echo $BASHPID)
[ "$a" != "$b" ] && echo "sequential substitutions differ"
outer=$( inner=$(echo $BASHPID); echo "$BASHPID $inner" )
[ "${outer% *}" != "${outer#* }" ] && echo "outer and inner differ"

echo "=== the value is an integer and \$\$ stays the same"
(( a > 0 )) && echo "integer"
[ "$(echo $$)" = "$$" ] && echo "\$\$ unchanged in \$( )"

echo "=== it is restored when the subshell ends"
before=$BASHPID; (:); x=$(:); [ "$BASHPID" = "$before" ] && echo "restored"

echo "=== an EXIT trap in a subshell sees the subshell's value"
z=$( trap 'echo "$BASHPID"' EXIT; echo "$BASHPID" )
[ "${z%$'\n'*}" = "${z#*$'\n'}" ] && echo "same value in the trap"

echo "=== per-subshell temp names do not collide"
mkdir -p _run_tmp
( echo one > "_run_tmp/bashpid.$BASHPID" ); ( echo two > "_run_tmp/bashpid.$BASHPID" )
ls _run_tmp/bashpid.* | wc -l | tr -d ' '
rm -f _run_tmp/bashpid.*
