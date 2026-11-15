#!/bin/bash
# `>&N` and `<&N` from a closed N fail, even when the copy the shell saves
# for the redirection lands on N, and the copies saved for what bash runs
# in a forked child (`$(...)`, a subshell, a pipeline stage) are closed
# there too (#301). Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

echo "=== closed sources"
echo "cmdsubst [$(echo x >&10 2>&1; echo $?)]"
echo direct >&10; echo "direct $?"
cat <&10; echo "input $?"
echo x 2>&10; echo "stderr $?"
exec {v}>&10; echo "fd variable $? v=${v-unset}"
echo same 3>&3; echo "same $?"

echo "=== copies saved for forked contexts"
echo "nested [$(echo "[$(echo x >&10 2>&1; echo $?)]")]"
echo "first stage $(echo x >&10 2>&1 | cat; echo ${PIPESTATUS[0]})"
( echo subshell >&10 ) > sub.txt; echo "subshell $? [$(cat sub.txt)]"
( ( echo inner >&11 ) > inner.txt ) > outer.txt; echo "inner $? [$(cat inner.txt)]"
{ echo group >&10; } > group.txt; echo "group $? [$(cat group.txt)]"

echo "=== descriptors the user opened still work"
exec {w}>w.txt
echo "w=$w [$(echo inside >&$w; echo $?)]"
( echo from-subshell >&$w )
echo x | { echo from-stage >&$w; }
exec {w}>&-
cat w.txt
coproc C { read -r line; echo "coproc got $line"; }
x=$(echo unrelated)
echo hello >&"${C[1]}"
read -r reply <&"${C[0]}"
echo "$reply"
wait

cd /
rm -rf "$D"
