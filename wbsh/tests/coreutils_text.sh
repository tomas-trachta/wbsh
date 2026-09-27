#!/bin/bash
# Smoke test for the text-handling coreutils.

echo "=== cat"
printf 'one\ntwo\nthree\n' | cat
echo --
printf 'one\ntwo\n' | cat -n

echo "=== head / tail"
seq 1 10 | head
echo --
seq 1 10 | head -3
echo --
seq 1 10 | tail -3

echo "=== wc"
printf 'one two three\nfour five\n' | wc
echo --
printf 'one two three\nfour five\n' | wc -l
echo --
printf 'one two three\nfour five\n' | wc -w
echo --
printf 'one two three\nfour five\n' | wc -c

echo "=== basename / dirname"
basename /a/b/c.txt
basename /a/b/c.txt .txt
dirname /a/b/c.txt
dirname c.txt

echo "=== fold"
echo "thisislongtextforfolding" | fold -w 5
echo --
echo "alpha beta gamma delta" | fold -s -w 10

echo "=== expand / unexpand"
printf 'a\tb\tc\n' | expand -t 4

echo "=== comm"
printf 'a\nb\nc\n' > /tmp/wbsh-c1
printf 'b\nc\nd\n' > /tmp/wbsh-c2
comm /tmp/wbsh-c1 /tmp/wbsh-c2
echo --
comm -12 /tmp/wbsh-c1 /tmp/wbsh-c2
rm -f /tmp/wbsh-c1 /tmp/wbsh-c2

echo "=== nl numbering"
printf 'a\n\nb\nc\n' | nl

echo "=== rev"
echo "abcdef" | rev

echo "=== done"
