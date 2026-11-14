set -u
f() { echo "$1"; }
( f ) 2>&1
echo "rc=$?"
