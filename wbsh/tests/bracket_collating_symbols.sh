#!/bin/bash
# POSIX collating symbols, equivalence classes and [:ascii:] in bracket
# expressions.

t() { case $1 in $2) echo "match $3" ;; *) echo "no $3" ;; esac; }
t p '[[.a.]-[.z.]]' 1
t ' ' '[[.space.]]' 2
t ' ' '[[.grave-accent.]]' 3
t 4 '[[.-.]-9]' 4
t - '[[.hyphen.]-9]' 5
t c '[[.yyz.]-[.z.]]' 6
t c '[[.yyz.][.a.]-z]' 7
t p '[[.a.]-[.zz.]p]' 8
t $'\t' '[[.space.][.tab.][.newline.]]' 9
t abc '[[:alpha:]][[=b=]][[:ascii:]]' 10
t abc '[[:alpha:]][[=B=]][[:ascii:]]' 11
t a '[[=b=]' 12
