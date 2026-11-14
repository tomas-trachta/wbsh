declare -a log=()
for i in 1 2 3; do printf -v 'log[i]' 'item-%d' "$i"; done
echo "${log[@]}"
