exec {fd}>out
echo "fd is number: $(( fd >= 10 ))"
echo via-varname >&$fd
exec {fd}>&-
cat out
