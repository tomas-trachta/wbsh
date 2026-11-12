#!/bin/bash
# Creating and destroying indexed and associative arrays.
# @stress n=1000
for ((i = 0; i < STRESS_ITERS; i++)); do
    a=(one two three "$i")
    declare -A m=([k]=v [i]=$i)
    a+=(four)
    m[x]=y
    unset a m
done
echo ok
