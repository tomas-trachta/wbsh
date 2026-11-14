IFS=: read -r a b <<< "k:v:w"
echo "[$a] [$b]"
IFS=' ' read -r a b <<< "  lead  rest  "
echo "[$a] [$b]"
