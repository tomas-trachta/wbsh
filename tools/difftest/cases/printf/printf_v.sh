printf -v out '%s-%s' a b
echo "$out"
printf -v arr[2] '%03d' 7
echo "${arr[2]}"
