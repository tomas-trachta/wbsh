( echo $(( 1 / 0 )) ) 2>&1
echo "rc=$?"
( echo $(( 5 % 0 )) ) 2>&1
echo "rc=$?"
