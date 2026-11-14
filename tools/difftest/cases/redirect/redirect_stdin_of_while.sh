printf '1\n2\n' > nums
n=0
while read -r x; do n=$((n + x)); done < nums
echo "$n"
