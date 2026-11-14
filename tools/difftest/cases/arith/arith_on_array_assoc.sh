declare -A m=([a]=3)
echo $(( m[a] * 2 ))
(( m[b] = 10 ))
echo "${m[b]}"
