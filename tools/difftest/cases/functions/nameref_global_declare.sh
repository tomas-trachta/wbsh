declare -n r=real
real=1
echo "$r"
r=2
echo "$real"
declare -p r
