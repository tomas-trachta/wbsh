#!/bin/bash
# Array corners against bash: negative subscripts, appending a list to a
# scalar, unsetting a whole array, arithmetic subscripts with spaces, read
# into elements, declare -p quoting, mapfile -d and anchored empty patterns.

echo "=== negative subscripts"
a=(1 2 3 4)
a[-1]=last
echo "${a[@]}"
a[-4]+=x
echo "${a[@]}"
unset 'a[-2]'
echo "${a[@]} / ${!a[@]}"
( a[-9]=z; echo unreached ) 2>/dev/null
echo "too negative rc=$?"

echo "=== appending a list to a scalar"
s=str
s+=(arr more)
declare -p s

echo "=== unset of a whole array"
b=(1 2 3)
unset 'b[@]'
echo "b[@]: ${#b[@]} declared=$(declare -p b >/dev/null 2>&1 && echo yes || echo no)"
c=(1 2 3)
unset 'c[*]'
echo "c[*]: ${#c[@]} declared=$(declare -p c >/dev/null 2>&1 && echo yes || echo no)"

echo "=== arithmetic subscripts with spaces"
d=(a b c d)
echo "${d[ ${#d[@]} - 1 ]} ${d[ 1 + 1 ]} ${d[${#d[@]}-4]}"

echo "=== read into elements"
declare -A m
read -r 'm[k]' <<< "val ue"
echo "[${m[k]}]"
read -r 'n[2]' <<< "two"
declare -p n

echo "=== declare -p quoting"
q=(1 "two words" $'tab\there' $'nl\nx' 'dq"$')
declare -p q
r=$'a\x01b'
declare -p r
declare -A h=([k]=$'v\tw')
declare -p h
declare -a e
declare -p e

echo "=== mapfile -d"
printf 'x,y,z' | { mapfile -t -d , parts; printf '[%s]' "${parts[@]}"; echo; }
printf 'x:y:' | { readarray -d: kept; declare -p kept; }
printf 'a\0b c\0' | { mapfile -d '' -t nul; declare -p nul; }
printf 'a\0b\0' | { mapfile -d '' raw; declare -p raw; }

echo "=== anchored empty patterns"
f=(apple "" cherry)
printf '[%s]' "${f[@]/#/<}" "${f[@]/%/>}"
echo
set -- "a b" c
printf '[%s]' "${@/#/^}" "${*/%/$}"
echo
v=xyz
echo "${v/#/<}" "${v/%/>}" "${v//}" "[${unset_var/#/<}]"
