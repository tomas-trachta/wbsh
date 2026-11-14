set -- -x --y "a b"
echo "$# [$1] [$2] [$3]"
set - p q
echo "$*"
