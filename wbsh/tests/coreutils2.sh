#!/bin/bash
# Smoke test for the second batch of coreutils.

echo "=== sort"
printf 'banana\napple\ncherry\napple\n' | sort
echo --
printf 'banana\napple\ncherry\napple\n' | sort -u
echo --
printf '20\n3\n100\n5\n' | sort -n
echo --
printf '20\n3\n100\n5\n' | sort -rn

echo "=== uniq"
printf 'a\na\na\nb\nb\nc\n' | uniq
echo --
printf 'a\na\na\nb\nb\nc\n' | uniq -c
echo --
printf 'a\na\nb\nc\nc\n' | uniq -d
echo --
printf 'a\na\nb\nc\nc\n' | uniq -u

echo "=== tr"
printf 'Hello, World!\n' | tr a-z A-Z
echo --
printf 'a,b,c,d\n' | tr ',' ' '
echo --
printf 'aaabbcccd\n' | tr -s 'abc'
echo --
printf 'abc123xyz\n' | tr -d '0-9'

echo "=== cut"
printf 'one,two,three,four\nA,B,C,D\n' | cut -d, -f2
echo --
printf 'one,two,three,four\nA,B,C,D\n' | cut -d, -f2,4
echo --
printf 'abcdefghij\n' | cut -c2-5

echo "=== tee + paste + tac + rev + nl"
printf 'one\ntwo\nthree\n' | tee /tmp/wbsh-tee.txt > /dev/null
cat /tmp/wbsh-tee.txt
echo --
paste /tmp/wbsh-tee.txt /tmp/wbsh-tee.txt
echo --
tac /tmp/wbsh-tee.txt
echo --
echo "Hello" | rev
echo --
nl /tmp/wbsh-tee.txt
rm /tmp/wbsh-tee.txt

echo "=== date / seq"
date +"%Y-%m-%d"
seq 1 5
seq 0 2 10
echo --
seq -s , 1 5

echo "=== uname / id"
uname
uname -a
id

echo "=== realpath / readlink"
realpath .
realpath /c/Windows

echo "=== expr"
expr 2 + 3
expr 10 \* 4
expr length hello

echo "=== grep"
printf 'apple\nBanana\ncherry\nApple pie\n' | grep apple
echo --
printf 'apple\nBanana\ncherry\nApple pie\n' | grep -i apple
echo --
printf 'apple\nBanana\ncherry\nApple pie\n' | grep -v apple
echo --
printf 'apple\nBanana\ncherry\nApple pie\n' | grep -in '^a'
echo --
printf 'apple\nBanana\ncherry\nApple pie\n' | grep -c apple

echo "=== find"
find tests -type f -name '*.sh'

echo "=== xargs"
printf 'a b c\nd e\n' | xargs echo
printf '1\n2\n3\n4\n' | xargs -n 2 echo got

echo "=== done"
