set -e
n=0
while [ $n -lt 3 ]; do
    n=$((n+1))
    [ $n -eq 2 ] && continue
    echo "n=$n"
done
echo done
