#!/bin/bash
# eval and source parse new text each time; the parsed trees must be freed.
# @stress n=1000
printf 'v=$((v + 1))\n' > inc.sh
v=0
for ((i = 0; i < STRESS_ITERS; i++)); do
    eval "w$((i % 5))=\$i; if true; then u=\$((i * 2)); fi"
    . ./inc.sh
done
echo "$v"
