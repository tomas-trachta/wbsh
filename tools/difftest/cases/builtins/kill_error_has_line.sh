kill -9 nosuch
echo "status $?"
kill -s NOSIG 1
echo "status $?"
