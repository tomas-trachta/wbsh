#!/bin/bash
# Probe: ${x@X} transformations, ${x~}, $"...", declare -i and declare -n (bash 5.2 semantics).

echo "=== 1. \${x@Q} \${x@E} \${x@P}"
x="it's"
echo "${x@Q}"
t=$'tab\there'
echo "${t@Q}"
e=''
echo "[${e@Q}] [${unset_var@Q}]"
q='a\x41\tb'
echo "${q@E}"
p='\$ \\'
echo "${p@P}"

echo "=== 2. \${x@U} \${x@u} \${x@L} \${x~} \${x~~}"
z=hello
echo "${z@U} ${z@u} ${z@L} ${z^} ${z~} ${z~~}"
w=Hello
echo "${w~~} ${w~} ${w~~[el]}"
c=čŘž
echo "${c~~} ${c@U}"
lst=(ab Cd)
echo "${lst[@]~~} ${lst[@]~} ${lst[@]@U}"

echo "=== 3. \${x@A} \${x@a}"
declare -i n=9
echo "${x@A}"
echo "${n@A}"
echo "[${x@a}] [${n@a}]"
arr=(a "b c")
echo "${arr[@]@a}"
echo "${arr[@]@A}"
echo "${arr[@]@Q}"
set -- a 'b c'
echo "${@@Q}"
echo "${@@A}"

echo "=== 4. \${x@K} \${x@k}"
idx=(x 'y z')
echo "${idx[@]@K}"
printf '<%s>' "${idx[@]@k}"
echo
declare -A as=(["a b"]=1 [c]=$'x\ty')
echo "${as[@]@K}"
echo "${x@K}"

echo "=== 5. unknown operators are bad substitutions"
echo "${z@Z}"
echo "status $?"
echo "${z@}"
echo "${z.y}"

echo "=== 6. \$\"...\" is a double-quoted string"
name=world
echo $"hello $name"

echo "=== 7. declare -i"
declare -i sum
sum=2+3
echo "$sum"
sum+=4
echo "$sum"

echo "=== 8. declare -n"
v=1
declare -n r=v
echo "$r"
r=2
echo "$v ${!r}"
declare -p r
[[ -R r ]] && echo "-R r"
[[ -R v ]] || echo "not -R v"
r+=3
echo "$v"
add() { local -n into=$1; into+=("$2"); }
list=(a)
add list b
add list c
echo "${list[@]} (${#list[@]})"
count() { local -n _a=$1; echo "${#_a[@]} ${_a[1]} ${!_a[@]}"; }
count list
setv() { local -n out=$1; out="set by setv"; }
setv result
echo "$result"
declare -A m=([k]=v)
declare -n mr=m
mr[z]=w
echo "${m[z]} ${mr[k]}"
declare -n er='list[1]'
echo "$er"
er=B
echo "${list[@]}"
declare -n u
u=v
declare -p u
declare -n self=self
echo "rc=$?"
declare -n bad=1a
echo "rc=$?"
declare -i num=5
declare -n numref=num
numref=2+3
echo "$num"
f() { local -n ref=$1; echo "${ref[@]}" | cat; ( echo "subshell ${ref[1]}" ); echo "cmdsubst $(echo ${#ref[@]})"; }
f list
declare -n loop
for loop in v result; do echo "loop: $loop"; done
unset r
echo "v=${v-unset}"
declare -p r
declare +n r
declare -p r
declare -n r2=num
unset -n r2
declare -p r2 2>&1
echo "num still $num"
