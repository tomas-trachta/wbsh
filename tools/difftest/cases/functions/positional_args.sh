f() { echo "$# [$1] [$2] [$*]"; shift; echo "after shift: $*"; }
f a "b c" d
echo "outer: $#"
