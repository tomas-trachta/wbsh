#!/bin/bash
# Probe: ${x//pat/rep} and friends stay linear for literal and fixed-length
# patterns, and ** is computed by squaring (bash 5.2 semantics).

x=$(printf '%050000d' 0)
start=$SECONDS

echo "=== 1. replacement results"
y=${x//0/1}; echo "literal: ${#y} ${y:0:3}"
y=${x//00/4}; echo "two-byte literal: ${#y} ${y:0:3}"
y=${x//?/2}; echo "question mark: ${#y} ${y:0:3}"
y=${x//[0]/3}; echo "bracket: ${#y} ${y:0:3}"
y=${x/0/5}; echo "first only: ${#y} ${y:0:3}"
y=${x/#0/5}; echo "anchored start: ${#y} ${y:0:3}"
y=${x/%0/6}; echo "anchored end: ${#y} ${y: -3}"
shopt -s extglob
y=${x//@(0|00)/7}; echo "extglob alternatives: ${#y} ${y:0:3}"
y=${x//+(0)/8}; echo "extglob repeat: $y"
shopt -u extglob

echo "=== 2. prefix and suffix removal"
y=${x#0}; echo "${#y}"
y=${x##0}; echo "${#y}"
y=${x%0}; echo "${#y}"
y=${x%%?}; echo "${#y}"
y=${x#??}; echo "${#y}"

echo "=== 3. small cases keep their meaning"
z=abcabc
echo "${z//b/X} ${z/b/X} ${z//bc/} ${z#a} ${z%c} ${z/#ab/Y} ${z/%bc/Z} ${z//c?/Q}"
w='a*b*c'
echo "${w//\*/+} ${w//'*'/-} ${w//[*]/=} ${w%\**} ${w#*\*}"
u=čřžčřž
echo "${u//ř/R} ${u//?ž/_} ${u#č} ${u%ž}"

echo "=== 4. ** by squaring"
echo $(( 1 ** 3000000000 )) $(( 3 ** 4000000000000000000 )) $(( (-1) ** 999999999999 ))
echo $(( 2 ** 62 )) $(( 2 ** 63 )) $(( 2 ** 64 )) $(( 7 ** 0 )) $(( 0 ** 0 ))

elapsed=$(( SECONDS - start ))
if (( elapsed <= 3 )); then echo "=== fast"; else echo "=== slow: ${elapsed}s"; fi
