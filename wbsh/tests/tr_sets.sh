#!/bin/bash
# tr: GNU set syntax ([:class:], [=c=], [c*n], octal and letter escapes,
# ranges), squeeze / complement semantics, and file vs pipe input.

echo "=== classes"
echo 'hello world' | tr '[:lower:]' '[:upper:]'
echo 'Hello World' | tr '[:upper:]' '[:lower:]'
printf 'abc\n' | tr '[:upper:]' '[:lower:]'
printf 'a\tb c\n' | tr '[:blank:]' '_'
printf 'a\tb c\n' | tr '[:space:]' '_'
echo
printf 'x9F;\n' | tr '[:xdigit:]' 'Z'
printf 'a.b,c!\n' | tr -d '[:punct:]'
printf 'ab12\n' | tr -cd '[:digit:]'
echo
printf '\001\177x\n' | tr -d '[:cntrl:]' | od -c
printf 'ab\n' | tr '[:alnum:]' '[x*]'
echo

echo "=== escapes"
printf 'a1b5c\r\n' | tr -d '\015' | od -c
printf 'abc\n' | tr '\141' 'z'
printf 'a\\b\n' | tr '\\' '/'
printf 'abc\n' | tr 'a\-c' 'xyz'
printf 'A\n' | tr '\101' '\x'
printf 'a\nb\n' | tr '\n' ' '
echo
printf 'a\tb\n' | tr '\t' '\v' | od -c

echo "=== [c*n] and [=c=]"
printf 'abc\n' | tr 'a-c' '[x*]'
printf 'abcdef\n' | tr 'a-f' '[x*2]y'
printf 'abcdef\n' | tr 'a-f' 'p[x*]q'
printf 'abcdefghij\n' | tr 'a-j' '[x*010]'
printf 'abc\n' | tr '[a*2]' x
printf 'abc\n' | tr 'a' '[b*]x'
printf 'aeb\n' | tr '[=e=]' 'E'
printf 'abc\n' | tr -d '[=a=]'

echo "=== literal brackets and dashes"
printf 'a]b[c\n' | tr '[]' 'xy'
printf 'abc\n' | tr '[a-c]' 'xyz'
printf 'a-b\n' | tr a- xy
printf 'a-b\n' | tr -- -a yx

echo "=== squeeze / complement / truncate"
printf 'aabbcc  dd\n' | tr -s ' '
printf 'aabbcc  dd\n' | tr -s 'a-z'
printf 'aabbcc\n' | tr -s ab xy
printf 'aabbcc\n' | tr -ds a b
printf 'aabbcc\n' | tr -ds a c
printf 'abcabc\n' | tr -s a-c x
printf 'ab12\n' | tr -c 'a-z' 'X'
printf 'ab12\n' | tr -c ab 0-9 | od -c
printf 'hello\n' | tr -c 'a-z\n' '*'
printf 'abc\n' | tr -t a-c xy
printf 'aa\n' | tr aa xy

echo "=== file and pipe input"
printf 'first\nsecond line\nthird\n' > _tr_in.txt
tr 'a-z' 'A-Z' < _tr_in.txt
cat _tr_in.txt | tr -s 'a-z' 'A-Z'
{ read -r first; tr 'a-z' 'A-Z'; } < _tr_in.txt
rm -f _tr_in.txt
printf 'ab\0cd\n' | tr 'a-d' 'A-D' | od -c

echo "=== errors"
printf 'abc\n' | tr c-a x; echo "status $?"
printf 'abc\n' | tr '[:foo:]' x; echo "status $?"
printf 'abc\n' | tr a-z '[:digit:]'; echo "status $?"
printf 'abc\n' | tr 'a-z' '[:upper:]'; echo "status $?"
printf 'az\n' | tr '[:lower:]' '[:upper:][:upper:]'; echo "status $?"
printf 'abc\n' | tr a ''; echo "status $?"
printf 'abc\n' | tr 'ab' '[b*][c*]'; echo "status $?"
printf 'abc\n' | tr '[a*]' x; echo "status $?"
printf 'abc\n' | tr a '[=a=]'; echo "status $?"
printf 'abc\n' | tr a '[b*x]'; echo "status $?"
