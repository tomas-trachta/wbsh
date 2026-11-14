# requires: bash>=5.1
declare -A m=(one 1 two 2)
echo "${m[one]} ${m[two]}"
m+=(three 3)
echo "${m[three]}"
