# requires: bash>=5.3
unset u
x=${u:-fallback}; echo "$x"
( : "${u:?custom message}" ) 2>&1
echo "status=$?"
( : "${u?}" ) 2>&1
echo "status=$?"
