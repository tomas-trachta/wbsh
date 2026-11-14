declare -A m
m[*]=star
echo "after star: $?"
m[@]=at; echo "same line"
echo "after at: $? ${#m[@]}"
