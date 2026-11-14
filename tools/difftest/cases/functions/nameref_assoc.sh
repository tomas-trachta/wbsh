put() { local -n m=$1; m[$2]=$3; }
declare -A table
put table key value
echo "${table[key]}"
