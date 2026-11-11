#!/bin/bash
# Opening and closing file descriptors: every handle must be closed again.
# @stress n=1000 handles=16
for ((i = 0; i < STRESS_ITERS; i++)); do
    exec 3> "f$((i % 4))"
    echo "$i" >&3
    exec 3>&-
    exec {fd}< f0
    exec {fd}<&-
    echo "$i" >> log.txt 2>&1
done
echo ok
