declare -A m
printf -v 'm[key]' '%s' value
echo "${m[key]}"
