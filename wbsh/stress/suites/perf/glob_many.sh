#!/bin/bash
# Globbing and directory listing over thousands of files.
cd g || exit 1
n=0
for f in *.txt; do
    n=$(( n + 1 ))
done
echo "$n"
files=(file_1*.txt)
echo "${#files[@]}"
ls | wc -l
