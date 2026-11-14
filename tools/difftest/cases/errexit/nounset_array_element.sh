set -u
a=(x)
echo "${a[0]}"
( echo "${a[5]}" ) 2>&1
echo "rc=$?"
