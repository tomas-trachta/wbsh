#!/bin/bash
# Parameter expansion in a loop: substitution, trimming, case changes, substrings.
s='The quick brown fox jumps over the lazy dog'
n=0
for ((i = 0; i < 20000 * STRESS_N; i++)); do
    t=${s//o/0}
    t=${t#The }
    t=${t%dog}
    t=${t^^}
    t=${t:2:10}
    n=$(( n + ${#t} ))
done
echo "$n"
