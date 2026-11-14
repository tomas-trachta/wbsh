read -r -n 3 a <<< "abcdef"
echo "[$a]"
read -r -n 10 a <<< $'ab\ncd'
echo "[$a]"
read -r -N 4 a <<< $'ab\ncd'
printf '[%q]\n' "$a"
