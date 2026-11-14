declare -ra r=(1 2)
( r[0]=9 ) 2>&1
echo "rc=$? ${r[@]}"
