#!/bin/bash
# Process substitution: named pipes and relay threads must go away.
# @stress n=100 handles=32
for ((i = 0; i < STRESS_ITERS; i++)); do
    cat <(echo "$i") > /dev/null
    echo "$i" > >(cat > /dev/null)
done
wait
echo ok
