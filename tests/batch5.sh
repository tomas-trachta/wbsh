#!/bin/bash
echo "=== alias propagation through pipelines"
alias greet='echo hello-from-alias'
greet
greet | tr a-z A-Z
greet | wc -l

echo "=== globstar"
mkdir -p /tmp/wbsh-gs/a/b/c
touch /tmp/wbsh-gs/a/x.txt /tmp/wbsh-gs/a/b/y.txt /tmp/wbsh-gs/a/b/c/z.txt
echo "all .txt:"
echo /tmp/wbsh-gs/**/*.txt
echo "all in tree (last component):"
echo /tmp/wbsh-gs/**
rm -rf /tmp/wbsh-gs

echo "=== cmp"
echo same > /tmp/wbsh-cmp-a
echo same > /tmp/wbsh-cmp-b
cmp /tmp/wbsh-cmp-a /tmp/wbsh-cmp-b && echo "(identical)"
echo other > /tmp/wbsh-cmp-b
cmp /tmp/wbsh-cmp-a /tmp/wbsh-cmp-b
rm -f /tmp/wbsh-cmp-a /tmp/wbsh-cmp-b

echo "=== diff"
printf 'one\ntwo\nthree\n' > /tmp/wbsh-d-a
printf 'one\nTWO\nthree\nfour\n' > /tmp/wbsh-d-b
diff /tmp/wbsh-d-a /tmp/wbsh-d-b
echo --
diff -q /tmp/wbsh-d-a /tmp/wbsh-d-b
rm -f /tmp/wbsh-d-a /tmp/wbsh-d-b

echo "=== du"
mkdir -p /tmp/wbsh-du/sub
printf 'aaaa\n' > /tmp/wbsh-du/file1
printf 'bbbbbbbb\n' > /tmp/wbsh-du/sub/file2
du -s -h /tmp/wbsh-du
rm -rf /tmp/wbsh-du

echo "=== df"
df -h /c | head -2

echo "=== done"
