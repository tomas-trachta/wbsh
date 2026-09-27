#!/bin/bash
# Function propagation across pipeline boundaries.

echo "=== 1. Function at the head of a pipeline"
greet() { echo "hello $1 from func"; }
greet alice
greet alice | tr a-z A-Z

echo "=== 2. Function as the consumer (tail of pipeline)"
upcase() { tr a-z A-Z; }
echo "lowercase line" | upcase
seq 1 3 | upcase

echo "=== 3. Both ends are functions"
emit()   { for i in 1 2 3; do echo "x$i"; done; }
strip()  { while read line; do echo "got=$line"; done; }
emit | strip

echo "=== 4. Function uses an external inside its body"
words() { echo -e "alpha\nbeta\ngamma\ndelta\nbeta" | sort -u; }
words
words | wc -l
words | grep b

echo "=== 5. Function calling another function in a pipeline"
square() { echo $(($1 * $1)); }
nums()   { for i in 2 3 4; do echo $i; done; }
sqs()    { while read n; do square $n; done; }
nums | sqs

echo "=== 6. Self-referencing helper across two pipeline stages"
double() { echo $(($1 * 2)); }
emit2()  { for i in 1 2 3; do double $i; done; }
sumline() { read total; while read n; do total=$((total + n)); done; echo "sum=$total"; }
emit2 | sumline

echo "=== done"
