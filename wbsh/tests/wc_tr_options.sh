#!/bin/bash
# wc -L / -m with -c, tr -t and long options, and usage errors for unknown
# options and wrong operand counts (GNU coreutils semantics).

echo "=== wc -L"
printf 'abc\n' | wc -L
printf 'a\tb\n' | wc -L
printf 'abc\nhello world\n' | wc --max-line-length
printf 'no newline at end' | wc -L
printf 'abc\nhello world\n' | wc -lL

echo "=== wc -c -m"
printf 'abc\n' | wc -cm
printf 'abc\n' | wc --chars --bytes

echo "=== wc errors"
printf 'abc\n' | wc -x; echo "status $?"
printf 'abc\n' | wc --bogus; echo "status $?"

echo "=== tr -t / long options"
printf 'abc\n' | tr -t abc x
printf 'abc\n' | tr --truncate-set1 abc xy
printf 'abc\n' | tr abc x
printf 'abc\n' | tr --del a
printf 'abc\n' | tr --complement --delete 'a\n'
echo

echo "=== tr errors"
printf 'abc\n' | tr -x a b; echo "status $?"
printf 'abc\n' | tr a; echo "status $?"
printf 'abc\n' | tr -d a b; echo "status $?"
printf 'abc\n' | tr; echo "status $?"
printf 'abc\n' | tr a b c; echo "status $?"
