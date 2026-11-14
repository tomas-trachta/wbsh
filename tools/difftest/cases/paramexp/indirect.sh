name=target; target=value
echo "${!name}"
arr=(a b c); ref='arr[1]'
echo "${!ref}"
set -- p1 p2; n=2
echo "${!n}"
