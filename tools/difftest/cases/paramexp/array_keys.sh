a=(x y z); a[7]=q
echo "${!a[@]}"
declare -A m=([b]=2)
echo "${!m[@]}"
