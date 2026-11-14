echo data > >(tr a-z A-Z > out)
wait $!
cat out
