n=0
trap '(( n++ ))' DEBUG
a=1
b=2
c=3
trap - DEBUG
echo "debug ran $n times"
