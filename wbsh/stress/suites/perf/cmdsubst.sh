#!/bin/bash
# $(...) with a builtin inside: the subshell cost of the most common idiom.
n=0
for ((i = 0; i < 1000 * STRESS_N; i++)); do
    x=$(echo "$i")
    n=$(( n + ${#x} ))
done
echo "$n"
