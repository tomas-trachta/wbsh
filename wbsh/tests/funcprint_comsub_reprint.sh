#!/bin/bash
# declare -f prints a one-line $( ... ) the way bash 5.2+ re-prints the
# command it parsed: without the blanks just inside the parentheses
# (bash 5.3's cprint.right). Arithmetic, quoted text and a subshell
# right inside keep their spelling.

f() {
    for name in $( echo 1 2 3 ); do
        z=$(( 1 + 2 ))
        echo '$( kept )' $( (sub) )
    done
}
declare -f f
