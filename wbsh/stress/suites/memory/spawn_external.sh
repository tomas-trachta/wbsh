#!/bin/bash
# Native process creation: handles of finished children must be closed.
# @stress n=40 handles=32
exe="${SYSTEMROOT:-$SystemRoot}/System32/hostname.exe"
for ((i = 0; i < STRESS_ITERS; i++)); do
    "$exe" > /dev/null
    x=$("$exe")
done
echo ok
