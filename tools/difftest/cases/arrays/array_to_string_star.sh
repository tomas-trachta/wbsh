a=(1 2 3)
s="${a[*]}"
echo "$s"
IFS=+; echo "$(( ${a[*]} ))"
