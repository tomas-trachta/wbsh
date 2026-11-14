trap 'echo x' EXIT
trap 'echo int' INT
trap -p EXIT
trap -p
trap - INT
trap -p INT
echo listed
trap - EXIT
