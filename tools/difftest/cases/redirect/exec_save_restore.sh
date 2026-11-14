exec 3>&1
exec 1>captured
echo into file
exec 1>&3 3>&-
echo back on stdout
cat captured
