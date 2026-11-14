declare -A m
read -r 'm[k]' <<< "val"
echo "[${m[k]}]"
