#!/bin/bash
# Data from variables, files, read and globs stays data.
. "$STRESS_ROOT/lib/check.sh"
rm -f pwned*

severity critical
x='$(echo > pwned1)'
echo $x > /dev/null; echo "$x" > /dev/null; [[ $x == y ]]; case $x in y) ;; esac; test "$x" = y
read -r y <<< '$(echo > pwned2) `echo > pwned3`'
echo $y > /dev/null; printf '%s %b' "$y" "$y" > /dev/null
: > '$(echo > pwned4)'
for f in *; do echo "$f" > /dev/null; done
ls > /dev/null; set -- *; echo "$@" > /dev/null
x='; echo > pwned5'; echo $x > /dev/null
declare -A m; k='$(echo > pwned6)'; m[$k]=1; echo "${m[$k]}" > /dev/null
x='`echo > pwned7`'; y=${x//a/b}; z=${x^^}; echo "${#x}" > /dev/null
printf '%s\n' '$(echo > pwned8)' | while read -r line; do echo $line > /dev/null; done
"$STRESS_WBSH" -r -c 'echo "$1" > /dev/null; echo $1 > /dev/null' _ '$(echo > pwned9)' < /dev/null
for n in 1 2 3 4 5 6 7 8 9; do
    check "data with command syntax is not run (case $n)" [ ! -e "pwned$n" ]
done

severity medium
x='a[$(echo > pwnedA)]'
: $(( x )) 2>/dev/null
[ -e pwnedA ] && review "\$(( var )) on untrusted data runs commands through an array subscript (bash does the same; known footgun)"
x='a[$(echo > pwnedB)]'
[[ $x -eq 0 ]] 2>/dev/null
[ -e pwnedB ] && review "[[ var -eq N ]] on untrusted data runs commands through an array subscript (bash does the same)"
ref='a[$(echo > pwnedC)]'
: "${!ref}" 2>/dev/null
[ -e pwnedC ] && review "\${!ref} with an untrusted name runs commands through an array subscript (bash does the same)"
finish
