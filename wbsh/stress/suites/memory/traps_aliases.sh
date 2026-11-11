#!/bin/bash
# Setting and clearing traps and aliases.
# @stress n=2000
for ((i = 0; i < STRESS_ITERS; i++)); do
    trap "x=$i" USR1
    trap - USR1
    alias "a$((i % 3))=echo $i"
    unalias "a$((i % 3))"
done
echo ok
