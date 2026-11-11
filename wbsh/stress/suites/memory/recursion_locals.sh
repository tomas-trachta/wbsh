#!/bin/bash
# Repeated recursion with locals: frames must be released.
# @stress n=100
rec() {
    local depth=$1 pad="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    (( depth > 0 )) && rec $(( depth - 1 ))
}
for ((i = 0; i < STRESS_ITERS; i++)); do
    rec 50
done
echo ok
