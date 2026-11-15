exec 3>out3 {v}>outv
echo builtin | cat >&3
echo group | { cat; } >&3
echo external | sort >&3
echo nested | { echo inner | cat >&3; }
"$BASH" -c 'echo child >&3'
{ echo background >&3; } &
wait
echo variable | cat >&$v
{ echo compound | cat >&4; } 4>&3
printf 'one\ntwo\n' > in
exec 5<in
echo | { read -u 5 line; echo "stage read $line"; }
read -u 5 line
echo "shell read $line"
exec 3>&- {v}>&- 5<&-
echo closed | cat >&3
echo "closed status $?"
cat out3 outv
