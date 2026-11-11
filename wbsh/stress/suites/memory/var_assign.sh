#!/bin/bash
# Re-assigning the same variables must not grow memory.
# @stress n=5000
for ((i = 0; i < STRESS_ITERS; i++)); do
    x="value-$i-$RANDOM"
    y=${x//-/_}
    unset z
    z=$x$y
done
echo ok
