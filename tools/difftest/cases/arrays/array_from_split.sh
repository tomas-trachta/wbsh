s="a:b::c"
IFS=: read -ra parts <<< "$s"
echo "${#parts[@]}"
printf '[%s]' "${parts[@]}"; echo
