#!/bin/bash
# Function call overhead with locals and positional parameters.
add() {
    local a=$1 b=$2
    r=$(( a + b ))
}
for ((i = 0; i < 20000 * STRESS_N; i++)); do
    add "$i" 1
done
echo "$r"
