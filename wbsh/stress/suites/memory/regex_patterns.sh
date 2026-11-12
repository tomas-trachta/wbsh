#!/bin/bash
# Many distinct regexes and globs: compiled-pattern caches must stay bounded.
# @stress n=2000
for ((i = 0; i < STRESS_ITERS; i++)); do
    [[ "abc$i" =~ ^abc${i}$ ]]
    [[ "abc$i" == abc$i* ]]
    s=${i//[$((i % 10))]/x}
done
echo ok
