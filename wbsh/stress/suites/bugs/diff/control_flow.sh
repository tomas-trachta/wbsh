#!/bin/bash
# Loops, case, functions, return codes and break/continue levels.
for i in 1 2 3; do
    for j in a b c; do
        [ "$j" = b ] && continue
        [ "$i" = 3 ] && break 2
        echo "$i$j"
    done
done
n=0; while :; do n=$((n+1)); [ $n -ge 3 ] && break; done; echo "while $n"
until [ $n -le 0 ]; do n=$((n-1)); done; echo "until $n"
for ((i=0, j=10; i<j; i+=3, j-=3)); do echo "c $i $j"; done
for w in "a b" c; do echo "[$w]"; done
case "file.tar.gz" in *.zip) echo zip ;; *.tar.*) echo tar ;;& *.gz) echo gz ;; *) echo other ;; esac
case x in x) echo first ;& y) echo fall ;; z) echo no ;; esac
f() { return 7; }; f; echo "ret $?"
g() { local v=inner; h; }; h() { echo "dyn $v"; }; v=outer; g; echo "after $v"
r() { [ "$1" -gt 0 ] && { echo -n "$1 "; r $(( $1 - 1 )); } || echo end; }; r 4
true && false || echo or; ! true; echo "not $?"
false; echo "st $?"; (exit 42); echo "sub $?"
{ echo grouped; false; }; echo "grp $?"
set -- a b c; while [ $# -gt 0 ]; do echo "arg $1"; shift; done
select_out=$(echo 2 | { select x in p q r; do echo "sel $x"; break; done; } 2>/dev/null); echo "$select_out"
