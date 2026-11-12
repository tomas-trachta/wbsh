#!/bin/bash
# Capturing a big output once per iteration; the old value must be freed.
# @stress n=30
for ((i = 0; i < STRESS_ITERS; i++)); do
    x=$(head -c 1000000 /dev/zero | tr '\0' a)
done
echo "${#x}"
