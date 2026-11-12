#!/bin/bash
# Bundled filters on a 200k-line CSV: grep, sed, cut, sort, uniq, awk, wc.
grep -c error data.csv
sed 's/user/u/g' data.csv | cut -d, -f2 | sort | uniq -c | sort -rn | head -n 3
awk -F, '$3 == "ok" { s += $4 } END { print s }' data.csv
sort -t, -k4,4n data.csv | tail -n 1
wc -l < data.csv
