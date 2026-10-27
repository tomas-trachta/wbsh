#!/bin/bash
# cut: --output-delimiter, --complement, -b / -n / -z, long options, list
# syntax and usage errors (GNU coreutils semantics).

echo "=== --output-delimiter"
printf 'a,b,c\n' | cut -d, -f1,3 --output-delimiter=:
printf 'a,b,c,d\n' | cut -d, -f3,1 --output-delimiter=::
echo abcdef | cut -c1-2,3-4 --output-delimiter=:
echo abcdef | cut -c1-3,2-5 --output-delimiter=:
echo abcdef | cut -b5-,1 --output-delimiter=:

echo "=== --complement"
printf 'a,b,c\n' | cut -d, --complement -f2
echo abcdef | cut -c2,4 --complement --output-delimiter=:
echo abc | cut --compl -c1

echo "=== list syntax"
echo abcdef | cut -c '1 3'
echo abcdef | cut -c -3
echo abcdef | cut -c 4-
echo abcdef | cut -n -b2
printf 'a:b\nnodelim\n' | cut -d: -f2 -s
printf 'a:b\nnodelim\n' | cut --delimiter=: --fields=2
printf 'abc' | cut -c2 | od -c

echo "=== -z"
printf 'a1\0b2\0' | cut -z -c1 | od -c

echo "=== errors"
echo abc | cut -c0; echo "status $?"
echo abc | cut -f0; echo "status $?"
echo abc | cut -c3-1; echo "status $?"
echo abc | cut -c-; echo "status $?"
echo abc | cut -cx; echo "status $?"
echo abc | cut -fx; echo "status $?"
echo abc | cut -c ',1'; echo "status $?"
echo abc | cut -c1 -c2; echo "status $?"
echo abc | cut -c1 -d,; echo "status $?"
echo abc | cut -c1 -s; echo "status $?"
echo abc | cut -d ab -f1; echo "status $?"
echo abc | cut; echo "status $?"
echo abc | cut -x; echo "status $?"
echo abc | cut --bogus -f1; echo "status $?"
