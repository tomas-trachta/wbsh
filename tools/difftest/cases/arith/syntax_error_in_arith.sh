# requires: bash>=5.3
( echo $(( 1 + )) ) 2>&1
echo "rc=$?"
( echo $(( 3 3 )) ) 2>&1
echo "rc=$?"
