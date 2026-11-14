set -e
f() { false; echo "f continues because it is in a condition"; }
if f; then echo "f ok"; fi
f || echo "or"
echo end
