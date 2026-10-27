#!/bin/bash
# uniq: INPUT [OUTPUT] operands, -f / -s / -w key selection, -D grouping,
# -z records, and usage errors for unknown options (GNU coreutils semantics).

echo "=== OUTPUT operand"
printf 'x\nx\ny\n' > _uniq_in.txt
uniq _uniq_in.txt _uniq_out.txt
cat _uniq_out.txt
uniq -c _uniq_in.txt -
rm -f _uniq_in.txt _uniq_out.txt

echo "=== -f / -s / -w"
printf '1 a\n2 a\n' | uniq -f1
printf 'a  1 x\nb\t2 x\nc 3 y\n' | uniq -f2 -c
printf 'aXb\naYb\n' | uniq -w1 -c
printf 'xaa\nyab\n' | uniq -s1 -w1 -c
printf 'xaa\nyab\n' | uniq --skip-chars=1 --check-chars=2

echo "=== -i / -d / -u / -c"
printf 'A\na\nb\nB\nc\n' | uniq -ic
printf 'A\na\nb\nB\nc\n' | uniq -iu
printf 'A\na\nb\nB\nc\n' | uniq --ignore-case --rep

echo "=== -D / --all-repeated"
printf 'a\na\nb\nc\nc\nc\nd\n' | uniq -D
echo --
printf 'a\na\nb\nc\nc\nc\nd\n' | uniq --all-repeated=separate
echo --
printf 'a\na\nb\nc\nc\n' | uniq --all-repeated=prepend

echo "=== -z"
printf 'a\0a\0b' | uniq -z | od -c

echo "=== unterminated last line"
printf 'a\nb' | uniq

echo "=== errors"
printf 'a\n' | uniq -x; echo "status $?"
printf 'a\n' | uniq --bogus; echo "status $?"
printf 'a\n' | uniq -c -D; echo "status $?"
printf 'a\n' | uniq a b c; echo "status $?"
uniq -f x; echo "status $?"
printf 'a\n' | uniq --all-repeated=foo; echo "status $?"
uniq _uniq_missing.txt; echo "status $?"
