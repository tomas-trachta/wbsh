#!/bin/bash
# Exercises the legacy `$[ expr ]` arithmetic form. wbsh forwards it to
# the same evaluator as `$(( expr ))` — these checks pin both that the
# lexer recognises `$[`, and that the same set of operators agree.

echo "=== plain"
echo $[1 + 2]
echo $[ 5 * 6 ]
echo $[ 100 - 7 ]

echo "=== with vars"
n=10
echo $[ $n + 1 ]
echo $[ n * 2 ]      # bare names are valid in arithmetic context

echo "=== nested"
echo $[ (3 + 4) * 2 ]
echo $[ $[ 2 + 3 ] * 4 ]

echo "=== loop counter pattern"
i=0
sum=0
while [ $i -lt 5 ]
do
    sum=$[ $sum + $i ]
    i=$[ $i + 1 ]
done
echo "i=$i sum=$sum"

echo "=== mixed with \$((..))"
a=$(( 2 + 3 ))
b=$[ 2 + 3 ]
echo "a=$a b=$b match=$([ $a = $b ] && echo yes || echo no)"

echo "=== done"
