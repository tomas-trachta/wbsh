read -ra arr <<< "  x  y   z "
echo "${#arr[@]}: ${arr[*]}"
