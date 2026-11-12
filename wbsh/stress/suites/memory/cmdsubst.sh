#!/bin/bash
# $(...) captures.
# @stress n=300
for ((i = 0; i < STRESS_ITERS; i++)); do
    x=$(echo "$i")
    y=`printf '%s' "$x"`
done
echo ok
