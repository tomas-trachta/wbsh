f() { local IFS=-; echo "$*"; }
set -- 1 2 3
f "$@"
echo "$*"
