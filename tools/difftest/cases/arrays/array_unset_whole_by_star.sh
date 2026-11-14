# requires: bash>=5.2
a=(1 2 3)
unset 'a[@]'
echo "${#a[@]}"
declare -p a 2>&1 || echo "not declared"
