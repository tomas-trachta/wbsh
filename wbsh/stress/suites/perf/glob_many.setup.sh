#!/bin/bash
mkdir -p g
cd g || exit 1
for ((i = 0; i < 3000 * STRESS_N; i++)); do
    : > "file_$i.txt"
done
: > other.dat
