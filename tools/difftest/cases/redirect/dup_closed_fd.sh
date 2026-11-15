echo "cmdsubst [$(echo x >&10 2>&1; echo $?)]"
echo direct >&10
echo "direct $?"
cat <&10
echo "input $?"
echo x 2>&10
echo "stderr $?"
echo x 1>&10 2>&1
echo "both $?"
exec 4>&10
echo "exec $?"
echo "first stage $(echo x >&10 2>&1 | cat; echo ${PIPESTATUS[0]})"
( echo subshell >&10 ) > sub.txt
echo "subshell $? [$(cat sub.txt)]"
{ echo group >&10; } > group.txt
echo "group $? [$(cat group.txt)]"
f() { echo function >&10; }
f > function.txt
echo "function $? [$(cat function.txt)]"
echo same 3>&3
echo "same $?"
exec 3>out3
echo open >&3
exec 3>&-
cat out3
