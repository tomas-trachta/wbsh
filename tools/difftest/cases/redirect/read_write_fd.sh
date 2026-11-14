echo hello > rw
exec 4<> rw
read -r w <&4
echo "$w"
exec 4>&-
