trap 'echo "ERR at status $?"' ERR
false
true
(exit 2)
echo done
