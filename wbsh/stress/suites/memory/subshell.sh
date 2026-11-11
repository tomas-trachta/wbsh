#!/bin/bash
# ( ... ) subshells and builtin pipelines.
# @stress n=300
for ((i = 0; i < STRESS_ITERS; i++)); do
    ( a=$i; echo "$a" ) > /dev/null
    echo "$i" | cat > /dev/null
done
echo ok
