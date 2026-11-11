#!/bin/bash
# while read over a file: the classic slow shell loop.
n=0
while IFS=' ' read -r a b rest; do
    n=$(( n + a ))
done < lines.txt
echo "$n"
