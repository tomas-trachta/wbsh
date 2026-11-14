IFS=, read -r a b c <<< "1,2,3,4"
echo "[$a] [$b] [$c]"
IFS=, read -r a b <<< ",x,"
echo "[$a] [$b]"
