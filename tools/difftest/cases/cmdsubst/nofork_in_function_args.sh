# requires: bash>=5.3
f() { echo "$# [$1]"; }
f ${ printf 'a b'; }
f "${ printf 'a b'; }"
