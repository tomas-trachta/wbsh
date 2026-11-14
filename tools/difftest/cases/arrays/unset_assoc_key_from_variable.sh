declare -A m=([a]=1 [b]=2)
k=a
unset 'm[$k]'
echo "${!m[@]}"
unset 'nothere[1]'
echo "status $?"
