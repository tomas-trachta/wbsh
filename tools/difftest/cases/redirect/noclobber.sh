echo a > f
set -C
( echo b > f ) 2>/dev/null || echo "refused"
echo c >| f
cat f
