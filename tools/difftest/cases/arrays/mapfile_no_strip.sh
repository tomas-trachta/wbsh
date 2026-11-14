printf 'a\nb\n' > f.txt
mapfile m < f.txt
printf '<%s>' "${m[@]}"
echo
