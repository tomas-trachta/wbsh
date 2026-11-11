#!/bin/bash
# read with IFS, -r, -a, -d, -n and here-strings.
read -r a b c <<< "one two three four"; echo "[$a][$b][$c]"
IFS=: read -r x y <<< "k:v:w"; echo "[$x][$y]"
read -r -a arr <<< "p q  r"; echo "${#arr[@]} ${arr[2]}"
read -d , -r first <<< "f1,f2"; echo "[$first]"
read -n 3 -r part <<< "abcdef"; echo "[$part]"
read -r line <<< '  lead and trail  '; echo "[$line]"
IFS= read -r raw <<< '  lead and trail  '; echo "[$raw]"
read back <<< 'a\b\
c'; echo "[$back]"
printf 'l1\nl2\nl3' | while read -r l; do echo "got $l"; done
printf 'l1\nl2\nl3' | { n=0; while read -r l || [ -n "$l" ]; do n=$((n+1)); done; echo "n $n"; }
mapfile -t lines < <(printf 'm1\nm2\n'); echo "${#lines[@]} ${lines[1]}"
