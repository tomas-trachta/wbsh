trap 'echo parent' EXIT
( trap -p EXIT; echo "sub printed above" )
trap - EXIT
