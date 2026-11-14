a=`echo 'a\$b'`
b=$(echo 'a\$b')
echo "$a $b"
