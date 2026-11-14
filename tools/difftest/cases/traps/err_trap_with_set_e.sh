set -e
trap 'echo "ERR handler"' ERR
f() { false; echo "not reached"; }
f
echo "not reached either"
