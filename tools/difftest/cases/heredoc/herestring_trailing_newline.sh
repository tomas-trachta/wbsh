v=$(cat <<< "abc"; echo end)
echo "$v"
printf '%s' "x" | { read -r r; echo "rc=$? r=$r"; }
