#!/bin/bash
# Indexed and associative arrays: append, index, iterate.
declare -a a
declare -A m
for ((i = 0; i < 20000 * STRESS_N; i++)); do
    a+=("v$i")
    m["k$i"]=$i
done
s=0
for k in "${!m[@]}"; do
    s=$(( s + m[$k] ))
done
echo "${#a[@]} ${a[777]} $s"
