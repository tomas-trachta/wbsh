set -u
( echo $(( nope + 1 )) ) 2>&1
echo "rc=$?"
