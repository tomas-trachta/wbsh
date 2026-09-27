#!/bin/bash
# Smoke test for arithmetic, let, and bc.

echo "=== \$(( )) operators"
echo $((1+2))
echo $((10-3))
echo $((4*5))
echo $((20/3))
echo $((20%3))
echo $((2**8))
echo $(( (1+2)*3 ))
echo $(( -5 + 3 ))
echo $(( 0xff ))
echo $(( 010 ))
echo $(( 1 < 2 ))
echo $(( 2 == 2 ))
echo $(( 1 && 0 ))
echo $(( 1 || 0 ))

echo "=== let"
let a=5
let b=a*2
let c="(a+b)*2"
echo "a=$a b=$b c=$c"
let d++
echo "d after ++: $d"

echo "=== compound assigns"
n=10
n=$((n + 5))
echo "n=$n"
n=$((n * 2))
echo "n=$n"

echo "=== bc"
echo "1+2" | bc
echo "scale=4; 22/7" | bc
printf '2^10\n' | bc

echo "=== expr"
expr 2 + 3
expr 10 \* 4
expr 20 / 3
expr 20 % 3
expr length "abcdef"

echo "=== done"
