set -e
f() { false; echo "not printed"; }
f
echo "not printed either"
