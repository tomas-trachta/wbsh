set -e
f() { false && true; echo "f continues"; }
f
echo end
