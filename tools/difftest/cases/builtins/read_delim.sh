read -r -d : a <<< "left:right"
echo "[$a]"
read -r -d '' b <<< $'multi\nline'
echo "rc=$? [$b]"
