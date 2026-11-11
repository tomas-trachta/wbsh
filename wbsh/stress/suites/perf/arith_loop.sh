#!/bin/bash
# The interpreter loop: (( )) arithmetic and assignment, no commands spawned.
s=0
for ((i = 0; i < 100000 * STRESS_N; i++)); do
    s=$(( s + i % 7 ))
done
echo "$s"
