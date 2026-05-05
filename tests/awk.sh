#!/bin/bash
# Smoke test for awk.

echo "=== field selection"
printf 'one two three\nfour five six\n' | awk '{ print $1 }'
echo --
printf 'one two three\nfour five six\n' | awk '{ print $2, $3 }'
echo --
printf 'one two three\nfour five six\n' | awk '{ print NF }'
echo --
printf 'one two three\nfour five six\n' | awk '{ print NR, $0 }'

echo "=== field separator"
printf 'a:b:c\nd:e:f\n' | awk -F: '{ print $2 }'
echo --
printf 'a,b,c\n' | awk 'BEGIN { FS="," } { print $1, $3 }'

echo "=== BEGIN / END"
printf 'a\nb\nc\n' | awk 'BEGIN { print "start" } { print "row", NR, $0 } END { print "lines:", NR }'

echo "=== arithmetic"
printf '1\n2\n3\n4\n5\n' | awk '{ s += $1 } END { print s }'
echo --
printf '10\n20\n30\n' | awk '{ s += $1; n++ } END { print s/n }'

echo "=== conditionals"
printf '1\n2\n3\n4\n5\n' | awk '$1 > 2 { print }'
echo --
printf '1\n2\n3\n4\n5\n' | awk '$1 % 2 == 0 { print "even:", $1 }'

echo "=== printf"
printf 'a 1\nb 2\n' | awk '{ printf "%-3s %02d\n", $1, $2 }'

echo "=== string functions"
echo "hello world" | awk '{ print length($0) }'
echo "hello world" | awk '{ print toupper($0) }'
echo "HELLO" | awk '{ print tolower($0) }'
echo "abcdef" | awk '{ print substr($0, 2, 3) }'

echo "=== split"
echo "a,b,c,d" | awk '{ n=split($0, a, ","); for (i=1; i<=n; i++) print i, a[i] }'

echo "=== done"
