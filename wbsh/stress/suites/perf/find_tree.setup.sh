#!/bin/bash
for ((d = 0; d < 30 * STRESS_N; d++)); do
    mkdir -p "t/d$d/sub/deeper"
    for ((f = 0; f < 30; f++)); do
        : > "t/d$d/f$f.c"
        : > "t/d$d/sub/f$f.h"
        : > "t/d$d/sub/deeper/f$f.txt"
    done
done
