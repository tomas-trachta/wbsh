#!/bin/bash
# Probe: array subscripts inside arithmetic (bash 5.2 semantics).

echo "=== 1. reading elements"
a=(10 20 30); i=1
echo $(( a[i] * 2 ))
echo "$(( a[0] + a[1+1] )) $(( a[ 1 ] )) $(( a[a[2]/10-2] )) $(( a[i] ? a[0] : 9 ))"
echo "$(( a[-1] )) $(( a[-3] )) $(( u[3] )) $(( a[7] )) $(( a[2]**2 ))"
s=4; echo "scalar: $(( s[0] )) $(( s[1] )) $(( s[-1] ))"
b=(); b[0]='2+3'; echo "expression element: $(( b[0] * 2 ))"
j=1; echo "$(( a[j++] )) j=$j"

echo "=== 2. assigning elements"
(( a[2] = 5 )); echo "${a[2]}"
(( a[1]++ )); echo "${a[@]}"
(( ++a[1] )); echo "${a[@]}"
(( a[1] += 3, a[0] -= 1 )); echo "${a[@]}"
echo "$(( a[1]-- )) $(( --a[1] )) ${a[1]}"
(( a[-1] = 9 )); echo "${a[@]}"
(( a[5] = 1 )); echo "${a[@]} / ${!a[@]}"
(( s[2] = 6 )); echo "${s[@]} / ${!s[@]}"
x=$(( a[1] = 42 )); echo "$x ${a[1]}"
(( m[1+1] = 3 )); echo "${!m[@]}"
for ((k=0; k<3; k++)); do (( q[k] = k*k )); done; echo "${q[@]}"
let 'c[2]=7' 'c[0]+=10'; echo "${c[@]}"

echo "=== 3. evaluation order"
unset a; i=0; (( a[i++] = i )); echo "${a[0]} i=$i"
a=(5 6 7); i=0; (( a[i++] += 10 )); echo "${a[@]} i=$i"
a=(5 6 7); i=0; (( a[i++]++ )); echo "${a[@]} i=$i"
a=(5 6 7); i=0; (( ++a[i++] )); echo "${a[@]} i=$i"
c=(1 2 3); n=0; (( n || c[0]++ )); echo "${c[@]}"
(( 0 && c[1]++ )); echo "${c[@]}"
(( 1 ? c[2]++ : c[1]++ )); echo "${c[@]}"

echo "=== 4. associative arrays"
declare -A h
h[x]=3; h[' x ']=2; key=x
echo "$(( h[x] )) $(( h[$key] * 2 )) $(( h[nokey] )) $(( h[ x ] ))"
(( h[x]++ )); (( h[new] = 7 )); echo "${h[x]} ${h[new]}"
(( h[key] = 1 )); echo "${h[key]} ${h[x]}"

echo "=== 5. bad subscripts"
a=(1 2 3)
echo "$(( a[-10] ))"; echo "rc=$?"
(( a[-10] = 1 )); echo "rc=$? ${a[@]}"
echo "$(( a[] ))"; echo "rc=$?"
(echo "$(( a[1 ))"); echo "rc=$?"
(echo "$(( a[1/0] ))"); echo "rc=$?"
(echo "$(( 5 + a [1] ))"); echo "rc=$?"
(echo "$(( a[1]a ))"); echo "rc=$?"
(echo "$(( a[0][0] ))"); echo "rc=$?"
