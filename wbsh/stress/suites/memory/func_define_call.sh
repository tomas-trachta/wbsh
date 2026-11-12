#!/bin/bash
# Defining, calling and unsetting functions with locals.
# @stress n=2000
for ((i = 0; i < STRESS_ITERS; i++)); do
    f() { local a=$1 b=("$@"); r=${#b[@]}; }
    f "$i" x y
    unset -f f
done
echo ok
