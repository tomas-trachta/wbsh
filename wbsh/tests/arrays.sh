#!/bin/bash
# Probe: array operations.

echo "=== 1. indexed: dense"
a=(zero one two three)
echo "len=${#a[@]} all=${a[@]} second=${a[1]} last=${a[-1]:-OOR}"

echo "=== 2. indexed: keys / values / star vs at"
echo "keys: ${!a[@]}"
echo "vals @: ${a[@]}"
echo "vals *: ${a[*]}"
IFS=,
echo "vals * (IFS=,): ${a[*]}"
unset IFS

echo "=== 3. slice"
echo "slice 1:2 = ${a[@]:1:2}"
echo "slice 2:  = ${a[@]:2}"

echo "=== 4. append +="
b=(x y)
b+=(z)
b+=(p q)
echo "appended: ${b[@]}"

echo "=== 5. sparse"
declare -a s
s[0]=zero
s[5]=five
s[10]=ten
echo "sparse keys: ${!s[@]}"
echo "sparse vals: ${s[@]}"
echo "sparse len:  ${#s[@]}"

echo "=== 6. for loop quoting"
data=("a a" "b b" "c c")
echo "  quoted:"
for x in "${data[@]}"; do echo "    [$x]"; done
echo "  unquoted (gets word-split):"
for x in ${data[@]}; do echo "    [$x]"; done

echo "=== 7. unset element"
u=(a b c d)
unset 'u[1]'
echo "after unset u[1]: keys=${!u[@]} vals=${u[@]}"

echo "=== 8. assoc array"
declare -A m
m[apple]=red
m[banana]=yellow
m[grape]=purple
echo "  m[apple]=${m[apple]}"
echo "  m[banana]=${m[banana]}"
# Iterate a fixed known-keys list rather than ${!m[@]} | sort — pipelines
# inside command-substitution don't see arrays in wbsh today (separate
# limitation around array serialisation across pipeline self-spawn).
for k in apple banana grape; do
    echo "  $k=${m[$k]}"
done

echo "=== 9. array length of unset"
unset z
echo "len of unset: ${#z[@]}"

echo "=== done"
