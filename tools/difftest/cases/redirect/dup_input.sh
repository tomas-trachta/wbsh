printf 'abc\n' > in
exec 5<in
cat <&5
exec 5<&-
