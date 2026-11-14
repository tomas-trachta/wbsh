set -e
false && echo no
echo "after && list"
false || echo "or branch"
true && false || echo "fallback"
echo "end"
