#!/bin/bash
# Descriptors the user opened reach pipeline stages and other children, as
# a forked bash's do (#299); the shell's own copies (saved fds, coproc
# ends) reach none. Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

child_fds() {
    "$BASH" -c 'echo /proc/self/fd/*' | sed 's|/proc/self/fd/||g'
}

echo "=== stages write to fd 3"
exec 3>out3
echo builtin | cat >&3; echo "builtin $?"
echo group | { cat; } >&3; echo "group $?"
echo external | findstr external >&3; echo "external $?"
echo nested | { echo inner | cat >&3; }; echo "nested $?"
{ echo background >&3; } & wait $!; echo "background $?"
cat out3

echo "=== which descriptors a child gets"
exec 3>&-
echo "none open: $(child_fds)"
{ echo "inside a redirected group: $(child_fds)"; } > group.txt
cat group.txt
exec 3>out3 7<out3 {v}>outv
echo "3, 7 and {v}=$v open: $(child_fds)"
coproc C { cat; }
echo "with coproc ${C[0]} ${C[1]}: $(child_fds)"
exec {C[1]}>&-
wait
echo "grandchild: $("$BASH" -c '"$BASH" -c "echo /proc/self/fd/*"' | sed 's|/proc/self/fd/||g')"
exec 3>&- 7<&- {v}>&-
echo "closed again: $(child_fds)"

echo "=== a closed descriptor stays closed"
echo x | cat >&3; echo "status $?"

cd /
rm -rf "$D"
