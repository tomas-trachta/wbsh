#!/bin/bash
# Probe: declare -l / -u case conversion, plus assignment order in one command
# (bash 5.2 semantics).

echo "=== 1. declare -l / -u convert every assignment"
declare -l lo=ABC
echo "$lo"
declare -u up
up=abc
echo "$up"
up+=def
echo "$up"
declare -l lo2
read -r lo2 <<< "HeLLo"
echo "$lo2"
printf -v up '%s' xyz
echo "$up"
for up in one two; do :; done
echo "$up"
declare -u ue=čřž
declare -l le=ČŘŽ
echo "$ue $le"
f() { local -l loc=QQ; echo "$loc"; }
f
declare -p lo up

echo "=== 2. arrays"
declare -u arr=(a b)
echo "${arr[@]}"
arr[2]=c
arr+=(d)
echo "${arr[@]}"
declare -A m
declare -u m
m[k]=val
echo "${m[k]}"

echo "=== 3. switching attributes"
x=Mixed
declare -u x
echo "$x"
x=again
echo "$x"
declare -l x
x=MiXeD
echo "$x"
declare +l x
x=MiXeD
echo "$x"
declare -lu both=AbC
echo "$both"

echo "=== 4. compound assignments keep their order"
a=(b [3]=c)
declare -p a
a2=([2]=x y z)
declare -p a2
a3=(p [5]=q r [1]=s t)
declare -p a3
a3+=(u [9]=v w)
declare -p a3
declare -A pairs=(k1 v1 k2 v2)
echo "${pairs[k1]} ${pairs[k2]}"

echo "=== 5. assignments in one command apply left to right"
unset a b
a=1 b=$a
echo "b=$b"
c=5 d=$((c+1)) e=$d
echo "$d $e"
y=1 list=(z $y) w=${list[1]}
echo "w=$w"
k=3 list[k]=v
echo "${list[3]}"
unset p q
p=1 q=$p sh -c 'echo "command sees q=$q"'
echo "p after the command: ${p-unset}"
g() { echo "function sees q=$q"; }
p=2 q=$p g
{ x=1 >/nonexistent/dir/file; } 2>/dev/null
echo "x=$x"
