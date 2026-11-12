#!/bin/bash
# CreateProcess cost: a small native Windows program run many times.
exe="${SYSTEMROOT:-$SystemRoot}/System32/hostname.exe"
for ((i = 0; i < 100 * STRESS_N; i++)); do
    "$exe" > /dev/null
done
echo done
