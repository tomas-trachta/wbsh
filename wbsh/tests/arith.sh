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

echo "=== arithmetic errors fail the command"
for expression in '1/0' '5 % 0' '08' '2#2' '65#1' '12ab' '1 +' '1 2' '1 @ 2' '1 = 2' \
        '2 ** -1' '1 ? 2'; do
    echo "\$(( $expression )) -> $(( $expression ))"
    echo "  status $?"
done
(( 1/0 )); echo "(( 1/0 )) status $?"
let 'x = 1/0'; echo "let status $?"
y=1/0; echo $(( y + 1 )); echo "nested status $?"
x=5; : $(( x = 1/0 )); echo "x after failed assignment: $x"
x=3; : $(( x /= 0 )); echo "x after failed /=: $x"

echo "=== arithmetic edge cases"
echo $(( 0x )) $(( 2# )) $(( 64#zZ@_ )) $(( 0X1f )) $(( 017 )) $(( 10#08 ))
echo $(( ++5 )) $(( --5 )) $(( 2 ** 62 )) $(( 3 ** 40 ))
echo $(( (-9223372036854775807 - 1) / -1 )) $(( (-9223372036854775807 - 1) % -1 ))

echo "=== done"
