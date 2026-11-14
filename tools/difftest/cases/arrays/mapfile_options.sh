printf '%s\n' a b c d e f > input.txt
mapfile -t -s 1 -n 3 arr < input.txt
echo "${arr[@]}"
mapfile -t -O 5 arr2 < input.txt
echo "${!arr2[@]}"
